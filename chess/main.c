#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <poll.h>
#include <signal.h>

#define MAX_MOVES 2048

typedef struct {
    int from;
    int to;
    int promo; // 'q', 'r', 'b', 'n'
} Move;

typedef struct {
    int board[64];    // 0 = empty, positive = white, negative = black
    int turn;         // 1 = White, -1 = Black
    int castle[4];    // WK, WQ, BK, BQ
    int ep_sq;        // -1 if none, otherwise 0-63
    int halfmove;
    int fullmove;
} BoardState;

typedef struct {
    Move m;
    char san[16];
} HistoryEntry;

// Global State
BoardState state;
BoardState history[MAX_MOVES];
int current_history_index = 0;

HistoryEntry move_history[MAX_MOVES];
int move_history_count = 0;

// GUI Selection state
int cursor_sq = 28; // Start cursor near the center
int selected_sq = -1;
int legal_targets[64];

// Game Configuration
int game_mode = 0;     // 0 = White vs Engine, 1 = Black vs Engine, 2 = PvP (Local)
int limit_type = 1;    // 0 = Time (ms), 1 = Depth (ply), 2 = Nodes
int limit_value = 10;  // Default: Depth 10
char status_msg[256] = "Welcome! Use WASD/Arrows to navigate, Space/Enter to select.";

// Engine Communication
int to_engine[2];
int from_engine[2];
pid_t engine_pid = -1;
int engine_online = 0;

struct termios orig_termios;

// Standard starting board
const int start_board[64] = {
    -4, -2, -3, -5, -6, -3, -2, -4, // Rank 8 (black)
    -1, -1, -1, -1, -1, -1, -1, -1, // Rank 7
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     1,  1,  1,  1,  1,  1,  1,  1, // Rank 2
     4,  2,  3,  5,  6,  3,  2,  4  // Rank 1 (white)
};

// Forward Declarations
void restore_terminal();
void draw_ui();
int get_legal_moves(BoardState *st, Move moves[]);
int is_square_attacked(BoardState *st, int sq, int attacker_color);
void apply_move(BoardState *st, Move m);

void cleanup() {
    restore_terminal();
    if (engine_pid > 0) {
        kill(engine_pid, SIGTERM);
        waitpid(engine_pid, NULL, WNOHANG);
    }
}

void handle_sigint(int sig) {
    exit(0);
}

void init_terminal() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    atexit(cleanup);
    signal(SIGINT, handle_sigint);
    printf("\x1b[?25l"); // Hide terminal cursor
}

void restore_terminal() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\x1b[?25h\x1b[0m\n"); // Restore cursor & colors
}

// Write line to UCI Engine
void send_to_engine(const char *cmd) {
    if (engine_online) {
        write(to_engine[1], cmd, strlen(cmd));
    }
}

// Read line from UCI Engine (Non-blocking parser helper)
int read_line_from_engine(char *buf, int max_len) {
    struct pollfd pfd;
    pfd.fd = from_engine[0];
    pfd.events = POLLIN;
    int ready = poll(&pfd, 1, 50); // 50ms timeout
    if (ready > 0) {
        int bytes = 0;
        while (bytes < max_len - 1) {
            char c;
            int r = read(from_engine[0], &c, 1);
            if (r <= 0) break;
            buf[bytes++] = c;
            if (c == '\n') break;
        }
        buf[bytes] = '\0';
        return bytes;
    }
    return 0;
}

// Spawns and handshakes with the engine
void init_engine() {
    if (pipe(to_engine) < 0 || pipe(from_engine) < 0) return;
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[1]);
        close(from_engine[0]);
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDERR_FILENO);
        execlp("stockfish", "stockfish", (char*)NULL);
        exit(1); // Fails if engine is not found
    }
    close(to_engine[0]);
    close(from_engine[1]);

    // Fast probe to verify if Stockfish is alive and responding
    engine_online = 1;
    send_to_engine("uci\n");
    char response[512];
    int attempts = 15;
    int handshook = 0;
    while (attempts-- > 0) {
        int len = read_line_from_engine(response, sizeof(response));
        if (len > 0 && strstr(response, "uciok")) {
            handshook = 1;
            break;
        }
        usleep(50000);
    }
    if (!handshook) {
        engine_online = 0;
        kill(engine_pid, SIGTERM);
        engine_pid = -1;
    } else {
        send_to_engine("isready\n");
        while (1) {
            int len = read_line_from_engine(response, sizeof(response));
            if (len > 0 && strstr(response, "readyok")) break;
            usleep(20000);
        }
    }
}

