#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>

// Piece representations
#define EMPTY 0
#define PAWN 1
#define KNIGHT 2
#define BISHOP 3
#define ROOK 4
#define QUEEN 5
#define KING 6

#define WHITE 8
#define BLACK 16
#define COLOR_MASK 24
#define TYPE_MASK 7

// Search Limit Settings
enum SearchLimitType { LIMIT_DEPTH, LIMIT_NODES, LIMIT_TIME };
typedef struct {
    int type;
    int value;
} SearchLimit;

SearchLimit limits[] = {
    {LIMIT_DEPTH, 6},
    {LIMIT_DEPTH, 10},
    {LIMIT_DEPTH, 14},
    {LIMIT_TIME, 500},
    {LIMIT_TIME, 1500},
    {LIMIT_TIME, 3000},
    {LIMIT_NODES, 25000},
    {LIMIT_NODES, 100000},
    {LIMIT_NODES, 500000}
};
int num_limits = sizeof(limits) / sizeof(limits[0]);
int current_limit_idx = 1; // Default: Depth 10

// Play modes
enum PlayMode { VS_ENGINE_BLACK, VS_ENGINE_WHITE, TWO_PLAYER, ENGINE_VS_ENGINE };
int play_mode = VS_ENGINE_BLACK;

// Moves representation
typedef struct {
    int from;
    int to;
    int promotion;
} Move;

// Game State History
typedef struct {
    int board[64];
    int turn;
    int castle; // bitmask: 1: WK, 2: WQ, 4: BK, 8: BQ
    int ep_sq;
    Move last_move;
    char last_move_str[16];
    int fullmove;
} BoardState;

BoardState history[1024];
int history_count = 0;

// Program State
int running = 1;
int cursor_sq = 52; // starts at e2
int selected_sq = -1;
char engine_path[256] = "stockfish";
int engine_in = -1;
int engine_out = -1;
pid_t engine_pid = -1;
int engine_thinking = 0;

struct termios orig_termios;

// Forward Declarations
void disableRawMode();
void enableRawMode();
int is_on_board(int r, int c);
int is_square_attacked(int board[64], int sq, int attacker_color);
int generate_moves(BoardState *state, Move moves[256]);
int make_move(const BoardState *src, BoardState *dst, Move m);
int get_legal_moves(BoardState *state, Move moves[256]);
void get_move_san(BoardState *state, Move m, char *san);
void move_to_uci(Move m, char *buf);
void trigger_engine_if_needed();
void stop_engine();

// Clean Terminal handling
void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h\033[0m\n"); // Show cursor, reset styling
}

void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

// Chess Engine Rules Implementation
void init_board(BoardState *state) {
    memset(state, 0, sizeof(BoardState));
    int back_row[] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
    for (int col = 0; col < 8; col++) {
        state->board[0 * 8 + col] = BLACK | back_row[col];
        state->board[1 * 8 + col] = BLACK | PAWN;
        state->board[6 * 8 + col] = WHITE | PAWN;
        state->board[7 * 8 + col] = WHITE | back_row[col];
    }
    state->turn = WHITE;
    state->castle = 15;
    state->ep_sq = -1;
    state->fullmove = 1;
    state->last_move = (Move){-1, -1, 0};
}

int is_on_board(int r, int c) {
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}

