#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/select.h>
#include <fcntl.h>
#include <signal.h>

/* Chess engine & rules definitions */
#define EMPTY 0
#define WHITE 8
#define BLACK 16
#define COLOR_MASK (WHITE | BLACK)
#define PIECE_MASK 7

#define PAWN 1
#define KNIGHT 2
#define BISHOP 3
#define ROOK 4
#define QUEEN 5
#define KING 6

#define KEY_UP 1000
#define KEY_DOWN 1001
#define KEY_LEFT 1002
#define KEY_RIGHT 1003

#define IN_BOUNDS(r, c) ((r) >= 0 && (r) < 8 && (c) >= 0 && (c) < 8)

typedef struct {
    int from;
    int to;
    int promotion;
} Move;

typedef struct {
    int board[64];
    int turn;       // 0 = White, 1 = Black
    int castling;   // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep;         // En passant target square (-1 if none)
    Move last_move;
} BoardState;

/* Global state variables */
typedef enum { TC_DEPTH, TC_NODES, TC_TIME } TCType;
TCType tc_type = TC_DEPTH;
int tc_val = 10; // Default: Depth 10

int engine_mode = 1; // 0 = PvP, 1 = PvE (Engine is Black), 2 = EvP (Engine is White), 3 = EvE

int engine_in[2];
int engine_out[2];
pid_t engine_pid = -1;
int engine_connected = 0;

char engine_buf[4096];
int engine_buf_len = 0;

char san_history[2048][16];

struct termios orig_termios;

/* Terminal utilities */
void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void prompt_input(const char *prompt, char *buffer, int max_len) {
    disableRawMode();
    printf("\033[?25h\n%s", prompt);
    fflush(stdout);
    if (fgets(buffer, max_len, stdin)) {
        buffer[strcspn(buffer, "\n")] = 0;
    }
    printf("\033[?25l");
    enableRawMode();
}

/* Engine process management */
void start_engine(const char *path) {
    if (pipe(engine_in) < 0 || pipe(engine_out) < 0) return;
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(engine_in[0], STDIN_FILENO);
        dup2(engine_out[1], STDOUT_FILENO);
        close(engine_in[1]);
        close(engine_out[0]);
        execlp(path, path, NULL);
        exit(1);
    }
    close(engine_in[0]);
    close(engine_out[1]);

    int flags = fcntl(engine_out[0], F_GETFL, 0);
    fcntl(engine_out[0], F_SETFL, flags | O_NONBLOCK);

    engine_connected = 1;
    write(engine_in[1], "uci\n", 4);
    write(engine_in[1], "isready\n", 8);
}

void move_to_uci(Move m, char *buf) {
    char files[] = "abcdefgh";
    char ranks[] = "87654321";
    int f1 = m.from % 8, r1 = m.from / 8;
    int f2 = m.to % 8, r2 = m.to / 8;
    if (m.promotion == QUEEN) sprintf(buf, "%c%c%c%cq", files[f1], ranks[r1], files[f2], ranks[r2]);
    else if (m.promotion == ROOK) sprintf(buf, "%c%c%c%cr", files[f1], ranks[r1], files[f2], ranks[r2]);
    else if (m.promotion == BISHOP) sprintf(buf, "%c%c%c%cb", files[f1], ranks[r1], files[f2], ranks[r2]);
    else if (m.promotion == KNIGHT) sprintf(buf, "%c%c%c%cn", files[f1], ranks[r1], files[f2], ranks[r2]);
    else sprintf(buf, "%c%c%c%c", files[f1], ranks[r1], files[f2], ranks[r2]);
}

void send_position_and_go(BoardState *history, int count) {
    if (!engine_connected) return;
    char buf[16384];
    int len = sprintf(buf, "position startpos moves");
    for (int i = 1; i < count; i++) {
        char mv_str[10];
        move_to_uci(history[i].last_move, mv_str);
        len += sprintf(buf + len, " %s", mv_str);
    }
    len += sprintf(buf + len, "\n");
    write(engine_in[1], buf, len);

    if (tc_type == TC_DEPTH) len = sprintf(buf, "go depth %d\n", tc_val);
    else if (tc_type == TC_NODES) len = sprintf(buf, "go nodes %d\n", tc_val);
    else if (tc_type == TC_TIME) len = sprintf(buf, "go movetime %d\n", tc_val);
    write(engine_in[1], buf, len);
}

