#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_MOVES 1024

// --- Core Data Structures ---
typedef struct {
    int from;
    int to;
    int promo; // 0=none, 2=N, 3=B, 4=R, 5=Q
} Move;

typedef struct {
    int board[64];    // 1 to 6 (P, N, B, R, Q, K), negative for Black, 0 for empty
    int turn;         // 1 = White, -1 = Black
    int castling;     // Bits: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep_square;    // -1 or 0-63
    int halfmove;
    int fullmove;
    char pgn[16];     // SAN move notation string for this specific state transition
} GameState;

typedef enum { TC_DEPTH, TC_NODES, TC_TIME } TCType;

// --- Global App State ---
GameState state;
GameState history[MAX_MOVES];
Move moves_played[MAX_MOVES];
int history_count = 0;

int cursor_sq = 52; // Starts on e2
int selected_sq = -1;

TCType tc_type = TC_DEPTH;
int tc_value = 10; // Default: depth 10

int engine_color = -1; // -1 = Black, 1 = White, 0 = Manual PvP
char engine_path[256] = "stockfish";
int to_engine[2], from_engine[2];
pid_t engine_pid = -1;
int engine_thinking = 0;

struct termios orig_termios;

// --- Helper Declarations ---
void init_board(GameState *st);
int generate_pseudo_moves(const int *board, int turn, int castling, int ep_sq, Move *moves);
int get_legal_moves(const int *board, int turn, int castling, int ep_sq, Move *pseudo, int pc, Move *legal);
int is_attacked(const int *board, int target_sq, int attacker_color);
void apply_move(GameState *st, Move m);
void get_san(const int *board, int turn, int castling, int ep_sq, Move m, char *buf);

// --- Terminal Control ---
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // Show cursor
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

// Read non-blocking single-key or escape codes
int get_key() {
    char c;
    int nread = read(STDIN_FILENO, &c, 1);
    if (nread <= 0) return 0;
    if (c == '\033') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return '\033';
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return '\033';
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 'W'; // Up Arrow -> W
                case 'B': return 'S'; // Down Arrow -> S
                case 'C': return 'D'; // Right Arrow -> D
                case 'D': return 'A'; // Left Arrow -> A
            }
        }
        return '\033';
    }
    return c;
}

// --- Engine Communication via Subprocess ---
void kill_engine() {
    if (engine_pid != -1) {
        kill(engine_pid, SIGKILL);
        waitpid(engine_pid, NULL, 0);
        close(to_engine[1]);
        close(from_engine[0]);
        engine_pid = -1;
    }
}

void init_engine() {
    kill_engine();
    if (pipe(to_engine) < 0 || pipe(from_engine) < 0) return;
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[1]);
        close(from_engine[0]);
        int dev_null = open("/dev/null", O_WRONLY);
        dup2(dev_null, STDERR_FILENO);
        execlp(engine_path, engine_path, (char*)NULL);
        exit(1); // Exit child if engine path fails
    }
    close(to_engine[0]);
    close(from_engine[1]);
    fcntl(from_engine[0], F_SETFL, O_NONBLOCK);

    write(to_engine[1], "uci\n", 4);
    usleep(50000);
    write(to_engine[1], "isready\n", 8);
}

int is_engine_alive() {
    if (engine_pid == -1) return 0;
    int status;
    pid_t res = waitpid(engine_pid, &status, WNOHANG);
    if (res > 0) {
        engine_pid = -1;
        return 0;
    }
    return 1;
}

void write_engine(const char *cmd) {
    if (is_engine_alive()) {
        write(to_engine[1], cmd, strlen(cmd));
    }
}

int engine_available() {
    if (!is_engine_alive()) return 0;
    struct pollfd pfd;
    pfd.fd = from_engine[0];
    pfd.events = POLLIN;
    return poll(&pfd, 1, 0) > 0;
}

int read_engine_line(char *buf, int max_len) {
    int len = 0;
    char c;
    while (len < max_len - 1) {
        if (read(from_engine[0], &c, 1) > 0) {
            if (c == '\n') break;
            if (c != '\r') buf[len++] = c;
        } else {
            break;
        }
    }
    buf[len] = '\0';
    return len;
}

