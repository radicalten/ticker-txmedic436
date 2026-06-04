#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

// Chess Piece Definitions
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

#define WHITE  8
#define BLACK  16

// Board representation
typedef struct {
    unsigned char board[64];
    int turn;       // WHITE or BLACK
    int castling;   // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep;         // En passant square (-1 if none)
} BoardState;

// History for Undo & PGN
typedef struct {
    BoardState state;
    char san[16];   // Standard Algebraic Notation
    char uci[8];    // Engine Coordinate Move (e.g., "e2e4")
} HistoryEntry;

#define MAX_HISTORY 2048
HistoryEntry history[MAX_HISTORY];
int history_count = 0;

BoardState current_state;

// UI Selection & Cursor State
int cursor_r = 0; // 0..7
int cursor_c = 0; // 0..7
int selected_sq = -1; // -1 if no selection
int running = 1;

// Engine Subprocess Communication
int engine_in[2];   // GUI writes to engine's stdin
int engine_out[2];  // GUI reads from engine's stdout
pid_t engine_pid = -1;
int engine_active = 0;
int engine_thinking = 0;
char engine_name[128] = "Disconnected";

// Terminal Configuration
struct termios orig_termios;

// Forward Declarations
void init_board(BoardState *s);
int is_move_legal(BoardState *s, int from, int to);
int is_pseudo_legal(BoardState *s, int from, int to);
int is_square_attacked(BoardState *s, int sq, int attacker_color);
int is_king_in_check(BoardState *s, int color);
int has_no_legal_moves(BoardState *s, int color);
void apply_move_internal(BoardState *s, int from, int to, int promo);
void generate_san(BoardState *s, int from, int to, int promo, char *san);
void draw_board();
void draw_side_panel();
void trigger_engine();

// Clean Exit and Terminal Restoration
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?1049l"); // Restore original screen buffer
    printf("\033[?25h");   // Re-show terminal cursor
    fflush(stdout);
    
    // Kill engine child process cleanly
    if (engine_pid > 0) {
        write(engine_in[1], "quit\n", 5);
        close(engine_in[1]);
        close(engine_out[0]);
        kill(engine_pid, SIGTERM);
        waitpid(engine_pid, NULL, WNOHANG);
    }
}

void handle_sigint(int sig) {
    (void)sig;
    exit(0); // Triggers raw mode disable via atexit
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    signal(SIGINT, handle_sigint);
    
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    
    printf("\033[?1049h"); // Use alternate screen buffer
    printf("\033[?25l");   // Hide cursor
    fflush(stdout);
}

// Spawns Stockfish via standard macOS / POSIX pipes
int start_engine(const char *path) {
    if (pipe(engine_in) < 0 || pipe(engine_out) < 0) return 0;
    
    engine_pid = fork();
    if (engine_pid < 0) return 0;
    
    if (engine_pid == 0) {
        // Redirect standard I/O for Child
        dup2(engine_in[0], STDIN_FILENO);
        dup2(engine_out[1], STDOUT_FILENO);
        close(engine_in[1]);
        close(engine_out[0]);
        
        // Suppress engine stderr output to prevent terminal artifacts
        int fd_null = open("/dev/null", O_WRONLY);
        if (fd_null != -1) {
            dup2(fd_null, STDERR_FILENO);
            close(fd_null);
        }
        
        execlp(path, path, (char *)NULL);
        execl(path, path, (char *)NULL);
        exit(1); // Exit if exec fails
    }
    
    // Parent Process configuration
    close(engine_in[0]);
    close(engine_out[1]);
    
    // Set non-blocking reads on engine stdout
    int flags = fcntl(engine_out[0], F_GETFL, 0);
    fcntl(engine_out[0], F_SETFL, flags | O_NONBLOCK);
    return 1;
}

// Non-blocking engine output buffer accumulator
char engine_read_buffer[8192];
int engine_read_len = 0;