/* Chess rules engine */
void init_board(BoardState *state) {
    for (int i = 0; i < 64; i++) state->board[i] = EMPTY;
    state->board[0] = ROOK | BLACK; state->board[7] = ROOK | BLACK;
    state->board[56] = ROOK | WHITE; state->board[63] = ROOK | WHITE;
    state->board[1] = KNIGHT | BLACK; state->board[6] = KNIGHT | BLACK;
    state->board[57] = KNIGHT | WHITE; state->board[62] = KNIGHT | WHITE;
    state->board[2] = BISHOP | BLACK; state->board[5] = BISHOP | BLACK;
    state->board[58] = BISHOP | WHITE; state->board[61] = BISHOP | WHITE;
    state->board[3] = QUEEN | BLACK; state->board[59] = QUEEN | WHITE;
    state->board[4] = KING | BLACK; state->board[60] = KING | WHITE;
    for (int i = 8; i < 16; i++) state->board[i] = PAWN | BLACK;
    for (int i = 48; i < 56; i++) state->board[i] = PAWN | WHITE;

    state->turn = 0;
    state->castling = 1 | 2 | 4 | 8;
    state->ep = -1;
    state->last_move = (Move){-1, -1, 0};
}

int is_square_attacked(const BoardState *state, int sq, int attacker_color) {
    int r = sq / 8, c = sq % 8;
    int opp_pawn = PAWN | attacker_color;
    int opp_knight = KNIGHT | attacker_color;
    int opp_bishop = BISHOP | attacker_color;
    int opp_rook = ROOK | attacker_color;
    int opp_queen = QUEEN | attacker_color;
    int opp_king = KING | attacker_color;

    // Knight attacks
    int kn_dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int kn_dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn_dr[i], nc = c + kn_dc[i];
        if (IN_BOUNDS(nr, nc)) {
            if (state->board[nr * 8 + nc] == opp_knight) return 1;
        }
    }

    // Sliding attacks (Rooks and Queens)
    int r_dr[] = {-1, 1, 0, 0};
    int r_dc[] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += r_dr[i]; nc += r_dc[i];
            if (!IN_BOUNDS(nr, nc)) break;
            int p = state->board[nr * 8 + nc];
            if (p != EMPTY) {
                if (p == opp_rook || p == opp_queen) return 1;
                break;
            }
        }
    }

    // Sliding attacks (Bishops and Queens)
    int b_dr[] = {-1, -1, 1, 1};
    int b_dc[] = {-1, 1, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += b_dr[i]; nc += b_dc[i];
            if (!IN_BOUNDS(nr, nc)) break;
            int p = state->board[nr * 8 + nc];
            if (p != EMPTY) {
                if (p == opp_bishop || p == opp_queen) return 1;
                break;
            }
        }
    }

    // Pawn attacks
    if (attacker_color == WHITE) {
        if (r < 7) {
            if (c > 0 && state->board[(r + 1) * 8 + (c - 1)] == opp_pawn) return 1;
            if (c < 7 && state->board[(r + 1) * 8 + (c + 1)] == opp_pawn) return 1;
        }
    } else {
        if (r > 0) {
            if (c > 0 && state->board[(r - 1) * 8 + (c - 1)] == opp_pawn) return 1;
            if (c < 7 && state->board[(r - 1) * 8 + (c + 1)] == opp_pawn) return 1;
        }
    }

    // King attacks
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = r + dr, nc = c + dc;
            if (IN_BOUNDS(nr, nc)) {
                if (state->board[nr * 8 + nc] == opp_king) return 1;
            }
        }
    }
    return 0;
}

void make_move(const BoardState *prev, BoardState *next, Move m) {
    *next = *prev;
    int p = next->board[m.from];
    int color = p & COLOR_MASK;
    next->board[m.from] = EMPTY;

    // Handle En Passant capture
    if ((p & PIECE_MASK) == PAWN && m.to == prev->ep) {
        next->board[m.to + (color == WHITE ? 8 : -8)] = EMPTY;
    }

    next->board[m.to] = p;

    // Handle Pawn promotion
    if ((p & PIECE_MASK) == PAWN && m.promotion != 0) {
        next->board[m.to] = m.promotion | color;
    }

    // Handle En Passant registration
    if ((p & PIECE_MASK) == PAWN && abs(m.from - m.to) == 16) {
        next->ep = (m.from + m.to) / 2;
    } else {
        next->ep = -1;
    }

    // Handle castling rook moves
    if ((p & PIECE_MASK) == KING) {
        if (m.from == 60 && m.to == 62) { next->board[63] = EMPTY; next->board[61] = ROOK | WHITE; }
        else if (m.from == 60 && m.to == 58) { next->board[56] = EMPTY; next->board[59] = ROOK | WHITE; }
        else if (m.from == 4 && m.to == 6) { next->board[7] = EMPTY; next->board[5] = ROOK | BLACK; }
        else if (m.from == 4 && m.to == 2) { next->board[0] = EMPTY; next->board[3] = ROOK | BLACK; }

        if (color == WHITE) next->castling &= ~3;
        else next->castling &= ~12;
    }

    // Revoke castling rights upon rook moves/captures
    if (m.from == 56 || m.to == 56) next->castling &= ~2;
    if (m.from == 63 || m.to == 63) next->castling &= ~1;
    if (m.from == 0 || m.to == 0) next->castling &= ~8;
    if (m.from == 7 || m.to == 7) next->castling &= ~4;

    next->turn = 1 - prev->turn;
}

