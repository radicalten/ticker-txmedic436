#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>

/* Board Mailbox Indexes */
#define FR_TO_SQ(f, r) ((r + 2) * 10 + (f + 1))
#define SQ_TO_FILE(sq) ((sq % 10) - 1)
#define SQ_TO_RANK(sq) ((sq / 10) - 2)

enum { EMPTY, WP, WN, WB, WR, WQ, WK, BP, BN, BB, BR, BQ, BK, OFFBOARD = 99 };

#define PIECE_COLOR(p) ((p) == EMPTY ? -1 : ((p) >= WP && (p) <= WK ? 0 : 1))

typedef struct {
    int board[120];
    int side;              /* 0 = White, 1 = Black */
    int castling[4];       /* WK, WQ, BK, BQ */
    int en_passant;        /* Mailbox index or 0 */
} BoardState;

typedef struct {
    int from;
    int to;
    int promotion;
    char san[16];          /* Standard Algebraic Notation */
} Move;

typedef struct {
    BoardState state;
    Move last_move;
} HistoryEntry;

typedef struct {
    Move moves[256];
    int count;
} MoveList;

/* Global State */
struct termios orig_termios;
BoardState board_state;
MoveList current_legal_moves;
Move last_move = {0, 0, 0, ""};

#define MAX_HISTORY 2048
HistoryEntry history[MAX_HISTORY];
int history_count = 0;

int cursor_x = 4; /* e file */
int cursor_y = 1; /* 2nd rank */
int selected_sq = -1;

/* Engine Pipe Variables */
int engine_in_pipe[2];
int engine_out_pipe[2];
pid_t engine_pid = -1;
int engine_running = 0;
int engine_thinking = 0;
int engine_side = -1; /* -1: PVP, 0: White is Engine, 1: Black is Engine, 2: Both */
char engine_path[256] = "";

/* Time Control Options */
int tc_mode = 0;   /* 0 = Depth, 1 = Nodes, 2 = Movetime */
int tc_value = 10; /* Default: Depth 10 */

char status_msg[128] = "Welcome! Use arrow keys to select and space to move.";

/* Prototypes */
void generate_moves(BoardState *state, MoveList *list);
int is_square_attacked(BoardState *state, int sq, int attacker_side);
void make_move(BoardState *from, BoardState *to, Move move);
int is_move_legal(BoardState *state, Move move);

/* Restore Terminal State on exit */
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); /* Show cursor */
    fflush(stdout);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); /* Hide cursor */
    fflush(stdout);
}

/* Engine Subprocess Handling */
void stop_engine() {
    if (engine_pid > 0) {
        kill(engine_pid, SIGTERM);
        engine_pid = -1;
        engine_running = 0;
        engine_thinking = 0;
    }
}

int start_engine(const char *path) {
    stop_engine();
    if (pipe(engine_in_pipe) < 0 || pipe(engine_out_pipe) < 0) return 0;

    engine_pid = fork();
    if (engine_pid < 0) return 0;

    if (engine_pid == 0) { /* Child Process */
        dup2(engine_in_pipe[0], STDIN_FILENO);
        dup2(engine_out_pipe[1], STDOUT_FILENO);
        close(engine_in_pipe[1]);
        close(engine_out_pipe[0]);

        char *args[] = {(char *)path, NULL};
        execvp(path, args);
        exit(1); /* Exits if execution fails */
    } else { /* Parent */
        close(engine_in_pipe[0]);
        close(engine_out_pipe[1]);

        /* Set engine output to Non-Blocking */
        int flags = fcntl(engine_out_pipe[0], F_GETFL, 0);
        fcntl(engine_out_pipe[0], F_SETFL, flags | O_NONBLOCK);
        engine_running = 1;

        /* Handshake */
        write(engine_in_pipe[1], "uci\n", 4);
        write(engine_in_pipe[1], "isready\n", 8);
        return 1;
    }
}

void send_to_engine(const char *cmd) {
    if (!engine_running) return;
    write(engine_in_pipe[1], cmd, strlen(cmd));
    write(engine_in_pipe[1], "\n", 1);
}

char engine_buf[8192];
int engine_buf_len = 0;