// --- Graphics & Rendering ---
const char* get_piece_glyph(int piece) {
    switch (abs(piece)) {
        case 1: return "♟";
        case 2: return "♞";
        case 3: return "♝";
        case 4: return "♜";
        case 5: return "♛";
        case 6: return "♚";
        default: return " ";
    }
}

void draw_ui(const char *status_msg) {
    printf("\033[H"); // Reset cursor to top-left (no clear screen = no flicker)
    printf("\n   \033[1;36m=== TERMINAL CHESS CORE ===\033[0m\033[K\n\n");
    printf("       a  b  c  d  e  f  g  h\033[K\n");
    printf("     +------------------------+\033[K\n");

    for (int r = 0; r < 8; r++) {
        printf("   %d |", 8 - r);
        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            int piece = state.board[sq];

            int bg = ((r + c) % 2 == 0) ? 252 : 239; // Default board squares (sand vs dark grey)
            if (sq == cursor_sq) bg = 81;            // Cursor blue
            else if (sq == selected_sq) bg = 214;    // Selected orange

            int fg = (piece > 0) ? 231 : 16;         // White vs Black chess piece color
            printf("\033[1;38;5;%d;48;5;%dm %s \033[0m", fg, bg, get_piece_glyph(piece));
        }
        printf("| %d\033[K\n", 8 - r);
    }
    printf("     +------------------------+\033[K\n");
    printf("       a  b  c  d  e  f  g  h\033[K\n\n");

    printf(" Turn: %s\033[K\n", (state.turn == 1) ? "\033[1;37mWhite (Human)\033[0m" : "\033[1;30mBlack\033[0m");
    const char* tc_names[] = { "Search Depth", "Max Nodes", "Move Time (ms)" };
    printf(" Configured Limit: %s (%d)\033[K\n", tc_names[tc_type], tc_value);
    printf(" Match Engine: %s (%s)\033[K\n", (engine_color == 1) ? "White" : (engine_color == -1) ? "Black" : "None (PvP Mode)", is_engine_alive() ? "Online" : "Offline");
    printf(" Game Status: %s\033[K\n", status_msg);

    // Render PGN Moves beautifully
    printf(" PGN: ");
    int move_num = 1;
    for (int i = 0; i < history_count; i++) {
        if (i % 2 == 0) printf("%d. ", move_num++);
        printf("%s ", history[i].pgn);
    }
    printf("\033[K\n\n");

    printf(" \033[1;33mControls:\033[0m\033[K\n");
    printf(" [Arrows/WASD] Move Cursor  [Space/Enter] Select/Place  [U] Undo\033[K\n");
    printf(" [T] Time Control Setup    [E] Configure Engine Path    [R] Reset Game  [Q] Quit\033[K\n");
    fflush(stdout);
}

// --- Dynamic Input Dialogs ---
void get_string_input(const char *prompt, char *output, int max_len) {
    disable_raw_mode();
    printf("\033[?25h"); // Show cursor
    printf("\n\033[1;33m%s\033[0m", prompt);
    fflush(stdout);
    if (fgets(output, max_len, stdin)) {
        output[strcspn(output, "\n")] = 0;
    }
    enable_raw_mode();
}

void configure_time_control() {
    char buf[64];
    get_string_input("Select metric - [1] Depth  [2] Nodes  [3] Milliseconds: ", buf, sizeof(buf));
    int choice = atoi(buf);
    if (choice >= 1 && choice <= 3) {
        tc_type = (TCType)(choice - 1);
        get_string_input("Enter limit value: ", buf, sizeof(buf));
        int val = atoi(buf);
        if (val > 0) tc_value = val;
    }
}

void configure_engine() {
    char buf[128];
    get_string_input("Assign Engine Side - [1] White  [-1] Black  [0] Manual PvP: ", buf, sizeof(buf));
    engine_color = atoi(buf);
    get_string_input("Enter path to UCI Engine (e.g., 'stockfish'): ", buf, sizeof(buf));
    if (strlen(buf) > 0) {
        strcpy(engine_path, buf);
    }
    init_engine();
}