int read_engine_line(char *line_out, int max_len) {
    char ch;
    while (read(engine_out[0], &ch, 1) > 0) {
        if (ch == '\n') {
            engine_read_buffer[engine_read_len] = '\0';
            strncpy(line_out, engine_read_buffer, max_len);
            engine_read_len = 0;
            return 1;
        } else if (engine_read_len < (int)sizeof(engine_read_buffer) - 2) {
            engine_read_buffer[engine_read_len++] = ch;
        }
    }
    return 0;
}

void send_engine(const char *cmd) {
    if (engine_pid <= 0) return;
    write(engine_in[1], cmd, strlen(cmd));
    write(engine_in[1], "\n", 1);
}

// Probe typical system locations on macOS
int init_engine_uci() {
    const char *paths[] = {
        "stockfish",
        "/opt/homebrew/bin/stockfish",
        "/usr/local/bin/stockfish",
        "./stockfish"
    };
    int path_count = sizeof(paths) / sizeof(paths[0]);
    int connected = 0;
    
    for (int i = 0; i < path_count; i++) {
        if (start_engine(paths[i])) {
            connected = 1;
            break;
        }
    }
    if (!connected) return 0;
    
    send_engine("uci");
    char line[1024];
    int uci_ok = 0;
    
    for (int i = 0; i < 200; i++) { // Max 2 seconds spin wait
        while (read_engine_line(line, sizeof(line))) {
            if (strncmp(line, "id name ", 8) == 0) {
                strncpy(engine_name, line + 8, sizeof(engine_name) - 1);
            }
            if (strcmp(line, "uciok") == 0) uci_ok = 1;
        }
        if (uci_ok) break;
        usleep(10000);
    }
    if (!uci_ok) return 0;
    
    send_engine("isready");
    int ready = 0;
    for (int i = 0; i < 200; i++) {
        while (read_engine_line(line, sizeof(line))) {
            if (strcmp(line, "readyok") == 0) ready = 1;
        }
        if (ready) break;
        usleep(10000);
    }
    if (!ready) return 0;
    
    send_engine("ucinewgame");
    engine_active = 1;
    return 1;
}

void init_board(BoardState *s) {
    memset(s->board, EMPTY, 64);
    s->board[0] = WHITE | ROOK;   s->board[7] = WHITE | ROOK;
    s->board[1] = WHITE | KNIGHT; s->board[6] = WHITE | KNIGHT;
    s->board[2] = WHITE | BISHOP; s->board[5] = WHITE | BISHOP;
    s->board[3] = WHITE | QUEEN;  s->board[4] = WHITE | KING;
    
    s->board[56] = BLACK | ROOK;   s->board[63] = BLACK | ROOK;
    s->board[57] = BLACK | KNIGHT; s->board[62] = BLACK | KNIGHT;
    s->board[58] = BLACK | BISHOP; s->board[61] = BLACK | BISHOP;
    s->board[59] = BLACK | QUEEN;  s->board[60] = BLACK | KING;
    
    for (int i = 0; i < 8; i++) {
        s->board[8 + i] = WHITE | PAWN;
        s->board[48 + i] = BLACK | PAWN;
    }
    s->turn = WHITE;
    s->castling = 15; // WK | WQ | BK | BQ
    s->ep = -1;
}

// Convert board square to UCI Coordinate (e.g., 12 -> "e2")
void format_square(int sq, char *buf) {
    buf[0] = 'a' + (sq % 8);
    buf[1] = '1' + (sq / 8);
    buf[2] = '\0';
}

int parse_square(const char *str) {
    int file = str[0] - 'a';
    int rank = str[1] - '1';
    if (file < 0 || file >= 8 || rank < 0 || rank >= 8) return -1;
    return rank * 8 + file;
}