int parse_engine_output(char *move_str) {
    if (!engine_running) return 0;
    char temp_buf[1024];
    int n = read(engine_out_pipe[0], temp_buf, sizeof(temp_buf) - 1);
    if (n <= 0) return 0;
    temp_buf[n] = '\0';

    for (int i = 0; i < n; i++) {
        if (engine_buf_len < sizeof(engine_buf) - 2) {
            engine_buf[engine_buf_len++] = temp_buf[i];
        }
    }
    engine_buf[engine_buf_len] = '\0';

    char *line_start = engine_buf;
    char *newline;
    int parsed_bestmove = 0;
    while ((newline = strchr(line_start, '\n')) != NULL) {
        *newline = '\0';
        if (strncmp(line_start, "bestmove ", 9) == 0) {
            sscanf(line_start + 9, "%s", move_str);
            parsed_bestmove = 1;
        }
        line_start = newline + 1;
    }

    int consumed = line_start - engine_buf;
    if (consumed > 0) {
        memmove(engine_buf, line_start, engine_buf_len - consumed);
        engine_buf_len -= consumed;
        engine_buf[engine_buf_len] = '\0';
    }
    return parsed_bestmove;
}

/* Chess Setup */
void init_board(BoardState *state) {
    for (int i = 0; i < 120; i++) state->board[i] = OFFBOARD;
    for (int r = 0; r < 8; r++) {
        for (int f = 0; f < 8; f++) {
            state->board[FR_TO_SQ(f, r)] = EMPTY;
        }
    }
    /* Set pawns */
    for (int f = 0; f < 8; f++) {
        state->board[FR_TO_SQ(f, 1)] = WP;
        state->board[FR_TO_SQ(f, 6)] = BP;
    }
    /* Set pieces */
    int pieces[] = {WR, WN, WB, WQ, WK, WB, WN, WR};
    int b_pieces[] = {BR, BN, BB, BQ, BK, BB, BN, BR};
    for (int f = 0; f < 8; f++) {
        state->board[FR_TO_SQ(f, 0)] = pieces[f];
        state->board[FR_TO_SQ(f, 7)] = b_pieces[f];
    }
    state->side = 0;
    state->castling[0] = state->castling[1] = state->castling[2] = state->castling[3] = 1;
    state->en_passant = 0;
}

void add_move(MoveList *list, int from, int to, int promotion) {
    if (list->count < 256) {
        list->moves[list->count].from = from;
        list->moves[list->count].to = to;
        list->moves[list->count].promotion = promotion;
        list->moves[list->count].san[0] = '\0';
        list->count++;
    }
}

