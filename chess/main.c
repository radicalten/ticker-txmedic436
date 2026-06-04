#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/select.h>

/* --- Board State Structures & Constants --- */
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

#define WHITE  8
#define BLACK  16
#define COLOR_MASK 24
#define PIECE_MASK 7

#define MODE_VS_ENGINE  0
#define MODE_TWO_PLAYERS 1

#define ENGINE_IDLE     0
#define ENGINE_THINKING 1

typedef struct {
    int from;
    int to;
    int promo; 
    int flags; // 1 = EP capture, 2 = double pawn push, 4 = castling
} Move;

typedef struct {
    uint8_t board[64];
    uint8_t turn;      // WHITE or BLACK
    uint8_t castle;    // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep_square;     // Target square index for EP capture, -1 if none
    int halfmove;
    int fullmove;
} Board;

typedef enum { TC_DEPTH, TC_NODES, TC_TIME } TCType;
typedef struct {
    TCType type;
    int value; 
} TimeControl;

/* --- Global State --- */
#define MAX_MOVES 2048
Board board_history[MAX_MOVES];
Move move_history[MAX_MOVES];
char san_history[MAX_MOVES][16];
int history_idx = 0;

int play_mode = MODE_VS_ENGINE;
int engine_state = ENGINE_IDLE;
TimeControl tc = { TC_DEPTH, 10 }; // Default Search Depth = 10 plies

int cursor_row = 6;
int cursor_col = 4;
int selected_square = -1;

char selected_engine_path[512] = "";
int engine_in[2];   // Write to engine
int engine_out[2];  // Read from engine
pid_t engine_pid = -1;

struct termios orig_termios;

/* --- Coordinate Helpers --- */
int get_row(int sq) { return sq / 8; }
int get_col(int sq) { return sq % 8; }