// Move Generator Calculations
int get_piece_moves(BoardState *st, int sq, Move moves[], int count) {
    int p = st->board[sq];
    if (p == 0 || (p > 0 && st->turn < 0) || (p < 0 && st->turn > 0)) return count;
    int p_type = abs(p);
    int color = (p > 0) ? 1 : -1;
    int r = sq / 8, c = sq % 8;

    if (p_type == 1) { // Pawn
        int dir = -color;
        int next = sq + dir * 8;
        if (next >= 0 && next < 64 && st->board[next] == 0) {
            if ((color == 1 && r == 1) || (color == -1 && r == 6)) {
                moves[count++] = (Move){sq, next, 'q'};
                moves[count++] = (Move){sq, next, 'r'};
                moves[count++] = (Move){sq, next, 'b'};
                moves[count++] = (Move){sq, next, 'n'};
            } else {
                moves[count++] = (Move){sq, next, 0};
                int dnext = sq + dir * 16;
                if (((color == 1 && r == 6) || (color == -1 && r == 1)) && st->board[dnext] == 0) {
                    moves[count++] = (Move){sq, dnext, 0};
                }
            }
        }
        int caps[2] = {sq + dir * 8 - 1, sq + dir * 8 + 1};
        for (int i = 0; i < 2; i++) {
            int target = caps[i];
            if (target >= 0 && target < 64 && abs(target % 8 - c) == 1) {
                int tp = st->board[target];
                if ((tp != 0 && ((tp > 0 && color < 0) || (tp < 0 && color > 0))) || target == st->ep_sq) {
                    if ((color == 1 && r == 1) || (color == -1 && r == 6)) {
                        moves[count++] = (Move){sq, target, 'q'};
                        moves[count++] = (Move){sq, target, 'r'};
                        moves[count++] = (Move){sq, target, 'b'};
                        moves[count++] = (Move){sq, target, 'n'};
                    } else {
                        moves[count++] = (Move){sq, target, 0};
                    }
                }
            }
        }
    } else if (p_type == 2) { // Knight
        int kt_offsets[] = {-17, -15, -10, -6, 6, 10, 15, 17};
        for (int i = 0; i < 8; i++) {
            int target = sq + kt_offsets[i];
            if (target >= 0 && target < 64) {
                if (abs(r - target / 8) * abs(c - target % 8) == 2) {
                    int tp = st->board[target];
                    if (tp == 0 || (tp > 0 && color < 0) || (tp < 0 && color > 0)) {
                        moves[count++] = (Move){sq, target, 0};
                    }
                }
            }
        }
    } else if (p_type == 6) { // King
        int k_offsets[] = {-9, -8, -7, -1, 1, 7, 8, 9};
        for (int i = 0; i < 8; i++) {
            int target = sq + k_offsets[i];
            if (target >= 0 && target < 64) {
                if (abs(r - target / 8) <= 1 && abs(c - target % 8) <= 1) {
                    int tp = st->board[target];
                    if (tp == 0 || (tp > 0 && color < 0) || (tp < 0 && color > 0)) {
                        moves[count++] = (Move){sq, target, 0};
                    }
                }
            }
        }
        // Castling
        if (color == 1) {
            if (st->castle[0] && st->board[61] == 0 && st->board[62] == 0 &&
                !is_square_attacked(st, 60, -1) && !is_square_attacked(st, 61, -1) && !is_square_attacked(st, 62, -1)) {
                moves[count++] = (Move){60, 62, 0};
            }
            if (st->castle[1] && st->board[59] == 0 && st->board[58] == 0 && st->board[57] == 0 &&
                !is_square_attacked(st, 60, -1) && !is_square_attacked(st, 59, -1) && !is_square_attacked(st, 58, -1)) {
                moves[count++] = (Move){60, 58, 0};
            }
        } else {
            if (st->castle[2] && st->board[5] == 0 && st->board[6] == 0 &&
                !is_square_attacked(st, 4, 1) && !is_square_attacked(st, 5, 1) && !is_square_attacked(st, 6, 1)) {
                moves[count++] = (Move){4, 6, 0};
            }
            if (st->castle[3] && st->board[3] == 0 && st->board[2] == 0 && st->board[1] == 0 &&
                !is_square_attacked(st, 4, 1) && !is_square_attacked(st, 3, 1) && !is_square_attacked(st, 2, 1)) {
                moves[count++] = (Move){4, 2, 0};
            }
        }
    } else { // sliding pieces: Bishop (3), Rook (4), Queen (5)
        int dirs[8], num_dirs = 0;
        if (p_type == 3 || p_type == 5) {
            dirs[num_dirs++] = -9; dirs[num_dirs++] = -7; dirs[num_dirs++] = 7; dirs[num_dirs++] = 9;
        }
        if (p_type == 4 || p_type == 5) {
            dirs[num_dirs++] = -8; dirs[num_dirs++] = 8; dirs[num_dirs++] = -1; dirs[num_dirs++] = 1;
        }
        for (int d = 0; d < num_dirs; d++) {
            int step = dirs[d];
            int curr = sq;
            while (1) {
                int pr = curr / 8, pc = curr % 8;
                curr += step;
                if (curr < 0 || curr > 63) break;
                int cr = curr / 8, cc = curr % 8;
                if (abs(step) == 1 && cr != pr) break;
                if (abs(step) == 8 && cc != pc) break;
                if ((abs(step) == 7 || abs(step) == 9) && (abs(cr - pr) != 1 || abs(cc - pc) != 1)) break;

                int tp = st->board[curr];
                if (tp == 0) {
                    moves[count++] = (Move){sq, curr, 0};
                } else {
                    if ((tp > 0 && color < 0) || (tp < 0 && color > 0)) {
                        moves[count++] = (Move){sq, curr, 0};
                    }
                    break;
                }
            }
        }
    }
    return count;
}