int is_square_attacked(int board[64], int sq, int attacker_color) {
    int r = sq / 8, c = sq % 8;
    int opponent = attacker_color;

    // Knight attacks
    int kn_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int kn_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn_r[i], nc = c + kn_c[i];
        if (is_on_board(nr, nc)) {
            int p = board[nr * 8 + nc];
            if ((p & COLOR_MASK) == opponent && (p & TYPE_MASK) == KNIGHT) return 1;
        }
    }

    // King attacks
    int k_r[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int k_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + k_r[i], nc = c + k_c[i];
        if (is_on_board(nr, nc)) {
            int p = board[nr * 8 + nc];
            if ((p & COLOR_MASK) == opponent && (p & TYPE_MASK) == KING) return 1;
        }
    }

    // Pawn attacks
    int p_dir = (opponent == WHITE) ? 1 : -1;
    int p_row = r + p_dir;
    if (p_row >= 0 && p_row < 8) {
        if (c - 1 >= 0) {
            int p = board[p_row * 8 + c - 1];
            if ((p & COLOR_MASK) == opponent && (p & TYPE_MASK) == PAWN) return 1;
        }
        if (c + 1 < 8) {
            int p = board[p_row * 8 + c + 1];
            if ((p & COLOR_MASK) == opponent && (p & TYPE_MASK) == PAWN) return 1;
        }
    }

    // Rook / Queen slide attacks
    int r_dirs[4][2] = {{1,0}, {-1,0}, {0,1}, {0,-1}};
    for (int d = 0; d < 4; d++) {
        int nr = r + r_dirs[d][0], nc = c + r_dirs[d][1];
        while (is_on_board(nr, nc)) {
            int p = board[nr * 8 + nc];
            if (p != EMPTY) {
                if ((p & COLOR_MASK) == opponent && ((p & TYPE_MASK) == ROOK || (p & TYPE_MASK) == QUEEN)) return 1;
                break;
            }
            nr += r_dirs[d][0]; nc += r_dirs[d][1];
        }
    }

    // Bishop / Queen slide attacks
    int b_dirs[4][2] = {{1,1}, {1,-1}, {-1,1}, {-1,-1}};
    for (int d = 0; d < 4; d++) {
        int nr = r + b_dirs[d][0], nc = c + b_dirs[d][1];
        while (is_on_board(nr, nc)) {
            int p = board[nr * 8 + nc];
            if (p != EMPTY) {
                if ((p & COLOR_MASK) == opponent && ((p & TYPE_MASK) == BISHOP || (p & TYPE_MASK) == QUEEN)) return 1;
                break;
            }
            nr += b_dirs[d][0]; nc += b_dirs[d][1];
        }
    }
    return 0;
}

