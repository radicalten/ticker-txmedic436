#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/select.h>
#include <termios.h>
#include <signal.h>

/* Chess definitions */
#define WHITE 8
#define BLACK 16
#define PIECE_MASK 7
#define COLOR_MASK 24

#define EMPTY 0
#define PAWN 1
#define KNIGHT 2
#define BISHOP 3
#define ROOK 4
#define QUEEN 5
#define KING 6

#define BOTH 0  // local multiplayer

typedef struct {
    int from;
    int to;
    int promo; // QUEEN, ROOK, BISHOP, KNIGHT or 0
} Move;

typedef struct {
    int board[64];
    int active_color;
    int castle_rights; // Bit 0: WK, Bit 1: WQ, Bit 2: BK, Bit 3: BQ
    int ep_square;      // -1, or square index of the EP target
    int halfmove;
    int fullmove;
} BoardState;

typedef enum { TC_DEPTH, TC_NODES, TC_TIME } TimeControlMode;

/* Global state */
BoardState state_history[1024];
int state_history_count = 0;
Move move_history[1024];
int move_history_count = 0;
char pgn_history[1024][16];

int human_color = WHITE; // WHITE, BLACK, or BOTH (local)
TimeControlMode tc_mode = TC_DEPTH;
int tc_value = 10; // Default depth 10

int engine_in[2];  // Pipe engine stdin
int engine_out[2]; // Pipe engine stdout
pid_t engine_pid = -1;
int has_engine = 0;

struct termios orig_termios;

const char* piece_symbols[25] = {
    [EMPTY] = " ",
    [WHITE | PAWN]   = "♙", [WHITE | KNIGHT] = "♘", [WHITE | BISHOP] = "♗", [WHITE | ROOK] = "♖", [WHITE | QUEEN] = "♕", [WHITE | KING] = "♔",
    [BLACK | PAWN]   = "♟", [BLACK | KNIGHT] = "♞", [BLACK | BISHOP] = "♝", [BLACK | ROOK] = "♜", [BLACK | QUEEN] = "♛", [BLACK | KING] = "♚"
};

/* Terminal Manipulation Helpers */
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

int readKey() {
    char c;
    int nread;
    while ((nread = read(STDIN_FILENO, &c, 1)) == 0);
    if (nread == -1) return 0;
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 'w'; // Up Arrow
                case 'B': return 's'; // Down Arrow
                case 'C': return 'd'; // Right Arrow
                case 'D': return 'a'; // Left Arrow
            }
        }
        return '\x1b';
    }
    return c;
}

/* Engine Process Spawning */
void start_engine(const char *path) {
    if (pipe(engine_in) < 0 || pipe(engine_out) < 0) {
        perror("pipe failed");
        return;
    }
    engine_pid = fork();
    if (engine_pid < 0) {
        perror("fork failed");
        return;
    }
    if (engine_pid == 0) {
        // Child process redirects standard streams
        dup2(engine_in[0], STDIN_FILENO);
        dup2(engine_out[1], STDOUT_FILENO);
        close(engine_in[1]);
        close(engine_out[0]);
        execl(path, path, (char *)NULL);
        perror("exec failed");
        exit(1);
    } else {
        // Parent process settings
        close(engine_in[0]);
        close(engine_out[1]);
        fcntl(engine_out[0], F_SETFL, O_NONBLOCK);
        has_engine = 1;
    }
}

void stop_engine() {
    if (has_engine && engine_pid > 0) {
        write(engine_in[1], "quit\n", 5);
        close(engine_in[1]);
        close(engine_out[0]);
        kill(engine_pid, SIGTERM);
    }
}

/* Base Chess Operations */
void init_board(BoardState *state) {
    memset(state, 0, sizeof(BoardState));
    int back_row[8] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
    for (int i = 0; i < 8; i++) {
        state->board[0 * 8 + i] = back_row[i] | WHITE;
        state->board[1 * 8 + i] = PAWN | WHITE;
        for (int r = 2; r < 6; r++) state->board[r * 8 + i] = EMPTY;
        state->board[6 * 8 + i] = PAWN | BLACK;
        state->board[7 * 8 + i] = back_row[i] | BLACK;
    }
    state->active_color = WHITE;
    state->castle_rights = 15;
    state->ep_square = -1;
    state->halfmove = 0;
    state->fullmove = 1;
}