int is_square_attacked(BoardState *st, int sq, int attacker_color) {
    // Knight checks
    int kt_offsets[] = {-17, -15, -10, -6, 6, 10, 15, 17};
    int r = sq / 8, c = sq % 8;
    for (int i = 0; i < 8; i++) {
        int target = sq + kt_offsets[i];
        if (target >= 0 && target < 64) {
            if (abs(r - target / 8) * abs(c - target % 8) == 2) {
                if (st->board[target] == 2 * attacker_color) return 1;
            }
        }
    }
    // King checks
    int k_offsets[] = {-9, -8, -7, -1, 1, 7, 8, 9};
    for (int i = 0; i < 8; i++) {
        int target = sq + k_offsets[i];
        if (target >= 0 && target < 64) {
            if (abs(r - target / 8) <= 1 && abs(c - target % 8) <= 1) {
                if (st->board[target] == 6 * attacker_color) return 1;
            }
        }
    }
    // Pawn checks
    int dir = (attacker_color == 1) ? 1 : -1; // Attacking from down to up if white
    int p_targets[2] = {sq + dir * 8 - 1, sq + dir * 8 + 1};
    for (int i = 0; i < 2; i++) {
        int target = p_targets[i];
        if (target >= 0 && target < 64 && abs(target % 8 - c) == 1) {
            if (st->board[target] == 1 * attacker_color) return 1;
        }
    }
    // Orthogonal sliding checks
    int r_dirs[] = {-8, 8, -1, 1};
    for (int d = 0; d < 4; d++) {
        int step = r_dirs[d];
        int curr = sq;
        while (1) {
            int pr = curr / 8, pc = curr % 8;
            curr += step;
            if (curr < 0 || curr > 63) break;
            int cr = curr / 8, cc = curr % 8;
            if (abs(step) == 1 && cr != pr) break;
            if (abs(step) == 8 && cc != pc) break;
            int tp = st->board[curr];
            if (tp != 0) {
                if (tp == 4 * attacker_color || tp == 5 * attacker_color) return 1;
                break;
            }
        }
    }
    // Diagonal sliding checks
    int b_dirs[] = {-9, -7, 7, 9};
    for (int d = 0; d < 4; d++) {
        int step = b_dirs[d];
        int curr = sq;
        while (1) {
            int pr = curr / 8, pc = curr % 8;
            curr += step;
            if (curr < 0 || curr > 63) break;
            int cr = curr / 8, cc = curr % 8;
            if (abs(cr - pr) != 1 || abs(cc - pc) != 1) break;
            int tp = st->board[curr];
            if (tp != 0) {
                if (tp == 3 * attacker_color || tp == 5 * attacker_color) return 1;
                break;
            }
        }
    }
    return 0;
}