// Full Chess validation logic
int is_pseudo_legal(BoardState *s, int from, int to) {
    int p = s->board[from] & 7;
    int c = s->board[from] & 24;
    int tc = s->board[to] & 24;
    if (tc == c) return 0;
    
    int r1 = from / 8, c1 = from % 8;
    int r2 = to / 8, c2 = to % 8;
    int dr = r2 - r1, dc = c2 - c1;
    int adr = abs(dr), adc = abs(dc);
    
    switch (p) {
        case PAWN: {
            int dir = (c == WHITE) ? 1 : -1;
            int start_rank = (c == WHITE) ? 1 : 6;
            if (dc == 0 && dr == dir && s->board[to] == EMPTY) return 1;
            if (dc == 0 && dr == 2 * dir && r1 == start_rank && s->board[to] == EMPTY && s->board[from + 8*dir] == EMPTY) return 1;
            if (adc == 1 && dr == dir) {
                if (s->board[to] != EMPTY) return 1;
                if (to == s->ep) return 1;
            }
            return 0;
        }
        case KNIGHT:
            return (adr * adc == 2);
        case BISHOP:
            if (adr != adc) return 0;
            break;
        case ROOK:
            if (dr != 0 && dc != 0) return 0;
            break;
        case QUEEN:
            if (adr != adc && dr != 0 && dc != 0) return 0;
            break;
        case KING:
            if (adr <= 1 && adc <= 1) return 1;
            // Castling checks
            if (dr == 0 && adc == 2) {
                if (is_square_attacked(s, from, (c == WHITE) ? BLACK : WHITE)) return 0;
                if (c == WHITE) {
                    if (to == 6 && (s->castling & 1)) {
                        if (s->board[5] == EMPTY && s->board[6] == EMPTY && !is_square_attacked(s, 5, BLACK)) return 1;
                    }
                    if (to == 2 && (s->castling & 2)) {
                        if (s->board[1] == EMPTY && s->board[2] == EMPTY && s->board[3] == EMPTY && !is_square_attacked(s, 3, BLACK)) return 1;
                    }
                } else {
                    if (to == 62 && (s->castling & 4)) {
                        if (s->board[61] == EMPTY && s->board[62] == EMPTY && !is_square_attacked(s, 61, WHITE)) return 1;
                    }
                    if (to == 58 && (s->castling & 8)) {
                        if (s->board[57] == EMPTY && s->board[58] == EMPTY && s->board[59] == EMPTY && !is_square_attacked(s, 59, WHITE)) return 1;
                    }
                }
            }
            return 0;
    }
    
    // Path validation for sliders (Bishop, Rook, Queen)
    int step_r = (dr == 0) ? 0 : (dr > 0 ? 1 : -1);
    int step_c = (dc == 0) ? 0 : (dc > 0 ? 1 : -1);
    int curr_r = r1 + step_r, curr_c = c1 + step_c;
    while (curr_r != r2 || curr_c != c2) {
        if (s->board[curr_r * 8 + curr_c] != EMPTY) return 0;
        curr_r += step_r;
        curr_c += step_c;
    }
    return 1;
}

int is_square_attacked(BoardState *s, int sq, int attacker_color) {
    int r = sq / 8, c = sq % 8;
    int p_dir = (attacker_color == WHITE) ? -1 : 1;
    int pr = r + p_dir;
    
    // Pawn attacks
    if (pr >= 0 && pr < 8) {
        if (c - 1 >= 0 && s->board[pr*8 + (c - 1)] == (attacker_color | PAWN)) return 1;
        if (c + 1 < 8 && s->board[pr*8 + (c + 1)] == (attacker_color | PAWN)) return 1;
    }
    // Knight attacks
    int kr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int kc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kr[i], nc = c + kc[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (s->board[nr*8 + nc] == (attacker_color | KNIGHT)) return 1;
        }
    }
    // King attacks
    int kir[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int kic[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kir[i], nc = c + kic[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (s->board[nr*8 + nc] == (attacker_color | KING)) return 1;
        }
    }
    // Diagonal sliding (Bishop / Queen)
    int dr[] = {-1, -1, 1, 1};
    int dc[] = {-1, 1, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + dr[i], nc = c + dc[i];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int p = s->board[nr*8 + nc];
            if (p != EMPTY) {
                if (p == (attacker_color | BISHOP) || p == (attacker_color | QUEEN)) return 1;
                break;
            }
            nr += dr[i]; nc += dc[i];
        }
    }
    // Orthogonal sliding (Rook / Queen)
    int rr[] = {-1, 1, 0, 0};
    int rc[] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + rr[i], nc = c + rc[i];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int p = s->board[nr*8 + nc];
            if (p != EMPTY) {
                if (p == (attacker_color | ROOK) || p == (attacker_color | QUEEN)) return 1;
                break;
            }
            nr += rr[i]; nc += rc[i];
        }
    }
    return 0;
}