int prompt_promotion() {
    printf("\033[H\n\r\033[K\033[1;33mPromote pawn to: [q] Queen, [r] Rook, [b] Bishop, [n] Knight: \033[0m");
    fflush(stdout);
    while (1) {
        char c = get_key();
        if (c == 'q' || c == 'Q') return 5;
        if (c == 'r' || c == 'R') return 4;
        if (c == 'b' || c == 'B') return 3;
        if (c == 'n' || c == 'N') return 2;
        usleep(10000);
    }
}

// --- Game Logic Handlers ---
int is_valid_sq(int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 8; }
int get_piece(const int *board, int r, int c) {
    if (!is_valid_sq(r, c)) return 99; // Represents off-board boundaries
    return board[r * 8 + c];
}

int is_attacked(const int *board, int target_sq, int attacker_color) {
    int tr = target_sq / 8, tc = target_sq % 8;

    // Knight attacks
    int kr[] = {-2, -2, -1, -1, 1, 1, 2, 2}, kc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int p = get_piece(board, tr + kr[i], tc + kc[i]);
        if (p != 99 && p == attacker_color * 2) return 1;
    }

    // King attacks
    int kir[] = {-1, -1, -1, 0, 0, 1, 1, 1}, kic[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int p = get_piece(board, tr + kir[i], tc + kic[i]);
        if (p != 99 && p == attacker_color * 6) return 1;
    }

    // Pawn attacks
    int p_dir = (attacker_color == 1) ? 1 : -1;
    int p1 = get_piece(board, tr + p_dir, tc - 1);
    int p2 = get_piece(board, tr + p_dir, tc + 1);
    if (p1 != 99 && p1 == attacker_color * 1) return 1;
    if (p2 != 99 && p2 == attacker_color * 1) return 1;

    // Sliding lines (Rook + Queen)
    int rr[] = {-1, 1, 0, 0}, rc[] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
        int r = tr, c = tc;
        while (1) {
            r += rr[i]; c += rc[i];
            int p = get_piece(board, r, c);
            if (p == 99) break;
            if (p != 0) {
                if (p == attacker_color * 4 || p == attacker_color * 5) return 1;
                break;
            }
        }
    }

    // Sliding diagonals (Bishop + Queen)
    int br[] = {-1, -1, 1, 1}, bc[] = {-1, 1, -1, 1};
    for (int i = 0; i < 4; i++) {
        int r = tr, c = tc;
        while (1) {
            r += br[i]; c += bc[i];
            int p = get_piece(board, r, c);
            if (p == 99) break;
            if (p != 0) {
                if (p == attacker_color * 3 || p == attacker_color * 5) return 1;
                break;
            }
        }
    }
    return 0;
}