int get_legal_moves(BoardState *st, Move moves[]) {
    Move pseudo[512];
    int p_count = 0;
    for (int sq = 0; sq < 64; sq++) {
        p_count = get_piece_moves(st, sq, pseudo, p_count);
    }
    int l_count = 0;
    for (int i = 0; i < p_count; i++) {
        BoardState temp = *st;
        apply_move(&temp, pseudo[i]);
        int king_sq = -1;
        for (int s = 0; s < 64; s++) {
            if (temp.board[s] == 6 * st->turn) {
                king_sq = s;
                break;
            }
        }
        if (king_sq != -1 && !is_square_attacked(&temp, king_sq, -st->turn)) {
            moves[l_count++] = pseudo[i];
        }
    }
    return l_count;
}

void apply_move(BoardState *st, Move m) {
    int p = st->board[m.from];
    int p_type = abs(p);

    if (p_type == 1 && m.to == st->ep_sq) {
        st->board[m.to + (st->turn == 1 ? 8 : -8)] = 0;
    }

    if (p_type == 6) {
        if (m.from == 60) {
            if (m.to == 62) { st->board[61] = st->board[63]; st->board[63] = 0; }
            else if (m.to == 58) { st->board[59] = st->board[56]; st->board[56] = 0; }
            st->castle[0] = st->castle[1] = 0;
        } else if (m.from == 4) {
            if (m.to == 6) { st->board[5] = st->board[7]; st->board[7] = 0; }
            else if (m.to == 2) { st->board[3] = st->board[0]; st->board[0] = 0; }
            st->castle[2] = st->castle[3] = 0;
        }
    }

    // Invalidate castling flags on Rook movements/captures
    if (m.from == 56 || m.to == 56) st->castle[1] = 0;
    if (m.from == 63 || m.to == 63) st->castle[0] = 0;
    if (m.from == 0  || m.to == 0)  st->castle[3] = 0;
    if (m.from == 7  || m.to == 7)  st->castle[2] = 0;

    if (p_type == 1 && abs(m.from - m.to) == 16) {
        st->ep_sq = (m.from + m.to) / 2;
    } else {
        st->ep_sq = -1;
    }

    if (m.promo) {
        int sign = st->turn;
        int pt = 5;
        if (m.promo == 'r') pt = 4;
        else if (m.promo == 'b') pt = 3;
        else if (m.promo == 'n') pt = 2;
        st->board[m.to] = sign * pt;
    } else {
        st->board[m.to] = p;
    }
    st->board[m.from] = 0;
    st->turn = -st->turn;
}

// Converts a move into Standard Algebraic Notation (SAN)
void get_move_san(BoardState *prev, Move m, char *san) {
    int p = prev->board[m.from];
    int p_type = abs(p);
    int is_capture = (prev->board[m.to] != 0) || (p_type == 1 && m.to == prev->ep_sq);

    if (p_type == 6 && abs(m.from - m.to) == 2) {
        if (m.to > m.from) strcpy(san, "O-O");
        else strcpy(san, "O-O-O");
        return;
    }

    int len = 0;
    if (p_type != 1) {
        char pieces[] = " PNBRQK";
        san[len++] = pieces[p_type];

        // Simple piece disambiguation
        int need_file = 0, need_rank = 0;
        Move temp_moves[256];
        int tc = get_legal_moves(prev, temp_moves);
        for (int i = 0; i < tc; i++) {
            if (temp_moves[i].from != m.from && temp_moves[i].to == m.to) {
                int other_p = prev->board[temp_moves[i].from];
                if (abs(other_p) == p_type) {
                    if (temp_moves[i].from % 8 != m.from % 8) need_file = 1;
                    else need_rank = 1;
                }
            }
        }
        if (need_file) san[len++] = 'a' + (m.from % 8);
        if (need_rank) san[len++] = '8' - (m.from / 8);
    } else {
        if (is_capture) san[len++] = 'a' + (m.from % 8);
    }

    if (is_capture) san[len++] = 'x';
    san[len++] = 'a' + (m.to % 8);
    san[len++] = '8' - (m.to / 8);

    if (m.promo) {
        san[len++] = '=';
        san[len++] = toupper(m.promo);
    }

    BoardState post = *prev;
    apply_move(&post, m);
    Move post_moves[256];
    int post_lc = get_legal_moves(&post, post_moves);
    int op_king = -1;
    for (int s = 0; s < 64; s++) {
        if (post.board[s] == -6 * prev->turn) {
            op_king = s;
            break;
        }
    }
    if (op_king != -1 && is_square_attacked(&post, op_king, prev->turn)) {
        if (post_lc == 0) san[len++] = '#';
        else san[len++] = '+';
    }
    san[len] = '\0';
}