/* --- Terminal Control --- */
void disable_raw_mode() {
    printf("\033[?25h"); // Show cursor
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

/* --- UCI Engine Pipeline Control --- */
int launch_engine(const char *path) {
    if (pipe(engine_in) < 0 || pipe(engine_out) < 0) return 0;
    engine_pid = fork();
    if (engine_pid == 0) { // Child Process
        dup2(engine_in[0], STDIN_FILENO);
        dup2(engine_out[1], STDOUT_FILENO);
        close(engine_in[1]);
        close(engine_out[0]);
        close(STDERR_FILENO);
        execlp(path, path, (char *)NULL);
        exit(1);
    }
    close(engine_in[0]);
    close(engine_out[1]);
    fcntl(engine_out[0], F_SETFL, O_NONBLOCK);
    return 1;
}

void send_engine(const char *cmd) {
    if (engine_pid != -1) {
        write(engine_in[1], cmd, strlen(cmd));
        write(engine_in[1], "\n", 1);
    }
}

int read_engine_line(char *buf, int max_len) {
    static char line_buf[4096];
    static int line_idx = 0;
    char ch;
    while (read(engine_out[0], &ch, 1) > 0) {
        if (ch == '\n') {
            line_buf[line_idx] = '\0';
            strncpy(buf, line_buf, max_len);
            line_idx = 0;
            return 1;
        } else if (line_idx < sizeof(line_buf) - 1) {
            line_buf[line_idx++] = ch;
        }
    }
    return 0;
}

void wait_for(const char *expected) {
    char buf[1024];
    while (1) {
        if (read_engine_line(buf, sizeof(buf))) {
            if (strstr(buf, expected)) break;
        }
        usleep(1000);
    }
}

int find_engine() {
    const char *paths[] = {
        "stockfish",
        "/opt/homebrew/bin/stockfish",
        "/usr/local/bin/stockfish",
        "./stockfish"
    };
    for (int i = 0; i < 4; i++) {
        if (strcmp(paths[i], "stockfish") == 0) {
            FILE *f = popen("which stockfish 2>/dev/null", "r");
            if (f) {
                char buf[512];
                if (fgets(buf, sizeof(buf), f)) {
                    buf[strcspn(buf, "\r\n")] = 0;
                    if (strlen(buf) > 0 && access(buf, X_OK) == 0) {
                        strcpy(selected_engine_path, buf);
                        pclose(f);
                        return 1;
                    }
                }
                pclose(f);
            }
        } else {
            if (access(paths[i], X_OK) == 0) {
                strcpy(selected_engine_path, paths[i]);
                return 1;
            }
        }
    }
    return 0;
}

/* --- Chess Engine Movement & Rule Enforcement --- */
void init_board(Board *b) {
    memset(b, 0, sizeof(Board));
    uint8_t back_row[8] = { ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK };
    for (int i = 0; i < 8; i++) {
        b->board[i] = BLACK | back_row[i];
        b->board[8 + i] = BLACK | PAWN;
        b->board[48 + i] = WHITE | PAWN;
        b->board[56 + i] = WHITE | back_row[i];
    }
    b->turn = WHITE;
    b->castle = 15;
    b->ep_square = -1;
    b->fullmove = 1;
}

int is_square_attacked(const Board *b, int sq, int attacker_color) {
    int opponent = attacker_color;
    if (opponent == WHITE) {
        int p1 = sq + 7, p2 = sq + 9;
        if (p1 >= 0 && p1 < 64 && abs(get_col(p1) - get_col(sq)) == 1 && b->board[p1] == (WHITE | PAWN)) return 1;
        if (p2 >= 0 && p2 < 64 && abs(get_col(p2) - get_col(sq)) == 1 && b->board[p2] == (WHITE | PAWN)) return 1;
    } else {
        int p1 = sq - 7, p2 = sq - 9;
        if (p1 >= 0 && p1 < 64 && abs(get_col(p1) - get_col(sq)) == 1 && b->board[p1] == (BLACK | PAWN)) return 1;
        if (p2 >= 0 && p2 < 64 && abs(get_col(p2) - get_col(sq)) == 1 && b->board[p2] == (BLACK | PAWN)) return 1;
    }

    int knight_offsets[8] = { -17, -15, -10, -6, 6, 10, 15, 17 };
    for (int i = 0; i < 8; i++) {
        int target = sq + knight_offsets[i];
        if (target >= 0 && target < 64 && abs(get_col(target) - get_col(sq)) <= 2) {
            if (b->board[target] == (opponent | KNIGHT)) return 1;
        }
    }

    int ortho[4] = { -8, 8, -1, 1 };
    for (int i = 0; i < 4; i++) {
        int step = ortho[i];
        int curr = sq;
        while (1) {
            if (step == -1 || step == 1) {
                if (get_row(curr + step) != get_row(curr)) break;
            }
            curr += step;
            if (curr < 0 || curr >= 64) break;
            uint8_t pc = b->board[curr];
            if (pc != EMPTY) {
                if (pc == (opponent | ROOK) || pc == (opponent | QUEEN)) return 1;
                break;
            }
        }
    }

    int diag[4] = { -9, -7, 7, 9 };
    for (int i = 0; i < 4; i++) {
        int step = diag[i];
        int curr = sq;
        while (1) {
            int prev_col = get_col(curr);
            int next = curr + step;
            if (next < 0 || next >= 64 || abs(get_col(next) - prev_col) != 1) break;
            curr = next;
            uint8_t pc = b->board[curr];
            if (pc != EMPTY) {
                if (pc == (opponent | BISHOP) || pc == (opponent | QUEEN)) return 1;
                break;
            }
        }
    }

    int king_dirs[8] = { -9, -8, -7, -1, 1, 7, 8, 9 };
    for (int i = 0; i < 8; i++) {
        int target = sq + king_dirs[i];
        if (target >= 0 && target < 64 && abs(get_col(target) - get_col(sq)) <= 1) {
            if (b->board[target] == (opponent | KING)) return 1;
        }
    }
    return 0;
}

int find_king(const Board *b, int color) {
    for (int i = 0; i < 64; i++) {
        if (b->board[i] == (color | KING)) return i;
    }
    return -1;
}

int is_in_check(const Board *b, int color) {
    int kpos = find_king(b, color);
    if (kpos == -1) return 0;
    return is_square_attacked(b, kpos, color == WHITE ? BLACK : WHITE);
}

int generate_moves(const Board *b, Move *moves) {
    int count = 0;
    int us = b->turn;
    int them = (us == WHITE) ? BLACK : WHITE;

    for (int i = 0; i < 64; i++) {
        uint8_t pc = b->board[i];
        if (pc == EMPTY || (pc & COLOR_MASK) != us) continue;
        int type = pc & PIECE_MASK;
        int r = get_row(i);
        int c = get_col(i);

        if (type == PAWN) {
            int dir = (us == WHITE) ? -8 : 8;
            int start_row = (us == WHITE) ? 6 : 1;
            int promo_row = (us == WHITE) ? 0 : 7;

            int next = i + dir;
            if (next >= 0 && next < 64 && b->board[next] == EMPTY) {
                if (get_row(next) == promo_row) {
                    moves[count++] = (Move){i, next, QUEEN, 0};
                    moves[count++] = (Move){i, next, ROOK, 0};
                    moves[count++] = (Move){i, next, BISHOP, 0};
                    moves[count++] = (Move){i, next, KNIGHT, 0};
                } else {
                    moves[count++] = (Move){i, next, 0, 0};
                    if (r == start_row) {
                        int d_next = i + 2 * dir;
                        if (b->board[d_next] == EMPTY) {
                            moves[count++] = (Move){i, d_next, 0, 2};
                        }
                    }
                }
            }

            int caps[2] = { dir - 1, dir + 1 };
            for (int k = 0; k < 2; k++) {
                int target = i + caps[k];
                if (target >= 0 && target < 64 && abs(get_col(target) - c) == 1) {
                    if (b->board[target] != EMPTY && (b->board[target] & COLOR_MASK) == them) {
                        if (get_row(target) == promo_row) {
                            moves[count++] = (Move){i, target, QUEEN, 0};
                            moves[count++] = (Move){i, target, ROOK, 0};
                            moves[count++] = (Move){i, target, BISHOP, 0};
                            moves[count++] = (Move){i, target, KNIGHT, 0};
                        } else {
                            moves[count++] = (Move){i, target, 0, 0};
                        }
                    }
                    if (target == b->ep_square) {
                        moves[count++] = (Move){i, target, 0, 1};
                    }
                }
            }
        }
        else if (type == KNIGHT) {
            int offsets[8] = { -17, -15, -10, -6, 6, 10, 15, 17 };
            for (int o = 0; o < 8; o++) {
                int target = i + offsets[o];
                if (target >= 0 && target < 64 && abs(get_col(target) - c) <= 2) {
                    if (b->board[target] == EMPTY || (b->board[target] & COLOR_MASK) == them) {
                        moves[count++] = (Move){i, target, 0, 0};
                    }
                }
            }
        }
        else if (type == BISHOP || type == QUEEN) {
            int diag_dirs[4] = { -9, -7, 7, 9 };
            for (int d = 0; d < 4; d++) {
                int step = diag_dirs[d];
                int curr = i;
                while (1) {
                    int p_col = get_col(curr);
                    int next = curr + step;
                    if (next < 0 || next >= 64 || abs(get_col(next) - p_col) != 1) break;
                    curr = next;
                    if (b->board[curr] == EMPTY) {
                        moves[count++] = (Move){i, curr, 0, 0};
                    } else {
                        if ((b->board[curr] & COLOR_MASK) == them) moves[count++] = (Move){i, curr, 0, 0};
                        break;
                    }
                }
            }
        }
        if (type == ROOK || type == QUEEN) {
            int ortho_dirs[4] = { -8, 8, -1, 1 };
            for (int d = 0; d < 4; d++) {
                int step = ortho_dirs[d];
                int curr = i;
                while (1) {
                    if (step == -1 || step == 1) {
                        if (get_row(curr + step) != get_row(curr)) break;
                    }
                    curr += step;
                    if (curr < 0 || curr >= 64) break;
                    if (b->board[curr] == EMPTY) {
                        moves[count++] = (Move){i, curr, 0, 0};
                    } else {
                        if ((b->board[curr] & COLOR_MASK) == them) moves[count++] = (Move){i, curr, 0, 0};
                        break;
                    }
                }
            }
        }
        else if (type == KING) {
            int king_dirs[8] = { -9, -8, -7, -1, 1, 7, 8, 9 };
            for (int d = 0; d < 8; d++) {
                int target = i + king_dirs[d];
                if (target >= 0 && target < 64 && abs(get_col(target) - c) <= 1) {
                    if (b->board[target] == EMPTY || (b->board[target] & COLOR_MASK) == them) {
                        moves[count++] = (Move){i, target, 0, 0};
                    }
                }
            }
            if (us == WHITE) {
                if ((b->castle & 1) && b->board[61] == EMPTY && b->board[62] == EMPTY) {
                    if (!is_square_attacked(b, 60, BLACK) && !is_square_attacked(b, 61, BLACK) && !is_square_attacked(b, 62, BLACK)) {
                        moves[count++] = (Move){60, 62, 0, 4};
                    }
                }
                if ((b->castle & 2) && b->board[59] == EMPTY && b->board[58] == EMPTY && b->board[57] == EMPTY) {
                    if (!is_square_attacked(b, 60, BLACK) && !is_square_attacked(b, 59, BLACK) && !is_square_attacked(b, 58, BLACK)) {
                        moves[count++] = (Move){60, 58, 0, 4};
                    }
                }
            } else {
                if ((b->castle & 4) && b->board[5] == EMPTY && b->board[6] == EMPTY) {
                    if (!is_square_attacked(b, 4, WHITE) && !is_square_attacked(b, 5, WHITE) && !is_square_attacked(b, 6, WHITE)) {
                        moves[count++] = (Move){4, 6, 0, 4};
                    }
                }
                if ((b->castle & 8) && b->board[3] == EMPTY && b->board[2] == EMPTY && b->board[1] == EMPTY) {
                    if (!is_square_attacked(b, 4, WHITE) && !is_square_attacked(b, 3, WHITE) && !is_square_attacked(b, 2, WHITE)) {
                        moves[count++] = (Move){4, 2, 0, 4};
                    }
                }
            }
        }
    }
    return count;
}

void make_move(Board *b, Move m) {
    int us = b->turn;
    int them = (us == WHITE) ? BLACK : WHITE;
    uint8_t pc = b->board[m.from];
    int next_ep = -1;

    if (m.flags == 1) { // EP Capture
        int cap_sq = m.to + (us == WHITE ? 8 : -8);
        b->board[cap_sq] = EMPTY;
    }
    if (m.flags == 2) { // Double Pawn Push
        next_ep = m.to + (us == WHITE ? 8 : -8);
    }
    if (m.flags == 4) { // Castling
        if (m.to == 62) { b->board[61] = b->board[63]; b->board[63] = EMPTY; }
        else if (m.to == 58) { b->board[59] = b->board[56]; b->board[56] = EMPTY; }
        else if (m.to == 6) { b->board[5] = b->board[7]; b->board[7] = EMPTY; }
        else if (m.to == 2) { b->board[3] = b->board[0]; b->board[0] = EMPTY; }
    }

    if ((pc & PIECE_MASK) == KING) {
        b->castle &= (us == WHITE) ? ~3 : ~12;
    }
    if (m.from == 56 || m.to == 56) b->castle &= ~2;
    if (m.from == 63 || m.to == 63) b->castle &= ~1;
    if (m.from == 0 || m.to == 0) b->castle &= ~8;
    if (m.from == 7 || m.to == 7) b->castle &= ~4;

    b->board[m.to] = m.promo ? (us | m.promo) : pc;
    b->board[m.from] = EMPTY;
    b->turn = them;
    b->ep_square = next_ep;
    if (us == BLACK) b->fullmove++;
}

int generate_legal_moves(const Board *b, Move *legals) {
    Move pseudo[256];
    int count = generate_moves(b, pseudo);
    int legal_count = 0;
    for (int i = 0; i < count; i++) {
        Board temp = *b;
        make_move(&temp, pseudo[i]);
        if (!is_in_check(&temp, b->turn)) {
            legals[legal_count++] = pseudo[i];
        }
    }
    return legal_count;
}

/* --- Notation Formatters --- */
void get_san(const Board *b, Move m, char *san) {
    if (m.flags == 4) {
        strcpy(san, (m.to == 62 || m.to == 6) ? "O-O" : "O-O-O");
    } else {
        int pc = b->board[m.from];
        int type = pc & PIECE_MASK;
        int cap = (b->board[m.to] != EMPTY) || (m.flags == 1);
        int idx = 0;

        if (type != PAWN) {
            san[idx++] = "  NBRQK"[type];
            Move legals[256];
            int l_cnt = generate_legal_moves(b, legals);
            int same_file = 0, same_rank = 0, multi = 0;
            for (int i = 0; i < l_cnt; i++) {
                if (legals[i].from != m.from && legals[i].to == m.to && b->board[legals[i].from] == pc) {
                    multi = 1;
                    if (get_col(legals[i].from) == get_col(m.from)) same_rank = 1;
                    else same_file = 1;
                }
            }
            if (multi) {
                if (!same_file) san[idx++] = 'a' + get_col(m.from);
                else if (!same_rank) san[idx++] = '8' - get_row(m.from);
                else {
                    san[idx++] = 'a' + get_col(m.from);
                    san[idx++] = '8' - get_row(m.from);
                }
            }
        } else if (cap) {
            san[idx++] = 'a' + get_col(m.from);
        }

        if (cap) san[idx++] = 'x';
        san[idx++] = 'a' + get_col(m.to);
        san[idx++] = '8' - get_row(m.to);

        if (m.promo) {
            san[idx++] = '=';
            san[idx++] = "  NBRQ"[m.promo];
        }
        san[idx] = '\0';
    }

    Board temp = *b;
    make_move(&temp, m);
    if (is_in_check(&temp, temp.turn)) {
        Move legals[256];
        strcat(san, (generate_legal_moves(&temp, legals) == 0) ? "#" : "+");
    }
}

void move_to_uci(Move m, char *str) {
    int idx = 0;
    str[idx++] = 'a' + get_col(m.from);
    str[idx++] = '8' - get_row(m.from);
    str[idx++] = 'a' + get_col(m.to);
    str[idx++] = '8' - get_row(m.to);
    if (m.promo) {
        str[idx++] = "  nbrq"[m.promo];
    }
    str[idx] = '\0';
}

int parse_uci_move(const Board *b, const char *str, Move *m) {
    if (strlen(str) < 4) return 0;
    int f_col = str[0] - 'a';
    int f_row = '8' - str[1];
    int t_col = str[2] - 'a';
    int t_row = '8' - str[3];
    int promo = 0;
    if (str[4] == 'q') promo = QUEEN;
    else if (str[4] == 'r') promo = ROOK;
    else if (str[4] == 'b') promo = BISHOP;
    else if (str[4] == 'n') promo = KNIGHT;

    Move legals[256];
    int count = generate_legal_moves(b, legals);
    for (int i = 0; i < count; i++) {
        if (legals[i].from == (f_row * 8 + f_col) && legals[i].to == (t_row * 8 + t_col)) {
            if (promo == 0 || legals[i].promo == promo) {
                *m = legals[i];
                return 1;
            }
        }
    }
    return 0;
}

void send_position() {
    char cmd[8192] = "position startpos";
    if (history_idx > 0) {
        strcat(cmd, " moves");
        for (int i = 0; i < history_idx; i++) {
            char uci_str[16];
            move_to_uci(move_history[i], uci_str);
            strcat(cmd, " ");
            strcat(cmd, uci_str);
        }
    }
    send_engine(cmd);
}

void send_go() {
    char cmd[128];
    if (tc.type == TC_DEPTH) sprintf(cmd, "go depth %d", tc.value);
    else if (tc.type == TC_NODES) sprintf(cmd, "go nodes %d", tc.value);
    else sprintf(cmd, "go movetime %d", tc.value);
    send_engine(cmd);
}

/* --- Interactive Graphic Rendering --- */
void print_pgn_sidebar(int line_offset) {
    int pair_cnt = (history_idx + 1) / 2;
    int start_pair = pair_cnt - 7;
    if (start_pair < 0) start_pair = 0;

    int pair_idx = start_pair + line_offset;
    if (pair_idx < pair_cnt) {
        char w_move[16] = "";
        char b_move[16] = "";
        if (pair_idx * 2 < history_idx) strcpy(w_move, san_history[pair_idx * 2]);
        if (pair_idx * 2 + 1 < history_idx) strcpy(b_move, san_history[pair_idx * 2 + 1]);
        printf("  %2d. %-7s %-7s", pair_idx + 1, w_move, b_move);
    } else {
        printf("                   ");
    }
}

void draw_sidebar_line(int line) {
    printf("    │ ");
    switch (line) {
        case 0:  printf("\033[1;36mSETTINGS & STATUS\033[0m"); break;
        case 1:  printf("─────────────────"); break;
        case 2:  printf("Mode:    %s", play_mode == MODE_VS_ENGINE ? "Vs Engine (Stockfish)" : "Two Players (Hotseat)"); break;
        case 3:  printf("Turn:    %s", board_history[history_idx].turn == WHITE ? "\033[1;33mWhite\033[0m" : "\033[1;35mBlack\033[0m"); break;
        case 4: {
            char tc_buf[64];
            if (tc.type == TC_DEPTH) sprintf(tc_buf, "Depth (%d plies)", tc.value);
            else if (tc.type == TC_NODES) sprintf(tc_buf, "Nodes (%d)", tc.value);
            else sprintf(tc_buf, "Time (%d ms/mv)", tc.value);
            printf("Limits:  %s", tc_buf);
            break;
        }
        case 5:  printf("Engine:  %s", engine_state == ENGINE_THINKING ? "\033[1;31mThinking...\033[0m" : "Idle"); break;
        case 6:  printf("Path:    %s", strlen(selected_engine_path) > 0 ? "Connected" : "(None)"); break;
        case 7:  printf("─────────────────"); break;
        case 8:  printf("\033[1;32mMOVE HISTORY (PGN)\033[0m"); break;
        default:
            if (line >= 9 && line <= 15) {
                print_pgn_sidebar(line - 9);
            }
            break;
    }
}

void draw_game() {
    printf("\033[H"); // Cursor to Home (no flickering)
    printf("\033[1;36m      === CHESS ENGINE TERMINAL GUI ===\033[0m\n\n");

    Board *b = &board_history[history_idx];
    int legal_dests[64] = {0};
    if (selected_square != -1) {
        Move legals[256];
        int count = generate_legal_moves(b, legals);
        for (int i = 0; i < count; i++) {
            if (legals[i].from == selected_square) {
                legal_dests[legals[i].to] = 1;
            }
        }
    }

    int in_check = is_in_check(b, b->turn);
    int king_pos = find_king(b, b->turn);

    for (int r = 0; r < 8; r++) {
        for (int sub = 0; sub < 2; sub++) {
            if (sub == 0) printf("  %d ", 8 - r);
            else printf("    ");

            for (int c = 0; c < 8; c++) {
                int sq = r * 8 + c;
                const char *bg = "";

                if (in_check && sq == king_pos)                      bg = "\033[48;5;196m"; // Check: Red
                else if (sq == selected_square)                      bg = "\033[48;5;110m"; // Selected: Sky Blue
                else if (sq == (cursor_row * 8 + cursor_col))        bg = "\033[48;5;214m"; // Cursor: Orange
                else if (legal_dests[sq])                            bg = "\033[48;5;151m"; // Legal Move: Green
                else if (history_idx > 0 && (sq == move_history[history_idx - 1].from || sq == move_history[history_idx - 1].to)) {
                    bg = "\033[48;5;186m"; // Last Move: Khaki
                } else if ((r + c) % 2 == 0) {
                    bg = "\033[48;5;252m"; // Light Background
                } else {
                    bg = "\033[48;5;239m"; // Dark Background
                }

                printf("%s", bg);

                if (sub == 0) {
                    if (legal_dests[sq]) printf("  •   ");
                    else printf("      ");
                } else {
                    uint8_t pc = b->board[sq];
                    if (pc != EMPTY) {
                        const char *fg = (pc & COLOR_MASK) == WHITE ? "\033[38;5;220m\033[1m" : "\033[38;5;16m\033[1m";
                        char p_char = " PNBRQK"[pc & PIECE_MASK];
                        printf("  %s%c\033[0m%s   ", fg, p_char, bg);
                    } else {
                        printf("      ");
                    }
                }
                printf("\033[0m");
            }
            draw_sidebar_line(r * 2 + sub);
            printf("\n");
        }
    }
    printf("        a     b     c     d     e     f     g     h\n\n");
}

void draw_controls() {
    printf("\033[1mCONTROLS:\033[0m\n");
    printf("  [\033[1;33mArrows/WASD\033[0m] Move Cursor   [\033[1;33mSPACE/ENTER\033[0m] Select/Play   [\033[1;33mU\033[0m] Undo Move\n");
    printf("  [\033[1;33mT\033[0m] Time Controls       [\033[1;33mM\033[0m] Toggle Game Mode           [\033[1;33mQ\033[0m] Quit Game\n");
}

/* --- Dynamic Input Menus --- */
int read_key() {
    char c;
    int nread = read(STDIN_FILENO, &c, 1);
    if (nread <= 0) return 0;
    if (c == '\033') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return '\033';
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return '\033';
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 'w';
                case 'B': return 's';
                case 'C': return 'd';
                case 'D': return 'a';
            }
        }
        return '\033';
    }
    return c;
}