int generate_pseudo_moves(const int *board, int turn, int castling, int ep_sq, Move *moves) {
    int count = 0;
    for (int sq = 0; sq < 64; sq++) {
        int p = board[sq];
        if (p == 0 || (p > 0 && turn == -1) || (p < 0 && turn == 1)) continue;
        int r = sq / 8, c = sq % 8, type = abs(p);

        if (type == 1) { // Pawn
            int dir = (turn == 1) ? -1 : 1;
            int r1 = r + dir;
            if (is_valid_sq(r1, c) && board[r1 * 8 + c] == 0) {
                if (r1 == 0 || r1 == 7) {
                    for (int pr = 2; pr <= 5; pr++) moves[count++] = (Move){sq, r1 * 8 + c, pr};
                } else {
                    moves[count++] = (Move){sq, r1 * 8 + c, 0};
                }
                int r2 = r + 2 * dir;
                int start_r = (turn == 1) ? 6 : 1;
                if (r == start_r && board[r2 * 8 + c] == 0) {
                    moves[count++] = (Move){sq, r2 * 8 + c, 0};
                }
            }
            int dc[] = {-1, 1};
            for (int i = 0; i < 2; i++) {
                int nc = c + dc[i];
                if (is_valid_sq(r1, nc)) {
                    int dest = r1 * 8 + nc;
                    int target = board[dest];
                    if ((target != 0 && ((turn == 1 && target < 0) || (turn == -1 && target > 0))) || dest == ep_sq) {
                        if (r1 == 0 || r1 == 7) {
                            for (int pr = 2; pr <= 5; pr++) moves[count++] = (Move){sq, dest, pr};
                        } else {
                            moves[count++] = (Move){sq, dest, 0};
                        }
                    }
                }
            }
        } else if (type == 2) { // Knight
            int kr[] = {-2, -2, -1, -1, 1, 1, 2, 2}, kc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
            for (int i = 0; i < 8; i++) {
                int nr = r + kr[i], nc = c + kc[i];
                if (is_valid_sq(nr, nc)) {
                    int dest = nr * 8 + nc;
                    int enemy = board[dest];
                    if (enemy == 0 || (turn == 1 && enemy < 0) || (turn == -1 && enemy > 0)) {
                        moves[count++] = (Move){sq, dest, 0};
                    }
                }
            }
        } else if (type == 3 || type == 5) { // Bishop / Queen
            int br[] = {-1, -1, 1, 1}, bc[] = {-1, 1, -1, 1};
            for (int i = 0; i < 4; i++) {
                int nr = r, nc = c;
                while (1) {
                    nr += br[i]; nc += bc[i];
                    if (!is_valid_sq(nr, nc)) break;
                    int dest = nr * 8 + nc, enemy = board[dest];
                    if (enemy == 0) moves[count++] = (Move){sq, dest, 0};
                    else {
                        if ((turn == 1 && enemy < 0) || (turn == -1 && enemy > 0)) moves[count++] = (Move){sq, dest, 0};
                        break;
                    }
                }
            }
        }
        if (type == 4 || type == 5) { // Rook / Queen
            int rr[] = {-1, 1, 0, 0}, rc[] = {0, 0, -1, 1};
            for (int i = 0; i < 4; i++) {
                int nr = r, nc = c;
                while (1) {
                    nr += rr[i]; nc += rc[i];
                    if (!is_valid_sq(nr, nc)) break;
                    int dest = nr * 8 + nc, enemy = board[dest];
                    if (enemy == 0) moves[count++] = (Move){sq, dest, 0};
                    else {
                        if ((turn == 1 && enemy < 0) || (turn == -1 && enemy > 0)) moves[count++] = (Move){sq, dest, 0};
                        break;
                    }
                }
            }
        } else if (type == 6) { // King
            int kir[] = {-1, -1, -1, 0, 0, 1, 1, 1}, kic[] = {-1, 0, 1, -1, 1, -1, 0, 1};
            for (int i = 0; i < 8; i++) {
                int nr = r + kir[i], nc = c + kic[i];
                if (is_valid_sq(nr, nc)) {
                    int dest = nr * 8 + nc, enemy = board[dest];
                    if (enemy == 0 || (turn == 1 && enemy < 0) || (turn == -1 && enemy > 0)) {
                        moves[count++] = (Move){sq, dest, 0};
                    }
                }
            }
            // Castling
            if (turn == 1) {
                if ((castling & 1) && board[61] == 0 && board[62] == 0) {
                    if (!is_attacked(board, 60, -1) && !is_attacked(board, 61, -1) && !is_attacked(board, 62, -1)) {
                        moves[count++] = (Move){60, 62, 0};
                    }
                }
                if ((castling & 2) && board[59] == 0 && board[58] == 0 && board[57] == 0) {
                    if (!is_attacked(board, 60, -1) && !is_attacked(board, 59, -1) && !is_attacked(board, 58, -1)) {
                        moves[count++] = (Move){60, 58, 0};
                    }
                }
            } else {
                if ((castling & 4) && board[5] == 0 && board[6] == 0) {
                    if (!is_attacked(board, 4, 1) && !is_attacked(board, 5, 1) && !is_attacked(board, 6, 1)) {
                        moves[count++] = (Move){4, 6, 0};
                    }
                }
                if ((castling & 8) && board[3] == 0 && board[2] == 0 && board[1] == 0) {
                    if (!is_attacked(board, 4, 1) && !is_attacked(board, 3, 1) && !is_attacked(board, 2, 1)) {
                        moves[count++] = (Move){4, 2, 0};
                    }
                }
            }
        }
    }
    return count;
}