void print_uci_move(Move m, char *buf) {
    sprintf(buf, "%c%c%c%c", 'a' + (m.from % 8), '8' - (m.from / 8), 'a' + (m.to % 8), '8' - (m.to / 8));
    if (m.promo) sprintf(buf + 4, "%c", m.promo);
}

int parse_uci_move(const char *str, Move *m) {
    if (strlen(str) < 4) return 0;
    int f_col = str[0] - 'a', f_row = '8' - str[1];
    int t_col = str[2] - 'a', t_row = '8' - str[3];
    if (f_col < 0 || f_col > 7 || f_row < 0 || f_row > 7 || t_col < 0 || t_col > 7 || t_row < 0 || t_row > 7) return 0;
    m->from = f_row * 8 + f_col;
    m->to = t_row * 8 + t_col;
    m->promo = (strlen(str) == 5) ? tolower(str[4]) : 0;
    return 1;
}

char piece_char(int p) {
    int ap = abs(p);
    char pcs[] = " PNBRQK";
    return (ap >= 1 && ap <= 6) ? pcs[ap] : ' ';
}

// Configures the square-rendering colors in 256-color terminal format
void set_colors(int sq, int sel_sq, int cur_sq, int is_legal, int is_last, int is_check, int piece) {
    int bg = ((sq / 8) + (sq % 8)) % 2 == 0 ? 253 : 242; // standard board pattern
    int fg = (piece > 0) ? 15 : 16; // 15=white, 16=black

    if (is_last) bg = 75;      // Last move sky blue
    if (is_legal) bg = 114;    // Legal destinations soft green
    if (sel_sq == sq) bg = 214; // Selected piece bright amber
    if (is_check) bg = 196;    // Checked king red
    if (cur_sq == sq) bg = 201; // Cursor box neon magenta

    printf("\x1b[38;5;%d;48;5;%dm", fg, bg);
}

void draw_sidebar(int row) {
    printf("   │ ");
    switch (row) {
        case 0:
            printf("\x1b[1;35mGAME MODE:\x1b[0m ");
            if (game_mode == 0) printf("Player (White) vs Stockfish");
            else if (game_mode == 1) printf("Player (Black) vs Stockfish");
            else printf("Player vs Player (Local)");
            break;
        case 1:
            printf("\x1b[1;35mSETTINGS:\x1b[0m  ");
            if (limit_type == 0) printf("Time Control: %d ms/move", limit_value);
            else if (limit_type == 1) printf("Depth: %d ply", limit_value);
            else printf("Nodes: %d", limit_value);
            break;
        case 2:
            printf("\x1b[1;35mTURN:\x1b[0m      %s", state.turn == 1 ? "\x1b[1;37mWhite\x1b[0m" : "\x1b[1;30mBlack\x1b[0m");
            break;
        case 3:
            printf("\x1b[1;35mENGINE:\x1b[0m    %s", engine_online ? "\x1b[1;32mOnline\x1b[0m" : "\x1b[1;31mOffline (PvP Fallback)\x1b[0m");
            break;
        case 4:
            printf("\x1b[1;33mRECENT MOVES (PGN):\x1b[0m");
            break;
        case 5:
        case 6:
        case 7: {
            int items_per_row = 4;
            int max_visible_moves = 12;
            int start_idx = (move_history_count > max_visible_moves) ? (move_history_count - max_visible_moves) & ~1 : 0;
            int row_offset = (row - 5) * items_per_row;
            for (int i = 0; i < items_per_row; i++) {
                int idx = start_idx + row_offset + i;
                if (idx < move_history_count) {
                    if (idx % 2 == 0) printf("%d. %s ", (idx / 2) + 1, move_history[idx].san);
                    else printf("%s  ", move_history[idx].san);
                }
            }
            break;
        }
    }
}