int is_king_in_check(BoardState *s, int color) {
    for (int i = 0; i < 64; i++) {
        if (s->board[i] == (color | KING)) {
            return is_square_attacked(s, i, (color == WHITE) ? BLACK : WHITE);
        }
    }
    return 0;
}

int is_move_legal(BoardState *s, int from, int to) {
    if (from < 0 || from >= 64 || to < 0 || to >= 64) return 0;
    if ((s->board[from] & 24) != s->turn) return 0;
    if (!is_pseudo_legal(s, from, to)) return 0;
    
    BoardState temp = *s;
    apply_move_internal(&temp, from, to, EMPTY);
    if (is_king_in_check(&temp, s->turn)) return 0;
    return 1;
}

int has_no_legal_moves(BoardState *s, int color) {
    BoardState temp = *s;
    temp.turn = color;
    for (int from = 0; from < 64; from++) {
        if ((temp.board[from] & 24) == color) {
            for (int to = 0; to < 64; to++) {
                if (is_move_legal(&temp, from, to)) return 0;
            }
        }
    }
    return 1;
}

void apply_move_internal(BoardState *s, int from, int to, int promo) {
    int p = s->board[from] & 7;
    int c = s->board[from] & 24;
    
    if (p == PAWN && to == s->ep) {
        s->board[to - (c == WHITE ? 8 : -8)] = EMPTY; // En passant capture
    }
    
    if (p == KING) {
        if (from == 4 && to == 6) { s->board[5] = WHITE | ROOK; s->board[7] = EMPTY; }
        else if (from == 4 && to == 2) { s->board[3] = WHITE | ROOK; s->board[0] = EMPTY; }
        else if (from == 60 && to == 62) { s->board[61] = BLACK | ROOK; s->board[63] = EMPTY; }
        else if (from == 60 && to == 58) { s->board[59] = BLACK | ROOK; s->board[56] = EMPTY; }
    }
    
    // Castling Rights Maintenance
    if (p == KING) {
        s->castling &= (c == WHITE) ? ~3 : ~12;
    }
    if (p == ROOK) {
        if (from == 0) s->castling &= ~2;
        if (from == 7) s->castling &= ~1;
        if (from == 56) s->castling &= ~8;
        if (from == 63) s->castling &= ~4;
    }
    if (to == 0) s->castling &= ~2;
    if (to == 7) s->castling &= ~1;
    if (to == 56) s->castling &= ~8;
    if (to == 63) s->castling &= ~4;
    
    // EP Target Square Tracking
    if (p == PAWN && abs(to - from) == 16) {
        s->ep = from + (c == WHITE ? 8 : -8);
    } else {
        s->ep = -1;
    }
    
    s->board[to] = promo ? (c | promo) : s->board[from];
    s->board[from] = EMPTY;
    s->turn = (s->turn == WHITE) ? BLACK : WHITE;
}