/* Compact and complete pseudo-legal moves generator */
void generate_moves(BoardState *state, MoveList *list) {
    list->count = 0;
    int side = state->side;
    for (int sq = 0; sq < 120; sq++) {
        if (state->board[sq] == OFFBOARD || state->board[sq] == EMPTY) continue;
        int piece = state->board[sq];
        if (PIECE_COLOR(piece) != side) continue;

        int row = SQ_TO_RANK(sq);

        if (piece == WP || piece == BP) {
            int dir = (piece == WP) ? 10 : -10;
            int start_row = (piece == WP) ? 1 : 6;
            int promo_row = (piece == WP) ? 6 : 1;

            /* Push 1 */
            int next_sq = sq + dir;
            if (state->board[next_sq] == EMPTY) {
                if (SQ_TO_RANK(next_sq) == promo_row) {
                    add_move(list, sq, next_sq, (piece == WP) ? WQ : BQ);
                    add_move(list, sq, next_sq, (piece == WP) ? WR : BR);
                    add_move(list, sq, next_sq, (piece == WP) ? WB : BB);
                    add_move(list, sq, next_sq, (piece == WP) ? WN : BN);
                } else {
                    add_move(list, sq, next_sq, 0);
                    /* Push 2 */
                    int double_sq = sq + 2 * dir;
                    if (row == start_row && state->board[double_sq] == EMPTY) {
                        add_move(list, sq, double_sq, 0);
                    }
                }
            }
            /* Pawn Captures */
            int caps[] = {sq + dir - 1, sq + dir + 1};
            for (int i = 0; i < 2; i++) {
                int cap_sq = caps[i];
                if (state->board[cap_sq] == OFFBOARD) continue;
                if (state->board[cap_sq] != EMPTY && PIECE_COLOR(state->board[cap_sq]) != side) {
                    if (SQ_TO_RANK(cap_sq) == promo_row) {
                        add_move(list, sq, cap_sq, (piece == WP) ? WQ : BQ);
                        add_move(list, sq, cap_sq, (piece == WP) ? WR : BR);
                        add_move(list, sq, cap_sq, (piece == WP) ? WB : BB);
                        add_move(list, sq, cap_sq, (piece == WP) ? WN : BN);
                    } else {
                        add_move(list, sq, cap_sq, 0);
                    }
                } else if (cap_sq == state->en_passant) {
                    add_move(list, sq, cap_sq, 0);
                }
            }
        }
        else if (piece == WN || piece == BN) {
            int jumps[] = {-21, -19, -12, -8, 8, 12, 19, 21};
            for (int i = 0; i < 8; i++) {
                int dest = sq + jumps[i];
                if (state->board[dest] == OFFBOARD) continue;
                if (state->board[dest] == EMPTY || PIECE_COLOR(state->board[dest]) != side) {
                    add_move(list, sq, dest, 0);
                }
            }
        }
        else if (piece == WB || piece == BB || piece == WR || piece == BR || piece == WQ || piece == BQ) {
            int dirs[8];
            int num_dirs = 0;
            if (piece == WB || piece == BB || piece == WQ || piece == BQ) {
                dirs[num_dirs++] = -11; dirs[num_dirs++] = -9;
                dirs[num_dirs++] = 9;  dirs[num_dirs++] = 11;
            }
            if (piece == WR || piece == BR || piece == WQ || piece == BQ) {
                dirs[num_dirs++] = -10; dirs[num_dirs++] = -1;
                dirs[num_dirs++] = 1;   dirs[num_dirs++] = 10;
            }
            for (int d = 0; d < num_dirs; d++) {
                int step = dirs[d];
                int dest = sq + step;
                while (state->board[dest] != OFFBOARD) {
                    if (state->board[dest] == EMPTY) {
                        add_move(list, sq, dest, 0);
                    } else {
                        if (PIECE_COLOR(state->board[dest]) != side) {
                            add_move(list, sq, dest, 0);
                        }
                        break;
                    }
                    dest += step;
                }
            }
        }
        else if (piece == WK || piece == BK) {
            int dirs[] = {-11, -10, -9, -1, 1, 9, 10, 11};
            for (int i = 0; i < 8; i++) {
                int dest = sq + dirs[i];
                if (state->board[dest] == OFFBOARD) continue;
                if (state->board[dest] == EMPTY || PIECE_COLOR(state->board[dest]) != side) {
                    add_move(list, sq, dest, 0);
                }
            }
            /* Castling checks */
            if (side == 0) {
                if (state->castling[0] && state->board[26] == EMPTY && state->board[27] == EMPTY) {
                    if (!is_square_attacked(state, 25, 1) && !is_square_attacked(state, 26, 1) && !is_square_attacked(state, 27, 1))
                        add_move(list, 25, 27, 0);
                }
                if (state->castling[1] && state->board[24] == EMPTY && state->board[23] == EMPTY && state->board[22] == EMPTY) {
                    if (!is_square_attacked(state, 25, 1) && !is_square_attacked(state, 24, 1) && !is_square_attacked(state, 23, 1))
                        add_move(list, 25, 23, 0);
                }
            } else {
                if (state->castling[2] && state->board[96] == EMPTY && state->board[97] == EMPTY) {
                    if (!is_square_attacked(state, 95, 0) && !is_square_attacked(state, 96, 0) && !is_square_attacked(state, 97, 0))
                        add_move(list, 95, 97, 0);
                }
                if (state->castling[3] && state->board[94] == EMPTY && state->board[93] == EMPTY && state->board[92] == EMPTY) {
                    if (!is_square_attacked(state, 95, 0) && !is_square_attacked(state, 94, 0) && !is_square_attacked(state, 93, 0))
                        add_move(list, 95, 93, 0);
                }
            }
        }
    }
}

int is_square_attacked(BoardState *state, int sq, int attacker_side) {
    /* Pawn attacks */
    if (attacker_side == 0) {
        if (state->board[sq - 11] == WP || state->board[sq - 9] == WP) return 1;
    } else {
        if (state->board[sq + 11] == BP || state->board[sq + 9] == BP) return 1;
    }
    /* Knight attacks */
    int knight_jumps[] = {-21, -19, -12, -8, 8, 12, 19, 21};
    int enemy_knight = (attacker_side == 0) ? WN : BN;
    for (int i = 0; i < 8; i++) {
        int dest = sq + knight_jumps[i];
        if (state->board[dest] == enemy_knight) return 1;
    }
    /* King attacks */
    int king_dirs[] = {-11, -10, -9, -1, 1, 9, 10, 11};
    int enemy_king = (attacker_side == 0) ? WK : BK;
    for (int i = 0; i < 8; i++) {
        int dest = sq + king_dirs[i];
        if (state->board[dest] == enemy_king) return 1;
    }
    /* Bishop & Queen sliding */
    int b_dirs[] = {-11, -9, 9, 11};
    int enemy_bishop = (attacker_side == 0) ? WB : BB;
    int enemy_queen = (attacker_side == 0) ? WQ : BQ;
    for (int d = 0; d < 4; d++) {
        int step = b_dirs[d];
        int dest = sq + step;
        while (state->board[dest] != OFFBOARD) {
            if (state->board[dest] != EMPTY) {
                if (state->board[dest] == enemy_bishop || state->board[dest] == enemy_queen) return 1;
                break;
            }
            dest += step;
        }
    }
    /* Rook & Queen sliding */
    int r_dirs[] = {-10, -1, 1, 10};
    int enemy_rook = (attacker_side == 0) ? WR : BR;
    for (int d = 0; d < 4; d++) {
        int step = r_dirs[d];
        int dest = sq + step;
        while (state->board[dest] != OFFBOARD) {
            if (state->board[dest] != EMPTY) {
                if (state->board[dest] == enemy_rook || state->board[dest] == enemy_queen) return 1;
                break;
            }
            dest += step;
        }
    }
    return 0;
}