int generate_moves(BoardState *state, Move moves[256]) {
    int count = 0;
    int side = state->turn;
    int opponent = (side == WHITE) ? BLACK : WHITE;

    for (int sq = 0; sq < 64; sq++) {
        int p = state->board[sq];
        if (p == EMPTY || (p & COLOR_MASK) != side) continue;
        int type = p & TYPE_MASK;
        int r = sq / 8, c = sq % 8;

        if (type == PAWN) {
            int dir = (side == WHITE) ? -1 : 1;
            int nr = r + dir;
            if (is_on_board(nr, c) && state->board[nr * 8 + c] == EMPTY) {
                if (nr == 0 || nr == 7) {
                    moves[count++] = (Move){sq, nr * 8 + c, QUEEN};
                    moves[count++] = (Move){sq, nr * 8 + c, ROOK};
                    moves[count++] = (Move){sq, nr * 8 + c, BISHOP};
                    moves[count++] = (Move){sq, nr * 8 + c, KNIGHT};
                } else {
                    moves[count++] = (Move){sq, nr * 8 + c, 0};
                }
                int start_row = (side == WHITE) ? 6 : 1;
                int nnr = r + 2 * dir;
                if (r == start_row && state->board[nnr * 8 + c] == EMPTY) {
                    moves[count++] = (Move){sq, nnr * 8 + c, 0};
                }
            }
            int dc_opts[2] = {-1, 1};
            for (int i = 0; i < 2; i++) {
                int nc = c + dc_opts[i];
                int nr = r + dir;
                if (is_on_board(nr, nc)) {
                    int target = state->board[nr * 8 + nc];
                    if (target != EMPTY && (target & COLOR_MASK) == opponent) {
                        if (nr == 0 || nr == 7) {
                            moves[count++] = (Move){sq, nr * 8 + nc, QUEEN};
                            moves[count++] = (Move){sq, nr * 8 + nc, ROOK};
                            moves[count++] = (Move){sq, nr * 8 + nc, BISHOP};
                            moves[count++] = (Move){sq, nr * 8 + nc, KNIGHT};
                        } else {
                            moves[count++] = (Move){sq, nr * 8 + nc, 0};
                        }
                    }
                    if (state->ep_sq == nr * 8 + nc) {
                        moves[count++] = (Move){sq, state->ep_sq, 0};
                    }
                }
            }
        } else if (type == KNIGHT) {
            int kn_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
            int kn_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
            for (int i = 0; i < 8; i++) {
                int nr = r + kn_r[i], nc = c + kn_c[i];
                if (is_on_board(nr, nc)) {
                    int target = state->board[nr * 8 + nc];
                    if (target == EMPTY || (target & COLOR_MASK) == opponent) {
                        moves[count++] = (Move){sq, nr * 8 + nc, 0};
                    }
                }
            }
        } else if (type == BISHOP || type == QUEEN || type == ROOK) {
            int dirs[8][2];
            int d_count = 0;
            if (type == BISHOP || type == QUEEN) {
                int b_dirs[4][2] = {{1,1}, {1,-1}, {-1,1}, {-1,-1}};
                for (int i=0; i<4; i++) { dirs[d_count][0] = b_dirs[i][0]; dirs[d_count][1] = b_dirs[i][1]; d_count++; }
            }
            if (type == ROOK || type == QUEEN) {
                int r_dirs[4][2] = {{1,0}, {-1,0}, {0,1}, {0,-1}};
                for (int i=0; i<4; i++) { dirs[d_count][0] = r_dirs[i][0]; dirs[d_count][1] = r_dirs[i][1]; d_count++; }
            }
            for (int d = 0; d < d_count; d++) {
                int nr = r + dirs[d][0], nc = c + dirs[d][1];
                while (is_on_board(nr, nc)) {
                    int target = state->board[nr * 8 + nc];
                    if (target == EMPTY) {
                        moves[count++] = (Move){sq, nr * 8 + nc, 0};
                    } else {
                        if ((target & COLOR_MASK) == opponent) {
                            moves[count++] = (Move){sq, nr * 8 + nc, 0};
                        }
                        break;
                    }
                    nr += dirs[d][0]; nc += dirs[d][1];
                }
            }
        } else if (type == KING) {
            int k_r[] = {-1, -1, -1, 0, 0, 1, 1, 1};
            int k_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
            for (int i = 0; i < 8; i++) {
                int nr = r + k_r[i], nc = c + k_c[i];
                if (is_on_board(nr, nc)) {
                    int target = state->board[nr * 8 + nc];
                    if (target == EMPTY || (target & COLOR_MASK) == opponent) {
                        moves[count++] = (Move){sq, nr * 8 + nc, 0};
                    }
                }
            }
            // Castling
            if (side == WHITE) {
                if ((state->castle & 1) && state->board[60] == (WHITE|KING)) {
                    if (state->board[61] == EMPTY && state->board[62] == EMPTY) {
                        if (!is_square_attacked(state->board, 60, BLACK) &&
                            !is_square_attacked(state->board, 61, BLACK) &&
                            !is_square_attacked(state->board, 62, BLACK)) {
                            moves[count++] = (Move){60, 62, 0};
                        }
                    }
                }
                if ((state->castle & 2) && state->board[60] == (WHITE|KING)) {
                    if (state->board[59] == EMPTY && state->board[58] == EMPTY && state->board[57] == EMPTY) {
                        if (!is_square_attacked(state->board, 60, BLACK) &&
                            !is_square_attacked(state->board, 59, BLACK) &&
                            !is_square_attacked(state->board, 58, BLACK)) {
                            moves[count++] = (Move){60, 58, 0};
                        }
                    }
                }
            } else {
                if ((state->castle & 4) && state->board[4] == (BLACK|KING)) {
                    if (state->board[5] == EMPTY && state->board[6] == EMPTY) {
                        if (!is_square_attacked(state->board, 4, WHITE) &&
                            !is_square_attacked(state->board, 5, WHITE) &&
                            !is_square_attacked(state->board, 6, WHITE)) {
                            moves[count++] = (Move){4, 6, 0};
                        }
                    }
                }
                if ((state->castle & 8) && state->board[4] == (BLACK|KING)) {
                    if (state->board[3] == EMPTY && state->board[2] == EMPTY && state->board[1] == EMPTY) {
                        if (!is_square_attacked(state->board, 4, WHITE) &&
                            !is_square_attacked(state->board, 3, WHITE) &&
                            !is_square_attacked(state->board, 2, WHITE)) {
                            moves[count++] = (Move){4, 2, 0};
                        }
                    }
                }
            }
        }
    }
    return count;
}

