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

#define MAX_HISTORY 2048

// Board representation
typedef struct {
    int board[64]; // P=1, N=2, B=3, R=4, Q=5, K=6 (Positive=White, Negative=Black)
    int turn;      // 1 = White, -1 = Black
    int castle;    // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep;        // En-passant square (0-63), -1 if none
    int halfmoves; // For 50-move rule
    int fullmoves;
} BoardState;

typedef struct {
    int from;
    int to;
    int promo; // 0=None, 2=N, 3=B, 4=R, 5=Q
} Move;

// Global GUI state
BoardState current_state;
BoardState history[MAX_HISTORY];
Move move_history[MAX_HISTORY];
int history_count = 0;

int cursor_r = 6;  // Screen row (0-7)
int cursor_c = 4;  // Screen col (0-7)
int selected_sq = -1;

int board_orientation = 1; // 1 = White on bottom, -1 = Black on bottom
int user_side = 1;         // 1 = White, -1 = Black, 0 = Hotseat, 2 = Watch (AI vs AI)

int time_control_type = 0;   // 0 = Time (ms), 1 = Depth, 2 = Nodes
int time_control_val = 1;    // Default: 1 ms

// EDIT THIS LINE TO CHANGE ENGINE PATH IN SOURCE CODE
char engine_path[256] = "stockfish";

int engine_in[2] = {-1, -1};
int engine_out[2] = {-1, -1};
pid_t engine_pid = -1;
int engine_thinking = 0;
long long engine_nps = 0;    // Tracks current/last search nodes per second

char engine_buffer[8192];
int engine_buf_len = 0;
struct termios orig_termios;

// Forward declarations
void init_board(BoardState *state);
int is_legal_move(const BoardState *state, Move m);
int has_legal_moves(const BoardState *state);
int is_square_attacked(const BoardState *state, int sq, int attacker);
void make_move(const BoardState *src, BoardState *dst, Move m);
void print_side_panel_line(int panel_row);
void print_recent_moves(int row);
void send_to_engine(const char *cmd);
int find_king(const BoardState *state, int color);
int count_repetitions(const BoardState *state);
int get_promo_choice();