int is_square_attacked(const BoardState *state, int sq, int attacker_col) {
    int r = sq / 8, f = sq % 8;
    int knight_dirs[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + knight_dirs[i][0], nf = f + knight_dirs[i][1];
        if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
            int p = state->board[nr * 8 + nf];
            if ((p & COLOR_MASK) == attacker_col && (p & PIECE_MASK) == KNIGHT) return 1;
        }
    }
    int king_dirs[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + king_dirs[i][0], nf = f + king_dirs[i][1];
        if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
            int p = state->board[nr * 8 + nf];
            if ((p & COLOR_MASK) == attacker_col && (p & PIECE_MASK) == KING) return 1;
        }
    }
    int p_dir = (attacker_col == WHITE) ? -1 : 1;
    for (int df = -1; df <= 1; df += 2) {
        int nf = f + df;
        int nr = r + p_dir;
        if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
            int p = state->board[nr * 8 + nf];
            if ((p & COLOR_MASK) == attacker_col && (p & PIECE_MASK) == PAWN) return 1;
        }
    }
    int rook_dirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nf = f;
        while (1) {
            nr += rook_dirs[i][0]; nf += rook_dirs[i][1];
            if (nr < 0 || nr >= 8 || nf < 0 || nf >= 8) break;
            int p = state->board[nr * 8 + nf];
            if (p != EMPTY) {
                if ((p & COLOR_MASK) == attacker_col && ((p & PIECE_MASK) == ROOK || (p & PIECE_MASK) == QUEEN)) return 1;
                break;
            }
        }
    }
    int bishop_dirs[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nf = f;
        while (1) {
            nr += bishop_dirs[i][0]; nf += bishop_dirs[i][1];
            if (nr < 0 || nr >= 8 || nf < 0 || nf >= 8) break;
            int p = state->board[nr * 8 + nf];
            if (p != EMPTY) {
                if ((p & COLOR_MASK) == attacker_col && ((p & PIECE_MASK) == BISHOP || (p & PIECE_MASK) == QUEEN)) return 1;
                break;
            }
        }
    }
    return 0;
}

int is_in_check(const BoardState *state, int color) {
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == (KING | color)) {
            king_sq = i;
            break;
        }
    }
    if (king_sq == -1) return 0;
    return is_square_attacked(state, king_sq, (color == WHITE) ? BLACK : WHITE);
}