int generate_legal_moves(const BoardState *state, Move *moves) {
    Move pseudo[256];
    int p_count = 0;
    int side = (state->turn == 0) ? WHITE : BLACK;
    int opp_side = (state->turn == 0) ? BLACK : WHITE;

    for (int sq = 0; sq < 64; sq++) {
        int p = state->board[sq];
        if (p == EMPTY || (p & COLOR_MASK) != side) continue;
        int type = p & PIECE_MASK;
        int r = sq / 8, c = sq % 8;

        if (type == PAWN) {
            int dir = (side == WHITE) ? -8 : 8;
            int target = sq + dir;
            if (target >= 0 && target < 64 && state->board[target] == EMPTY) {
                if (target / 8 == 0 || target / 8 == 7) {
                    moves[p_count++] = (Move){sq, target, QUEEN};
                    moves[p_count++] = (Move){sq, target, ROOK};
                    moves[p_count++] = (Move){sq, target, BISHOP};
                    moves[p_count++] = (Move){sq, target, KNIGHT};
                } else {
                    moves[p_count++] = (Move){sq, target, 0};
                }
                int start_rank = (side == WHITE) ? 6 : 1;
                if (r == start_rank && state->board[sq + 2 * dir] == EMPTY) {
                    moves[p_count++] = (Move){sq, sq + 2 * dir, 0};
                }
            }
            int cap_cols[2] = {c - 1, c + 1};
            for (int i = 0; i < 2; i++) {
                int nc = cap_cols[i];
                if (nc >= 0 && nc < 8) {
                    int t_sq = (r + (side == WHITE ? -1 : 1)) * 8 + nc;
                    int tp = state->board[t_sq];
                    if (tp != EMPTY && (tp & COLOR_MASK) == opp_side) {
                        if (t_sq / 8 == 0 || t_sq / 8 == 7) {
                            moves[p_count++] = (Move){sq, t_sq, QUEEN};
                            moves[p_count++] = (Move){sq, t_sq, ROOK};
                            moves[p_count++] = (Move){sq, t_sq, BISHOP};
                            moves[p_count++] = (Move){sq, t_sq, KNIGHT};
                        } else {
                            moves[p_count++] = (Move){sq, t_sq, 0};
                        }
                    } else if (t_sq == state->ep) {
                        moves[p_count++] = (Move){sq, t_sq, 0};
                    }
                }
            }
        } else if (type == KNIGHT) {
            int dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
            int dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
            for (int i = 0; i < 8; i++) {
                int nr = r + dr[i], nc = c + dc[i];
                if (IN_BOUNDS(nr, nc)) {
                    int t_sq = nr * 8 + nc;
                    int tp = state->board[t_sq];
                    if (tp == EMPTY || (tp & COLOR_MASK) == opp_side) {
                        moves[p_count++] = (Move){sq, t_sq, 0};
                    }
                }
            }
        } else if (type == BISHOP || type == QUEEN) {
            int b_dr[] = {-1, -1, 1, 1};
            int b_dc[] = {-1, 1, -1, 1};
            for (int i = 0; i < 4; i++) {
                int nr = r, nc = c;
                while (1) {
                    nr += b_dr[i]; nc += b_dc[i];
                    if (!IN_BOUNDS(nr, nc)) break;
                    int t_sq = nr * 8 + nc;
                    int tp = state->board[t_sq];
                    if (tp == EMPTY) {
                        moves[p_count++] = (Move){sq, t_sq, 0};
                    } else {
                        if ((tp & COLOR_MASK) == opp_side) moves[p_count++] = (Move){sq, t_sq, 0};
                        break;
                    }
                }
            }
        }
        if (type == ROOK || type == QUEEN) {
            int dr[] = {-1, 1, 0, 0};
            int dc[] = {0, 0, -1, 1};
            for (int i = 0; i < 4; i++) {
                int nr = r, nc = c;
                while (1) {
                    nr += dr[i]; nc += dc[i];
                    if (!IN_BOUNDS(nr, nc)) break;
                    int t_sq = nr * 8 + nc;
                    int tp = state->board[t_sq];
                    if (tp == EMPTY) {
                        moves[p_count++] = (Move){sq, t_sq, 0};
                    } else {
                        if ((tp & COLOR_MASK) == opp_side) moves[p_count++] = (Move){sq, t_sq, 0};
                        break;
                    }
                }
            }
        } else if (type == KING) {
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    if (dr == 0 && dc == 0) continue;
                    int nr = r + dr, nc = c + dc;
                    if (IN_BOUNDS(nr, nc)) {
                        int t_sq = nr * 8 + nc;
                        int tp = state->board[t_sq];
                        if (tp == EMPTY || (tp & COLOR_MASK) == opp_side) {
                            moves[p_count++] = (Move){sq, t_sq, 0};
                        }
                    }
                }
            }
            // Add castling checks
            if (side == WHITE && sq == 60) {
                if ((state->castling & 1) && state->board[61] == EMPTY && state->board[62] == EMPTY) moves[p_count++] = (Move){60, 62, 0};
                if ((state->castling & 2) && state->board[59] == EMPTY && state->board[58] == EMPTY && state->board[57] == EMPTY) moves[p_count++] = (Move){60, 58, 0};
            } else if (side == BLACK && sq == 4) {
                if ((state->castling & 4) && state->board[5] == EMPTY && state->board[6] == EMPTY) moves[p_count++] = (Move){4, 6, 0};
                if ((state->castling & 8) && state->board[3] == EMPTY && state->board[2] == EMPTY && state->board[1] == EMPTY) moves[p_count++] = (Move){4, 2, 0};
            }
        }
    }

    int legal_count = 0;
    for (int i = 0; i < p_count; i++) {
        Move m = moves[i];
        BoardState next;
        if ((state->board[m.from] & PIECE_MASK) == KING) {
            if (m.from == 60 && m.to == 62) {
                if (is_square_attacked(state, 60, opp_side) || is_square_attacked(state, 61, opp_side) || is_square_attacked(state, 62, opp_side)) continue;
            }
            if (m.from == 60 && m.to == 58) {
                if (is_square_attacked(state, 60, opp_side) || is_square_attacked(state, 59, opp_side) || is_square_attacked(state, 58, opp_side)) continue;
            }
            if (m.from == 4 && m.to == 6) {
                if (is_square_attacked(state, 4, opp_side) || is_square_attacked(state, 5, opp_side) || is_square_attacked(state, 6, opp_side)) continue;
            }
            if (m.from == 4 && m.to == 2) {
                if (is_square_attacked(state, 4, opp_side) || is_square_attacked(state, 3, opp_side) || is_square_attacked(state, 2, opp_side)) continue;
            }
        }

        make_move(state, &next, m);
        int king_sq = -1;
        for (int s = 0; s < 64; s++) {
            if (next.board[s] == (KING | side)) { king_sq = s; break; }
        }
        if (king_sq != -1 && !is_square_attacked(&next, king_sq, opp_side)) {
            moves[legal_count++] = m;
        }
    }
    return legal_count;
}