// Clean up termios and terminate engine processes
void cleanup() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h\033[2J\033[H"); // Show cursor, clear screen
    fflush(stdout);
    if (engine_pid > 0) {
        kill(engine_pid, SIGKILL);
    }
}

void handle_signal(int sig) {
    (void)sig;
    exit(0);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; // 100ms timeout
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

// Spawns the UCI Engine
void start_engine(const char *path) {
    if (pipe(engine_in) < 0 || pipe(engine_out) < 0) return;
    engine_pid = fork();
    if (engine_pid == 0) { // Child
        dup2(engine_in[0], STDIN_FILENO);
        dup2(engine_out[1], STDOUT_FILENO);
        close(engine_in[1]);
        close(engine_out[0]);
        // Silence stderr
        int fd_err = open("/dev/null", O_WRONLY);
        dup2(fd_err, STDERR_FILENO);

        char *args[] = {(char *)path, NULL};
        execvp(args[0], args);
        exit(1); // Exit if execution fails
    } else if (engine_pid > 0) { // Parent
        close(engine_in[0]);
        close(engine_out[1]);
        fcntl(engine_out[0], F_SETFL, O_NONBLOCK);
        write(engine_in[1], "uci\nisready\n", 12);
    }
}

void send_to_engine(const char *cmd) {
    if (engine_pid > 0) {
        // Prevent writing to dead pipelines from triggering unhandled signals
        int ignored = write(engine_in[1], cmd, strlen(cmd));
        (void)ignored;
    }
}

// Maps board array index to GUI screen rows/cols based on orientation
int screen_to_board_sq(int r, int c) {
    if (board_orientation == 1) {
        return r * 8 + c;
    } else {
        return (7 - r) * 8 + (7 - c);
    }
}

// Move encoding/decoding
void move_to_uci(Move m, char *buf) {
    int f_col = m.from % 8;
    int f_row = 8 - (m.from / 8);
    int t_col = m.to % 8;
    int t_row = 8 - (m.to / 8);
    sprintf(buf, "%c%d%c%d", 'a' + f_col, f_row, 'a' + t_col, t_row);
    if (m.promo != 0) {
        char p = 'q';
        if (m.promo == 2) p = 'n';
        if (m.promo == 3) p = 'b';
        if (m.promo == 4) p = 'r';
        sprintf(buf + 4, "%c", p);
    }
}

Move uci_to_move(const char *str) {
    Move m = {-1, -1, 0};
    if (strlen(str) < 4) return m;
    int f_col = str[0] - 'a';
    int f_row = 8 - (str[1] - '0');
    int t_col = str[2] - 'a';
    int t_row = 8 - (str[3] - '0');
    m.from = f_row * 8 + f_col;
    m.to = t_row * 8 + t_col;
    if (strlen(str) == 5) {
        char p = str[4];
        if (p == 'n') m.promo = 2;
        else if (p == 'b') m.promo = 3;
        else if (p == 'r') m.promo = 4;
        else m.promo = 5; // Default promo to Queen
    }
    return m;
}

// Saves board history
void push_state(const BoardState *state, Move m) {
    if (history_count < MAX_HISTORY - 1) {
        history[history_count] = *state;
        move_history[history_count] = m;
        history_count++;
    }
}

// UCI Position Command Builder (With overflow prevention)
void trigger_engine_move() {
    engine_nps = 0; // Reset search metrics for the new calculation
    static char cmd[32768]; // Allocate statically to prevent stack overflows
    cmd[0] = '\0';
    strcpy(cmd, "position startpos moves");
    
    int len = strlen(cmd);
    for (int i = 0; i < history_count; i++) {
        char uci_m[10];
        move_to_uci(move_history[i], uci_m);
        int move_len = strlen(uci_m);
        
        // Ensure we don't write outside the static buffer boundary
        if (len + 1 + move_len + 2 >= (int)sizeof(cmd)) {
            break; 
        }
        cmd[len++] = ' ';
        strcpy(cmd + len, uci_m);
        len += move_len;
    }
    cmd[len++] = '\n';
    cmd[len] = '\0';
    
    send_to_engine(cmd);

    char go_cmd[256];
    if (time_control_type == 0) {
        sprintf(go_cmd, "go movetime %d\n", time_control_val);
    } else if (time_control_type == 1) {
        sprintf(go_cmd, "go depth %d\n", time_control_val);
    } else {
        sprintf(go_cmd, "go nodes %d\n", time_control_val);
    }
    send_to_engine(go_cmd);
}

// Parsing incoming engine text
void process_engine_output(char *line) {
    // Sanitize carriage returns and trailing spaces
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[len - 1] = '\0';
        len--;
    }

    // Capture nodes per second (nps) from engine update lines
    if (strncmp(line, "info", 4) == 0) {
        char *nps_ptr = strstr(line, " nps ");
        if (nps_ptr) {
            long long val;
            if (sscanf(nps_ptr, " nps %lld", &val) == 1) {
                engine_nps = val;
            }
        }
    }

    if (strncmp(line, "bestmove", 8) == 0) {
        char move_str[16];
        // Enforce safe width limits during string parse scans
        if (sscanf(line, "bestmove %15s", move_str) == 1) {
            if (strcmp(move_str, "(none)") == 0 || strcmp(move_str, "NULL") == 0) {
                engine_thinking = 0;
                return;
            }
            Move m = uci_to_move(move_str);
            if (is_legal_move(&current_state, m)) {
                push_state(&current_state, m);
                BoardState next;
                make_move(&current_state, &next, m);
                current_state = next;
            }
            engine_thinking = 0;
        }
    }
}

// Dynamic stream parser ensures stdout pipes never clog
void read_from_engine() {
    char tmp[4096];
    int n;
    while ((n = read(engine_out[0], tmp, sizeof(tmp) - 1)) > 0) {
        tmp[n] = '\0';
        for (int i = 0; i < n; i++) {
            if (tmp[i] == '\n') {
                if (engine_buf_len > 0) {
                    engine_buffer[engine_buf_len] = '\0';
                    process_engine_output(engine_buffer);
                    engine_buf_len = 0;
                }
            } else if (tmp[i] != '\r') {
                if (engine_buf_len < (int)sizeof(engine_buffer) - 1) {
                    engine_buffer[engine_buf_len++] = tmp[i];
                } else {
                    engine_buffer[engine_buf_len] = '\0';
                    process_engine_output(engine_buffer);
                    engine_buf_len = 0;
                    engine_buffer[engine_buf_len++] = tmp[i];
                }
            }
        }
    }
}

// Game Rules Validation Logic
int find_king(const BoardState *state, int color) {
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == color * 6) return i;
    }
    return -1;
}