int generate_pseudo_moves(const BoardState *state, Move *moves) {
    int count = 0;
    int active = state->active_color;
    int opp = (active == WHITE) ? BLACK : WHITE;
    int p_dir = (active == WHITE) ? 1 : -1;
    int start_rank = (active == WHITE) ? 1 : 6;
    int promo_rank = (active == WHITE) ? 7 : 0;

    for (int sq = 0; sq < 64; sq++) {
        int p = state->board[sq];
        if ((p & COLOR_MASK) != active) continue;
        int type = p & PIECE_MASK;
        int r = sq / 8, f = sq % 8;

        if (type == PAWN) {
            int to = sq + p_dir * 8;
            if (state->board[to] == EMPTY) {
                if (to / 8 == promo_rank) {
                    moves[count++] = (Move){sq, to, QUEEN};
                    moves[count++] = (Move){sq, to, ROOK};
                    moves[count++] = (Move){sq, to, BISHOP};
                    moves[count++] = (Move){sq, to, KNIGHT};
                } else {
                    moves[count++] = (Move){sq, to, 0};
                    if (r == start_rank && state->board[sq + p_dir * 16] == EMPTY) {
                        moves[count++] = (Move){sq, sq + p_dir * 16, 0};
                    }
                }
            }
            for (int df = -1; df <= 1; df += 2) {
                int nf = f + df;
                if (nf >= 0 && nf < 8) {
                    int to_cap = (r + p_dir) * 8 + nf;
                    int cap_p = state->board[to_cap];
                    if ((cap_p != EMPTY && (cap_p & COLOR_MASK) == opp) || to_cap == state->ep_square) {
                        if (to_cap / 8 == promo_rank) {
                            moves[count++] = (Move){sq, to_cap, QUEEN};
                            moves[count++] = (Move){sq, to_cap, ROOK};
                            moves[count++] = (Move){sq, to_cap, BISHOP};
                            moves[count++] = (Move){sq, to_cap, KNIGHT};
                        } else {
                            moves[count++] = (Move){sq, to_cap, 0};
                        }
                    }
                }
            }
        } else if (type == KNIGHT) {
            int dirs[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
            for (int i = 0; i < 8; i++) {
                int nr = r + dirs[i][0], nf = f + dirs[i][1];
                if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
                    int dest = nr * 8 + nf;
                    if (state->board[dest] == EMPTY || (state->board[dest] & COLOR_MASK) == opp) {
                        moves[count++] = (Move){sq, dest, 0};
                    }
                }
            }
        } else if (type == BISHOP || type == QUEEN || type == ROOK) {
            if (type == BISHOP || type == QUEEN) {
                int dirs[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
                for (int i = 0; i < 4; i++) {
                    int nr = r, nf = f;
                    while (1) {
                        nr += dirs[i][0]; nf += dirs[i][1];
                        if (nr < 0 || nr >= 8 || nf < 0 || nf >= 8) break;
                        int dest = nr * 8 + nf;
                        if (state->board[dest] == EMPTY) {
                            moves[count++] = (Move){sq, dest, 0};
                        } else {
                            if ((state->board[dest] & COLOR_MASK) == opp) {
                                moves[count++] = (Move){sq, dest, 0};
                            }
                            break;
                        }
                    }
                }
            }
            if (type == ROOK || type == QUEEN) {
                int dirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
                for (int i = 0; i < 4; i++) {
                    int nr = r, nf = f;
                    while (1) {
                        nr += dirs[i][0]; nf += dirs[i][1];
                        if (nr < 0 || nr >= 8 || nf < 0 || nf >= 8) break;
                        int dest = nr * 8 + nf;
                        if (state->board[dest] == EMPTY) {
                            moves[count++] = (Move){sq, dest, 0};
                        } else {
                            if ((state->board[dest] & COLOR_MASK) == opp) {
                                moves[count++] = (Move){sq, dest, 0};
                            }
                            break;
                        }
                    }
                }
            }
        } else if (type == KING) {
            int dirs[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
            for (int i = 0; i < 8; i++) {
                int nr = r + dirs[i][0], nf = f + dirs[i][1];
                if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
                    int dest = nr * 8 + nf;
                    if (state->board[dest] == EMPTY || (state->board[dest] & COLOR_MASK) == opp) {
                        moves[count++] = (Move){sq, dest, 0};
                    }
                }
            }
            if (active == WHITE) {
                if (!is_in_check(state, WHITE)) {
                    if ((state->castle_rights & 1) && state->board[5] == EMPTY && state->board[6] == EMPTY) {
                        if (!is_square_attacked(state, 5, BLACK) && !is_square_attacked(state, 6, BLACK)) {
                            moves[count++] = (Move){4, 6, 0};
                        }
                    }
                    if ((state->castle_rights & 2) && state->board[3] == EMPTY && state->board[2] == EMPTY && state->board[1] == EMPTY) {
                        if (!is_square_attacked(state, 3, BLACK) && !is_square_attacked(state, 2, BLACK)) {
                            moves[count++] = (Move){4, 2, 0};
                        }
                    }
                }
            } else {
                if (!is_in_check(state, BLACK)) {
                    if ((state->castle_rights & 4) && state->board[61] == EMPTY && state->board[62] == EMPTY) {
                        if (!is_square_attacked(state, 61, WHITE) && !is_square_attacked(state, 62, WHITE)) {
                            moves[count++] = (Move){60, 62, 0};
                        }
                    }
                    if ((state->castle_rights & 8) && state->board[59] == EMPTY && state->board[58] == EMPTY && state->board[57] == EMPTY) {
                        if (!is_square_attacked(state, 59, WHITE) && !is_square_attacked(state, 58, WHITE)) {
                            moves[count++] = (Move){60, 58, 0};
                        }
                    }
                }
            }
        }
    }
    return count;
}

void make_move(BoardState *state, Move move) {
    int from = move.from;
    int to = move.to;
    int piece = state->board[from];
    int type = piece & PIECE_MASK;
    int color = piece & COLOR_MASK;
    int opp_color = (color == WHITE) ? BLACK : WHITE;
    int next_ep_square = -1;

    state->board[to] = state->board[from];
    state->board[from] = EMPTY;

    if (type == PAWN && to == state->ep_square) {
        int ep_cap_sq = to - ((color == WHITE) ? 8 : -8);
        state->board[ep_cap_sq] = EMPTY;
    }
    if (type == PAWN && abs(to - from) == 16) {
        next_ep_square = from + ((color == WHITE) ? 8 : -8);
    }
    if (type == PAWN && move.promo != 0) {
        state->board[to] = move.promo | color;
    }
    if (type == KING) {
        if (from == 4 && to == 6 && color == WHITE) {
            state->board[5] = state->board[7]; state->board[7] = EMPTY;
        } else if (from == 4 && to == 2 && color == WHITE) {
            state->board[3] = state->board[0]; state->board[0] = EMPTY;
        } else if (from == 60 && to == 62 && color == BLACK) {
            state->board[61] = state->board[63]; state->board[63] = EMPTY;
        } else if (from == 60 && to == 58 && color == BLACK) {
            state->board[59] = state->board[56]; state->board[56] = EMPTY;
        }
    }
    if (type == KING) {
        if (color == WHITE) state->castle_rights &= ~3;
        else state->castle_rights &= ~12;
    }
    if (from == 0 || to == 0) state->castle_rights &= ~2;
    if (from == 7 || to == 7) state->castle_rights &= ~1;
    if (from == 56 || to == 56) state->castle_rights &= ~8;
    if (from == 63 || to == 63) state->castle_rights &= ~4;

    state->ep_square = next_ep_square;
    if (color == BLACK) state->fullmove++;
    state->active_color = opp_color;
}

int get_legal_moves(const BoardState *state, Move *legal_moves) {
    Move pseudo[256];
    int num_pseudo = generate_pseudo_moves(state, pseudo);
    int legal_count = 0;
    for (int i = 0; i < num_pseudo; i++) {
        BoardState temp = *state;
        make_move(&temp, pseudo[i]);
        if (!is_in_check(&temp, state->active_color)) {
            legal_moves[legal_count++] = pseudo[i];
        }
    }
    return legal_count;
}

/* Notation Generation Utilities */
void move_to_san(const BoardState *state, Move m, char *buf) {
    int piece = state->board[m.from];
    int type = piece & PIECE_MASK;
    int col = piece & COLOR_MASK;

    if (type == KING) {
        if (m.from == 4 && m.to == 6 && col == WHITE) { strcpy(buf, "O-O"); goto check_status; }
        if (m.from == 4 && m.to == 2 && col == WHITE) { strcpy(buf, "O-O-O"); goto check_status; }
        if (m.from == 60 && m.to == 62 && col == BLACK) { strcpy(buf, "O-O"); goto check_status; }
        if (m.from == 60 && m.to == 58 && col == BLACK) { strcpy(buf, "O-O-O"); goto check_status; }
    }

    char *p = buf;
    if (type == PAWN) {
        if (m.to % 8 != m.from % 8) {
            *p++ = 'a' + (m.from % 8);
            *p++ = 'x';
        }
        *p++ = 'a' + (m.to % 8);
        *p++ = '1' + (m.to / 8);
        if (m.promo != 0) {
            *p++ = '=';
            if (m.promo == QUEEN) *p++ = 'Q';
            else if (m.promo == ROOK) *p++ = 'R';
            else if (m.promo == BISHOP) *p++ = 'B';
            else if (m.promo == KNIGHT) *p++ = 'N';
        }
    } else {
        char p_char = ' ';
        if (type == KNIGHT) p_char = 'N';
        else if (type == BISHOP) p_char = 'B';
        else if (type == ROOK) p_char = 'R';
        else if (type == QUEEN) p_char = 'Q';
        else if (type == KING) p_char = 'K';
        *p++ = p_char;

        Move legal[256];
        int num_legal = get_legal_moves(state, legal);
        int duplicate = 0, same_file = 0, same_rank = 0;
        for (int i = 0; i < num_legal; i++) {
            if (legal[i].from != m.from && legal[i].to == m.to) {
                int p_other = state->board[legal[i].from];
                if (p_other == piece) {
                    duplicate = 1;
                    if (legal[i].from % 8 == m.from % 8) same_file = 1;
                    if (legal[i].from / 8 == m.from / 8) same_rank = 1;
                }
            }
        }
        if (duplicate) {
            if (!same_file) *p++ = 'a' + (m.from % 8);
            else if (!same_rank) *p++ = '1' + (m.from / 8);
            else {
                *p++ = 'a' + (m.from % 8);
                *p++ = '1' + (m.from / 8);
            }
        }
        if (state->board[m.to] != EMPTY) *p++ = 'x';
        *p++ = 'a' + (m.to % 8);
        *p++ = '1' + (m.to / 8);
    }
    *p = '\0';

check_status:;
    BoardState temp = *state;
    make_move(&temp, m);
    int opp = (col == WHITE) ? BLACK : WHITE;
    if (is_in_check(&temp, opp)) {
        Move opp_legal[256];
        if (get_legal_moves(&temp, opp_legal) == 0) strcat(buf, "#");
        else strcat(buf, "+");
    }
}

void move_to_uci(Move m, char *buf) {
    int f1 = m.from % 8, r1 = m.from / 8;
    int f2 = m.to % 8, r2 = m.to / 8;
    if (m.promo == 0) {
        sprintf(buf, "%c%c%c%c", 'a' + f1, '1' + r1, 'a' + f2, '1' + r2);
    } else {
        char p_char = 'q';
        if (m.promo == ROOK) p_char = 'r';
        else if (m.promo == BISHOP) p_char = 'b';
        else if (m.promo == KNIGHT) p_char = 'n';
        sprintf(buf, "%c%c%c%c%c", 'a' + f1, '1' + r1, 'a' + f2, '1' + r2, p_char);
    }
}

Move parse_uci(const BoardState *state, const char *uci) {
    Move m = {0};
    if (strlen(uci) < 4) return m;
    int f1 = uci[0] - 'a', r1 = uci[1] - '1';
    int f2 = uci[2] - 'a', r2 = uci[3] - '1';
    m.from = r1 * 8 + f1;
    m.to = r2 * 8 + f2;
    if (strlen(uci) >= 5) {
        char p = uci[4];
        if (p == 'q') m.promo = QUEEN;
        else if (p == 'r') m.promo = ROOK;
        else if (p == 'b') m.promo = BISHOP;
        else if (p == 'n') m.promo = KNIGHT;
    }
    return m;
}

/* Board Interface Rendering */
void render_board(const BoardState *state, int cursor_sq, int selected_sq, const Move *legal_moves, int num_legal_moves) {
    printf("\033[2J\033[H");
    printf("\033[1;36m=== CHESS TERMINAL GUI ===\033[0m\n\n");

    printf(" Time Control: \033[1;32m");
    if (tc_mode == TC_DEPTH) printf("Depth %d", tc_value);
    else if (tc_mode == TC_NODES) printf("Nodes %d", tc_value);
    else printf("Time limit %d ms", tc_value);
    printf("\033[0m | Turn: \033[1;33m%s\033[0m\n\n", (state->active_color == WHITE) ? "White" : "Black");

    int flip = (human_color == BLACK);
    int highlight[64] = {0};
    if (selected_sq != -1) {
        for (int i = 0; i < num_legal_moves; i++) {
            if (legal_moves[i].from == selected_sq) highlight[legal_moves[i].to] = 1;
        }
    }

    printf("   ");
    for (int f = (flip ? 7 : 0); (flip ? f >= 0 : f < 8); (flip ? f-- : f++)) {
        printf(" %c ", 'a' + f);
    }
    printf("\n");

    for (int r = (flip ? 0 : 7); (flip ? r < 8 : r >= 0); (flip ? r++ : r--)) {
        printf(" %d ", r + 1);
        for (int f = (flip ? 7 : 0); (flip ? f >= 0 : f < 8); (flip ? f-- : f++)) {
            int sq = r * 8 + f;
            int p = state->board[sq];
            int is_light = ((r + f) % 2 != 0);

            // Set dynamic background colors
            const char *bg = is_light ? "\033[48;5;251m" : "\033[48;5;240m";
            if (sq == cursor_sq) {
                bg = "\033[48;5;33m"; // Vivid Cursor
            } else if (sq == selected_sq) {
                bg = "\033[48;5;179m"; // Selected Square
            } else if (highlight[sq]) {
                bg = is_light ? "\033[48;5;114m" : "\033[48;5;108m"; // Valid Destination Highlight
            }

            // Set piece foreground styles
            const char *fg = "\033[38;5;16m"; // Dark pieces
            if (p != EMPTY && (p & COLOR_MASK) == WHITE) {
                fg = "\033[38;5;15m\033[1m"; // Bold Light pieces
            }
            printf("%s%s %s \033[0m", bg, fg, (p != EMPTY) ? piece_symbols[p] : " ");
        }
        printf(" %d\n", r + 1);
    }

    printf("   ");
    for (int f = (flip ? 7 : 0); (flip ? f >= 0 : f < 8); (flip ? f-- : f++)) {
        printf(" %c ", 'a' + f);
    }
    printf("\n\n");

    // Display 5 Recent Moves
    printf("Recent Moves (PGN): \033[1;35m");
    int start_move = state_history_count - 6;
    if (start_move < 0) start_move = 0;
    for (int i = start_move; i < state_history_count - 1; i++) {
        int fm = state_history[i].fullmove;
        if (state_history[i].active_color == WHITE) {
            printf("%d. %s ", fm, pgn_history[i]);
        } else {
            printf("%s ", pgn_history[i]);
        }
    }
    printf("\033[0m\n\n");

    printf("[Arrows/WASD] Cursor  [Space/Enter] Select/Move  [U] Undo\n");
    printf("[T] Change Time Control  [R] Reset Game  [Q] Exit App\n");
}

/* Core System Loops & Controls */
int get_promotion_choice() {
    printf("\nPromote Pawn to: \033[1;32m[Q]ueen\033[0m, \033[1;32m[R]ook\033[0m, \033[1;32m[B]ishop\033[0m, \033[1;32m[K]night\033[0m: ");
    fflush(stdout);
    while (1) {
        int key = readKey();
        if (key == 'q' || key == 'Q') return QUEEN;
        if (key == 'r' || key == 'R') return ROOK;
        if (key == 'b' || key == 'B') return BISHOP;
        if (key == 'k' || key == 'K' || key == 'n' || key == 'N') return KNIGHT;
    }
}

void change_time_control() {
    disableRawMode();
    printf("\n\033[1;36m=== Configure Time Control ===\033[0m\n");
    printf("1. Set Depth Target (e.g., 12 moves ahead)\n");
    printf("2. Set Node Target (e.g., 100000 nodes)\n");
    printf("3. Set Time Target per Turn (in milliseconds, e.g., 3000 ms)\n");
    printf("Select mode (1-3): ");
    char choice[16];
    if (fgets(choice, sizeof(choice), stdin)) {
        int ch = atoi(choice);
        if (ch >= 1 && ch <= 3) {
            printf("Enter target size: ");
            char val_str[32];
            if (fgets(val_str, sizeof(val_str), stdin)) {
                int val = atoi(val_str);
                if (val > 0) {
                    if (ch == 1) { tc_mode = TC_DEPTH; tc_value = val; }
                    else if (ch == 2) { tc_mode = TC_NODES; tc_value = val; }
                    else { tc_mode = TC_TIME; tc_value = val; }
                    printf("\033[1;32mSettings updated successfully!\033[0m\n");
                }
            }
        }
    }
    sleep(1);
    enableRawMode();
}

char engine_buf[4096];
int engine_buf_len = 0;

int read_engine_output(char *best_move_out) {
    char ch;
    while (read(engine_out[0], &ch, 1) > 0) {
        if (ch == '\n' || ch == '\r') {
            if (engine_buf_len > 0) {
                engine_buf[engine_buf_len] = '\0';
                if (strncmp(engine_buf, "bestmove", 8) == 0) {
                    sscanf(engine_buf, "bestmove %s", best_move_out);
                    engine_buf_len = 0;
                    return 1;
                }
                engine_buf_len = 0;
            }
        } else if (engine_buf_len < sizeof(engine_buf) - 2) {
            engine_buf[engine_buf_len++] = ch;
        }
    }
    return 0;
}

void get_engine_move(char *best_move_out) {
    char cmd[16384] = "position startpos moves";
    for (int i = 0; i < move_history_count; i++) {
        char move_str[10];
        move_to_uci(move_history[i], move_str);
        strcat(cmd, " ");
        strcat(cmd, move_str);
    }
    strcat(cmd, "\n");
    write(engine_in[1], cmd, strlen(cmd));

    char go_cmd[128];
    if (tc_mode == TC_DEPTH) sprintf(go_cmd, "go depth %d\n", tc_value);
    else if (tc_mode == TC_NODES) sprintf(go_cmd, "go nodes %d\n", tc_value);
    else sprintf(go_cmd, "go movetime %d\n", tc_value);
    write(engine_in[1], go_cmd, strlen(go_cmd));

    int found = 0;
    int spinner_frame = 0;
    const char *spinner = "/-\\|";
    while (!found) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(engine_out[0], &fds);
        struct timeval tv = {0, 100000}; // 100ms
        int ret = select(engine_out[0] + 1, &fds, NULL, NULL, &tv);
        if (ret > 0) {
            if (read_engine_output(best_move_out)) found = 1;
        } else {
            printf("\rEngine thinking %c...", spinner[spinner_frame]);
            fflush(stdout);
            spinner_frame = (spinner_frame + 1) % 4;
        }
    }
    printf("\r                      \r");
}