// Generate human-readable PGN moves
void generate_san(BoardState *s, int from, int to, int promo, char *san) {
    int p = s->board[from] & 7;
    int c = s->board[from] & 24;
    int tc = s->board[to];
    
    if (p == KING && abs(from - to) == 2) {
        strcpy(san, (to > from) ? "O-O" : "O-O-O");
        return;
    }
    
    int ptr = 0;
    if (p != PAWN) {
        const char pcs[] = "?NBRQK";
        san[ptr++] = pcs[p];
        
        // Disambiguation
        int ambiguous = 0, same_file = 0, same_rank = 0;
        for (int i = 0; i < 64; i++) {
            if (i != from && s->board[i] == (c | p)) {
                if (is_pseudo_legal(s, i, to)) {
                    ambiguous = 1;
                    if (i % 8 == from % 8) same_file = 1;
                    if (i / 8 == from / 8) same_rank = 1;
                }
            }
        }
        if (ambiguous) {
            if (!same_file) san[ptr++] = 'a' + (from % 8);
            else if (!same_rank) san[ptr++] = '1' + (from / 8);
            else {
                san[ptr++] = 'a' + (from % 8);
                san[ptr++] = '1' + (from / 8);
            }
        }
    } else if (tc != EMPTY || to == s->ep) {
        san[ptr++] = 'a' + (from % 8);
    }
    
    if (tc != EMPTY || (p == PAWN && to == s->ep)) san[ptr++] = 'x';
    san[ptr++] = 'a' + (to % 8);
    san[ptr++] = '1' + (to / 8);
    
    if (promo) {
        san[ptr++] = '=';
        const char pcs[] = "?NBRQK";
        san[ptr++] = pcs[promo];
    }
    
    BoardState temp = *s;
    apply_move_internal(&temp, from, to, promo);
    int opp = (c == WHITE) ? BLACK : WHITE;
    if (is_king_in_check(&temp, opp)) {
        san[ptr++] = has_no_legal_moves(&temp, opp) ? '#' : '+';
    }
    san[ptr] = '\0';
}

void trigger_engine() {
    if (!engine_active || engine_pid <= 0) return;
    engine_thinking = 1;
    send_engine("position startpos moves");
    for (int i = 0; i < history_count; i++) {
        char temp[16];
        sprintf(temp, " %s", history[i].uci);
        write(engine_in[1], temp, strlen(temp));
    }
    send_engine("\ngo movetime 1000"); // Engine has 1s to respond
}

// GUI promotion selection prompt overlay
int get_promotion_piece() {
    printf("\033[24;42H\033[1;33mPromote: [Q]ueen, [R]ook, [B]ishop, [N]ight?\033[0m");
    fflush(stdout);
    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) > 0) {
            if (c == 'q' || c == 'Q') return QUEEN;
            if (c == 'r' || c == 'R') return ROOK;
            if (c == 'b' || c == 'B') return BISHOP;
            if (c == 'n' || c == 'N') return KNIGHT;
        }
    }
}