int is_square_attacked(const BoardState *state, int sq, int attacker) {
    int r = sq / 8, c = sq % 8;

    // Knight attacks
    int k_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int k_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + k_r[i], nc = c + k_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 2) return 1;
        }
    }

    // King attacks
    int kg_r[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int kg_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kg_r[i], nc = c + kg_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 6) return 1;
        }
    }

    // Pawn attacks
    int p_offset = (attacker == 1) ? 1 : -1;
    for (int dc = -1; dc <= 1; dc += 2) {
        int nr = r + p_offset, nc = c + dc;
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 1) return 1;
        }
    }

    // Bishop / Queen diagonals
    int d_r[] = {-1, -1, 1, 1};
    int d_c[] = {-1, 1, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + d_r[i], nc = c + d_c[i];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int target = state->board[nr * 8 + nc];
            if (target != 0) {
                if (target == attacker * 3 || target == attacker * 5) return 1;
                break;
            }
            nr += d_r[i]; nc += d_c[i];
        }
    }

    // Rook / Queen slide
    int s_r[] = {-1, 1, 0, 0};
    int s_c[] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + s_r[i], nc = c + s_c[i];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int target = state->board[nr * 8 + nc];
            if (target != 0) {
                if (target == attacker * 4 || target == attacker * 5) return 1;
                break;
            }
            nr += s_r[i]; nc += s_c[i];
        }
    }
    return 0;
}

int is_pseudo_legal_move(const BoardState *state, Move m) {
    int p = state->board[m.from];
    int target = state->board[m.to];
    int turn = state->turn;

    if (p == 0) return 0;
    if ((turn == 1 && p < 0) || (turn == -1 && p > 0)) return 0;
    if (m.from == m.to) return 0;
    if (target != 0 && ((turn == 1 && target > 0) || (turn == -1 && target < 0))) return 0;

    int fr = m.from / 8, fc = m.from % 8;
    int tr = m.to / 8, tc = m.to % 8;
    int dr = tr - fr, dc = tc - fc;
    int abs_dr = abs(dr), abs_dc = abs(dc);

    switch (abs(p)) {
        case 1: { // Pawn
            int dir = (turn == 1) ? -1 : 1;
            if (dc == 0 && dr == dir && target == 0) return 1;
            int start_r = (turn == 1) ? 6 : 1;
            if (dc == 0 && fr == start_r && dr == 2 * dir) {
                if (state->board[(fr + dir) * 8 + fc] == 0 && target == 0) return 1;
            }
            if (abs_dc == 1 && dr == dir) {
                if (target != 0) return 1;
                if (m.to == state->ep) return 1;
            }
            return 0;
        }
        case 2: // Knight
            return ((abs_dr == 2 && abs_dc == 1) || (abs_dr == 1 && abs_dc == 2));
        case 3: { // Bishop
            if (abs_dr != abs_dc) return 0;
            int sr = (dr > 0) ? 1 : -1, sc = (dc > 0) ? 1 : -1;
            int r = fr + sr, c = fc + sc;
            while (r != tr && c != tc) {
                if (state->board[r * 8 + c] != 0) return 0;
                r += sr; c += sc;
            }
            return 1;
        }
        case 4: { // Rook
            if (dr != 0 && dc != 0) return 0;
            int sr = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
            int sc = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);
            int r = fr + sr, c = fc + sc;
            while (r != tr || c != tc) {
                if (state->board[r * 8 + c] != 0) return 0;
                r += sr; c += sc;
            }
            return 1;
        }
        case 5: { // Queen
            if (abs_dr != abs_dc && dr != 0 && dc != 0) return 0;
            int sr = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
            int sc = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);
            if (abs_dr == abs_dc) {
                sr = (dr > 0) ? 1 : -1;
                sc = (dc > 0) ? 1 : -1;
            }
            int r = fr + sr, c = fc + sc;
            while (r != tr || c != tc) {
                if (state->board[r * 8 + c] != 0) return 0;
                r += sr; c += sc;
            }
            return 1;
        }
        case 6: { // King
            if (abs_dr <= 1 && abs_dc <= 1) return 1;
            // Castling
            if (dr == 0 && abs_dc == 2) {
                if (turn == 1 && fr == 7 && fc == 4) {
                    if (m.to == 62 && (state->castle & 1)) {
                        if (state->board[61] == 0 && state->board[62] == 0) {
                            if (!is_square_attacked(state, 60, -1) &&
                                !is_square_attacked(state, 61, -1) &&
                                !is_square_attacked(state, 62, -1)) return 1;
                        }
                    }
                    if (m.to == 58 && (state->castle & 2)) {
                        if (state->board[59] == 0 && state->board[58] == 0 && state->board[57] == 0) {
                            if (!is_square_attacked(state, 60, -1) &&
                                !is_square_attacked(state, 59, -1) &&
                                !is_square_attacked(state, 58, -1)) return 1;
                        }
                    }
                }
                if (turn == -1 && fr == 0 && fc == 4) {
                    if (m.to == 6 && (state->castle & 4)) {
                        if (state->board[5] == 0 && state->board[6] == 0) {
                            if (!is_square_attacked(state, 4, 1) &&
                                !is_square_attacked(state, 5, 1) &&
                                !is_square_attacked(state, 6, 1)) return 1;
                        }
                    }
                    if (m.to == 2 && (state->castle & 8)) {
                        if (state->board[3] == 0 && state->board[2] == 0 && state->board[1] == 0) {
                            if (!is_square_attacked(state, 4, 1) &&
                                !is_square_attacked(state, 3, 1) &&
                                !is_square_attacked(state, 2, 1)) return 1;
                        }
                    }
                }
            }
            return 0;
        }
    }
    return 0;
}