int main() {
    printf("Enter path to UCI chess engine (e.g. /opt/homebrew/bin/stockfish)\n");
    printf("or press [ENTER] to skip and play in local 2-player mode: ");
    char path[256];
    if (fgets(path, sizeof(path), stdin)) {
        path[strcspn(path, "\n")] = 0;
        if (strlen(path) > 0) {
            start_engine(path);
            char init_cmd[] = "uci\nisready\n";
            write(engine_in[1], init_cmd, strlen(init_cmd));
            sleep(1);
            if (kill(engine_pid, 0) == 0) {
                printf("Engine started successfully!\n");
                printf("\nChoose game layout:\n1. Human is White (vs Engine)\n2. Human is Black (vs Engine)\nSelect format (1 or 2): ");
                char choice[16];
                if (fgets(choice, sizeof(choice), stdin) && choice[0] == '2') {
                    human_color = BLACK;
                } else {
                    human_color = WHITE;
                }
            } else {
                printf("Failed to interface with engine path. Defaulting to Local Game Mode.\n");
                human_color = BOTH;
                sleep(2);
            }
        } else {
            human_color = BOTH;
        }
    }

    enableRawMode();
    init_board(&state_history[0]);
    state_history_count = 1;

    int cursor_sq = (human_color == BLACK) ? 60 : 4; // Start cursor on respective King square
    int selected_sq = -1;

    while (1) {
        BoardState *curr_state = &state_history[state_history_count - 1];
        Move legal[256];
        int num_legal = get_legal_moves(curr_state, legal);

        // Turn Handshake
        if (has_engine && curr_state->active_color != human_color && num_legal > 0) {
            render_board(curr_state, cursor_sq, selected_sq, legal, num_legal);
            char engine_uci[16];
            get_engine_move(engine_uci);
            Move m = parse_uci(curr_state, engine_uci);
            move_to_san(curr_state, m, pgn_history[state_history_count - 1]);
            
            BoardState next_state = *curr_state;
            make_move(&next_state, m);
            
            move_history[move_history_count++] = m;
            state_history[state_history_count++] = next_state;
            continue;
        }

        render_board(curr_state, cursor_sq, selected_sq, legal, num_legal);

        if (num_legal == 0) {
            if (is_in_check(curr_state, curr_state->active_color)) {
                printf("\033[1;31mGame Over! Checkmate. %s wins.\033[0m\n", (curr_state->active_color == WHITE) ? "Black" : "White");
            } else {
                printf("\033[1;33mGame Over! Stalemate.\033[0m\n");
            }
            disableRawMode();
            stop_engine();
            return 0;
        }

        int key = readKey();
        int flip = (human_color == BLACK);

        if (key == 'q' || key == 'Q') {
            disableRawMode();
            stop_engine();
            printf("\nExited Game loop.\n");
            return 0;
        } else if (key == 'r' || key == 'R') {
            init_board(&state_history[0]);
            state_history_count = 1;
            move_history_count = 0;
            selected_sq = -1;
            cursor_sq = (human_color == BLACK) ? 60 : 4;
        } else if (key == 'u' || key == 'U') {
            int undo_steps = (human_color == BOTH) ? 1 : 2;
            if (state_history_count > undo_steps) {
                state_history_count -= undo_steps;
                move_history_count -= undo_steps;
                selected_sq = -1;
            }
        } else if (key == 't' || key == 'T') {
            change_time_control();
        } else if (key == 'w' || key == 's' || key == 'a' || key == 'd') {
            int r = cursor_sq / 8;
            int f = cursor_sq % 8;
            if (key == 'w') { // Up on screen
                if (flip) { if (r > 0) r--; } else { if (r < 7) r++; }
            } else if (key == 's') { // Down on screen
                if (flip) { if (r < 7) r++; } else { if (r > 0) r--; }
            } else if (key == 'a') { // Left on screen
                if (flip) { if (f < 7) f++; } else { if (f > 0) f--; }
            } else if (key == 'd') { // Right on screen
                if (flip) { if (f > 0) f--; } else { if (f < 7) f++; }
            }
            cursor_sq = r * 8 + f;
        } else if (key == ' ' || key == '\r' || key == '\n') {
            if (selected_sq == -1) {
                if (curr_state->board[cursor_sq] != EMPTY && (curr_state->board[cursor_sq] & COLOR_MASK) == curr_state->active_color) {
                    selected_sq = cursor_sq;
                }
            } else {
                if (cursor_sq == selected_sq) {
                    selected_sq = -1;
                } else {
                    int matched = 0;
                    Move m = {0};
                    for (int i = 0; i < num_legal; i++) {
                        if (legal[i].from == selected_sq && legal[i].to == cursor_sq) {
                            matched = 1;
                            m = legal[i];
                            break;
                        }
                    }
                    if (matched) {
                        if (m.promo != 0) {
                            m.promo = get_promotion_choice();
                        }
                        move_to_san(curr_state, m, pgn_history[state_history_count - 1]);
                        
                        BoardState next_state = *curr_state;
                        make_move(&next_state, m);
                        
                        move_history[move_history_count++] = m;
                        state_history[state_history_count++] = next_state;
                        selected_sq = -1;
                    } else {
                        if (curr_state->board[cursor_sq] != EMPTY && (curr_state->board[cursor_sq] & COLOR_MASK) == curr_state->active_color) {
                            selected_sq = cursor_sq; // Switch piece focus
                        } else {
                            selected_sq = -1; // Deselect
                        }
                    }
                }
            }
        }
    }

    disableRawMode();
    stop_engine();
    return 0;
}