void handle_keyboard() {
    char c;
    if (read(STDIN_FILENO, &c, 1) <= 0) return;
    
    if (c == '\033') { // Escape / Arrow Sequence
        char seq[2];
        int old_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, old_flags | O_NONBLOCK);
        if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
            if (seq[0] == '[') {
                switch (seq[1]) {
                    case 'A': if (cursor_r < 7) cursor_r++; break; // Up
                    case 'B': if (cursor_r > 0) cursor_r--; break; // Down
                    case 'C': if (cursor_c < 7) cursor_c++; break; // Right
                    case 'D': if (cursor_c > 0) cursor_c--; break; // Left
                }
            }
        }
        fcntl(STDIN_FILENO, F_SETFL, old_flags);
    } else if (c == ' ' || c == '\n' || c == '\r') {
        int sq = cursor_r * 8 + cursor_c;
        if (selected_sq == -1) {
            if (current_state.board[sq] != EMPTY && (current_state.board[sq] & 24) == current_state.turn) {
                selected_sq = sq;
            }
        } else {
            if (sq == selected_sq) {
                selected_sq = -1;
            } else if (current_state.board[sq] != EMPTY && (current_state.board[sq] & 24) == current_state.turn) {
                selected_sq = sq; // Shift selection
            } else {
                if (is_move_legal(&current_state, selected_sq, sq)) {
                    int promo = EMPTY;
                    int p = current_state.board[selected_sq] & 7;
                    if (p == PAWN && (sq / 8 == 7 || sq / 8 == 0)) {
                        promo = get_promotion_piece();
                    }
                    
                    char san[16], uci[8], f_sq[3], t_sq[3];
                    generate_san(&current_state, selected_sq, sq, promo, san);
                    format_square(selected_sq, f_sq);
                    format_square(sq, t_sq);
                    sprintf(uci, "%s%s", f_sq, t_sq);
                    if (promo) {
                        const char p_chars[] = "?pbrqk";
                        sprintf(uci + 4, "%c", p_chars[promo]);
                    }
                    
                    // Push to history
                    history[history_count].state = current_state;
                    strcpy(history[history_count].san, san);
                    strcpy(history[history_count].uci, uci);
                    history_count++;
                    
                    apply_move_internal(&current_state, selected_sq, sq, promo);
                    selected_sq = -1;
                    
                    if (engine_active && current_state.turn == BLACK) {
                        trigger_engine();
                    }
                }
            }
        }
    } else if (c == 'u' || c == 'U') { // Undo (Rolls back both player and engine move)
        if (!engine_thinking) {
            int rollback = (engine_active) ? 2 : 1;
            if (history_count >= rollback) {
                history_count -= rollback;
                if (history_count == 0) init_board(&current_state);
                else current_state = history[history_count - 1].state;
                selected_sq = -1;
            }
        }
    } else if (c == 'n' || c == 'N') { // Reset Board
        init_board(&current_state);
        history_count = 0;
        selected_sq = -1;
    } else if (c == 'e' || c == 'E') { // Toggle engine
        engine_active = !engine_active;
        if (engine_active && current_state.turn == BLACK && !engine_thinking) {
            trigger_engine();
        }
    } else if (c == 'q' || c == 'Q') {
        running = 0;
    }
}

void parse_and_apply_engine_move(const char *line) {
    char move_str[16];
    if (sscanf(line, "bestmove %s", move_str) == 1) {
        int from = parse_square(move_str);
        int to = parse_square(move_str + 2);
        if (from == -1 || to == -1) return;
        
        int promo = EMPTY;
        if (strlen(move_str) >= 5) {
            char p_char = move_str[4];
            if (p_char == 'q') promo = QUEEN;
            else if (p_char == 'r') promo = ROOK;
            else if (p_char == 'b') promo = BISHOP;
            else if (p_char == 'n') promo = KNIGHT;
        }
        
        char san[16];
        generate_san(&current_state, from, to, promo, san);
        
        history[history_count].state = current_state;
        strcpy(history[history_count].san, san);
        strncpy(history[history_count].uci, move_str, 8);
        history_count++;
        
        apply_move_internal(&current_state, from, to, promo);
    }
}