int verify_move(const int *board, int turn, int castling, int ep_sq, Move m) {
    int temp[64];
    memcpy(temp, board, 64 * sizeof(int));
    int p = temp[m.from];
    temp[m.to] = (m.promo == 0) ? p : (turn * m.promo);
    temp[m.from] = 0;

    if (abs(p) == 1 && m.to == ep_sq) {
        temp[m.to + ((turn == 1) ? 8 : -8)] = 0;
    }
    if (abs(p) == 6 && abs(m.from - m.to) == 2) {
        if (m.to == 62) { temp[61] = temp[63]; temp[63] = 0; }
        else if (m.to == 58) { temp[59] = temp[56]; temp[56] = 0; }
        else if (m.to == 6) { temp[5] = temp[7]; temp[7] = 0; }
        else if (m.to == 2) { temp[3] = temp[0]; temp[0] = 0; }
    }

    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (temp[i] == turn * 6) { king_sq = i; break; }
    }
    if (king_sq == -1) return 0;
    return !is_attacked(temp, king_sq, -turn);
}

int get_legal_moves(const int *board, int turn, int castling, int ep_sq, Move *pseudo, int pc, Move *legal) {
    int count = 0;
    for (int i = 0; i < pc; i++) {
        if (verify_move(board, turn, castling, ep_sq, pseudo[i])) {
            legal[count++] = pseudo[i];
        }
    }
    return count;
}

void apply_move(GameState *st, Move m) {
    int p = st->board[m.from];
    int turn = st->turn;

    st->halfmove = (abs(p) == 1 || st->board[m.to] != 0) ? 0 : st->halfmove + 1;

    int next_ep = -1;
    if (abs(p) == 1 && abs(m.from - m.to) == 16) {
        next_ep = m.from + ((turn == 1) ? -8 : 8);
    }

    // Castling flag mutations
    if (m.from == 60) st->castling &= ~3;
    if (m.from == 63) st->castling &= ~1;
    if (m.from == 56) st->castling &= ~2;
    if (m.from == 4)  st->castling &= ~12;
    if (m.from == 7)  st->castling &= ~4;
    if (m.from == 0)  st->castling &= ~8;

    if (m.to == 63) st->castling &= ~1;
    if (m.to == 56) st->castling &= ~2;
    if (m.to == 7)  st->castling &= ~4;
    if (m.to == 0)  st->castling &= ~8;

    st->board[m.to] = (m.promo == 0) ? p : (turn * m.promo);
    st->board[m.from] = 0;

    if (abs(p) == 1 && m.to == st->ep_square) {
        st->board[m.to + ((turn == 1) ? 8 : -8)] = 0;
    }

    if (abs(p) == 6 && abs(m.from - m.to) == 2) {
        if (m.to == 62) { st->board[61] = st->board[63]; st->board[63] = 0; }
        else if (m.to == 58) { st->board[59] = st->board[56]; st->board[56] = 0; }
        else if (m.to == 6) { st->board[5] = st->board[7]; st->board[7] = 0; }
        else if (m.to == 2) { st->board[3] = st->board[0]; st->board[0] = 0; }
    }

    st->ep_square = next_ep;
    if (turn == -1) st->fullmove++;
    st->turn = -turn;
}

void get_san(const int *board, int turn, int castling, int ep_sq, Move m, char *buf) {
    int p = board[m.from];
    int type = abs(p);
    int is_cap = (board[m.to] != 0 || (type == 1 && m.to == ep_sq));

    if (type == 6 && (m.from == 60 && m.to == 62 || m.from == 4 && m.to == 6)) {
        strcpy(buf, "O-O"); return;
    }
    if (type == 6 && (m.from == 60 && m.to == 58 || m.from == 4 && m.to == 2)) {
        strcpy(buf, "O-O-O"); return;
    }

    char *ptr = buf;
    if (type != 1) {
        *ptr++ = "  NBRQK"[type];
    } else if (is_cap) {
        *ptr++ = 'a' + (m.from % 8);
    }

    if (is_cap) *ptr++ = 'x';
    *ptr++ = 'a' + (m.to % 8);
    *ptr++ = '1' + (7 - (m.to / 8));

    if (m.promo != 0) {
        *ptr++ = '=';
        *ptr++ = "  NBRQK"[m.promo];
    }
    *ptr = '\0';
}