void make_move(BoardState *from, BoardState *to, Move move) {
    *to = *from;
    int piece = to->board[move.from];
    to->board[move.from] = EMPTY;
    to->en_passant = 0;

    if (move.promotion != 0) {
        to->board[move.to] = move.promotion;
    } else {
        to->board[move.to] = piece;
    }

    /* En Passant capture execution */
    if ((piece == WP || piece == BP) && move.to == from->en_passant) {
        int capture_sq = move.to + ((piece == WP) ? -10 : 10);
        to->board[capture_sq] = EMPTY;
    }

    /* Double push En Passant target setup */
    if (piece == WP && (move.to - move.from == 20)) to->en_passant = move.from + 10;
    if (piece == BP && (move.from - move.to == 20)) to->en_passant = move.from - 10;

    /* Castling execution */
    if (piece == WK) {
        if (move.from == 25 && move.to == 27) { to->board[28] = EMPTY; to->board[26] = WR; }
        else if (move.from == 25 && move.to == 23) { to->board[21] = EMPTY; to->board[24] = WR; }
        to->castling[0] = to->castling[1] = 0;
    } else if (piece == BK) {
        if (move.from == 95 && move.to == 97) { to->board[98] = EMPTY; to->board[96] = BR; }
        else if (move.from == 95 && move.to == 93) { to->board[91] = EMPTY; to->board[94] = BR; }
        to->castling[2] = to->castling[3] = 0;
    }

    /* Castling rights updates */
    if (move.from == 21 || move.to == 21) to->castling[1] = 0;
    if (move.from == 28 || move.to == 28) to->castling[0] = 0;
    if (move.from == 91 || move.to == 91) to->castling[3] = 0;
    if (move.from == 98 || move.to == 98) to->castling[2] = 0;

    to->side ^= 1;
}

int is_move_legal(BoardState *state, Move move) {
    BoardState temp;
    make_move(state, &temp, move);
    int active_side = state->side;
    int king_piece = (active_side == 0) ? WK : BK;
    int king_sq = -1;
    for (int sq = 0; sq < 120; sq++) {
        if (temp.board[sq] == king_piece) { king_sq = sq; break; }
    }
    if (king_sq == -1) return 0;
    return !is_square_attacked(&temp, king_sq, active_side ^ 1);
}

/* SAN Move Notation Generator */
void get_move_san(BoardState *state, Move move, char *san) {
    int piece = state->board[move.from];
    int p_type = piece > 6 ? piece - 6 : piece;

    if (p_type == WK) {
        if (move.from == 25 && move.to == 27) { strcpy(san, "O-O"); return; }
        if (move.from == 25 && move.to == 23) { strcpy(san, "O-O-O"); return; }
        if (move.from == 95 && move.to == 97) { strcpy(san, "O-O"); return; }
        if (move.from == 95 && move.to == 93) { strcpy(san, "O-O-O"); return; }
    }

    int f_from = SQ_TO_FILE(move.from);
    int f_to = SQ_TO_FILE(move.to);
    int r_to = SQ_TO_RANK(move.to);

    const char *pieces_chars = "  NBRQK";
    int is_cap = (state->board[move.to] != EMPTY) || ((piece == WP || piece == BP) && move.to == state->en_passant);

    int len = 0;
    if (p_type == WP) {
        if (is_cap) {
            san[len++] = 'a' + f_from;
            san[len++] = 'x';
        }
        san[len++] = 'a' + f_to;
        san[len++] = '1' + r_to;
        if (move.promotion != 0) {
            san[len++] = '=';
            int promo_p = move.promotion > 6 ? move.promotion - 6 : move.promotion;
            san[len++] = pieces_chars[promo_p];
        }
    } else {
        san[len++] = pieces_chars[p_type];
        if (is_cap) san[len++] = 'x';
        san[len++] = 'a' + f_to;
        san[len++] = '1' + r_to;
    }
    san[len] = '\0';

    BoardState temp;
    make_move(state, &temp, move);
    int enemy_king_piece = (state->side == 0) ? BK : WK;
    int king_sq = -1;
    for (int sq = 0; sq < 120; sq++) {
        if (temp.board[sq] == enemy_king_piece) { king_sq = sq; break; }
    }
    if (king_sq != -1 && is_square_attacked(&temp, king_sq, state->side)) {
        MoveList test_list;
        generate_moves(&temp, &test_list);
        int has_legal = 0;
        for (int i = 0; i < test_list.count; i++) {
            if (is_move_legal(&temp, test_list.moves[i])) { has_legal = 1; break; }
        }
        strcat(san, has_legal ? "+" : "#");
    }
}