void configure_time_control() {
    disable_raw_mode();
    printf("\n\033[1;32m--- Time Control Menu ---\033[0m\n");
    printf("1. Search Depth limit (plies)\n");
    printf("2. Search Node limit\n");
    printf("3. Move Time limit (milliseconds)\n");
    printf("Select Choice (1-3): ");
    fflush(stdout);

    char opt[32];
    if (fgets(opt, sizeof(opt), stdin)) {
        int choice = atoi(opt);
        if (choice >= 1 && choice <= 3) {
            printf("Enter parameter value: ");
            fflush(stdout);
            char val_str[32];
            if (fgets(val_str, sizeof(val_str), stdin)) {
                int val = atoi(val_str);
                if (val > 0) {
                    if (choice == 1) { tc.type = TC_DEPTH; tc.value = val; }
                    else if (choice == 2) { tc.type = TC_NODES; tc.value = val; }
                    else { tc.type = TC_TIME; tc.value = val; }
                }
            }
        }
    }
    enable_raw_mode();
    printf("\033[2J");
}

void toggle_mode() {
    if (play_mode == MODE_VS_ENGINE) {
        play_mode = MODE_TWO_PLAYERS;
    } else {
        play_mode = MODE_VS_ENGINE;
        if (engine_pid == -1) {
            if (find_engine()) {
                launch_engine(selected_engine_path);
                send_engine("uci");
                wait_for("uciok");
                send_engine("isready");
                wait_for("readyok");
            } else {
                printf("\n\033[1;31mError: Stockfish engine binary was not found!\033[0m\n");
                printf("Standard offline mode is active.\nPress any key to return...");
                fflush(stdout);
                char c;
                read(STDIN_FILENO, &c, 1);
                play_mode = MODE_TWO_PLAYERS;
                printf("\033[2J");
            }
        }
    }
}