/* SAN / PGN String builders */
void get_square_str(int sq, char *buf) {
    buf[0] = 'a' + (sq % 8);
    buf[1] = '8' - (sq / 8);
    buf[2] = '\0';
}

void get_san_move(const BoardState *prev, Move m, char *san) {
    int p = prev->board[m.from];
    int type = p & PIECE_MASK;
    int color = p & COLOR_MASK;
    int opp_color = (color == WHITE) ? BLACK : WHITE;

    if (type == KING) {
        if (m.from == 60 && m.to == 62) { strcpy(san, "O-O"); goto check_status; }
        if (m.from == 60 && m.to == 58) { strcpy(san, "O-O-O"); goto check_status; }
        if (m.from == 4 && m.to == 6) { strcpy(san, "O-O"); goto check_status; }
        if (m.from == 4 && m.to == 2) { strcpy(san, "O-O-O"); goto check_status; }
    }

    char piece_char = ' ';
    if (type == KNIGHT) piece_char = 'N';
    else if (type == BISHOP) piece_char = 'B';
    else if (type == ROOK) piece_char = 'R';
    else if (type == QUEEN) piece_char = 'Q';
    else if (type == KING) piece_char = 'K';

    int is_capture = (prev->board[m.to] != EMPTY) || (type == PAWN && m.to == prev->ep);
    int pos = 0;

    if (type == PAWN) {
        if (is_capture) {
            san[pos++] = 'a' + (m.from % 8);
            san[pos++] = 'x';
        }
        get_square_str(m.to, san + pos);
        pos += 2;
        if (m.promotion != 0) {
            san[pos++] = '=';
            if (m.promotion == QUEEN) san[pos++] = 'Q';
            else if (m.promotion == ROOK) san[pos++] = 'R';
            else if (m.promotion == BISHOP) san[pos++] = 'B';
            else if (m.promotion == KNIGHT) san[pos++] = 'N';
        }
        san[pos] = '\0';
    } else {
        san[pos++] = piece_char;
        Move leg_moves[256];
        int n_leg = generate_legal_moves(prev, leg_moves);
        int conflict = 0, need_file = 0, need_rank = 0;
        for (int i = 0; i < n_leg; i++) {
            Move tm = leg_moves[i];
            if (tm.from != m.from && tm.to == m.to && (prev->board[tm.from] & PIECE_MASK) == type) {
                conflict = 1;
                if (tm.from % 8 == m.from % 8) need_rank = 1;
                else need_file = 1;
            }
        }
        if (conflict) {
            if (need_file) san[pos++] = 'a' + (m.from % 8);
            if (need_rank) san[pos++] = '8' - (m.from / 8);
        }
        if (is_capture) san[pos++] = 'x';
        get_square_str(m.to, san + pos);
        pos += 2;
        san[pos] = '\0';
    }

check_status:;
    BoardState next;
    make_move(prev, &next, m);
    int opp_king = -1;
    for (int i = 0; i < 64; i++) {
        if (next.board[i] == (KING | opp_color)) { opp_king = i; break; }
    }
    int is_ch = is_square_attacked(&next, opp_king, color);
    Move opp_moves[256];
    int opp_n = generate_legal_moves(&next, opp_moves);
    if (is_ch) {
        strcat(san, opp_n == 0 ? "#" : "+");
    }
}