int is_legal_move(const BoardState *state, Move m) {
    if (!is_pseudo_legal_move(state, m)) return 0;
    BoardState next;
    make_move(state, &next, m);
    int king = find_king(&next, state->turn);
    if (king == -1) return 0;
    return !is_square_attacked(&next, king, -state->turn);
}

int has_legal_moves(const BoardState *state) {
    for (int f = 0; f < 64; f++) {
        if (state->board[f] == 0) continue;
        if ((state->turn == 1 && state->board[f] < 0) || (state->turn == -1 && state->board[f] > 0)) continue;
        for (int t = 0; t < 64; t++) {
            Move m = {f, t, 0};
            if (abs(state->board[f]) == 1 && (t / 8 == 0 || t / 8 == 7)) {
                m.promo = 5; // Validate via auto-Queen promo simulation
            }
            if (is_legal_move(state, m)) return 1;
        }
    }
    return 0;
}

int count_repetitions(const BoardState *state) {
    int count = 1; // Count current active position
    for (int i = 0; i < history_count; i++) {
        if (state->turn == history[i].turn &&
            state->castle == history[i].castle &&
            state->ep == history[i].ep &&
            memcmp(state->board, history[i].board, sizeof(state->board)) == 0) {
            count++;
        }
    }
    return count;
}

void make_move(const BoardState *src, BoardState *dst, Move m) {
    *dst = *src;
    int p = dst->board[m.from];
    
    // Accurately pre-calculate captures before the board layout is updated
    int is_capture = (src->board[m.to] != 0) || (abs(p) == 1 && m.to == src->ep);

    dst->board[m.from] = 0;

    if (m.promo != 0) {
        dst->board[m.to] = dst->turn * m.promo;
    } else {
        dst->board[m.to] = p;
    }

    if (abs(p) == 1 && m.to == dst->ep) {
        int p_dir = (dst->turn == 1 ? 8 : -8);
        int mt_cap = m.to + p_dir;
        dst->board[mt_cap] = 0;
    }

    dst->ep = -1;
    if (abs(p) == 1 && abs(m.from - m.to) == 16) {
        dst->ep = m.from + (dst->turn == 1 ? -8 : 8);
    }

    if (abs(p) == 6) {
        if (m.from == 60 && m.to == 62) { dst->board[61] = dst->board[63]; dst->board[63] = 0; }
        else if (m.from == 60 && m.to == 58) { dst->board[59] = dst->board[56]; dst->board[56] = 0; }
        else if (m.from == 4 && m.to == 6) { dst->board[5] = dst->board[7]; dst->board[7] = 0; }
        else if (m.from == 4 && m.to == 2) { dst->board[3] = dst->board[0]; dst->board[0] = 0; }
    }

    if (m.from == 60) dst->castle &= ~1 & ~2;
    if (m.from == 4)  dst->castle &= ~4 & ~8;
    if (m.from == 63 || m.to == 63) dst->castle &= ~1;
    if (m.from == 56 || m.to == 56) dst->castle &= ~2;
    if (m.from == 7  || m.to == 7)  dst->castle &= ~4;
    if (m.from == 0  || m.to == 0)  dst->castle &= ~8;

    dst->turn = -dst->turn;
    
    // Reset 50-move half-move rule metric strictly on pawn pushes or any captures
    if (abs(p) == 1 || is_capture) dst->halfmoves = 0;
    else dst->halfmoves++;
    
    if (dst->turn == 1) dst->fullmoves++;
}