void draw_board() {
    const char *syms[] = {" ", "♟", "♞", "♝", "♜", "♛", "♚"};
    
    for (int vr = 0; vr < 8; vr++) {
        int r = 7 - vr;
        for (int cell_row = 0; cell_row < 2; cell_row++) {
            // Print rank side-numbers
            printf("\033[%d;2H", vr * 2 + 3);
            if (cell_row == 0) printf(" %d ", r + 1);
            else printf("   ");
            
            for (int c = 0; c < 8; c++) {
                int sq = r * 8 + c;
                int is_dark = (r + c) % 2 == 0;
                
                // Color theme: Mahogany Wood (Dark) and Sandy Tan (Light)
                const char *bg = is_dark ? "\033[48;5;95m" : "\033[48;5;180m";
                
                if (sq == selected_sq) {
                    bg = "\033[48;5;214m"; // Gold Highlight
                } else if (r == cursor_r && c == cursor_c) {
                    bg = "\033[48;5;39m";  // Sky Blue Cursor
                }
                
                printf("\033[%d;%dH%s", vr * 2 + 3 + cell_row, 5 + c * 4, bg);
                if (cell_row == 0) {
                    int p = current_state.board[sq];
                    if (p == EMPTY) {
                        printf("    ");
                    } else {
                        const char *fg = (p & WHITE) ? "\033[38;5;255m\033[1m" : "\033[38;5;233m\033[1m";
                        printf(" %s%s%s  ", fg, syms[p & 7], bg);
                    }
                } else {
                    printf("    ");
                }
            }
        }
    }
    printf("\033[0m"); // Reset terminal colors
    
    // File letters footer
    printf("\033[%d;5H", 8 * 2 + 3);
    for (int c = 0; c < 8; c++) printf("  %c ", 'a' + c);
    printf("\n");
}

void draw_side_panel() {
    // Clear Panel rows
    for (int i = 1; i <= 24; i++) printf("\033[%d;40H\033[K", i);
    
    printf("\033[2;42H\033[1;36m♟ TERMCHESS GUI v1.0\033[0m");
    printf("\033[4;42HEngine: \033[32m%s\033[0m", engine_name);
    printf("\033[5;42HMode:   %s", engine_active ? "Player vs Engine" : "Player vs Player");
    printf("\033[6;42HActive: \033[1m%s\033[0m", (current_state.turn == WHITE) ? "WHITE" : "BLACK");
    
    if (engine_thinking) {
        printf("\033[8;42H\033[5;31m⌛ Stockfish is searching...\033[0m");
    }
    
    // Render PGN History Stream
    printf("\033[10;42H\033[4;37mPGN Move History:\033[0m");
    int row = 12;
    int idx = 0;
    while (idx < history_count) {
        int move_num = (idx / 2) + 1;
        printf("\033[%d;42H%2d. %-8s", row, move_num, history[idx].san);
        if (idx + 1 < history_count) {
            printf(" %-8s", history[idx + 1].san);
        } else {
            printf(" ...");
        }
        row++;
        idx += 2;
        if (row > 21) {
            printf("\033[%d;42H... and %d more", row, (history_count - idx) / 2);
            break;
        }
    }
    
    // Visual Control Footer
    printf("\033[23;42H\033[2;37mArrows: Select | Space/Enter: Move\033[0m");
    printf("\033[24;42H\033[2;37m[U] Undo  |  [N] New Game  |  [E] Engine  |  [Q] Quit\033[0m");
    fflush(stdout);
}

int main() {
    init_board(&current_state);
    enable_raw_mode();
    
    // Initialize Screen
    printf("\033[2J\033[H");
    draw_side_panel();
    draw_board();
    
    // Attempt system engine integration
    init_engine_uci();
    draw_side_panel();
    
    fd_set read_fds;
    struct timeval timeout;
    
    while (running) {
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        int max_fd = STDIN_FILENO;
        
        if (engine_thinking && engine_pid > 0) {
            FD_SET(engine_out[0], &read_fds);
            if (engine_out[0] > max_fd) max_fd = engine_out[0];
        }
        
        timeout.tv_sec = 0;
        timeout.tv_usec = 25000; // 25ms poll delay (highly responsive)
        
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (activity > 0) {
            if (FD_ISSET(STDIN_FILENO, &read_fds)) {
                handle_keyboard();
                draw_board();
                draw_side_panel();
            }
            if (engine_thinking && FD_ISSET(engine_out[0], &read_fds)) {
                char line[1024];
                while (read_engine_line(line, sizeof(line))) {
                    if (strncmp(line, "bestmove", 8) == 0) {
                        parse_and_apply_engine_move(line);
                        engine_thinking = 0;
                        draw_board();
                        draw_side_panel();
                        break;
                    }
                }
            }
        }
    }
    return 0;
}
