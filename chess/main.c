#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>

#define MAX_MOVES 2048

#define EMPTY 0
#define P 1
#define N 2
#define B 3
#define R 4
#define Q 5
#define K 6

#define ROW(sq) ((sq) >> 3)
#define COL(sq) ((sq) & 7)

typedef struct {
    int from;
    int to;
    int promo;
} Move;

typedef struct {
    int board[64];
    int turn;       // 1 for White, -1 for Black
    int castling;   // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep_square;  // -1 if none, otherwise destination square index
    int halfmove;
    int fullmove;
} BoardState;

typedef enum { MODE_VS_WHITE, MODE_VS_BLACK, MODE_PASS_AND_PLAY, MODE_ENGINE_VS_ENGINE } GameMode;
typedef enum { TC_DEPTH, TC_NODES, TC_TIME } TCType;

typedef struct {
    TCType type;
    int depth;
    int nodes;
    int time_ms;
} TimeControl;

// Game State Registers
BoardState board_state;
BoardState history_states[MAX_MOVES];
Move history_moves[MAX_MOVES];
char history_san[MAX_MOVES][16];
int history_count = 0;

Move last_move = {-1, -1, 0};
int last_move_valid = 0;
int cursor_sq = 44; // Starts on e3
int selected_sq = -1;

Move legal_moves[256];
int legal_moves_count = 0;

// Engine Pipe variables
int engine_in[2], engine_out[2];
pid_t engine_pid = -1;
int engine_active = 0;

GameMode game_mode = MODE_VS_WHITE;
TimeControl tc = { TC_DEPTH, 10, 100000, 2000 };

// Raw terminal handler
struct termios orig_termios;

// Unicode Solid Pieces (Differentiated via high contrast ANSI coloring)
const char *glyphs[] = {" ", "♟", "♞", "♝", "♜", "♛", "♚"};

// Forward Declarations
int generate_legal_moves(const BoardState *s, Move *legal_moves);
int is_in_check(const BoardState *s, int color);
int is_square_attacked(const BoardState *s, int sq, int attacker_color);
void make_move(const BoardState *src, BoardState *dst, Move m);