int make_move(const BoardState *src, BoardState *dst, Move m) {
    *dst = *src;
    int side = src->turn;
    int opponent = (side == WHITE) ? BLACK : WHITE;

    int piece = dst->board[m.from];
    int type = piece & TYPE_MASK;

    if (type == PAWN && m.to == dst->ep_sq) {
        int cap_sq = m.to + (side == WHITE ? 8 : -8);
        dst->board[cap_sq] = EMPTY;
    }

    dst->ep_sq = -1;
    if (type == PAWN && abs(m.to - m.from) == 16) {
        dst->ep_sq = m.from + (side == WHITE ? -8 : 8);
    }

    dst->board[m.to] = piece;
    dst->board[m.from] = EMPTY;

    if (type == PAWN && m.promotion != 0) {
        dst->board[m.to] = side | m.promotion;
    }

    if (type == KING) {
        if (m.from == 60 && m.to == 62) {
            dst->board[61] = dst->board[63]; dst->board[63] = EMPTY;
        } else if (m.from == 60 && m.to == 58) {
            dst->board[59] = dst->board[56]; dst->board[56] = EMPTY;
        } else if (m.from == 4 && m.to == 6) {
            dst->board[5] = dst->board[7]; dst->board[7] = EMPTY;
        } else if (m.from == 4 && m.to == 2) {
            dst->board[3] = dst->board[0]; dst->board[0] = EMPTY;
        }
    }

    if (type == KING) {
        if (side == WHITE) dst->castle &= ~3;
        else dst->castle &= ~12;
    }
    if (m.from == 56 || m.to == 56) dst->castle &= ~2;
    if (m.from == 63 || m.to == 63) dst->castle &= ~1;
    if (m.from == 0 || m.to == 0) dst->castle &= ~8;
    if (m.from == 7 || m.to == 7) dst->castle &= ~4;

    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (dst->board[i] == (side | KING)) {
            king_sq = i;
            break;
        }
    }
    if (king_sq != -1 && is_square_attacked(dst->board, king_sq, opponent)) {
        return 0; // Leaves king in check
    }

    dst->turn = opponent;
    if (dst->turn == WHITE) {
        dst->fullmove++;
    }
    return 1;
}

int get_legal_moves(BoardState *state, Move moves[256]) {
    Move temp_moves[256];
    int count = generate_moves(state, temp_moves);
    int legal_count = 0;
    for (int i = 0; i < count; i++) {
        BoardState dummy;
        if (make_move(state, &dummy, temp_moves[i])) {
            moves[legal_count++] = temp_moves[i];
        }
    }
    return legal_count;
}

// Generate Standard Algebraic Notation
void get_move_san(BoardState *state, Move m, char *san) {
    int piece = state->board[m.from];
    int type = piece & TYPE_MASK;
    int side = state->turn;
    int opponent = (side == WHITE) ? BLACK : WHITE;

    if (type == KING) {
        if (m.from == 60 && m.to == 62) { strcpy(san, "O-O"); goto check_suffix; }
        if (m.from == 60 && m.to == 58) { strcpy(san, "O-O-O"); goto check_suffix; }
        if (m.from == 4 && m.to == 6) { strcpy(san, "O-O"); goto check_suffix; }
        if (m.from == 4 && m.to == 2) { strcpy(san, "O-O-O"); goto check_suffix; }
    }

    char p_char = '\0';
    if (type == KNIGHT) p_char = 'N';
    else if (type == BISHOP) p_char = 'B';
    else if (type == ROOK) p_char = 'R';
    else if (type == QUEEN) p_char = 'Q';
    else if (type == KING) p_char = 'K';

    int is_capture = (state->board[m.to] != EMPTY) || (type == PAWN && m.to == state->ep_sq);
    int len = 0;

    if (type == PAWN) {
        if (is_capture) {
            san[len++] = 'a' + (m.from % 8);
            san[len++] = 'x';
        }
        san[len++] = 'a' + (m.to % 8);
        san[len++] = '8' - (m.to / 8);
        if (m.promotion) {
            san[len++] = '=';
            if (m.promotion == QUEEN) san[len++] = 'Q';
            else if (m.promotion == ROOK) san[len++] = 'R';
            else if (m.promotion == BISHOP) san[len++] = 'B';
            else if (m.promotion == KNIGHT) san[len++] = 'N';
        }
    } else {
        san[len++] = p_char;
        Move all_moves[256];
        int m_count = get_legal_moves(state, all_moves);
        int need_file = 0, need_rank = 0, alt_count = 0;
        for (int i = 0; i < m_count; i++) {
            Move alt = all_moves[i];
            if (alt.from != m.from && alt.to == m.to && state->board[alt.from] == piece) {
                alt_count++;
                if ((alt.from % 8) == (m.from % 8)) need_rank = 1;
                else need_file = 1;
            }
        }
        if (alt_count > 0) {
            if (!need_file && !need_rank) need_file = 1;
            if (need_file) san[len++] = 'a' + (m.from % 8);
            if (need_rank) san[len++] = '8' - (m.from / 8);
        }
        if (is_capture) san[len++] = 'x';
        san[len++] = 'a' + (m.to % 8);
        san[len++] = '8' - (m.to / 8);
    }
    san[len] = '\0';

check_suffix:;
    BoardState next;
    if (make_move(state, &next, m)) {
        int opp_king_sq = -1;
        for (int i = 0; i < 64; i++) {
            if (next.board[i] == (opponent | KING)) { opp_king_sq = i; break; }
        }
        int in_check = 0;
        if (opp_king_sq != -1) {
            in_check = is_square_attacked(next.board, opp_king_sq, side);
        }
        Move opp_moves[256];
        int opp_legal = get_legal_moves(&next, opp_moves);
        if (in_check) {
            if (opp_legal == 0) strcat(san, "#");
            else strcat(san, "+");
        }
    }
}