Move parse_uci_move(BoardState *state, const char *uci) {
    Move m = {0, 0, 0, ""};
    if (strlen(uci) < 4) return m;
    int f_from = uci[0] - 'a';
    int r_from = uci[1] - '1';
    int f_to = uci[2] - 'a';
    int r_to = uci[3] - '1';

    m.from = FR_TO_SQ(f_from, r_from);
    m.to = FR_TO_SQ(f_to, r_to);

    if (uci[4] != '\0') {
        char p = uci[4];
        if (state->side == 0) {
            if (p == 'n') m.promotion = WN;
            else if (p == 'b') m.promotion = WB;
            else if (p == 'r') m.promotion = WR;
            else if (p == 'q') m.promotion = WQ;
        } else {
            if (p == 'n') m.promotion = BN;
            else if (p == 'b') m.promotion = BB;
            else if (p == 'r') m.promotion = BR;
            else if (p == 'q') m.promotion = BQ;
        }
    }
    return m;
}

void construct_engine_moves_string(char *buf, int max_len) {
    int len = snprintf(buf, max_len, "position startpos moves");
    for (int i = 0; i < history_count; i++) {
        Move m = history[i].last_move;
        int f_from = SQ_TO_FILE(m.from);
        int r_from = SQ_TO_RANK(m.from);
        int f_to = SQ_TO_FILE(m.to);
        int r_to = SQ_TO_RANK(m.to);
        char p_char = '\0';
        if (m.promotion != 0) {
            int p_norm = m.promotion > 6 ? m.promotion - 6 : m.promotion;
            if (p_norm == WN) p_char = 'n';
            else if (p_norm == WB) p_char = 'b';
            else if (p_norm == WR) p_char = 'r';
            else if (p_norm == WQ) p_char = 'q';
        }
        if (p_char) {
            len += snprintf(buf + len, max_len - len, " %c%c%c%c%c", 'a' + f_from, '1' + r_from, 'a' + f_to, '1' + r_to, p_char);
        } else {
            len += snprintf(buf + len, max_len - len, " %c%c%c%c", 'a' + f_from, '1' + r_from, 'a' + f_to, '1' + r_to);
        }
    }
}

/* Text & Graphics Colors */
const char *piece_str(int p) {
    switch (p) {
        case WP: return "♙"; case WN: return "♘"; case WB: return "♗";
        case WR: return "♖"; case WQ: return "♕"; case WK: return "♔";
        case BP: return "♟"; case BN: return "♞"; case BB: return "♝";
        case BR: return "♜"; case BQ: return "♛"; case BK: return "♚";
        default: return " ";
    }
}

int get_piece_color_id(int p) {
    if (p >= WP && p <= WK) return 21;  /* High-contrast electric blue for white pieces */
    if (p >= BP && p <= BK) return 233; /* Pure dark black for black pieces */
    return 0;
}

int get_sq_bg_color(int sq, int cursor_sq, int selected, int is_legal_dest) {
    int piece = board_state.board[sq];
    if ((piece == WK || piece == BK) && PIECE_COLOR(piece) == board_state.side) {
        if (is_square_attacked(&board_state, sq, board_state.side ^ 1)) {
            return 196; /* Red background for King in check */
        }
    }
    if (sq == cursor_sq) return 99;      /* Purple for Cursor */
    if (sq == selected) return 33;       /* Sky Blue for selected piece */
    if (is_legal_dest) return 71;        /* Muted Green for legal landing square */
    if (sq == last_move.from || sq == last_move.to) return 221; /* Warm Yellow for last move */

    int f = SQ_TO_FILE(sq);
    int r = SQ_TO_RANK(sq);
    return ((f + r) % 2 == 1) ? 252 : 242; /* Soft white/gray chessboard texture */
}