void disable_raw_mode() {
    printf("\e[?25h"); // Show cursor
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\e[?25l"); // Hide cursor
}

void init_board(BoardState *s) {
    memset(s, 0, sizeof(BoardState));
    int back_row[8] = {R, N, B, Q, K, B, N, R};
    for (int i = 0; i < 8; i++) {
        s->board[0 * 8 + i] = -back_row[i];
        s->board[1 * 8 + i] = -P;
        s->board[6 * 8 + i] = P;
        s->board[7 * 8 + i] = back_row[i];
    }
    s->turn = 1;
    s->castling = 15;
    s->ep_square = -1;
    s->halfmove = 0;
    s->fullmove = 1;
}

int is_valid_sq(int r, int c) {
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}

void generate_pseudo_moves(const BoardState *s, Move *moves, int *count) {
    *count = 0;
    int turn = s->turn;
    for (int sq = 0; sq < 64; sq++) {
        int p = s->board[sq];
        if (p == EMPTY) continue;
        int color = p > 0 ? 1 : -1;
        if (color != turn) continue;

        int abs_p = p > 0 ? p : -p;
        int r = ROW(sq);
        int c = COL(sq);

        if (abs_p == P) {
            int dir = (turn == 1) ? -1 : 1;
            int start_row = (turn == 1) ? 6 : 1;
            int promo_row = (turn == 1) ? 0 : 7;

            int nr = r + dir;
            if (nr >= 0 && nr < 8 && s->board[nr * 8 + c] == EMPTY) {
                if (nr == promo_row) {
                    moves[(*count)++] = (Move){sq, nr * 8 + c, Q};
                    moves[(*count)++] = (Move){sq, nr * 8 + c, R};
                    moves[(*count)++] = (Move){sq, nr * 8 + c, B};
                    moves[(*count)++] = (Move){sq, nr * 8 + c, N};
                } else {
                    moves[(*count)++] = (Move){sq, nr * 8 + c, 0};
                }
                int nnr = r + 2 * dir;
                if (r == start_row && s->board[nnr * 8 + c] == EMPTY) {
                    moves[(*count)++] = (Move){sq, nnr * 8 + c, 0};
                }
            }

            int dc_opts[2] = {-1, 1};
            for (int i = 0; i < 2; i++) {
                int nc = c + dc_opts[i];
                if (nc >= 0 && nc < 8) {
                    int nsq = nr * 8 + nc;
                    int target = s->board[nsq];
                    if (target != EMPTY && (target > 0 ? 1 : -1) == -turn) {
                        if (nr == promo_row) {
                            moves[(*count)++] = (Move){sq, nsq, Q};
                            moves[(*count)++] = (Move){sq, nsq, R};
                            moves[(*count)++] = (Move){sq, nsq, B};
                            moves[(*count)++] = (Move){sq, nsq, N};
                        } else {
                            moves[(*count)++] = (Move){sq, nsq, 0};
                        }
                    }
                    if (nsq == s->ep_square) {
                        moves[(*count)++] = (Move){sq, nsq, 0};
                    }
                }
            }
        } else if (abs_p == N) {
            int dr[8] = {-2, -2, -1, -1, 1, 1, 2, 2};
            int dc[8] = {-1, 1, -2, 2, -2, 2, -1, 1};
            for (int i = 0; i < 8; i++) {
                int nr = r + dr[i];
                int nc = c + dc[i];
                if (is_valid_sq(nr, nc)) {
                    int nsq = nr * 8 + nc;
                    int target = s->board[nsq];
                    if (target == EMPTY || (target > 0 ? 1 : -1) == -turn) {
                        moves[(*count)++] = (Move){sq, nsq, 0};
                    }
                }
            }
        } else if (abs_p == B || abs_p == R || abs_p == Q) {
            int start_dir = (abs_p == R) ? 4 : 0;
            int end_dir = (abs_p == B) ? 4 : 8;
            int dr[8] = {-1, -1, 1, 1, -1, 1, 0, 0};
            int dc[8] = {-1, 1, -1, 1, 0, 0, -1, 1};

            for (int d = start_dir; d < end_dir; d++) {
                int nr = r, nc = c;
                while (1) {
                    nr += dr[d];
                    nc += dc[d];
                    if (!is_valid_sq(nr, nc)) break;
                    int nsq = nr * 8 + nc;
                    int target = s->board[nsq];
                    if (target == EMPTY) {
                        moves[(*count)++] = (Move){sq, nsq, 0};
                    } else {
                        if ((target > 0 ? 1 : -1) == -turn) {
                            moves[(*count)++] = (Move){sq, nsq, 0};
                        }
                        break;
                    }
                }
            }
        } else if (abs_p == K) {
            int dr[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
            int dc[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
            for (int i = 0; i < 8; i++) {
                int nr = r + dr[i];
                int nc = c + dc[i];
                if (is_valid_sq(nr, nc)) {
                    int nsq = nr * 8 + nc;
                    int target = s->board[nsq];
                    if (target == EMPTY || (target > 0 ? 1 : -1) == -turn) {
                        moves[(*count)++] = (Move){sq, nsq, 0};
                    }
                }
            }
            if (turn == 1) {
                if ((s->castling & 1) && s->board[61] == EMPTY && s->board[62] == EMPTY) {
                    if (!is_square_attacked(s, 60, -1) && !is_square_attacked(s, 61, -1) && !is_square_attacked(s, 62, -1)) {
                        moves[(*count)++] = (Move){60, 62, 0};
                    }
                }
                if ((s->castling & 2) && s->board[59] == EMPTY && s->board[58] == EMPTY && s->board[57] == EMPTY) {
                    if (!is_square_attacked(s, 60, -1) && !is_square_attacked(s, 59, -1) && !is_square_attacked(s, 58, -1)) {
                        moves[(*count)++] = (Move){60, 58, 0};
                    }
                }
            } else {
                if ((s->castling & 4) && s->board[5] == EMPTY && s->board[6] == EMPTY) {
                    if (!is_square_attacked(s, 4, 1) && !is_square_attacked(s, 5, 1) && !is_square_attacked(s, 6, 1)) {
                        moves[(*count)++] = (Move){4, 6, 0};
                    }
                }
                if ((s->castling & 8) && s->board[3] == EMPTY && s->board[2] == EMPTY && s->board[1] == EMPTY) {
                    if (!is_square_attacked(s, 4, 1) && !is_square_attacked(s, 3, 1) && !is_square_attacked(s, 2, 1)) {
                        moves[(*count)++] = (Move){4, 2, 0};
                    }
                }
            }
        }
    }
}

int is_square_attacked(const BoardState *s, int sq, int attacker_color) {
    int r = ROW(sq);
    int c = COL(sq);

    int k_dr[8] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int k_dc[8] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + k_dr[i];
        int nc = c + k_dc[i];
        if (is_valid_sq(nr, nc)) {
            if (s->board[nr * 8 + nc] == attacker_color * N) return 1;
        }
    }

    int king_dr[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int king_dc[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + king_dr[i];
        int nc = c + king_dc[i];
        if (is_valid_sq(nr, nc)) {
            if (s->board[nr * 8 + nc] == attacker_color * K) return 1;
        }
    }

    int p_dir = (attacker_color == 1) ? 1 : -1;
    int p_dr = r + p_dir;
    int p_dc_opts[2] = {-1, 1};
    for (int i = 0; i < 2; i++) {
        int p_dc = c + p_dc_opts[i];
        if (is_valid_sq(p_dr, p_dc)) {
            if (s->board[p_dr * 8 + p_dc] == attacker_color * P) return 1;
        }
    }

    int b_dr[4] = {-1, -1, 1, 1};
    int b_dc[4] = {-1, 1, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += b_dr[i];
            nc += b_dc[i];
            if (!is_valid_sq(nr, nc)) break;
            int p = s->board[nr * 8 + nc];
            if (p != EMPTY) {
                if (p == attacker_color * B || p == attacker_color * Q) return 1;
                break;
            }
        }
    }

    int r_dr[4] = {-1, 1, 0, 0};
    int r_dc[4] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += r_dr[i];
            nc += r_dc[i];
            if (!is_valid_sq(nr, nc)) break;
            int p = s->board[nr * 8 + nc];
            if (p != EMPTY) {
                if (p == attacker_color * R || p == attacker_color * Q) return 1;
                break;
            }
        }
    }
    return 0;
}