void move_to_uci(Move m, char *buf) {
    buf[0] = 'a' + (m.from % 8);
    buf[1] = '8' - (m.from / 8);
    buf[2] = 'a' + (m.to % 8);
    buf[3] = '8' - (m.to / 8);
    if (m.promotion) {
        if (m.promotion == QUEEN) buf[4] = 'q';
        else if (m.promotion == ROOK) buf[4] = 'r';
        else if (m.promotion == BISHOP) buf[4] = 'b';
        else if (m.promotion == KNIGHT) buf[4] = 'n';
        buf[5] = '\0';
    } else {
        buf[4] = '\0';
    }
}

// Spawning UCI Process via standard Unix pipes
void start_engine(const char *path) {
    int pipe_to_eng[2], pipe_from_eng[2];
    if (pipe(pipe_to_eng) < 0 || pipe(pipe_from_eng) < 0) return;

    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(pipe_to_eng[0], STDIN_FILENO);
        dup2(pipe_from_eng[1], STDOUT_FILENO);
        close(pipe_to_eng[1]);
        close(pipe_from_eng[0]);
        char *argv[] = {(char *)path, NULL};
        execvp(path, argv);
        exit(1);
    } else {
        close(pipe_to_eng[0]);
        close(pipe_from_eng[1]);
        engine_in = pipe_to_eng[1];
        engine_out = pipe_from_eng[0];

        int flags = fcntl(engine_out, F_GETFL, 0);
        fcntl(engine_out, F_SETFL, flags | O_NONBLOCK);

        write(engine_in, "uci\nisready\nuccinewgame\n", 25);
    }
}

void stop_engine() {
    if (engine_pid > 0) {
        write(engine_in, "quit\n", 5);
        close(engine_in);
        close(engine_out);
        kill(engine_pid, SIGTERM);
        engine_pid = -1;
    }
}

void send_position_and_go() {
    if (engine_in == -1) return;
    char cmd[8192] = "position startpos moves";
    int len = strlen(cmd);
    for (int i = 1; i < history_count; i++) {
        char uci_m[10];
        move_to_uci(history[i].last_move, uci_m);
        len += snprintf(cmd + len, sizeof(cmd) - len, " %s", uci_m);
    }
    len += snprintf(cmd + len, sizeof(cmd) - len, "\n");
    write(engine_in, cmd, len);

    char go_cmd[128];
    SearchLimit sl = limits[current_limit_idx];
    if (sl.type == LIMIT_DEPTH) {
        snprintf(go_cmd, sizeof(go_cmd), "go depth %d\n", sl.value);
    } else if (sl.type == LIMIT_NODES) {
        snprintf(go_cmd, sizeof(go_cmd), "go nodes %d\n", sl.value);
    } else {
        snprintf(go_cmd, sizeof(go_cmd), "go movetime %d\n", sl.value);
    }
    write(engine_in, go_cmd, strlen(go_cmd));
}

void process_engine_line(const char *line) {
    if (strncmp(line, "bestmove ", 9) == 0) {
        char move_str[16];
        if (sscanf(line + 9, "%s", move_str) == 1) {
            if (strlen(move_str) >= 4) {
                int from = ( '8' - move_str[1] ) * 8 + ( move_str[0] - 'a' );
                int to = ( '8' - move_str[3] ) * 8 + ( move_str[2] - 'a' );
                int promo = 0;
                if (strlen(move_str) == 5) {
                    char p = move_str[4];
                    if (p == 'q') promo = QUEEN;
                    else if (p == 'r') promo = ROOK;
                    else if (p == 'b') promo = BISHOP;
                    else if (p == 'n') promo = KNIGHT;
                }
                Move m = (Move){from, to, promo};
                BoardState next;
                if (make_move(&history[history_count - 1], &next, m)) {
                    get_move_san(&history[history_count - 1], m, next.last_move_str);
                    next.last_move = m;
                    history[history_count++] = next;
                    engine_thinking = 0;
                }
            }
        }
    }
}