// GUI Drawing and Terminal ANSI Output
void draw_ui() {
    printf("\033[H\r\n"); // Place cursor top-left (no full clear to prevent flickering)

    // 1. Unified Game Header (Status, Player Modes & Time-Control combined)
    const char *turn_str = (current_state.turn == 1) ? "\033[1;33mWhite\033[0m" : "\033[1;35mBlack\033[0m";
    int king = find_king(&current_state, current_state.turn);
    int is_ch = is_square_attacked(&current_state, king, -current_state.turn);
    int has_mov = has_legal_moves(&current_state);
    const char *w_play = (user_side == 1 || user_side == 0) ? "Hum" : "Eng";
    const char *b_play = (user_side == -1 || user_side == 0) ? "Hum" : "Eng";
    int repetitions = count_repetitions(&current_state);

    printf("  ");
    if (current_state.halfmoves >= 100) {
        printf("\033[1;36mDRAW (50-move rule)\033[0m");
    } else if (repetitions >= 3) {
        printf("\033[1;36mDRAW (threefold repetition)\033[0m");
    } else if (!has_mov) {
        if (is_ch) printf("\033[1;31mCHECKMATE!\033[0m");
        else printf("\033[1;36mSTALEMATE!\033[0m");
    } else if (is_ch) {
        printf("%s (\033[1;31mCHECK!\033[0m)", turn_str);
    } else {
        printf("%s's Turn", turn_str);
    }
    printf(" | W:%s B:%s", w_play, b_play);

    const char *types[] = {"Time-Limit", "Depth-Limit", "Node-Limit"};
    printf(" | %s", types[time_control_type]);
    if (time_control_type == 0) {
        printf(" (%d ms)", time_control_val);
    } else if (time_control_type == 1) {
        printf(" (depth %d)", time_control_val);
    } else {
        printf(" (%d nodes)", time_control_val);
    }
    printf("\033[K\r\n\r\n");

    // 2. Column Coordinates Header (Padded to 31 characters to align side panel perfectly)
    if (board_orientation == 1) {
        printf("     a  b  c  d  e  f  g  h    ");
    } else {
        printf("     h  g  f  e  d  c  b  a    ");
    }
    print_side_panel_line(0);
    printf("\033[K\r\n");

    // Determine if either king is in check
    int king_in_check = -1;
    int w_king = find_king(&current_state, 1);
    int b_king = find_king(&current_state, -1);
    if (w_king != -1 && is_square_attacked(&current_state, w_king, -1)) {
        king_in_check = w_king;
    } else if (b_king != -1 && is_square_attacked(&current_state, b_king, 1)) {
        king_in_check = b_king;
    }

    // 3. Render 8 Board Rows (Side panel index 1 to 8)
    for (int r = 0; r < 8; r++) {
        int rank_lbl = (board_orientation == 1) ? (8 - r) : (r + 1);
        printf("  %d ", rank_lbl);

        for (int c = 0; c < 8; c++) {
            int sq = screen_to_board_sq(r, c);
            int p = current_state.board[sq];

            int is_light = ((sq / 8) + (sq % 8)) % 2 == 0;
            const char *bg_color;

            int is_selected = (sq == selected_sq);
            int is_cursor = (r == cursor_r && c == cursor_c);

            // Highlight previous move
            int is_prev_move = 0;
            if (history_count > 0) {
                Move last_move = move_history[history_count - 1];
                if (sq == last_move.from || sq == last_move.to) {
                    is_prev_move = 1;
                }
            }

            int is_legal_dest = 0;
            if (selected_sq != -1) {
                Move test_m = {selected_sq, sq, 0};
                if (abs(current_state.board[selected_sq]) == 1 && (sq / 8 == 0 || sq / 8 == 7)) {
                    test_m.promo = 5;
                }
                if (is_legal_move(&current_state, test_m)) {
                    is_legal_dest = 1;
                }
            }

            if (is_cursor) {
                bg_color = "\033[48;5;208m"; // Bright orange for active cursor
            } else if (is_selected) {
                bg_color = "\033[48;5;34m";  // Green highlight for selected piece
            } else if (sq == king_in_check) {
                bg_color = "\033[48;5;196m"; // Bright red highlight for king in check
            } else if (is_prev_move) {
                bg_color = is_light ? "\033[48;5;75m" : "\033[48;5;68m"; // Sky/Steel Blue for previous moves
            } else if (is_legal_dest) {
                bg_color = is_light ? "\033[48;5;151m" : "\033[48;5;108m"; // Soft greens for legal destinations
            } else {
                bg_color = is_light ? "\033[48;5;180m" : "\033[48;5;94m"; // Wood tones (Light Maple vs Dark Walnut)
            }

            const char *piece_str = " ";
            const char *fg_color = "\033[38;5;232m"; // Black Pieces
            if (p != 0) {
                if (p > 0) fg_color = "\033[38;5;255m\033[1m"; // Bold white Pieces
                switch (abs(p)) {
                    case 1: piece_str = "♟"; break;
                    case 2: piece_str = "♞"; break;
                    case 3: piece_str = "♝"; break;
                    case 4: piece_str = "♜"; break;
                    case 5: piece_str = "♛"; break;
                    case 6: piece_str = "♚"; break;
                }
            }

            printf("%s%s %s \033[0m", bg_color, fg_color, piece_str);
        }

        printf(" %d ", rank_lbl);
        print_side_panel_line(r + 1);
        printf("\033[K\r\n"); // Wipe leftover characters to prevent screen residue
    }

    // 4. Column Coordinates Footer (Padded to 31 characters, side panel index 9)
    if (board_orientation == 1) {
        printf("     a  b  c  d  e  f  g  h    ");
    } else {
        printf("     h  g  f  e  d  c  b  a    ");
    }
    print_side_panel_line(9);
    printf("\033[K\r\n\r\n");

    printf(" \033[38;5;245m[Arrows] Navigate | [Enter/Space] Select | [U] Undo | [R] Reset Board\033[0m\033[K\r\n");
    printf(" \033[38;5;245m[O] Flip Board | [S] Switch Sides | [T] Change Time Control\033[0m\033[K\r\n");
    printf(" \033[38;5;245m[V] Adjust Value | [Q] Quit\033[0m\033[K\r\n\r\n");
    
    // Prints dynamic, formatted nodes-per-second count (NPS) beside the Engine Status 
    printf(" \033[38;5;248mEngine Status:\033[0m %s (%s)", engine_path, (engine_pid > 0) ? "\033[1;32mActive\033[0m" : "\033[1;31mUnavailable\033[0m");
    if (engine_pid > 0 && engine_nps > 0) {
        if (engine_nps >= 1000000) {
            printf(" | NPS: %.2fM", (double)engine_nps / 1000000.0);
        } else if (engine_nps >= 1000) {
            printf(" | NPS: %.1fk", (double)engine_nps / 1000.0);
        } else {
            printf(" | NPS: %lld", engine_nps);
        }
    }
    printf("\033[K\r\n\033[J");
    fflush(stdout);
}