int get_king_square(const BoardState *s, int color) {
    for (int i = 0; i < 64; i++) {
        if (s->board[i] == color * K) return i;
    }
    return -1;
}

int is_in_check(const BoardState *s, int color) {
    int ksq = get_king_square(s, color);
    if (ksq == -1) return 0;
    return is_square_attacked(s, ksq, -color);
}

int generate_legal_moves(const BoardState *s, Move *leg_mvs) {
    Move pseudo[256];
    int p_count = 0;
    generate_pseudo_moves(s, pseudo, &p_count);
    int l_count = 0;
    for (int i = 0; i < p_count; i++) {
        BoardState next;
        make_move(s, &next, pseudo[i]);
        if (!is_in_check(&next, s->turn)) {
            leg_mvs[l_count++] = pseudo[i];
        }
    }
    return l_count;
}

void make_move(const BoardState *src, BoardState *dst, Move m) {
    memcpy(dst, src, sizeof(BoardState));
    dst->ep_square = -1;

    int p = src->board[m.from];
    int turn = src->turn;

    dst->board[m.to] = m.promo ? turn * m.promo : p;
    dst->board[m.from] = EMPTY;

    int abs_p = p > 0 ? p : -p;
    if (abs_p == P) {
        if (abs(ROW(m.from) - ROW(m.to)) == 2) {
            dst->ep_square = (m.from + m.to) / 2;
        }
        if (m.to == src->ep_square) {
            dst->board[m.to + (turn == 1 ? 8 : -8)] = EMPTY;
        }
        dst->halfmove = 0;
    } else if (src->board[m.to] != EMPTY) {
        dst->halfmove = 0;
    } else {
        dst->halfmove++;
    }

    if (abs_p == K) {
        if (turn == 1) {
            dst->castling &= ~3;
            if (m.from == 60) {
                if (m.to == 62) { dst->board[61] = R; dst->board[63] = EMPTY; }
                else if (m.to == 58) { dst->board[59] = R; dst->board[56] = EMPTY; }
            }
        } else {
            dst->castling &= ~12;
            if (m.from == 4) {
                if (m.to == 6) { dst->board[5] = -R; dst->board[7] = EMPTY; }
                else if (m.to == 2) { dst->board[3] = -R; dst->board[0] = EMPTY; }
            }
        }
    }

    if (m.from == 63) dst->castling &= ~1;
    if (m.from == 56) dst->castling &= ~2;
    if (m.from == 7)  dst->castling &= ~4;
    if (m.from == 0)  dst->castling &= ~8;

    if (m.to == 63) dst->castling &= ~1;
    if (m.to == 56) dst->castling &= ~2;
    if (m.to == 7)  dst->castling &= ~4;
    if (m.to == 0)  dst->castling &= ~8;

    if (turn == -1) dst->fullmove++;
    dst->turn = -turn;
}