/* Beautiful Terminal Renderer (Updates Canvas In-Place) */
void draw_ui() {
    printf("\033[H"); /* Cursor back to Home */

    printf("\033[1;36m┌────────────────────────────────────────────────────────┐\033[0m\n");
    printf("\033[1;36m│              TERMINAL CHESS GUI (UCI COMPLIANT)        │\033[0m\n");
    printf("\033[1;36m└────────────────────────────────────────────────────────┘\033[0m\n\n");

    int cursor_sq = FR_TO_SQ(cursor_x, cursor_y);
    char legal_dest[120] = {0};
    if (selected_sq != -1) {
        for (int i = 0; i < current_legal_moves.count; i++) {
            if (current_legal_moves.moves[i].from == selected_sq && is_move_legal(&board_state, current_legal_moves.moves[i])) {
                legal_dest[current_legal_moves.moves[i].to] = 1;
            }
        }
    }

    /* Print board from Row 8 down to 1 */
    for (int r = 7; r >= 0; r--) {
        printf("  %d ", r + 1);
        for (int f = 0; f < 8; f++) {
            int sq = FR_TO_SQ(f, r);
            int bg = get_sq_bg_color(sq, cursor_sq, selected_sq, legal_dest[sq]);
            int p = board_state.board[sq];
            int fg = get_piece_color_id(p);
            printf("\033[48;5;%dm\033[38;5;%dm %s \033[0m", bg, fg, piece_str(p));
        }
        printf("\n");
    }
    printf("     a  b  c  d  e  f  g  h\n\n");

    /* Side and Status info */
    printf("\033[1mEngine Side:\033[0m ");
    if (engine_side == -1) printf("Disabled (PVP)\n");
    else if (engine_side == 0) printf("White\n");
    else if (engine_side == 1) printf("Black\n");
    else if (engine_side == 2) printf("Both\n");

    printf("\033[1mTime Control:\033[0m ");
    if (tc_mode == 0) printf("Depth (%d plies)\n", tc_value);
    else if (tc_mode == 1) printf("Nodes (%d)\n", tc_value);
    else printf("Movetime (%d ms)\n", tc_value);

    printf("\033[1mActive Side:\033[0m %s\n", board_state.side == 0 ? "\033[1;37mWhite\033[0m" : "\033[1;30mBlack\033[0m");
    printf("\033[1mStatus:\033[0m \033[1;33m%s\033[0m\033[K\n\n", status_msg);

    /* PGN Display (last 10 moves) */
    printf("\033[1mMoves (PGN):\033[0m ");
    int pgn_start = history_count > 10 ? history_count - 10 : 0;
    for (int i = pgn_start; i < history_count; i++) {
        if (i % 2 == 0) printf("%d. ", (i / 2) + 1);
        printf("%s ", history[i].last_move.san);
    }
    printf("\033[K\n\n");

    printf("\033[1;30mControls: Arrow Keys/WASD=Move | Space/Enter=Select | U=Undo | E=Engine Side/Path | T=Time Controls | Q=Quit\033[0m\n");
    fflush(stdout);
}

int query_user_promotion() {
    printf("\n\033[1;33mPromote! Press Q (Queen), R (Rook), B (Bishop), N (Knight): \033[0m");
    fflush(stdout);
    while (1) {
        char ch;
        if (read(STDIN_FILENO, &ch, 1) > 0) {
            if (ch == 'q' || ch == 'Q') return 0;
            if (ch == 'r' || ch == 'R') return 1;
            if (ch == 'b' || ch == 'B') return 2;
            if (ch == 'n' || ch == 'N') return 3;
        }
        usleep(10000);
    }
}

void configure_engine() {
    disable_raw_mode();
    printf("\033[2J\033[H");
    printf("=== ENGINE CONFIGURATION ===\n\n");
    printf("1. Toggle Engine Play Side (Current: %s)\n", 
           engine_side == -1 ? "PVP (Disabled)" : (engine_side == 0 ? "White" : (engine_side == 1 ? "Black" : "Both")));
    printf("2. Set Engine Executable Path (Current: %s)\n", engine_path[0] ? engine_path : "None Specified");
    printf("3. Back to Match\n\n");
    printf("Select option: ");
    fflush(stdout);

    char val[16];
    if (fgets(val, sizeof(val), stdin)) {
        if (val[0] == '1') {
            if (engine_side == -1) engine_side = 1;      /* PVP -> Black */
            else if (engine_side == 1) engine_side = 0;  /* Black -> White */
            else if (engine_side == 0) engine_side = 2;  /* White -> Both */
            else engine_side = -1;                       /* Both -> PVP */
        } else if (val[0] == '2') {
            printf("Enter full path to UCI Engine (e.g. /opt/homebrew/bin/stockfish): ");
            fflush(stdout);
            char path_input[256];
            if (fgets(path_input, sizeof(path_input), stdin)) {
                path_input[strcspn(path_input, "\n")] = 0;
                if (strlen(path_input) > 0) {
                    strcpy(engine_path, path_input);
                    if (start_engine(engine_path)) {
                        snprintf(status_msg, sizeof(status_msg), "Engine loaded successfully!");
                    } else {
                        snprintf(status_msg, sizeof(status_msg), "Failed to spawn UCI engine.");
                    }
                }
            }
        }
    }
    enable_raw_mode();
    printf("\033[2J\033[H");
}