char engine_read_buf[4096];
int engine_read_len = 0;

void check_engine_input() {
    if (engine_out == -1) return;
    char chunk[1024];
    ssize_t n;
    while ((n = read(engine_out, chunk, sizeof(chunk) - 1)) > 0) {
        chunk[n] = '\0';
        for (int i = 0; i < n; i++) {
            if (engine_read_len < (int)sizeof(engine_read_buf) - 2) {
                engine_read_buf[engine_read_len++] = chunk[i];
            }
            if (chunk[i] == '\n') {
                engine_read_buf[engine_read_len] = '\0';
                process_engine_line(engine_read_buf);
                engine_read_len = 0;
            }
        }
    }
}

void trigger_engine_if_needed() {
    BoardState *curr = &history[history_count - 1];
    int is_eng = 0;
    if (play_mode == VS_ENGINE_BLACK && curr->turn == BLACK) is_eng = 1;
    else if (play_mode == VS_ENGINE_WHITE && curr->turn == WHITE) is_eng = 1;
    else if (play_mode == ENGINE_VS_ENGINE) is_eng = 1;

    if (is_eng && !engine_thinking) {
        // Stop search if no moves left
        Move moves[256];
        if (get_legal_moves(curr, moves) > 0) {
            engine_thinking = 1;
            send_position_and_go();
        }
    }
}

// Delimited wrapping algorithm for displaying PGN Sidebar
int word_wrap(const char *input, int width, char lines[][128], int max_lines) {
    int line_idx = 0;
    const char *p = input;
    while (*p && line_idx < max_lines) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *end = p;
        const char *last_space = NULL;
        int count = 0;
        while (*end && count < width) {
            if (*end == ' ') last_space = end;
            end++;
            count++;
        }
        if (!*end || count < width) {
            snprintf(lines[line_idx], 128, "%.*s", (int)(end - p), p);
            line_idx++;
            break;
        }
        if (last_space && last_space > p) {
            snprintf(lines[line_idx], 128, "%.*s", (int)(last_space - p), p);
            p = last_space + 1;
        } else {
            snprintf(lines[line_idx], 128, "%.*s", width, p);
            p += width;
        }
        line_idx++;
    }
    return line_idx;
}

// Dynamic Terminal UI Drawing
const char *piece_symbols[] = {"", "♟", "♞", "♝", "♜", "♛", "♚"};

void print_square(int sq, int r, int c, int selected, int legal_targets[64], int last_from, int last_to, int check_sq, int piece) {
    int bg = -1;
    int is_dark = (r + c) % 2;

    if (sq == check_sq) {
        bg = 196; // Red Highlight for Check
    } else if (sq == selected) {
        bg = 214; // Bright Orange Highlight
    } else if (legal_targets[sq]) {
        bg = is_dark ? 29 : 114; // Green / Forest Green shading for Legal targets
    } else if (sq == last_from || sq == last_to) {
        bg = is_dark ? 24 : 117; // Blue shading for Previous move
    } else {
        bg = is_dark ? 240 : 251; // Cream/Beige contrast board squares
    }

    printf("\033[48;5;%dm", bg);

    if (piece == EMPTY) {
        printf("   ");
    } else {
        int color = piece & COLOR_MASK;
        int type = piece & TYPE_MASK;
        int fg = (color == WHITE) ? 231 : 16; // Standardize High-contrast Foreground
        printf("\033[38;5;%dm %s ", fg, piece_symbols[type]);
    }
}