void get_san(const BoardState *state, Move m, char *san_buf) {
    BoardState next;
    make_move(state, &next, m);

    int p = state->board[m.from];
    int abs_p = p > 0 ? p : -p;

    if (abs_p == K && abs(COL(m.from) - COL(m.to)) > 1) {
        if (COL(m.to) == 6) strcpy(san_buf, "O-O");
        else strcpy(san_buf, "O-O-O");
    } else {
        char piece_char[2] = "";
        if (abs_p == N) strcpy(piece_char, "N");
        else if (abs_p == B) strcpy(piece_char, "B");
        else if (abs_p == R) strcpy(piece_char, "R");
        else if (abs_p == Q) strcpy(piece_char, "Q");
        else if (abs_p == K) strcpy(piece_char, "K");

        int is_capture = (state->board[m.to] != EMPTY) || (abs_p == P && m.to == state->ep_square);

        char disambiguation[3] = "";
        if (abs_p != P && abs_p != K) {
            Move leg[256];
            int leg_count = generate_legal_moves(state, leg);
            int conflict = 0, same_file = 0, same_rank = 0;
            for (int i = 0; i < leg_count; i++) {
                if (leg[i].from != m.from && leg[i].to == m.to && state->board[leg[i].from] == p) {
                    conflict = 1;
                    if (COL(leg[i].from) == COL(m.from)) same_file = 1;
                    if (ROW(leg[i].from) == ROW(m.from)) same_rank = 1;
                }
            }
            if (conflict) {
                if (!same_file) {
                    disambiguation[0] = 'a' + COL(m.from);
                    disambiguation[1] = '\0';
                } else if (!same_rank) {
                    disambiguation[0] = '8' - ROW(m.from);
                    disambiguation[1] = '\0';
                } else {
                    disambiguation[0] = 'a' + COL(m.from);
                    disambiguation[1] = '8' - ROW(m.from);
                    disambiguation[2] = '\0';
                }
            }
        } else if (abs_p == P && is_capture) {
            disambiguation[0] = 'a' + COL(m.from);
            disambiguation[1] = '\0';
        }

        char cap_char[2] = "";
        if (is_capture) strcpy(cap_char, "x");

        char dest[3];
        dest[0] = 'a' + COL(m.to);
        dest[1] = '8' - ROW(m.to);
        dest[2] = '\0';

        char promo_char[4] = "";
        if (m.promo) {
            sprintf(promo_char, "=%c", (m.promo == N) ? 'N' : (m.promo == B) ? 'B' : (m.promo == R) ? 'R' : 'Q');
        }

        sprintf(san_buf, "%s%s%s%s%s", piece_char, disambiguation, cap_char, dest, promo_char);
    }

    Move opp_leg[256];
    int opp_leg_cnt = generate_legal_moves(&next, opp_leg);
    if (is_in_check(&next, next.turn)) {
        if (opp_leg_cnt == 0) strcat(san_buf, "#");
        else strcat(san_buf, "+");
    }
}

void move_to_str(Move m, char *buf) {
    buf[0] = 'a' + COL(m.from);
    buf[1] = '8' - ROW(m.from);
    buf[2] = 'a' + COL(m.to);
    buf[3] = '8' - ROW(m.to);
    if (m.promo) {
        if (m.promo == N) buf[4] = 'n';
        else if (m.promo == B) buf[4] = 'b';
        else if (m.promo == R) buf[4] = 'r';
        else buf[4] = 'q';
        buf[5] = '\0';
    } else {
        buf[4] = '\0';
    }
}

void start_engine(const char *path) {
    if (pipe(engine_in) < 0 || pipe(engine_out) < 0) return;
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(engine_in[0], STDIN_FILENO);
        dup2(engine_out[1], STDOUT_FILENO);
        close(engine_in[1]);
        close(engine_out[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDERR_FILENO);
        }
        execlp(path, path, NULL);
        exit(1);
    } else if (engine_pid > 0) {
        close(engine_in[0]);
        close(engine_out[1]);
        fcntl(engine_out[0], F_SETFL, O_NONBLOCK);
        engine_active = 1;
        write(engine_in[1], "uci\n", 4);
        write(engine_in[1], "isready\n", 8);
    }
}