void get_pgn_lines(char lines[3][60], int count) {
    strcpy(lines[0], ""); strcpy(lines[1], ""); strcpy(lines[2], "");
    char full_pgn[8192] = "";
    int len = 0;
    for (int i = 0; i < count - 1; i += 2) {
        char move_pair[64];
        if (i + 1 < count - 1) sprintf(move_pair, "%d. %s %s  ", (i / 2) + 1, san_history[i], san_history[i + 1]);
        else sprintf(move_pair, "%d. %s ... ", (i / 2) + 1, san_history[i]);
        if (len + strlen(move_pair) < sizeof(full_pgn)) {
            strcat(full_pgn, move_pair);
            len += strlen(move_pair);
        }
    }

    int total_len = strlen(full_pgn);
    int start_idx = 0;
    if (total_len > 120) {
        start_idx = total_len - 120;
        while (start_idx < total_len && full_pgn[start_idx] != ' ' && full_pgn[start_idx] != '\0') start_idx++;
    }

    const char *ptr = full_pgn + start_idx;
    for (int l = 0; l < 3; l++) {
        if (*ptr == '\0') break;
        while (*ptr == ' ') ptr++;
        int copy_len = 0, limit = 45, last_space = -1;
        while (ptr[copy_len] != '\0' && copy_len < limit) {
            if (ptr[copy_len] == ' ') last_space = copy_len;
            copy_len++;
        }
        if (ptr[copy_len] != '\0' && last_space > 20) copy_len = last_space;
        snprintf(lines[l], copy_len + 1, "%s", ptr);
        ptr += copy_len;
    }
}

int is_legal_dest(int to, Move *legal_moves, int legal_count, int selected_sq) {
    if (selected_sq == -1) return 0;
    for (int i = 0; i < legal_count; i++) {
        if (legal_moves[i].from == selected_sq && legal_moves[i].to == to) return 1;
    }
    return 0;
}

/* Graphics and Interface Rendering */
const char *piece_symbols[] = { " ", " ♟ ", " ♞ ", " ♝ ", " ♜ ", " ♛ ", " ♚ " };