void draw_ui() {
    printf("\033[H"); // Home cursor, redraw in place (stops screen-flicker)
    printf("\033[2K\033[1;36m  === CHESS TERMINAL GUI ===\033[0m   Engine status: %s\n", 
           (engine_pid > 0) ? "\033[32mActive\033[0m" : "\033[31mInative\033[0m");

    BoardState *curr = &history[history_count - 1];
    int check_sq = -1;
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (curr->board[i] == (curr->turn | KING)) { king_sq = i; break; }
    }
    if (king_sq != -1 && is_square_attacked(curr->board, king_sq, (curr->turn == WHITE) ? BLACK : WHITE)) {
        check_sq = king_sq;
    }

    int legal_targets[64] = {0};
    if (selected_sq != -1) {
        Move moves[256];
        int m_count = get_legal_moves(curr, moves);
        for (int i = 0; i < m_count; i++) {
            if (moves[i].from == selected_sq) legal_targets[moves[i].to] = 1;
        }
    }

    // Build SAN history buffer for Sidebar PGN display
    char pgn_buf[8192] = "";
    int pgn_len = 0;
    for (int i = 1; i < history_count; i++) {
        if (i % 2 == 1) {
            pgn_len += snprintf(pgn_buf + pgn_len, sizeof(pgn_buf) - pgn_len, "%d. %s", (i + 1) / 2, history[i].last_move_str);
        } else {
            pgn_len += snprintf(pgn_buf + pgn_len, sizeof(pgn_buf) - pgn_len, " %s  ", history[i].last_move_str);
        }
    }
    char pgn_lines[20][128];
    int wrapped_count = word_wrap(pgn_buf, 35, pgn_lines, 20);

    for (int r = 0; r < 8; r++) {
        printf("\033[2K  %d ", 8 - r);
        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            print_square(sq, r, c, (sq == cursor_sq) ? cursor_sq : selected_sq, legal_targets, curr->last_move.from, curr->last_move.to, check_sq, curr->board[sq]);
        }
        printf("\033[0m │ ");

        // UI Metadata Sidebar
        switch (r) {
            case 0: printf("\033[1mActive Side:\033[0m %s", (curr->turn == WHITE) ? "White" : "Black"); break;
            case 1: 
                printf("\033[1mGame Mode:\033[0m ");
                if (play_mode == VS_ENGINE_BLACK) printf("Player (W) vs Engine (B)");
                else if (play_mode == VS_ENGINE_WHITE) printf("Engine (W) vs Player (B)");
                else if (play_mode == TWO_PLAYER) printf("Local Multiplayer");
                else printf("Engine vs Engine Simulation");
                break;
            case 2:
                printf("\033[1mTime Control Limit:\033[0m ");
                SearchLimit sl = limits[current_limit_idx];
                if (sl.type == LIMIT_DEPTH) printf("Depth max: %d ply", sl.value);
                else if (sl.type == LIMIT_NODES) printf("Nodes max: %d", sl.value);
                else printf("Engine Thinktime: %d ms", sl.value);
                break;
            case 3: printf("\033[1mPGN Game Logs:\033[0m"); break;
            default: {
                int line_idx = r - 4;
                int start_line = wrapped_count - 4;
                if (start_line < 0) start_line = 0;
                int target = start_line + line_idx;
                if (target < wrapped_count) printf("%s", pgn_lines[target]);
            } break;
        }
        printf("\n");
    }

    printf("\033[2K     A  B  C  D  E  F  G  H     │\n");
    printf("\033[2K────────────────────────────────┼────────────────────────────────────────\n");
    printf("\033[2K \033[1mControl Keys:\033[0m                  │ \033[1mPlay Status:\033[0m\n");
    printf("\033[2K  [WASD / HJKL / Arrows] Move   │  ");

    if (engine_thinking) {
        printf("\033[33mEngine thinking...\033[0m\n");
    } else {
        Move moves[256];
        if (get_legal_moves(curr, moves) == 0) {
            if (check_sq != -1) printf("\033[1;31mCHECKMATE! %s Wins.\033[0m\n", (curr->turn == WHITE) ? "Black" : "White");
            else printf("\033[1;33mSTALEMATE! Game Draw.\033[0m\n");
        } else {
            if (check_sq != -1) printf("\033[1;31mCHECK!\033[0m\n");
            else printf("Game Ready\n");
        }
    }
    printf("\033[2K  [Space/Enter]   Select/Move   │  Target Engine: %s\n", engine_path);
    printf("\033[2K  [U]             Undo Move     │  (Cycle limits: [T] | Cycle modes: [M])\n");
    printf("\033[2K  [Q]             Quit Game     │\n");
    fflush(stdout);
}