Move get_engine_move(const char *go_command) {
    Move empty_m = {0,0,0};
    if (!engine_active) return empty_m;

    char cmd[16384];
    strcpy(cmd, "position startpos moves");
    for (int i = 0; i < history_count; i++) {
        char mstr[8];
        move_to_str(history_moves[i], mstr);
        strcat(cmd, " ");
        strcat(cmd, mstr);
    }
    strcat(cmd, "\n");
    write(engine_in[1], cmd, strlen(cmd));

    char go[256];
    sprintf(go, "%s\n", go_command);
    write(engine_in[1], go, strlen(go));

    char line[1024];
    int line_ptr = 0;
    while (1) {
        char c;
        int n = read(engine_out[0], &c, 1);
        if (n <= 0) {
            usleep(1000);
            continue;
        }
        if (c == '\n' || line_ptr >= sizeof(line) - 2) {
            line[line_ptr] = '\0';
            line_ptr = 0;
            if (strncmp(line, "bestmove", 8) == 0) {
                char bmove[32];
                sscanf(line, "bestmove %s", bmove);
                if (strcmp(bmove, "(none)") == 0) return empty_m;
                Move engine_m;
                engine_m.from = (bmove[0] - 'a') + (8 - (bmove[1] - '0')) * 8;
                engine_m.to = (bmove[2] - 'a') + (8 - (bmove[3] - '0')) * 8;
                engine_m.promo = 0;
                if (strlen(bmove) > 4) {
                    char p = bmove[4];
                    if (p == 'q') engine_m.promo = Q;
                    else if (p == 'r') engine_m.promo = R;
                    else if (p == 'b') engine_m.promo = B;
                    else if (p == 'n') engine_m.promo = N;
                }
                return engine_m;
            }
        } else {
            line[line_ptr++] = c;
        }
    }
    return empty_m;
}

int is_legal_dest(int sq) {
    if (selected_sq == -1) return 0;
    for (int i = 0; i < legal_moves_count; i++) {
        if (legal_moves[i].from == selected_sq && legal_moves[i].to == sq) {
            return 1;
        }
    }
    return 0;
}

void undo_move() {
    if (history_count > 0) {
        history_count--;
        board_state = history_states[history_count];
        if (history_count > 0) {
            last_move = history_moves[history_count - 1];
            last_move_valid = 1;
        } else {
            last_move_valid = 0;
        }
        selected_sq = -1;
    }
}

void move_cursor(int dr, int dc) {
    int r = ROW(cursor_sq) + dr;
    int c = COL(cursor_sq) + dc;
    if (is_valid_sq(r, c)) {
        cursor_sq = r * 8 + c;
    }
}