void draw_ui() {
    printf("\x1b[H"); // Reset cursor to (0,0) instead of clearing screen to eliminate flickering

    // Board Header Title
    printf("\x1b[1;36m┌────────────────────────────────────────┐──────────────────────────────────────────────┐\x1b[0m\x1b[K\n");
    printf("\x1b[1;36m│          TERMINAL CHESS GUI            │                 STATUS BAR                   │\x1b[0m\x1b[K\n");
    printf("\x1b[1;36m└────────────────────────────────────────┘──────────────────────────────────────────────┘\x1b[0m\x1b[K\n");

    int last_from = -1, last_to = -1;
    if (move_history_count > 0) {
        last_from = move_history[move_history_count - 1].m.from;
        last_to = move_history[move_history_count - 1].m.to;
    }

    int check_sq = -1;
    int king_sq = -1;
    for (int s = 0; s < 64; s++) {
        if (state.board[s] == 6 * state.turn) {
            king_sq = s;
            break;
        }
    }
    if (king_sq != -1 && is_square_attacked(&state, king_sq, -state.turn)) {
        check_sq = king_sq;
    }

    // Render ranks 8 down to 1
    for (int r = 0; r < 8; r++) {
        printf(" %d │", 8 - r);
        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            int piece = state.board[sq];
            int is_legal = legal_targets[sq];
            int is_last = (sq == last_from || sq == last_to);
            int is_check = (sq == check_sq);

            set_colors(sq, selected_sq, cursor_sq, is_legal, is_last, is_check, piece);
            char p_c = piece_char(piece);
            if (piece != 0) {
                printf("  %c  ", p_c);
            } else {
                if (is_legal) printf("  •  ");
                else printf("     ");
            }
        }
        printf("\x1b[0m"); // reset color
        draw_sidebar(r);
        printf("\x1b[K\n");
    }

    printf("   └────────────────────────────────────────┘\x1b[K\n");
    printf("       A    B    C    D    E    F    G    H \x1b[K\n");
    printf("\x1b[K\n");
    printf(" \x1b[1;33mControls:\x1b[0m [WASD/Arrows] Navigate | [Space/Enter] Select | [U] Undo\x1b[K\n");
    printf("           [M] Game Mode | [T] Time Control | [Q] Exit Game\x1b[K\n");
    printf("\x1b[K\n");
    printf(" \x1b[1;32mStatus:\x1b[0m %s\x1b[K\n", status_msg);
    printf("\x1b[K\n");
}