void draw_board(BoardState *state, int cursor_sq, int selected_sq, Move *legal_moves, int legal_count, BoardState *history, int history_count) {
    printf("\033[H"); // Rewind screen to top left coordinate in-place
    printf("\033[1;36m  === CHESS ENGINE TERMINAL GUI ===\033[0m\n\n");

    char pgn_lines[3][60];
    for (int i = 1; i < history_count; i++) {
        get_san_move(&history[i - 1], history[i].last_move, san_history[i - 1]);
    }
    get_pgn_lines(pgn_lines, history_count);

    for (int r = 0; r < 8; r++) {
        printf(" %d ", 8 - r);
        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            int piece = state->board[sq];
            int type = piece & PIECE_MASK;
            int color = piece & COLOR_MASK;

            // Compute background ANSI sequence
            if (sq == cursor_sq) printf("\033[48;5;208m"); // Orange cursor
            else if (sq == selected_sq) printf("\033[48;5;220m"); // Golden anchor
            else if (is_legal_dest(sq, legal_moves, legal_count, selected_sq)) printf("\033[48;5;114m"); // Green target
            else if (history_count > 1 && (sq == history[history_count - 1].last_move.from || sq == history[history_count - 1].last_move.to)) {
                printf("\033[48;5;111m"); // Highlighted Blue history trail
            } else if ((r + c) % 2 == 0) printf("\033[48;5;223m"); // Cream White square
            else printf("\033[48;5;130m"); // Brown Black square

            // Render Piece Foreground
            if (piece != EMPTY) {
                if (color == WHITE) printf("\033[38;5;255m\033[1m");
                else printf("\033[38;5;234m");
                printf("%s", piece_symbols[type]);
            } else {
                printf("   ");
            }
        }
        printf("\033[0m %d   ", 8 - r);

        // Sidebar widgets
        switch (r) {
            case 0: {
                int opp_king = -1;
                int side = (state->turn == 0) ? WHITE : BLACK;
                int opp_side = (state->turn == 0) ? BLACK : WHITE;
                for (int i = 0; i < 64; i++) {
                    if (state->board[i] == (KING | side)) { opp_king = i; break; }
                }
                int in_ch = is_square_attacked(state, opp_king, opp_side);
                printf("\033[1mTurn:\033[0m %s %s", (state->turn == 0) ? "\033[1;37mWHITE ♔\033[0m" : "\033[34mBLACK ♚\033[0m", in_ch ? "\033[1;31m[CHECK!]\033[0m" : "");
                break;
            }
            case 1: {
                const char *modes[] = { "Player vs Player", "Player vs Engine (Black)", "Engine (White) vs Player", "Engine vs Engine" };
                printf("\033[1mMode:\033[0m %s", modes[engine_mode]);
                break;
            }
            case 2:
                printf("\033[1mEngine:\033[0m %s", engine_connected ? "\033[1;32mActive\033[0m" : "\033[31mNone (Manual Only, press 'c')\033[0m");
                break;
            case 3:
                if (tc_type == TC_DEPTH) printf("\033[1mTime Ctrl:\033[0m Depth %d", tc_val);
                else if (tc_type == TC_NODES) printf("\033[1mTime Ctrl:\033[0m Nodes %d", tc_val);
                else printf("\033[1mTime Ctrl:\033[0m Movetime %d ms", tc_val);
                break;
            case 4:
                printf("\033[1;35mPGN Feed:\033[0m");
                break;
            case 5: printf("  %s", pgn_lines[0]); break;
            case 6: printf("  %s", pgn_lines[1]); break;
            case 7: printf("  %s", pgn_lines[2]); break;
        }
        printf("\033[K\n");
    }
    printf("    a  b  c  d  e  f  g  h\n\n");
    printf(" [Arrow Keys / WASD] Move Cursor   [Space/Enter] Select/Move   [U] Undo\n");
    printf(" [E] Cycle Engine Mode   [T] Change Time Control   [C] Spawn Engine   [Q] Quit\n");
    printf("\033[J");
    fflush(stdout);
}

int get_key() {
    char c;
    int n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return -1;
    if (c == '\033') {
        char seq[2];
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        int r1 = read(STDIN_FILENO, &seq[0], 1);
        int r2 = read(STDIN_FILENO, &seq[1], 1);
        fcntl(STDIN_FILENO, F_SETFL, flags);
        if (r1 > 0 && r2 > 0 && seq[0] == '[') {
            if (seq[1] == 'A') return KEY_UP;
            if (seq[1] == 'B') return KEY_DOWN;
            if (seq[1] == 'C') return KEY_RIGHT;
            if (seq[1] == 'D') return KEY_LEFT;
        }
        return '\033';
    }
    return c;
}

int get_key_timeout(int timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    int max_fd = STDIN_FILENO;
    if (engine_connected) {
        FD_SET(engine_out[0], &fds);
        if (engine_out[0] > max_fd) max_fd = engine_out[0];
    }
    int ret = select(max_fd + 1, &fds, NULL, NULL, &tv);
    if (ret > 0) {
        if (FD_ISSET(STDIN_FILENO, &fds)) return get_key();
        if (engine_connected && FD_ISSET(engine_out[0], &fds)) return -2;
    }
    return -1;
}