void startup_menu() {
    printf("\033[2J\033[H");
    printf("\033[1;36m================================================\033[0m\n");
    printf("\033[1;36m          CHESS ENGINE TERMINAL GUI             \033[0m\n");
    printf("\033[1;36m================================================\033[0m\n\n");
    printf("Searching path for Stockfish Engine...\n");

    if (find_engine()) {
        printf("\033[1;32mAuto-detected Stockfish binary at: %s\033[0m\n\n", selected_engine_path);
        printf("1. Play vs Stockfish (Default)\n");
        printf("2. Local Multiplayer (Hotseat)\n");
        printf("3. Load custom engine path\n");
        printf("Choose option (1-3) [Default 1]: ");
        fflush(stdout);

        char opt[128];
        if (fgets(opt, sizeof(opt), stdin)) {
            int choice = atoi(opt);
            if (choice == 2) play_mode = MODE_TWO_PLAYERS;
            else if (choice == 3) {
                printf("Enter path: ");
                fflush(stdout);
                if (fgets(selected_engine_path, sizeof(selected_engine_path), stdin)) {
                    selected_engine_path[strcspn(selected_engine_path, "\r\n")] = 0;
                }
                play_mode = MODE_VS_ENGINE;
            } else play_mode = MODE_VS_ENGINE;
        } else play_mode = MODE_VS_ENGINE;
    } else {
        printf("\033[1;31mNo Stockfish engine found on local paths.\033[0m\n\n");
        printf("1. Play Local Multiplayer (Hotseat)\n");
        printf("2. Enter custom engine path\n");
        printf("Choose option (1-2) [Default 1]: ");
        fflush(stdout);

        char opt[128];
        if (fgets(opt, sizeof(opt), stdin)) {
            int choice = atoi(opt);
            if (choice == 2) {
                printf("Enter path: ");
                fflush(stdout);
                if (fgets(selected_engine_path, sizeof(selected_engine_path), stdin)) {
                    selected_engine_path[strcspn(selected_engine_path, "\r\n")] = 0;
                }
                play_mode = MODE_VS_ENGINE;
            } else play_mode = MODE_TWO_PLAYERS;
        } else play_mode = MODE_TWO_PLAYERS;
    }

    if (play_mode == MODE_VS_ENGINE) {
        printf("Setting up communication pipes for engine...\n");
        if (!launch_engine(selected_engine_path)) {
            printf("\033[1;31mFailed to run engine binary. Defaulting to Hotseat.\033[0m\n");
            play_mode = MODE_TWO_PLAYERS;
            sleep(2);
        } else {
            send_engine("uci");
            wait_for("uciok");
            send_engine("isready");
            wait_for("readyok");
        }
    }
    printf("\033[2J");
}