void init_board(GameState *st) {
    int start[64] = {
        -4, -2, -3, -5, -6, -3, -2, -4,
        -1, -1, -1, -1, -1, -1, -1, -1,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         1,  1,  1,  1,  1,  1,  1,  1,
         4,  2,  3,  5,  6,  3,  2,  4
    };
    memcpy(st->board, start, 64 * sizeof(int));
    st->turn = 1;
    st->castling = 15;
    st->ep_square = -1;
    st->halfmove = 0;
    st->fullmove = 1;
    st->pgn[0] = '\0';
}

Move parse_coordinate_move(const char *str) {
    Move m = {-1, -1, 0};
    if (strlen(str) < 4) return m;
    int f_col = str[0] - 'a', f_row = 7 - (str[1] - '1');
    int t_col = str[2] - 'a', t_row = 7 - (str[3] - '1');
    m.from = f_row * 8 + f_col;
    m.to = t_row * 8 + t_col;
    if (str[4] != '\0' && str[4] != '\n' && str[4] != '\r' && str[4] != ' ') {
        if (str[4] == 'q') m.promo = 5;
        else if (str[4] == 'r') m.promo = 4;
        else if (str[4] == 'b') m.promo = 3;
        else if (str[4] == 'n') m.promo = 2;
    }
    return m;
}

void trigger_engine_search() {
    if (!is_engine_alive()) return;
    engine_thinking = 1;

    // Send absolute historical moves list to bypass complexity of FEN syncing
    char cmd[4096] = "position startpos moves";
    for (int i = 0; i < history_count; i++) {
        char move_str[16];
        int f = moves_played[i].from, t = moves_played[i].to, pr = moves_played[i].promo;
        sprintf(move_str, " %c%d%c%d", 'a' + (f % 8), 8 - (f / 8), 'a' + (t % 8), 8 - (t / 8));
        strcat(cmd, move_str);
        if (pr != 0) {
            char p_char = (pr == 5) ? 'q' : (pr == 4) ? 'r' : (pr == 3) ? 'b' : 'n';
            char tmp[2] = {p_char, '\0'};
            strcat(cmd, tmp);
        }
    }
    strcat(cmd, "\n");
    write_engine(cmd);

    // Apply Time Control rules
    if (tc_type == TC_DEPTH) sprintf(cmd, "go depth %d\n", tc_value);
    else if (tc_type == TC_NODES) sprintf(cmd, "go nodes %d\n", tc_value);
    else sprintf(cmd, "go movetime %d\n", tc_value);
    write_engine(cmd);
}

void check_engine_io() {
    if (!engine_thinking || !is_engine_alive()) return;
    char line[512];
    while (engine_available()) {
        read_engine_line(line, sizeof(line));
        if (strncmp(line, "bestmove", 8) == 0) {
            char move_str[16];
            sscanf(line, "bestmove %s", move_str);
            if (strcmp(move_str, "(none)") != 0 && strcmp(move_str, "NULL") != 0) {
                Move m = parse_coordinate_move(move_str);
                if (m.from != -1) {
                    Move pseudo[256], legal[256];
                    int pc = generate_pseudo_moves(state.board, state.turn, state.castling, state.ep_square, pseudo);
                    int lc = get_legal_moves(state.board, state.turn, state.castling, state.ep_square, pseudo, pc, legal);

                    for (int i = 0; i < lc; i++) {
                        if (legal[i].from == m.from && legal[i].to == m.to) {
                            get_san(state.board, state.turn, state.castling, state.ep_square, legal[i], state.pgn);
                            history[history_count] = state;
                            moves_played[history_count] = legal[i];
                            history_count++;
                            apply_move(&state, legal[i]);
                            break;
                        }
                    }
                }
            }
            engine_thinking = 0;
            break;
        }
    }
}

int check_game_over(char *msg) {
    Move pseudo[256], legal[256];
    int pc = generate_pseudo_moves(state.board, state.turn, state.castling, state.ep_square, pseudo);
    int lc = get_legal_moves(state.board, state.turn, state.castling, state.ep_square, pseudo, pc, legal);

    if (lc == 0) {
        int king_sq = -1;
        for (int i = 0; i < 64; i++) {
            if (state.board[i] == state.turn * 6) { king_sq = i; break; }
        }
        if (king_sq != -1 && is_attacked(state.board, king_sq, -state.turn)) {
            sprintf(msg, "\033[1;31mCheckmate! %s wins!\033[0m", (state.turn == 1) ? "Black" : "White");
        } else {
            sprintf(msg, "\033[1;33mStalemate! Match is a Draw.\033[0m");
        }
        return 1;
    }
    if (state.halfmove >= 100) {
        sprintf(msg, "\033[1;33mDraw by 50-move rule!\033[0m");
        return 1;
    }
    return 0;
}