void print_side_panel_line(int panel_row) {
    printf("   ");
    // All 10 available rows are printed sequentially
    print_recent_moves(panel_row);
}

void print_recent_moves(int row) {
    int total_full_moves = (history_count + 1) / 2;
    if (total_full_moves == 0) {
        return; // Render completely blank rows when history is cleared
    }
    int start_move = 1;
    if (total_full_moves > 10) {
        start_move = total_full_moves - 9;
    }
    int display = start_move + row;
    if (display > total_full_moves) {
        return; // Keep rows empty if they exceed the current history depth
    }

    int w_idx = (display - 1) * 2;
    int b_idx = w_idx + 1;

    printf("   %2d. ", display);
    if (w_idx < history_count) {
        char w_str[10];
        move_to_uci(move_history[w_idx], w_str);
        printf("%-6s", w_str);
    } else {
        printf("------");
    }

    printf(" ");

    if (b_idx < history_count) {
        char b_str[10];
        move_to_uci(move_history[b_idx], b_str);
        printf("%-6s", b_str);
    } else {
        if (w_idx < history_count) printf("...");
        else printf("------");
    }
}

// Blocks and prompts user synchronously to select an under-promotion piece
int get_promo_choice() {
    printf("\r\n \033[1;33mPromote pawn to: [Q]ueen, [R]ook, [B]ishop, [N]ight\033[0m");
    fflush(stdout);
    char c;
    int choice = 5;
    while (1) {
        if (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == 'q' || c == 'Q') { choice = 5; break; }
            if (c == 'r' || c == 'R') { choice = 4; break; }
            if (c == 'b' || c == 'B') { choice = 3; break; }
            if (c == 'n' || c == 'N' || c == 'k' || c == 'K') { choice = 2; break; }
        }
    }
    // Erase the prompt from screen right after selection is made
    printf("\r\033[K\033[A\033[K");
    fflush(stdout);
    return choice;
}