/* --- Main execution loop --- */
int main() {
    init_board(&board_history[0]);
    history_idx = 0;

    startup_menu();
    enable_raw_mode();

    while (1) {
        Board *b = &board_history[history_idx];
        Move legals[256];
        int legal_cnt = generate_legal_moves(b, legals);

        draw_game();
        draw_controls();

        if (legal_cnt == 0) {
            if (is_in_check(b, b->turn)) {
                printf("\033[1;31m   MATE DETECTED! Winner: %s\033[0m\n", b->turn == WHITE ? "Black" : "White");
            } else {
                printf("\033[1;33m   STALEMATE DETECTED! Draw game.\033[0m\n");
            }
        }

        // Trigger Engine turn
        if (play_mode == MODE_VS_ENGINE && engine_state == ENGINE_IDLE && legal_cnt > 0) {
            if (b->turn == BLACK) {
                engine_state = ENGINE_THINKING;
                draw_game();
                draw_controls();
                send_position();
                send_go();
            }
        }

        // Asynchronously check for Engine Moves
        if (engine_state == ENGINE_THINKING) {
            char line[1024];
            if (read_engine_line(line, sizeof(line))) {
                if (strncmp(line, "bestmove ", 9) == 0) {
                    char m_str[16];
                    sscanf(line, "bestmove %s", m_str);
                    Move m;
                    if (parse_uci_move(b, m_str, &m)) {
                        board_history[history_idx + 1] = *b;
                        get_san(b, m, san_history[history_idx]);
                        move_history[history_idx] = m;
                        make_move(&board_history[history_idx + 1], m);
                        history_idx++;
                        selected_square = -1;
                    }
                    engine_state = ENGINE_IDLE;
                    continue;
                }
            }
        }

        // Non-blocking key polling via select()
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 40000}; // 40ms interval
        int res = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);

        if (res > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
            int key = read_key();
            if (key == 'q' || key == 'Q') break;
            else if (key == 'w' || key == 'W') { if (cursor_row > 0) cursor_row--; }
            else if (key == 's' || key == 'S') { if (cursor_row < 7) cursor_row++; }
            else if (key == 'a' || key == 'A') { if (cursor_col > 0) cursor_col--; }
            else if (key == 'd' || key == 'D') { if (cursor_col < 7) cursor_col++; }
            else if (key == 'u' || key == 'U') { // Undo
                if (play_mode == MODE_VS_ENGINE) {
                    if (history_idx >= 2) {
                        history_idx -= 2;
                        engine_state = ENGINE_IDLE;
                        selected_square = -1;
                    }
                } else {
                    if (history_idx >= 1) {
                        history_idx -= 1;
                        selected_square = -1;
                    }
                }
            }
            else if (key == 't' || key == 'T') {
                configure_time_control();
            }
            else if (key == 'm' || key == 'M') {
                toggle_mode();
            }
            else if (key == ' ' || key == '\n') {
                int active_turn = (play_mode == MODE_TWO_PLAYERS) || (b->turn == WHITE);
                if (active_turn && engine_state == ENGINE_IDLE && legal_cnt > 0) {
                    int sq = cursor_row * 8 + cursor_col;
                    if (selected_square == -1) {
                        if (b->board[sq] != EMPTY && ((b->board[sq] & COLOR_MASK) == b->turn)) {
                            selected_square = sq;
                        }
                    } else {
                        if (sq == selected_square) {
                            selected_square = -1;
                        } else {
                            int found = 0;
                            Move active_move;
                            for (int i = 0; i < legal_cnt; i++) {
                                if (legals[i].from == selected_square && legals[i].to == sq) {
                                    found = 1;
                                    active_move = legals[i];
                                    break;
                                }
                            }
                            if (found) {
                                int promo = 0;
                                uint8_t pc = b->board[selected_square];
                                if ((pc & PIECE_MASK) == PAWN) {
                                    int end_r = get_row(sq);
                                    if ((b->turn == WHITE && end_r == 0) || (b->turn == BLACK && end_r == 7)) {
                                        printf("\n\033[1;33mPromotion Piece Choice: (Q)ueen, (R)ook, (B)ishop, (N)ight: \033[0m");
                                        fflush(stdout);
                                        while (1) {
                                            int p_key = read_key();
                                            if (p_key == 'q' || p_key == 'Q') { promo = QUEEN; break; }
                                            if (p_key == 'r' || p_key == 'R') { promo = ROOK; break; }
                                            if (p_key == 'b' || p_key == 'B') { promo = BISHOP; break; }
                                            if (p_key == 'n' || p_key == 'N') { promo = KNIGHT; break; }
                                        }
                                        active_move.promo = promo;
                                    }
                                }

                                board_history[history_idx + 1] = *b;
                                get_san(b, active_move, san_history[history_idx]);
                                move_history[history_idx] = active_move;
                                make_move(&board_history[history_idx + 1], active_move);
                                history_idx++;
                                selected_square = -1;
                            } else {
                                if (b->board[sq] != EMPTY && ((b->board[sq] & COLOR_MASK) == b->turn)) {
                                    selected_square = sq;
                                } else {
                                    selected_square = -1;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (engine_pid != -1) {
        send_engine("quit");
        close(engine_in[1]);
        close(engine_out[0]);
        kill(engine_pid, SIGTERM);
    }

    disable_raw_mode();
    printf("\033[2J\033[HThanks for playing terminal chess!\n");
    return 0;
}