void perform_undo() {
    // If playing against an engine, roll back two moves; if manual PvP, roll back one move.
    int double_undo = (engine_color != 0 && is_engine_alive());
    if (double_undo && history_count >= 2) {
        history_count--;
        state = history[history_count - 1];
        history_count--;
    } else if (history_count >= 1) {
        history_count--;
        state = history[history_count];
    }
    engine_thinking = 0;
    selected_sq = -1;
}

// --- Main Application Loop ---
int main() {
    init_board(&state);
    enable_raw_mode();
    printf("\033[2J"); // Clear screen once at startup
    init_engine();

    char status[256] = "Game Started.";

    while (1) {
        int over = check_game_over(status);

        if (!over && state.turn == engine_color && is_engine_alive()) {
            if (!engine_thinking) {
                strcpy(status, "Engine thinking...");
                draw_ui(status);
                trigger_engine_search();
            } else {
                check_engine_io();
            }
        } else if (!over) {
            if (engine_thinking) {
                check_engine_io();
            } else {
                strcpy(status, "Your Turn");
            }
        }

        draw_ui(status);

        int key = get_key();
        if (key == 'Q' || key == 'q') {
            break;
        } else if (key == 'W' || key == 'w') {
            if (cursor_sq >= 8) cursor_sq -= 8;
        } else if (key == 'S' || key == 's') {
            if (cursor_sq < 56) cursor_sq += 8;
        } else if (key == 'A' || key == 'a') {
            if (cursor_sq % 8 > 0) cursor_sq -= 1;
        } else if (key == 'D' || key == 'd') {
            if (cursor_sq % 8 < 7) cursor_sq += 1;
        } else if (key == ' ' || key == '\r' || key == '\n') {
            if (over || (state.turn == engine_color && is_engine_alive())) continue;

            if (selected_sq == -1) {
                int p = state.board[cursor_sq];
                if (p != 0 && ((state.turn == 1 && p > 0) || (state.turn == -1 && p < 0))) {
                    selected_sq = cursor_sq;
                }
            } else {
                Move pseudo[256], legal[256];
                int pc = generate_pseudo_moves(state.board, state.turn, state.castling, state.ep_square, pseudo);
                int lc = get_legal_moves(state.board, state.turn, state.castling, state.ep_square, pseudo, pc, legal);

                int success = 0;
                for (int i = 0; i < lc; i++) {
                    if (legal[i].from == selected_sq && legal[i].to == cursor_sq) {
                        Move executed = legal[i];
                        if (abs(state.board[selected_sq]) == 1 && (cursor_sq / 8 == 0 || cursor_sq / 8 == 7)) {
                            executed.promo = prompt_promotion();
                        }
                        get_san(state.board, state.turn, state.castling, state.ep_square, executed, state.pgn);

                        history[history_count] = state;
                        moves_played[history_count] = executed;
                        history_count++;

                        apply_move(&state, executed);
                        success = 1;
                        break;
                    }
                }

                if (!success) {
                    // Quick-reselect piece of own color
                    int p = state.board[cursor_sq];
                    if (p != 0 && ((state.turn == 1 && p > 0) || (state.turn == -1 && p < 0))) {
                        selected_sq = cursor_sq;
                    } else {
                        selected_sq = -1;
                    }
                } else {
                    selected_sq = -1;
                }
            }
        } else if (key == 'U' || key == 'u') {
            perform_undo();
        } else if (key == 'R' || key == 'r') {
            init_board(&state);
            history_count = 0;
            selected_sq = -1;
            engine_thinking = 0;
            strcpy(status, "Game Restarted.");
        } else if (key == 'T' || key == 't') {
            configure_time_control();
            printf("\033[2J");
        } else if (key == 'E' || key == 'e') {
            configure_engine();
            printf("\033[2J");
        }
        usleep(30000); // 30ms sleep cycle to prevent CPU thrashing
    }

    kill_engine();
    return 0;
}