void configure_time_controls() {
    disable_raw_mode();
    printf("\033[2J\033[H");
    printf("=== TIME CONTROLS ===\n\n");
    printf("1. Search Depth (current ply count limit)\n");
    printf("2. Max Nodes\n");
    printf("3. Direct Move Time Target (milliseconds)\n");
    printf("Choice: ");
    fflush(stdout);

    char val[16];
    if (fgets(val, sizeof(val), stdin)) {
        int select_mode = val[0] - '1';
        if (select_mode >= 0 && select_mode <= 2) {
            tc_mode = select_mode;
            printf("Enter desired integer value: ");
            fflush(stdout);
            char param[32];
            if (fgets(param, sizeof(param), stdin)) {
                int user_val = atoi(param);
                if (user_val > 0) tc_value = user_val;
            }
        }
    }
    enable_raw_mode();
    printf("\033[2J\033[H");
}

int check_game_over() {
    int has_legal = 0;
    for (int i = 0; i < current_legal_moves.count; i++) {
        if (is_move_legal(&board_state, current_legal_moves.moves[i])) { has_legal = 1; break; }
    }
    if (!has_legal) {
        int king_piece = (board_state.side == 0) ? WK : BK;
        int king_sq = -1;
        for (int sq = 0; sq < 120; sq++) {
            if (board_state.board[sq] == king_piece) { king_sq = sq; break; }
        }
        if (king_sq != -1 && is_square_attacked(&board_state, king_sq, board_state.side ^ 1)) {
            snprintf(status_msg, sizeof(status_msg), "Checkmate! %s wins.", board_state.side == 0 ? "Black" : "White");
        } else {
            snprintf(status_msg, sizeof(status_msg), "Stalemate! Draw.");
        }
        return 1;
    }
    return 0;
}