int poll_engine(Move *best_move, Move *legal_moves, int num_legal) {
    if (!engine_connected) return 0;
    char temp[1024];
    int n = read(engine_out[0], temp, sizeof(temp) - 1);
    if (n > 0) {
        temp[n] = '\0';
        if (engine_buf_len + n < (int)sizeof(engine_buf)) {
            memcpy(engine_buf + engine_buf_len, temp, n);
            engine_buf_len += n;
            engine_buf[engine_buf_len] = '\0';
        } else {
            engine_buf_len = 0;
        }

        char *line_start = engine_buf;
        char *newline;
        while ((newline = strchr(line_start, '\n')) != NULL) {
            *newline = '\0';
            if (strncmp(line_start, "bestmove ", 9) == 0) {
                char mv_str[10];
                sscanf(line_start, "bestmove %s", mv_str);
                for (int i = 0; i < num_legal; i++) {
                    char leg_str[10];
                    move_to_uci(legal_moves[i], leg_str);
                    if (strcmp(mv_str, leg_str) == 0) {
                        *best_move = legal_moves[i];
                        int consumed = (newline + 1) - engine_buf;
                        memmove(engine_buf, newline + 1, engine_buf_len - consumed);
                        engine_buf_len -= consumed;
                        return 1;
                    }
                }
            }
            line_start = newline + 1;
        }
        int consumed = line_start - engine_buf;
        if (consumed > 0) {
            memmove(engine_buf, line_start, engine_buf_len - consumed);
            engine_buf_len -= consumed;
        }
    }
    return 0;
}