void render_all() {
    char buf[65536];
    int ptr = 0;

    // Move cursor back to home (Top-left)
    ptr += sprintf(buf + ptr, "\e[H");

    ptr += sprintf(buf + ptr, "\e[1;36m  ♛   TERMINAL CHESS GUI (macOS/POSIX)   ♞\e[0m\n");
    ptr += sprintf(buf + ptr, "  =======================================================\n\n");

    for (int r = 0; r < 8; r++) {
        ptr += sprintf(buf + ptr, "  %d ", 8 - r);

        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;

            // Palette Definition
            int bg = ((r + c) % 2 == 0) ? 180 : 94; // Standard Tan / Dark Wood brown
            if (last_move_valid && (sq == last_move.from || sq == last_move.to)) {
                bg = 69; // Previous Move: Blue
            }
            if (sq == selected_sq) {
                bg = 117; // Selected Square: Pastel Cyan
            }
            if (is_legal_dest(sq)) {
                bg = 114; // Legal Moves: Green Shading
            }
            if (board_state.board[sq] == board_state.turn * K && is_in_check(&board_state, board_state.turn)) {
                bg = 167; // Active King in Check: Bright Red
            }
            if (sq == cursor_sq) {
                bg = 220; // Keyboard Selection Pointer: Gold
            }

            int p = board_state.board[sq];
            int fg = 0;
            const char *glyph = " ";
            if (p != EMPTY) {
                int abs_p = p > 0 ? p : -p;
                glyph = glyphs[abs_p];
                fg = (p > 0) ? 231 : 16; // Pure White pieces vs Deep Black pieces
            }

            if (p != EMPTY) {
                ptr += sprintf(buf + ptr, "\e[48;5;%dm\e[38;5;%dm  %s \e[0m", bg, fg, glyph);
            } else {
                ptr += sprintf(buf + ptr, "\e[48;5;%dm    \e[0m", bg);
            }
        }

        // Side Panels
        ptr += sprintf(buf + ptr, "   │ ");
        switch (r) {
            case 0:
                ptr += sprintf(buf + ptr, "\e[1mGAME MODE:\e[0m ");
                if (game_mode == MODE_VS_WHITE) ptr += sprintf(buf + ptr, "Play as White vs AI");
                else if (game_mode == MODE_VS_BLACK) ptr += sprintf(buf + ptr, "Play as Black vs AI");
                else if (game_mode == MODE_PASS_AND_PLAY) ptr += sprintf(buf + ptr, "Pass & Play (Local)");
                else ptr += sprintf(buf + ptr, "AI vs AI Auto-Play");
                break;
            case 1:
                ptr += sprintf(buf + ptr, "\e[1mENGINE:\e[0m ");
                if (engine_active) {
                    ptr += sprintf(buf + ptr, "\e[1;32mConnected (Stockfish)\e[0m");
                } else {
                    ptr += sprintf(buf + ptr, "\e[1;31mUnavailable (Pass & Play Only)\e[0m");
                }
                break;
            case 2:
                ptr += sprintf(buf + ptr, "\e[1mTIME CONTROL:\e[0m ");
                if (tc.type == TC_DEPTH) ptr += sprintf(buf + ptr, "Engine Search Depth: \e[1;35m%d moves\e[0m", tc.depth);
                else if (tc.type == TC_NODES) ptr += sprintf(buf + ptr, "Nodes Limit: \e[1;35m%d\e[0m", tc.nodes);
                else ptr += sprintf(buf + ptr, "Engine Allocation: \e[1;35m%d ms\e[0m", tc.time_ms);
                break;
            case 3:
                ptr += sprintf(buf + ptr, "\e[1mACTIVE TURN:\e[0m %s", (board_state.turn == 1) ? "\e[1;231mWHITE\e[0m" : "\e[1;30mBLACK\e[0m");
                break;
            case 4: {
                ptr += sprintf(buf + ptr, "\e[1mPGN HIST:\e[0m ");
                int start_move = (history_count > 6) ? history_count - 6 : 0;
                if (start_move % 2 != 0) start_move--;
                if (start_move < 0) start_move = 0;
                for (int i = start_move; i < history_count; i++) {
                    if (i % 2 == 0) {
                        ptr += sprintf(buf + ptr, "%d. %s ", (i / 2) + 1, history_san[i]);
                    } else {
                        ptr += sprintf(buf + ptr, "%s  ", history_san[i]);
                    }
                }
                break;
            }
            case 5:
                ptr += sprintf(buf + ptr, "\e[38;5;244m[W,A,S,D / Arrows] Move Pointer | [Space/Enter] Interact\e[0m");
                break;
            case 6:
                ptr += sprintf(buf + ptr, "\e[38;5;244m[U] Undo | [M] Swap Mode | [T] Cycle Time Config\e[0m");
                break;
            case 7:
                ptr += sprintf(buf + ptr, "\e[38;5;244m[+/-] Adjust Variable Limits | [ESC/Q] Quit Game\e[0m");
                break;
        }
        ptr += sprintf(buf + ptr, "\n");
    }
    ptr += sprintf(buf + ptr, "     a   b   c   d   e   f   g   h\n\n");
    write(STDOUT_FILENO, buf, ptr);
}