char ask_promotion() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\x1b[?25h"); // Show cursor
    char p = 'q';
    while (1) {
        printf("\x1b[H\x1b[J");
        printf("==================================\n");
        printf("       PAWN PROMOTION MENU        \n");
        printf("==================================\n");
        printf("Enter promotion piece ([q]ueen, [r]ook, [b]ishop, [n]ight): ");
        fflush(stdout);
        char input[10];
        if (fgets(input, sizeof(input), stdin)) {
            char choice = tolower(input[0]);
            if (choice == 'q' || choice == 'r' || choice == 'b' || choice == 'n') {
                p = choice;
                break;
            }
        }
    }
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\x1b[?25l"); // Hide cursor
    return p;
}

void trigger_engine_move() {
    if (!engine_online) return;
    sprintf(status_msg, "Engine is thinking...");
    draw_ui();

    // Construct the position path using move logs
    char cmd[16384] = "position startpos moves";
    for (int i = 0; i < move_history_count; i++) {
        char mv_str[10];
        print_uci_move(move_history[i].m, mv_str);
        strcat(cmd, " ");
        strcat(cmd, mv_str);
    }
    strcat(cmd, "\n");
    send_to_engine(cmd);

    // Build the "go" search constraint
    char go_cmd[128];
    if (limit_type == 0) sprintf(go_cmd, "go movetime %d\n", limit_value);
    else if (limit_type == 1) sprintf(go_cmd, "go depth %d\n", limit_value);
    else sprintf(go_cmd, "go nodes %d\n", limit_value);
    send_to_engine(go_cmd);

    char line[1024];
    while (1) {
        int len = read_line_from_engine(line, sizeof(line));
        if (len > 0 && strncmp(line, "bestmove", 8) == 0) {
            char mv_str[16];
            sscanf(line, "bestmove %s", mv_str);
            Move m;
            if (parse_uci_move(mv_str, &m)) {
                // Find and enforce the legal move matching the engine input
                Move legals[256];
                int lc = get_legal_moves(&state, legals);
                for (int i = 0; i < lc; i++) {
                    if (legals[i].from == m.from && legals[i].to == m.to) {
                        m.promo = legals[i].promo;
                        get_move_san(&state, m, move_history[move_history_count].san);
                        move_history[move_history_count].m = m;
                        move_history_count++;
                        apply_move(&state, m);
                        current_history_index++;
                        history[current_history_index] = state;
                        break;
                    }
                }
            }
            break;
        }
        usleep(10000);
    }
    sprintf(status_msg, "Engine played. Your turn!");
}

void configure_time_controls() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\x1b[?25h"); // Show cursor
    printf("\x1b[H\x1b[J");
    printf("==================================\n");
    printf("      TIME CONTROL MENU          \n");
    printf("==================================\n");
    printf("1. Set Max Depth (ply)\n");
    printf("2. Set Max Nodes\n");
    printf("3. Set Move Time Limit (ms)\n");
    printf("Enter choice (1-3): ");
    fflush(stdout);

    char input[64];
    if (fgets(input, sizeof(input), stdin)) {
        int opt = atoi(input);
        if (opt >= 1 && opt <= 3) {
            printf("Enter limit value: ");
            fflush(stdout);
            if (fgets(input, sizeof(input), stdin)) {
                int val = atoi(input);
                if (val > 0) {
                    limit_type = opt - 1;
                    limit_value = val;
                }
            }
        }
    }
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\x1b[?25l"); // Hide cursor
    sprintf(status_msg, "Settings updated successfully.");
}

void configure_game_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\x1b[?25h"); // Show cursor
    printf("\x1b[H\x1b[J");
    printf("==================================\n");
    printf("        GAME MODE MENU            \n");
    printf("==================================\n");
    printf("1. Play as White (vs Stockfish)\n");
    printf("2. Play as Black (vs Stockfish)\n");
    printf("3. Local Player vs Player\n");
    printf("Enter choice (1-3): ");
    fflush(stdout);

    char input[64];
    if (fgets(input, sizeof(input), stdin)) {
        int opt = atoi(input);
        if (opt >= 1 && opt <= 3) {
            game_mode = opt - 1;
            // Reset the entire board structure
            state.turn = 1;
            state.castle[0] = state.castle[1] = state.castle[2] = state.castle[3] = 1;
            state.ep_sq = -1;
            memcpy(state.board, start_board, sizeof(start_board));
            current_history_index = 0;
            history[current_history_index] = state;
            move_history_count = 0;
        }
    }
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\x1b[?25l"); // Hide cursor
    sprintf(status_msg, "Game mode reset.");
}

void trigger_undo() {
    int rollbacks = (game_mode == 2) ? 1 : 2;
    if (current_history_index >= rollbacks) {
        current_history_index -= rollbacks;
        state = history[current_history_index];
        move_history_count -= rollbacks;
        selected_sq = -1;
        memset(legal_targets, 0, sizeof(legal_targets));
        sprintf(status_msg, "Moves taken back.");
    } else {
        sprintf(status_msg, "Cannot undo any further!");
    }
}

int main() {
    // Clear screen on initial boot
    printf("\x1b[2J\x1b[H");

    init_terminal();
    init_engine();

    // Default configuration: Play White vs Stockfish (Local Fallback if engine offline)
    if (!engine_online) {
        game_mode = 2; // Default to Local PvP if no engine
        sprintf(status_msg, "Stockfish not found. Running in Local PvP Fallback mode.");
    }

    state.turn = 1;
    state.castle[0] = state.castle[1] = state.castle[2] = state.castle[3] = 1;
    state.ep_sq = -1;
    memcpy(state.board, start_board, sizeof(start_board));
    history[current_history_index] = state;

    while (1) {
        Move legals[256];
        int lc = get_legal_moves(&state, legals);

        // Check for checkmate/stalemate
        int king_sq = -1;
        for (int s = 0; s < 64; s++) {
            if (state.board[s] == 6 * state.turn) {
                king_sq = s;
                break;
            }
        }
        int is_check = (king_sq != -1) && is_square_attacked(&state, king_sq, -state.turn);
        if (lc == 0) {
            if (is_check) {
                sprintf(status_msg, "CHECKMATE! %s wins.", state.turn == 1 ? "Black" : "White");
            } else {
                sprintf(status_msg, "STALEMATE! Game is drawn.");
            }
        }

        draw_ui();

        // Check if it's the engine's turn to move
        if (lc > 0 && game_mode != 2) {
            if ((game_mode == 0 && state.turn == -1) || (game_mode == 1 && state.turn == 1)) {
                trigger_engine_move();
                continue;
            }
        }

        // Wait for keyboard inputs
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) continue;

        if (c == '\x1b') { // Parse ANSI Arrow Escape Sequence
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                if (seq[0] == '[') {
                    int r = cursor_sq / 8, col = cursor_sq % 8;
                    if (seq[1] == 'A' && r > 0) cursor_sq -= 8; // Up arrow
                    if (seq[1] == 'B' && r < 7) cursor_sq += 8; // Down arrow
                    if (seq[1] == 'C' && col < 7) cursor_sq += 1; // Right arrow
                    if (seq[1] == 'D' && col > 0) cursor_sq -= 1; // Left arrow
                }
            }
        } else if (c == 'w' || c == 'W') {
            if (cursor_sq / 8 > 0) cursor_sq -= 8;
        } else if (c == 's' || c == 'S') {
            if (cursor_sq / 8 < 7) cursor_sq += 8;
        } else if (c == 'a' || c == 'A') {
            if (cursor_sq % 8 > 0) cursor_sq -= 1;
        } else if (c == 'd' || c == 'D') {
            if (cursor_sq % 8 < 7) cursor_sq += 1;
        } else if (c == ' ' || c == '\n') { // Space or Enter select/execute
            if (selected_sq == -1) {
                // Select your color piece
                if (state.board[cursor_sq] != 0 && ((state.board[cursor_sq] > 0 && state.turn == 1) || (state.board[cursor_sq] < 0 && state.turn == -1))) {
                    selected_sq = cursor_sq;
                    memset(legal_targets, 0, sizeof(legal_targets));
                    for (int i = 0; i < lc; i++) {
                        if (legals[i].from == selected_sq) {
                            legal_targets[legals[i].to] = 1;
                        }
                    }
                    sprintf(status_msg, "Selected %c on %c%c. Choose destination.", piece_char(state.board[selected_sq]), 'a' + (selected_sq % 8), '8' - (selected_sq / 8));
                } else {
                    sprintf(status_msg, "That is not your piece!");
                }
            } else {
                if (cursor_sq == selected_sq) {
                    selected_sq = -1;
                    memset(legal_targets, 0, sizeof(legal_targets));
                    sprintf(status_msg, "Selection canceled.");
                } else if (legal_targets[cursor_sq]) {
                    // Assemble the requested move
                    Move executed_move = {selected_sq, cursor_sq, 0};

                    // Detect pawn promotion target
                    int p_type = abs(state.board[selected_sq]);
                    if (p_type == 1 && (cursor_sq / 8 == 0 || cursor_sq / 8 == 7)) {
                        executed_move.promo = ask_promotion();
                    }

                    get_move_san(&state, executed_move, move_history[move_history_count].san);
                    move_history[move_history_count].m = executed_move;
                    move_history_count++;

                    apply_move(&state, executed_move);
                    current_history_index++;
                    history[current_history_index] = state;

                    selected_sq = -1;
                    memset(legal_targets, 0, sizeof(legal_targets));
                    sprintf(status_msg, "Move played.");
                } else {
                    // If clicking another of your own pieces, change selection directly
                    if (state.board[cursor_sq] != 0 && ((state.board[cursor_sq] > 0 && state.turn == 1) || (state.board[cursor_sq] < 0 && state.turn == -1))) {
                        selected_sq = cursor_sq;
                        memset(legal_targets, 0, sizeof(legal_targets));
                        for (int i = 0; i < lc; i++) {
                            if (legals[i].from == selected_sq) {
                                legal_targets[legals[i].to] = 1;
                            }
                        }
                    } else {
                        selected_sq = -1;
                        memset(legal_targets, 0, sizeof(legal_targets));
                        sprintf(status_msg, "Invalid destination.");
                    }
                }
            }
        } else if (c == 'u' || c == 'U') {
            trigger_undo();
        } else if (c == 't' || c == 'T') {
            configure_time_controls();
        } else if (c == 'm' || c == 'M') {
            configure_game_mode();
        } else if (c == 'q' || c == 'Q') {
            break;
        }
    }

    return 0;
}