// GUI Action / Setting Handlers
void handle_select() {
    // If checkmate, stalemate, 50-move draw, or threefold repetition has been reached, do not process selections
    if (!has_legal_moves(&current_state) || current_state.halfmoves >= 100 || count_repetitions(&current_state) >= 3) {
        return;
    }

    int sq = screen_to_board_sq(cursor_r, cursor_c);
    if (selected_sq == -1) {
        int p = current_state.board[sq];
        if (p != 0 && ((current_state.turn == 1 && p > 0) || (current_state.turn == -1 && p < 0))) {
            selected_sq = sq;
        }
    } else {
        Move m = {selected_sq, sq, 0};
        int p = current_state.board[selected_sq];
        int is_promo = (abs(p) == 1 && (sq / 8 == 0 || sq / 8 == 7));
        if (is_promo) {
            m.promo = 5; // Validate with Queen first
        }

        if (is_legal_move(&current_state, m)) {
            if (is_promo) {
                m.promo = get_promo_choice();
            }
            push_state(&current_state, m);
            BoardState next;
            make_move(&current_state, &next, m);
            current_state = next;
            selected_sq = -1;
        } else {
            int target = current_state.board[sq];
            if (target != 0 && ((current_state.turn == 1 && target > 0) || (current_state.turn == -1 && target < 0))) {
                selected_sq = sq; // Switch focus to different owned piece
            } else {
                selected_sq = -1; // Deselect
            }
        }
    }
}

void handle_undo() {
    if (engine_thinking) {
        send_to_engine("stop\n");
        engine_thinking = 0;
    }
    engine_nps = 0;
    int step_back = (user_side == 1 || user_side == -1) ? 2 : 1;
    while (step_back > 0 && history_count > 0) {
        history_count--;
        current_state = history[history_count];
        step_back--;
    }
    selected_sq = -1;
}

void handle_reset_board() {
    if (engine_thinking) {
        send_to_engine("stop\n");
        engine_thinking = 0;
    }
    engine_nps = 0;
    init_board(&current_state);
    history_count = 0;
    selected_sq = -1;
    cursor_r = 6;
    cursor_c = 4;
    if (engine_pid > 0) {
        send_to_engine("ucinewgame\nisready\n");
    }
}

void handle_switch_sides() {
    if (engine_thinking) {
        send_to_engine("stop\n");
        engine_thinking = 0;
    }
    if (user_side == 1) user_side = -1;
    else if (user_side == -1) user_side = 0;
    else if (user_side == 0) user_side = 2;
    else user_side = 1;
}