int main() {
    printf("\033[2J\033[?25l");
    fflush(stdout);
    enableRawMode();

    BoardState history[2048];
    int history_count = 0;
    init_board(&history[history_count]);
    history_count++;

    int cursor_sq = 60; // Start at e1
    int selected_sq = -1;

    Move legal_moves[256];
    int legal_count = generate_legal_moves(&history[history_count - 1], legal_moves);

    // Attempt auto-connecting to stockfish if present in standard PATH
    start_engine("stockfish");

    int running = 1;
    int engine_searching = 0;

    while (running) {
        BoardState *current = &history[history_count - 1];
        draw_board(current, cursor_sq, selected_sq, legal_moves, legal_count, history, history_count);

        if (legal_count == 0) {
            int side = (current->turn == 0) ? WHITE : BLACK;
            int opp_side = (current->turn == 0) ? BLACK : WHITE;
            int king_sq = -1;
            for (int i = 0; i < 64; i++) {
                if (current->board[i] == (KING | side)) { king_sq = i; break; }
            }
            int in_ch = is_square_attacked(current, king_sq, opp_side);
            disableRawMode();
            printf("\n \033[1;31m*** GAME OVER: %s ***\033[0m\n", in_ch ? "CHECKMATE!" : "STALEMATE!");
            printf(" Press [Enter] to start new game...");
            fflush(stdout);
            getchar();
            enableRawMode();

            history_count = 0;
            init_board(&history[history_count]);
            history_count++;
            legal_count = generate_legal_moves(&history[0], legal_moves);
            selected_sq = -1;
            engine_searching = 0;
            continue;
        }

        int is_engine_turn = 0;
        if (engine_connected) {
            if (engine_mode == 1 && current->turn == 1) is_engine_turn = 1;
            else if (engine_mode == 2 && current->turn == 0) is_engine_turn = 1;
            else if (engine_mode == 3) is_engine_turn = 1;
        }

        if (is_engine_turn && !engine_searching) {
            send_position_and_go(history, history_count);
            engine_searching = 1;
        }

        int key = get_key_timeout(100);
        if (key == -2) {
            Move best_move;
            if (poll_engine(&best_move, legal_moves, legal_count)) {
                make_move(current, &history[history_count], best_move);
                history[history_count].last_move = best_move;
                history_count++;
                legal_count = generate_legal_moves(&history[history_count - 1], legal_moves);
                selected_sq = -1;
                engine_searching = 0;
            }
        } else if (key != -1) {
            if (key == KEY_UP || key == 'w' || key == 'W') {
                cursor_sq = (cursor_sq >= 8) ? cursor_sq - 8 : cursor_sq;
            } else if (key == KEY_DOWN || key == 's' || key == 'S') {
                cursor_sq = (cursor_sq < 56) ? cursor_sq + 8 : cursor_sq;
            } else if (key == KEY_LEFT || key == 'a' || key == 'A') {
                cursor_sq = (cursor_sq % 8 > 0) ? cursor_sq - 1 : cursor_sq;
            } else if (key == KEY_RIGHT || key == 'd' || key == 'D') {
                cursor_sq = (cursor_sq % 8 < 7) ? cursor_sq + 1 : cursor_sq;
            } else if (key == ' ' || key == '\n' || key == '\r') {
                if (selected_sq == -1) {
                    int p = current->board[cursor_sq];
                    if (p != EMPTY && (p & COLOR_MASK) == (current->turn == 0 ? WHITE : BLACK)) {
                        selected_sq = cursor_sq;
                    }
                } else {
                    if (is_legal_dest(cursor_sq, legal_moves, legal_count, selected_sq)) {
                        Move chosen_move = {0};
                        int found = 0, is_promo = 0;
                        int piece = current->board[selected_sq];
                        if ((piece & PIECE_MASK) == PAWN && (cursor_sq / 8 == 0 || cursor_sq / 8 == 7)) {
                            is_promo = 1;
                        }

                        if (is_promo) {
                            char choice[10] = "";
                            prompt_input("Promote pawn: [q]ueen, [r]ook, [b]ishop, [n]ight: ", choice, sizeof(choice));
                            int promo_piece = QUEEN;
                            if (choice[0] == 'r' || choice[0] == 'R') promo_piece = ROOK;
                            else if (choice[0] == 'b' || choice[0] == 'B') promo_piece = BISHOP;
                            else if (choice[0] == 'n' || choice[0] == 'N') promo_piece = KNIGHT;

                            for (int i = 0; i < legal_count; i++) {
                                if (legal_moves[i].from == selected_sq && legal_moves[i].to == cursor_sq && legal_moves[i].promotion == promo_piece) {
                                    chosen_move = legal_moves[i];
                                    found = 1;
                                    break;
                                }
                            }
                        } else {
                            for (int i = 0; i < legal_count; i++) {
                                if (legal_moves[i].from == selected_sq && legal_moves[i].to == cursor_sq) {
                                    chosen_move = legal_moves[i];
                                    found = 1;
                                    break;
                                }
                            }
                        }

                        if (found) {
                            make_move(current, &history[history_count], chosen_move);
                            history[history_count].last_move = chosen_move;
                            history_count++;
                            legal_count = generate_legal_moves(&history[history_count - 1], legal_moves);
                            selected_sq = -1;
                        } else {
                            selected_sq = -1;
                        }
                    } else {
                        int p = current->board[cursor_sq];
                        if (p != EMPTY && (p & COLOR_MASK) == (current->turn == 0 ? WHITE : BLACK)) {
                            selected_sq = cursor_sq;
                        } else {
                            selected_sq = -1;
                        }
                    }
                }
            } else if (key == 'u' || key == 'U') {
                if (engine_mode == 1 || engine_mode == 2) {
                    if (history_count > 2) {
                        history_count -= 2;
                        selected_sq = -1;
                        legal_count = generate_legal_moves(&history[history_count - 1], legal_moves);
                    }
                } else {
                    if (history_count > 1) {
                        history_count--;
                        selected_sq = -1;
                        legal_count = generate_legal_moves(&history[history_count - 1], legal_moves);
                    }
                }
                engine_searching = 0;
            } else if (key == 'e' || key == 'E') {
                engine_mode = (engine_mode + 1) % 4;
                engine_searching = 0;
            } else if (key == 't' || key == 'T') {
                char type_str[10] = "", val_str[20] = "";
                prompt_input("Time Control Type (d = Depth, n = Nodes, t = Movetime ms): ", type_str, sizeof(type_str));
                prompt_input("Enter limit value: ", val_str, sizeof(val_str));
                int val = atoi(val_str);
                if (val > 0) {
                    tc_val = val;
                    if (type_str[0] == 'd' || type_str[0] == 'D') tc_type = TC_DEPTH;
                    else if (type_str[0] == 'n' || type_str[0] == 'N') tc_type = TC_NODES;
                    else if (type_str[0] == 't' || type_str[0] == 'T') tc_type = TC_TIME;
                }
            } else if (key == 'c' || key == 'C') {
                char path[256] = "";
                prompt_input("Enter Path to UCI Chess Engine: ", path, sizeof(path));
                if (strlen(path) > 0) {
                    if (engine_pid != -1) kill(engine_pid, SIGKILL);
                    start_engine(path);
                }
            } else if (key == 'q' || key == 'Q') {
                running = 0;
            }
        }
    }

    if (engine_pid != -1) kill(engine_pid, SIGKILL);
    disableRawMode();
    printf("\033[?25h\033[0m\nGoodbye!\n");
    return 0;
}