int main() {
    init_board(&board_state);
    generate_moves(&board_state, &current_legal_moves);

    /* Search common paths for stockfish executable */
    const char *search_paths[] = {"stockfish", "/opt/homebrew/bin/stockfish", "/usr/local/bin/stockfish", "./stockfish"};
    for (int i = 0; i < 4; i++) {
        if (start_engine(search_paths[i])) {
            strcpy(engine_path, search_paths[i]);
            engine_side = 1; /* Match player as White vs Black Engine */
            break;
        }
    }

    enable_raw_mode();
    printf("\033[2J\033[H"); /* Initial Canvas Clear */

    int running = 1;
    int draw_needed = 1;

    int max_fd = STDIN_FILENO;

    while (running) {
        if (engine_running && engine_out_pipe[0] > max_fd) {
            max_fd = engine_out_pipe[0];
        }

        /* Automate engine calls if active side is mapped to engine */
        if (engine_running && !engine_thinking && !check_game_over()) {
            if (engine_side == 2 || engine_side == board_state.side) {
                char position_cmd[8192];
                construct_engine_moves_string(position_cmd, sizeof(position_cmd));
                send_to_engine(position_cmd);

                char go_cmd[128];
                if (tc_mode == 0) snprintf(go_cmd, sizeof(go_cmd), "go depth %d", tc_value);
                else if (tc_mode == 1) snprintf(go_cmd, sizeof(go_cmd), "go nodes %d", tc_value);
                else snprintf(go_cmd, sizeof(go_cmd), "go movetime %d", tc_value);

                send_to_engine(go_cmd);
                engine_thinking = 1;
                snprintf(status_msg, sizeof(status_msg), "Engine is thinking...");
                draw_needed = 1;
            }
        }

        if (draw_needed) {
            draw_ui();
            draw_needed = 0;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (engine_running) FD_SET(engine_out_pipe[0], &fds);

        struct timeval tv = {0, 15000}; /* Non-blocking update cycle */
        int activity = select(max_fd + 1, &fds, NULL, NULL, &tv);

        if (activity < 0) continue;

        if (FD_ISSET(STDIN_FILENO, &fds)) {
            char ch;
            if (read(STDIN_FILENO, &ch, 1) > 0) {
                if (ch == '\033') { /* Escape Sequences */
                    char seq[2];
                    if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                        if (seq[0] == '[') {
                            if (seq[1] == 'A') { if (cursor_y < 7) cursor_y++; draw_needed = 1; } /* Up */
                            else if (seq[1] == 'B') { if (cursor_y > 0) cursor_y--; draw_needed = 1; } /* Down */
                            else if (seq[1] == 'C') { if (cursor_x < 7) cursor_x++; draw_needed = 1; } /* Right */
                            else if (seq[1] == 'D') { if (cursor_x > 0) cursor_x--; draw_needed = 1; } /* Left */
                        }
                    }
                } else if (ch == 'w' || ch == 'W') { if (cursor_y < 7) cursor_y++; draw_needed = 1; }
                else if (ch == 's' || ch == 'S') { if (cursor_y > 0) cursor_y--; draw_needed = 1; }
                else if (ch == 'd' || ch == 'D') { if (cursor_x < 7) cursor_x++; draw_needed = 1; }
                else if (ch == 'a' || ch == 'A') { if (cursor_x > 0) cursor_x--; draw_needed = 1; }
                else if (ch == 'q' || ch == 'Q') {
                    running = 0;
                } else if (ch == 'u' || ch == 'U') {
                    /* Rollback history */
                    if (history_count > 0) {
                        history_count--;
                        board_state = history[history_count].state;
                        last_move = history[history_count].last_move;
                        if (history_count > 0) {
                            last_move = history[history_count - 1].last_move;
                        } else {
                            last_move = (Move){0, 0, 0, ""};
                        }
                        selected_sq = -1;
                        engine_thinking = 0; /* Clear thinking states if rolled back */
                        generate_moves(&board_state, &current_legal_moves);
                        snprintf(status_msg, sizeof(status_msg), "Took back last move.");
                        draw_needed = 1;
                    }
                } else if (ch == 'e' || ch == 'E') {
                    configure_engine();
                    max_fd = STDIN_FILENO;
                    draw_needed = 1;
                } else if (ch == 't' || ch == 'T') {
                    configure_time_controls();
                    draw_needed = 1;
                } else if (ch == ' ' || ch == '\n') {
                    int clicked_sq = FR_TO_SQ(cursor_x, cursor_y);
                    if (selected_sq == -1) {
                        if (board_state.board[clicked_sq] != EMPTY && PIECE_COLOR(board_state.board[clicked_sq]) == board_state.side) {
                            selected_sq = clicked_sq;
                            draw_needed = 1;
                        }
                    } else {
                        if (clicked_sq == selected_sq) {
                            selected_sq = -1;
                            draw_needed = 1;
                        } else if (board_state.board[clicked_sq] != EMPTY && PIECE_COLOR(board_state.board[clicked_sq]) == board_state.side) {
                            selected_sq = clicked_sq;
                            draw_needed = 1;
                        } else {
                            /* Attempt move execution */
                            int move_idx = -1;
                            for (int i = 0; i < current_legal_moves.count; i++) {
                                if (current_legal_moves.moves[i].from == selected_sq && current_legal_moves.moves[i].to == clicked_sq) {
                                    if (is_move_legal(&board_state, current_legal_moves.moves[i])) {
                                        move_idx = i;
                                        break;
                                    }
                                }
                            }
                            if (move_idx != -1) {
                                Move selected_move = current_legal_moves.moves[move_idx];
                                int piece = board_state.board[selected_move.from];

                                /* Human Pawn Promotion Selection */
                                if ((piece == WP || piece == BP) && (SQ_TO_RANK(selected_move.to) == 7 || SQ_TO_RANK(selected_move.to) == 0)) {
                                    int promo_choice = query_user_promotion();
                                    if (board_state.side == 0) {
                                        int mapping[] = {WQ, WR, WB, WN};
                                        selected_move.promotion = mapping[promo_choice];
                                    } else {
                                        int mapping[] = {BQ, BR, BB, BN};
                                        selected_move.promotion = mapping[promo_choice];
                                    }
                                }

                                get_move_san(&board_state, selected_move, selected_move.san);

                                history[history_count].state = board_state;
                                history[history_count].last_move = selected_move;
                                history_count++;

                                BoardState next;
                                make_move(&board_state, &next, selected_move);
                                board_state = next;
                                last_move = selected_move;
                                selected_sq = -1;

                                snprintf(status_msg, sizeof(status_msg), "Played %s", selected_move.san);
                                generate_moves(&board_state, &current_legal_moves);
                                check_game_over();
                            }
                            draw_needed = 1;
                        }
                    }
                }
            }
        }

        if (engine_running && FD_ISSET(engine_out_pipe[0], &fds)) {
            char uci_move[32] = {0};
            if (parse_engine_output(uci_move)) {
                Move em = parse_uci_move(&board_state, uci_move);
                if (em.from != 0 && em.to != 0) {
                    get_move_san(&board_state, em, em.san);

                    history[history_count].state = board_state;
                    history[history_count].last_move = em;
                    history_count++;

                    BoardState next;
                    make_move(&board_state, &next, em);
                    board_state = next;
                    last_move = em;

                    snprintf(status_msg, sizeof(status_msg), "Engine played %s", em.san);
                    generate_moves(&board_state, &current_legal_moves);
                    check_game_over();
                }
                engine_thinking = 0;
                draw_needed = 1;
            }
        }
    }

    stop_engine();
    return 0;
}