void adjust_time_control() {
    if (time_control_type == 0) { // Time-Limit
        int time_list[] = {1, 10, 50, 100, 500, 1000, 1500, 2000, 3000, 5000, 10000};
        int time_count = sizeof(time_list) / sizeof(time_list[0]);
        int found_idx = -1;
        for (int i = 0; i < time_count; i++) {
            if (time_control_val == time_list[i]) {
                found_idx = i;
                break;
            }
        }
        if (found_idx == -1 || found_idx == time_count - 1) {
            time_control_val = time_list[0];
        } else {
            time_control_val = time_list[found_idx + 1];
        }
    } else if (time_control_type == 1) { // Depth-Limit (1-20)
        time_control_val = (time_control_val % 20) + 1;
    } else { // Node-Limit
        int nodes_list[] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576, 2097152, 4194304, 8388608};
        int nodes_count = sizeof(nodes_list) / sizeof(nodes_list[0]);
        int found_idx = -1;
        for (int i = 0; i < nodes_count; i++) {
            if (time_control_val == nodes_list[i]) {
                found_idx = i;
                break;
            }
        }
        if (found_idx == -1 || found_idx == nodes_count - 1) {
            time_control_val = nodes_list[0];
        } else {
            time_control_val = nodes_list[found_idx + 1];
        }
    }
}

void handle_input() {
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == '\033') { // Escape / Arrow codes parsing
            char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                if (seq[0] == '[') {
                    switch (seq[1]) {
                        case 'A': if (cursor_r > 0) cursor_r--; break; // Up
                        case 'B': if (cursor_r < 7) cursor_r++; break; // Down
                        case 'C': if (cursor_c < 7) cursor_c++; break; // Right
                        case 'D': if (cursor_c > 0) cursor_c--; break; // Left
                    }
                }
            }
        } else if (c == ' ' || c == '\r' || c == '\n') {
            handle_select();
        } else if (c == 'u' || c == 'U') {
            handle_undo();
        } else if (c == 'r' || c == 'R') {
            handle_reset_board();
        } else if (c == 'o' || c == 'O') {
            board_orientation = -board_orientation;
        } else if (c == 's' || c == 'S') {
            handle_switch_sides();
        } else if (c == 't' || c == 'T') {
            time_control_type = (time_control_type + 1) % 3;
            time_control_val = (time_control_type == 0) ? 1 : (time_control_type == 1 ? 1 : 512);
        } else if (c == 'v' || c == 'V') {
            adjust_time_control();
        } else if (c == 'q' || c == 'Q') {
            exit(0);
        }
    }
}

void init_board(BoardState *state) {
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
    memcpy(state->board, start, sizeof(start));
    state->turn = 1;
    state->castle = 15;
    state->ep = -1;
    state->halfmoves = 0;
    state->fullmoves = 1;
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE to handle engine exits gracefully
    atexit(cleanup);

    init_board(&current_state);
    enable_raw_mode();
    printf("\033[2J\033[H"); // Initial Full-Screen Clear
    start_engine(engine_path);

    while (1) {
        int engine_active = 0;
        // Lock engine movement if checkmate, stalemate, 50-move draw, or threefold repetition draw state is active
        if (has_legal_moves(&current_state) && current_state.halfmoves < 100 && count_repetitions(&current_state) < 3) {
            if (user_side == 2) engine_active = 1;
            else if (user_side == 1 && current_state.turn == -1) engine_active = 1;
            else if (user_side == -1 && current_state.turn == 1) engine_active = 1;
        }

        if (engine_active && !engine_thinking && engine_pid > 0) {
            engine_thinking = 1;
            trigger_engine_move();
        }

        // Detect engine process death
        if (engine_pid > 0) {
            int stat;
            if (waitpid(engine_pid, &stat, WNOHANG) > 0) {
                engine_pid = -1;
                engine_thinking = 0;
            }
        }

        draw_ui();

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        int max_fd = STDIN_FILENO;
        if (engine_pid > 0) {
            FD_SET(engine_out[0], &fds);
            if (engine_out[0] > max_fd) max_fd = engine_out[0];
        }

        struct timeval tv = {0, 30000}; // 30ms latency constraint
        int act = select(max_fd + 1, &fds, NULL, NULL, &tv);

        if (act > 0) {
            if (FD_ISSET(STDIN_FILENO, &fds)) {
                handle_input();
            }
            if (engine_pid > 0 && FD_ISSET(engine_out[0], &fds)) {
                read_from_engine();
            }
        }
    }
    return 0;
}