void handle_select() {
    BoardState *curr = &history[history_count - 1];
    if (selected_sq == -1) {
        int p = curr->board[cursor_sq];
        if (p != EMPTY && (p & COLOR_MASK) == curr->turn) {
            selected_sq = cursor_sq;
        }
    } else {
        if (cursor_sq == selected_sq) {
            selected_sq = -1;
        } else {
            Move moves[256];
            int m_count = get_legal_moves(curr, moves);
            int idx = -1;
            for (int i = 0; i < m_count; i++) {
                if (moves[i].from == selected_sq && moves[i].to == cursor_sq) {
                    idx = i;
                    break;
                }
            }

            if (idx != -1) {
                Move m = moves[idx];
                int piece = curr->board[selected_sq];
                if ((piece & TYPE_MASK) == PAWN && (cursor_sq / 8 == 0 || cursor_sq / 8 == 7)) {
                    // Quick ASCII Promotion Selection Prompt
                    disableRawMode();
                    printf("\nPromote Pawn! Pick [Q]ueen, [R]ook, [B]ishop, [N]ight: ");
                    fflush(stdout);
                    char c = 'q';
                    while (1) {
                        int r = read(STDIN_FILENO, &c, 1);
                        if (r > 0) {
                            if (c == 'q' || c == 'Q') { m.promotion = QUEEN; break; }
                            if (c == 'r' || c == 'R') { m.promotion = ROOK; break; }
                            if (c == 'b' || c == 'B') { m.promotion = BISHOP; break; }
                            if (c == 'n' || c == 'N') { m.promotion = KNIGHT; break; }
                        }
                    }
                    enableRawMode();
                }

                BoardState next;
                if (make_move(curr, &next, m)) {
                    get_move_san(curr, m, next.last_move_str);
                    next.last_move = m;
                    history[history_count++] = next;
                    selected_sq = -1;
                    trigger_engine_if_needed();
                }
            } else {
                // Instantly reselect another piece of the active player
                int p = curr->board[cursor_sq];
                if (p != EMPTY && (p & COLOR_MASK) == curr->turn) {
                    selected_sq = cursor_sq;
                }
            }
        }
    }
}

void handle_undo() {
    if (play_mode == VS_ENGINE_BLACK || play_mode == VS_ENGINE_WHITE) {
        if (history_count >= 3) {
            history_count -= 2;
            selected_sq = -1;
            engine_thinking = 0;
        }
    } else {
        if (history_count >= 2) {
            history_count--;
            selected_sq = -1;
            engine_thinking = 0;
        }
    }
}

int is_key_pressed() {
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    return poll(&pfd, 1, 15) > 0;
}

int read_key() {
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == '\033') {
            char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\033';
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\033';
            if (seq[0] == '[') {
                switch (seq[1]) {
                    case 'A': return 'k'; // Map Arrow Keys to HJKL standard controls
                    case 'B': return 'j';
                    case 'C': return 'l';
                    case 'D': return 'h';
                }
            }
            return '\033';
        }
        return c;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        strncpy(engine_path, argv[1], sizeof(engine_path) - 1);
    }

    printf("\033[2J"); // Initial Clear Board
    init_board(&history[0]);
    history_count = 1;

    start_engine(engine_path);
    enableRawMode();

    // Trigger engine right away if playing White
    trigger_engine_if_needed();

    while (running) {
        check_engine_input();

        if (play_mode == ENGINE_VS_ENGINE && !engine_thinking) {
            trigger_engine_if_needed();
        }

        if (is_key_pressed()) {
            int key = read_key();
            if (key == 'q' || key == 'Q') {
                running = 0;
            } else if (key == 'u' || key == 'U') {
                handle_undo();
            } else if (key == 't' || key == 'T') {
                current_limit_idx = (current_limit_idx + 1) % num_limits;
            } else if (key == 'm' || key == 'M') {
                play_mode = (play_mode + 1) % 4;
                trigger_engine_if_needed();
            } else if (key == 'w' || key == 'W' || key == 'k') {
                int r = cursor_sq / 8;
                if (r > 0) cursor_sq = (r - 1) * 8 + (cursor_sq % 8);
            } else if (key == 's' || key == 'S' || key == 'j') {
                int r = cursor_sq / 8;
                if (r < 7) cursor_sq = (r + 1) * 8 + (cursor_sq % 8);
            } else if (key == 'a' || key == 'A' || key == 'h') {
                int c = cursor_sq % 8;
                if (c > 0) cursor_sq = (cursor_sq / 8) * 8 + (c - 1);
            } else if (key == 'd' || key == 'D' || key == 'l') {
                int c = cursor_sq % 8;
                if (c < 7) cursor_sq = (cursor_sq / 8) * 8 + (c + 1);
            } else if (key == ' ' || key == '\r' || key == '\n') {
                handle_select();
            }
        }

        draw_ui();
        usleep(10000); // 10ms frame throttling
    }

    stop_engine();
    return 0;
}