int main() {
    start_engine("stockfish");
    init_board(&board_state);
    enable_raw_mode();

    // Clear terminal screen completely before starting the GUI loop
    printf("\e[2J");

    while (1) {
        render_all();

        // Handle AI moves automatically when active
        int is_engine_turn = 0;
        if (engine_active) {
            if (game_mode == MODE_VS_WHITE && board_state.turn == -1) is_engine_turn = 1;
            else if (game_mode == MODE_VS_BLACK && board_state.turn == 1) is_engine_turn = 1;
            else if (game_mode == MODE_ENGINE_VS_ENGINE) is_engine_turn = 1;
        }

        if (is_engine_turn) {
            Move leg[256];
            int leg_cnt = generate_legal_moves(&board_state, leg);
            if (leg_cnt > 0) {
                char go_cmd[128];
                if (tc.type == TC_DEPTH) sprintf(go_cmd, "go depth %d", tc.depth);
                else if (tc.type == TC_NODES) sprintf(go_cmd, "go nodes %d", tc.nodes);
                else sprintf(go_cmd, "go movetime %d", tc.time_ms);

                Move m = get_engine_move(go_cmd);
                if (m.from != m.to) {
                    history_states[history_count] = board_state;
                    get_san(&board_state, m, history_san[history_count]);
                    history_moves[history_count] = m;
                    history_count++;

                    make_move(&board_state, &board_state, m);
                    last_move = m;
                    last_move_valid = 1;
                }
            }
            continue;
        }

        // Reading terminal inputs non-blockingly
        char c;
        int n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            usleep(10000);
            continue;
        }

        if (c == 'q' || c == '\x03') {
            break;
        }

        if (c == '\x1b') {
            char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                if (seq[0] == '[') {
                    switch (seq[1]) {
                        case 'A': move_cursor(-1, 0); break; // Up
                        case 'B': move_cursor(1, 0); break;  // Down
                        case 'C': move_cursor(0, 1); break;  // Right
                        case 'D': move_cursor(0, -1); break; // Left
                    }
                }
            } else {
                selected_sq = -1; // Escape clears current state selections
            }
        } else if (c == 'w' || c == 'W') {
            move_cursor(-1, 0);
        } else if (c == 's' || c == 'S') {
            move_cursor(1, 0);
        } else if (c == 'a' || c == 'A') {
            move_cursor(0, -1);
        } else if (c == 'd' || c == 'D') {
            move_cursor(0, 1);
        } else if (c == ' ' || c == '\r' || c == '\n') {
            if (selected_sq == -1) {
                int p = board_state.board[cursor_sq];
                if (p != EMPTY && (p > 0 ? 1 : -1) == board_state.turn) {
                    selected_sq = cursor_sq;
                    legal_moves_count = generate_legal_moves(&board_state, legal_moves);
                }
            } else {
                if (cursor_sq == selected_sq) {
                    selected_sq = -1;
                } else if (is_legal_dest(cursor_sq)) {
                    Move actual_m = {0,0,0};
                    for (int i = 0; i < legal_moves_count; i++) {
                        if (legal_moves[i].from == selected_sq && legal_moves[i].to == cursor_sq) {
                            actual_m = legal_moves[i];
                            break;
                        }
                    }
                    history_states[history_count] = board_state;
                    get_san(&board_state, actual_m, history_san[history_count]);
                    history_moves[history_count] = actual_m;
                    history_count++;

                    make_move(&board_state, &board_state, actual_m);
                    last_move = actual_m;
                    last_move_valid = 1;
                    selected_sq = -1;
                } else {
                    int p = board_state.board[cursor_sq];
                    if (p != EMPTY && (p > 0 ? 1 : -1) == board_state.turn) {
                        selected_sq = cursor_sq;
                        legal_moves_count = generate_legal_moves(&board_state, legal_moves);
                    } else {
                        selected_sq = -1;
                    }
                }
            }
        } else if (c == 'u' || c == 'U') {
            if (game_mode == MODE_PASS_AND_PLAY) {
                undo_move();
            } else {
                undo_move(); // Undo engine response
                undo_move(); // Undo human turn
            }
        } else if (c == 'm' || c == 'M') {
            game_mode = (game_mode + 1) % 4;
            selected_sq = -1;
        } else if (c == 't' || c == 'T') {
            tc.type = (tc.type + 1) % 3;
        } else if (c == '+' || c == '=') {
            if (tc.type == TC_DEPTH) { if (tc.depth < 30) tc.depth++; }
            else if (tc.type == TC_NODES) tc.nodes += 25000;
            else tc.time_ms += 500;
        } else if (c == '-') {
            if (tc.type == TC_DEPTH) { if (tc.depth > 1) tc.depth--; }
            else if (tc.type == TC_NODES) { if (tc.nodes > 25000) tc.nodes -= 25000; }
            else { if (tc.time_ms > 500) tc.time_ms -= 500; }
        }
    }

    disable_raw_mode();
    //if (engine_pid > 0) {
        //kill(engine_pid, SIGTERM);
    //}
    return 0;
}
