#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/select.h>
#include <fcntl.h>
#include <signal.h>

#define EMPTY 0
#define PAWN  1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6
#define WHITE  0x08
#define BLACK  0x10
#define COLOR_MASK 0x18
#define PIECE_MASK 0x07

#define MAX_HISTORY 2048

typedef struct {
    int from;
    int to;
    int promotion;
} Move;

typedef struct {
    int board[64];
    int active_color;
    int castling; // bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep_square;
    int halfmove;
    int fullmove;

    Move last_move;
    int has_last_move;
} GameState;

typedef enum { LIMIT_TIME, LIMIT_DEPTH, LIMIT_NODES } LimitType;

// Application State
GameState history[MAX_HISTORY];
int history_count = 0;

int cursor_sq = 52; // Starts on e2
int selected_sq = -1;
int legal_targets[64];
int num_targets = 0;

int engine_stdin_pipe[2];
int engine_stdout_pipe[2];
pid_t engine_pid = -1;
int engine_active = 0;
int engine_is_thinking = 0;
int game_mode = 0; // 0 = Player vs Engine, 1 = Self-Play
int human_color = WHITE;

LimitType search_limit_type = LIMIT_TIME;
int search_limit_value = 1000; // 1000 ms default
char engine_path[512] = "stockfish";

struct termios orig_termios;

// Forward Declarations
void get_legal_moves(const GameState *state, Move *moves, int *count);
int in_check(const GameState *state, int color);
void make_move(const GameState *from_state, GameState *to_state, Move move);
void render_screen();
void disable_raw_mode();
void enable_raw_mode();

// Utility function to check if a square is on-board
int on_board(int r, int c) {
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}

// Map piece representation to its unicode character
const char* piece_unicode(int piece) {
    int ptype = piece & PIECE_MASK;
    int color = piece & COLOR_MASK;
    if (color == WHITE) {
        switch (ptype) {
            case PAWN:   return "♙";
            case KNIGHT: return "♘";
            case BISHOP: return "♗";
            case ROOK:   return "♖";
            case QUEEN:  return "♕";
            case KING:   return "♔";
        }
    } else {
        switch (ptype) {
            case PAWN:   return "♟";
            case KNIGHT: return "♞";
            case BISHOP: return "♝";
            case ROOK:   return "♜";
            case QUEEN:  return "♛";
            case KING:   return "♚";
        }
    }
    return " ";
}

// Parse FEN string into GameState
void load_fen(GameState *state, const char *fen) {
    memset(state->board, 0, sizeof(state->board));
    int r = 0, c = 0;
    const char *p = fen;

    while (*p && *p != ' ') {
        if (*p == '/') {
            r++;
            c = 0;
        } else if (*p >= '1' && *p <= '8') {
            c += (*p - '0');
        } else {
            int color = (*p >= 'a' && *p <= 'z') ? BLACK : WHITE;
            int ptype = EMPTY;
            char uc = (*p >= 'a' && *p <= 'z') ? (*p - 'a' + 'A') : *p;
            if (uc == 'P') ptype = PAWN;
            else if (uc == 'N') ptype = KNIGHT;
            else if (uc == 'B') ptype = BISHOP;
            else if (uc == 'R') ptype = ROOK;
            else if (uc == 'Q') ptype = QUEEN;
            else if (uc == 'K') ptype = KING;

            state->board[r * 8 + c] = color | ptype;
            c++;
        }
        p++;
    }

    if (*p) p++;
    if (*p) {
        state->active_color = (*p == 'w') ? WHITE : BLACK;
        p++;
    }
    if (*p) p++;
    state->castling = 0;
    while (*p && *p != ' ') {
        if (*p == 'K') state->castling |= 1;
        else if (*p == 'Q') state->castling |= 2;
        else if (*p == 'k') state->castling |= 4;
        else if (*p == 'q') state->castling |= 8;
        p++;
    }
    if (*p) p++;
    state->ep_square = -1;
    if (*p && *p != '-') {
        int file = p[0] - 'a';
        int rank = 8 - (p[1] - '0');
        state->ep_square = rank * 8 + file;
        p += 2;
    } else if (*p == '-') {
        p++;
    }
    if (*p) p++;
    state->halfmove = 0;
    if (*p && *p != ' ') {
        state->halfmove = atoi(p);
        while (*p && *p != ' ') p++;
    }
    if (*p) p++;
    state->fullmove = 1;
    if (*p) {
        state->fullmove = atoi(p);
    }
    state->has_last_move = 0;
}

// Generate all pseudo-legal moves for a given position
void get_pseudo_moves(const GameState *state, Move *moves, int *count) {
    *count = 0;
    int us = state->active_color;
    int them = (us == WHITE) ? BLACK : WHITE;

    for (int i = 0; i < 64; i++) {
        int piece = state->board[i];
        if (piece == EMPTY || (piece & COLOR_MASK) != us) continue;

        int ptype = piece & PIECE_MASK;
        int r = i / 8;
        int c = i % 8;

        switch (ptype) {
            case PAWN: {
                int dir = (us == WHITE) ? -1 : 1;
                int start_row = (us == WHITE) ? 6 : 1;
                int promo_row = (us == WHITE) ? 0 : 7;

                int nr = r + dir;
                int nc = c;
                if (on_board(nr, nc) && state->board[nr * 8 + nc] == EMPTY) {
                    if (nr == promo_row) {
                        moves[(*count)++] = (Move){i, nr * 8 + nc, QUEEN};
                        moves[(*count)++] = (Move){i, nr * 8 + nc, ROOK};
                        moves[(*count)++] = (Move){i, nr * 8 + nc, BISHOP};
                        moves[(*count)++] = (Move){i, nr * 8 + nc, KNIGHT};
                    } else {
                        moves[(*count)++] = (Move){i, nr * 8 + nc, 0};
                        if (r == start_row) {
                            int nnr = r + 2 * dir;
                            if (state->board[nnr * 8 + nc] == EMPTY) {
                                moves[(*count)++] = (Move){i, nnr * 8 + nc, 0};
                            }
                        }
                    }
                }

                int dc[2] = {-1, 1};
                for (int k = 0; k < 2; k++) {
                    nr = r + dir;
                    nc = c + dc[k];
                    if (on_board(nr, nc)) {
                        int target = nr * 8 + nc;
                        int dest_piece = state->board[target];
                        if (dest_piece != EMPTY && (dest_piece & COLOR_MASK) == them) {
                            if (nr == promo_row) {
                                moves[(*count)++] = (Move){i, target, QUEEN};
                                moves[(*count)++] = (Move){i, target, ROOK};
                                moves[(*count)++] = (Move){i, target, BISHOP};
                                moves[(*count)++] = (Move){i, target, KNIGHT};
                            } else {
                                moves[(*count)++] = (Move){i, target, 0};
                            }
                        } else if (target == state->ep_square) {
                            moves[(*count)++] = (Move){i, target, 0};
                        }
                    }
                }
                break;
            }
            case KNIGHT: {
                int dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
                int dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
                for (int k = 0; k < 8; k++) {
                    int nr = r + dr[k];
                    int nc = c + dc[k];
                    if (on_board(nr, nc)) {
                        int target = nr * 8 + nc;
                        if (state->board[target] == EMPTY || (state->board[target] & COLOR_MASK) == them) {
                            moves[(*count)++] = (Move){i, target, 0};
                        }
                    }
                }
                break;
            }
            case BISHOP:
            case ROOK:
            case QUEEN: {
                int dr[8], dc[8], ndirs = 0;
                if (ptype == BISHOP || ptype == QUEEN) {
                    dr[ndirs] = -1; dc[ndirs++] = -1;
                    dr[ndirs] = -1; dc[ndirs++] = 1;
                    dr[ndirs] = 1;  dc[ndirs++] = -1;
                    dr[ndirs] = 1;  dc[ndirs++] = 1;
                }
                if (ptype == ROOK || ptype == QUEEN) {
                    dr[ndirs] = -1; dc[ndirs++] = 0;
                    dr[ndirs] = 1;  dc[ndirs++] = 0;
                    dr[ndirs] = 0;  dc[ndirs++] = -1;
                    dr[ndirs] = 0;  dc[ndirs++] = 1;
                }
                for (int d = 0; d < ndirs; d++) {
                    int nr = r, nc = c;
                    while (1) {
                        nr += dr[d];
                        nc += dc[d];
                        if (!on_board(nr, nc)) break;
                        int target = nr * 8 + nc;
                        if (state->board[target] == EMPTY) {
                            moves[(*count)++] = (Move){i, target, 0};
                        } else {
                            if ((state->board[target] & COLOR_MASK) == them) {
                                moves[(*count)++] = (Move){i, target, 0};
                            }
                            break;
                        }
                    }
                }
                break;
            }
            case KING: {
                int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
                int dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
                for (int k = 0; k < 8; k++) {
                    int nr = r + dr[k];
                    int nc = c + dc[k];
                    if (on_board(nr, nc)) {
                        int target = nr * 8 + nc;
                        if (state->board[target] == EMPTY || (state->board[target] & COLOR_MASK) == them) {
                            moves[(*count)++] = (Move){i, target, 0};
                        }
                    }
                }
                if (us == WHITE) {
                    if (state->castling & 1) {
                        if (state->board[61] == EMPTY && state->board[62] == EMPTY) {
                            moves[(*count)++] = (Move){60, 62, 0};
                        }
                    }
                    if (state->castling & 2) {
                        if (state->board[59] == EMPTY && state->board[58] == EMPTY && state->board[57] == EMPTY) {
                            moves[(*count)++] = (Move){60, 58, 0};
                        }
                    }
                } else {
                    if (state->castling & 4) {
                        if (state->board[5] == EMPTY && state->board[6] == EMPTY) {
                            moves[(*count)++] = (Move){4, 6, 0};
                        }
                    }
                    if (state->castling & 8) {
                        if (state->board[3] == EMPTY && state->board[2] == EMPTY && state->board[1] == EMPTY) {
                            moves[(*count)++] = (Move){4, 2, 0};
                        }
                    }
                }
                break;
            }
        }
    }
}

// Check if square is attacked by attacker_color
int is_square_attacked(const GameState *state, int square, int attacker_color) {
    int r = square / 8;
    int c = square % 8;

    int kn_dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int kn_dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int k = 0; k < 8; k++) {
        int nr = r + kn_dr[k];
        int nc = c + kn_dc[k];
        if (on_board(nr, nc)) {
            int p = state->board[nr * 8 + nc];
            if (p != EMPTY && (p & COLOR_MASK) == attacker_color && (p & PIECE_MASK) == KNIGHT) {
                return 1;
            }
        }
    }

    int k_dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int k_dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int k = 0; k < 8; k++) {
        int nr = r + k_dr[k];
        int nc = c + k_dc[k];
        if (on_board(nr, nc)) {
            int p = state->board[nr * 8 + nc];
            if (p != EMPTY && (p & COLOR_MASK) == attacker_color && (p & PIECE_MASK) == KING) {
                return 1;
            }
        }
    }

    int p_dir = (attacker_color == WHITE) ? 1 : -1;
    int p_row = r + p_dir;
    int p_cols[2] = {c - 1, c + 1};
    for (int k = 0; k < 2; k++) {
        if (on_board(p_row, p_cols[k])) {
            int p = state->board[p_row * 8 + p_cols[k]];
            if (p != EMPTY && (p & COLOR_MASK) == attacker_color && (p & PIECE_MASK) == PAWN) {
                return 1;
            }
        }
    }

    int diag_dr[] = {-1, -1, 1, 1};
    int diag_dc[] = {-1, 1, -1, 1};
    for (int d = 0; d < 4; d++) {
        int nr = r, nc = c;
        while (1) {
            nr += diag_dr[d];
            nc += diag_dc[d];
            if (!on_board(nr, nc)) break;
            int p = state->board[nr * 8 + nc];
            if (p != EMPTY) {
                if ((p & COLOR_MASK) == attacker_color && ((p & PIECE_MASK) == BISHOP || (p & PIECE_MASK) == QUEEN)) {
                    return 1;
                }
                break;
            }
        }
    }

    int orth_dr[] = {-1, 1, 0, 0};
    int orth_dc[] = {0, 0, -1, 1};
    for (int d = 0; d < 4; d++) {
        int nr = r, nc = c;
        while (1) {
            nr += orth_dr[d];
            nc += orth_dc[d];
            if (!on_board(nr, nc)) break;
            int p = state->board[nr * 8 + nc];
            if (p != EMPTY) {
                if ((p & COLOR_MASK) == attacker_color && ((p & PIECE_MASK) == ROOK || (p & PIECE_MASK) == QUEEN)) {
                    return 1;
                }
                break;
            }
        }
    }

    return 0;
}

// Detect check state
int in_check(const GameState *state, int color) {
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == (color | KING)) {
            king_sq = i;
            break;
        }
    }
    if (king_sq == -1) return 0;
    return is_square_attacked(state, king_sq, (color == WHITE) ? BLACK : WHITE);
}

// Generate legal moves only
void get_legal_moves(const GameState *state, Move *moves, int *count) {
    Move pseudo[256];
    int p_count = 0;
    get_pseudo_moves(state, pseudo, &p_count);
    *count = 0;

    for (int i = 0; i < p_count; i++) {
        int is_castling = 0;
        int castling_type = 0; 
        int piece = state->board[pseudo[i].from];
        if ((piece & PIECE_MASK) == KING && abs(pseudo[i].from - pseudo[i].to) == 2) {
            is_castling = 1;
            castling_type = (pseudo[i].to > pseudo[i].from) ? 1 : 2;
        }

        if (is_castling) {
            int us = state->active_color;
            int them = (us == WHITE) ? BLACK : WHITE;
            if (in_check(state, us)) continue;

            int step = (castling_type == 1) ? 1 : -1;
            int sq1 = pseudo[i].from + step;
            int sq2 = pseudo[i].from + 2 * step;

            GameState temp1;
            make_move(state, &temp1, (Move){pseudo[i].from, sq1, 0});
            if (is_square_attacked(state, sq1, them)) continue;

            GameState temp2;
            make_move(state, &temp2, (Move){pseudo[i].from, sq2, 0});
            if (is_square_attacked(&temp1, sq2, them)) continue;
        }

        GameState next_state;
        make_move(state, &next_state, pseudo[i]);
        if (!in_check(&next_state, state->active_color)) {
            moves[(*count)++] = pseudo[i];
        }
    }
}

// Advance board state with legal move
void make_move(const GameState *from_state, GameState *to_state, Move move) {
    *to_state = *from_state;
    int us = to_state->active_color;
    int them = (us == WHITE) ? BLACK : WHITE;

    int from = move.from;
    int to = move.to;
    int piece = to_state->board[from];

    to_state->ep_square = -1;
    to_state->board[to] = piece;
    to_state->board[from] = EMPTY;

    if (move.promotion != 0) {
        to_state->board[to] = us | move.promotion;
    }

    if ((piece & PIECE_MASK) == PAWN) {
        if (abs(from - to) == 16) {
            to_state->ep_square = (from + to) / 2;
        }
        if (to == from_state->ep_square) {
            int ep_capture_sq = to + ((us == WHITE) ? 8 : -8);
            to_state->board[ep_capture_sq] = EMPTY;
        }
    }

    if ((piece & PIECE_MASK) == KING) {
        if (abs(from - to) == 2) {
            if (to == 62) {
                to_state->board[61] = WHITE | ROOK;
                to_state->board[63] = EMPTY;
            } else if (to == 58) {
                to_state->board[59] = WHITE | ROOK;
                to_state->board[56] = EMPTY;
            } else if (to == 6) {
                to_state->board[5] = BLACK | ROOK;
                to_state->board[7] = EMPTY;
            } else if (to == 2) {
                to_state->board[3] = BLACK | ROOK;
                to_state->board[0] = EMPTY;
            }
        }
        if (us == WHITE) to_state->castling &= ~3;
        else to_state->castling &= ~12;
    }

    if (from == 56) to_state->castling &= ~2;
    if (from == 63) to_state->castling &= ~1;
    if (from == 0)  to_state->castling &= ~8;
    if (from == 7)  to_state->castling &= ~4;

    if (to == 56) to_state->castling &= ~2;
    if (to == 63) to_state->castling &= ~1;
    if (to == 0)  to_state->castling &= ~8;
    if (to == 7)  to_state->castling &= ~4;

    to_state->active_color = them;
    if (us == BLACK) {
        to_state->fullmove++;
    }
    if ((piece & PIECE_MASK) == PAWN || from_state->board[to] != EMPTY) {
        to_state->halfmove = 0;
    } else {
        to_state->halfmove++;
    }
}

// SAN formatting for PGN list
void get_san_move(const GameState *prev_state, Move move, char *san) {
    int piece = prev_state->board[move.from];
    int ptype = piece & PIECE_MASK;
    int us = piece & COLOR_MASK;

    if (ptype == KING && abs(move.from - move.to) == 2) {
        if (move.to > move.from) {
            strcpy(san, "O-O");
        } else {
            strcpy(san, "O-O-O");
        }
        GameState temp;
        make_move(prev_state, &temp, move);
        if (in_check(&temp, (us == WHITE) ? BLACK : WHITE)) {
            strcat(san, "+");
        }
        return;
    }

    char piece_chars[] = {'?', ' ', 'N', 'B', 'R', 'Q', 'K'};
    char p_char = piece_chars[ptype];

    char from_file = 'a' + (move.from % 8);
    char from_rank = '8' - (move.from / 8);
    char to_file = 'a' + (move.to % 8);
    char to_rank = '8' - (move.to / 8);

    char disambiguation[4] = "";
    int d_idx = 0;

    if (ptype != PAWN && ptype != KING) {
        Move legal[256];
        int count = 0;
        get_legal_moves(prev_state, legal, &count);
        int file_conflict = 0;
        int rank_conflict = 0;
        int conflict_count = 0;

        for (int i = 0; i < count; i++) {
            if (legal[i].to == move.to && legal[i].from != move.from) {
                int p_other = prev_state->board[legal[i].from];
                if (p_other == piece) {
                    conflict_count++;
                    if (legal[i].from % 8 == move.from % 8) file_conflict = 1;
                    if (legal[i].from / 8 == move.from / 8) rank_conflict = 1;
                }
            }
        }

        if (conflict_count > 0) {
            if (!file_conflict) {
                disambiguation[d_idx++] = from_file;
            } else if (!rank_conflict) {
                disambiguation[d_idx++] = from_rank;
            } else {
                disambiguation[d_idx++] = from_file;
                disambiguation[d_idx++] = from_rank;
            }
        }
    }
    disambiguation[d_idx] = '\0';

    int is_capture = (prev_state->board[move.to] != EMPTY) || (ptype == PAWN && move.to == prev_state->ep_square);

    char promo_str[8] = "";
    if (move.promotion != 0) {
        char p_char_promo = 'Q';
        if (move.promotion == ROOK) p_char_promo = 'R';
        else if (move.promotion == BISHOP) p_char_promo = 'B';
        else if (move.promotion == KNIGHT) p_char_promo = 'N';
        sprintf(promo_str, "=%c", p_char_promo);
    }

    char *ptr = san;
    if (ptype == PAWN) {
        if (is_capture) {
            *ptr++ = from_file;
            *ptr++ = 'x';
        }
        *ptr++ = to_file;
        *ptr++ = to_rank;
    } else {
        *ptr++ = p_char;
        for (int i = 0; i < d_idx; i++) {
            *ptr++ = disambiguation[i];
        }
        if (is_capture) {
            *ptr++ = 'x';
        }
        *ptr++ = to_file;
        *ptr++ = to_rank;
    }
    strcpy(ptr, promo_str);

    GameState temp;
    make_move(prev_state, &temp, move);
    if (in_check(&temp, (us == WHITE) ? BLACK : WHITE)) {
        Move m_temp[256];
        int c_temp = 0;
        get_legal_moves(&temp, m_temp, &c_temp);
        if (c_temp == 0) {
            strcat(san, "#");
        } else {
            strcat(san, "+");
        }
    }
}

// Dynamic PGN string Builder
void build_pgn_string(char *buf, int max_len) {
    buf[0] = '\0';
    char move_san[32];
    for (int i = 1; i < history_count; i++) {
        if (i % 2 != 0) {
            char move_num[16];
            sprintf(move_num, "%d. ", (i + 1) / 2);
            if (strlen(buf) + strlen(move_num) < (size_t)max_len) {
                strcat(buf, move_num);
            }
        }
        get_san_move(&history[i - 1], history[i].last_move, move_san);
        if (strlen(buf) + strlen(move_san) + 2 < (size_t)max_len) {
            strcat(buf, move_san);
            strcat(buf, " ");
        }
    }
}

// Spawns Stockfish / UCI engine
int start_engine_process(const char *path) {
    if (pipe(engine_stdin_pipe) < 0 || pipe(engine_stdout_pipe) < 0) {
        return 0;
    }

    engine_pid = fork();
    if (engine_pid < 0) {
        return 0;
    }

    if (engine_pid == 0) { // Child
        dup2(engine_stdin_pipe[0], STDIN_FILENO);
        dup2(engine_stdout_pipe[1], STDOUT_FILENO);

        close(engine_stdin_pipe[0]);
        close(engine_stdin_pipe[1]);
        close(engine_stdout_pipe[0]);
        close(engine_stdout_pipe[1]);

        execlp(path, path, NULL);
        exit(1);
    } else { // Parent
        close(engine_stdin_pipe[0]);
        close(engine_stdout_pipe[1]);

        int flags = fcntl(engine_stdout_pipe[0], F_GETFL, 0);
        fcntl(engine_stdout_pipe[0], F_SETFL, flags | O_NONBLOCK);

        engine_active = 1;
        return 1;
    }
}

void stop_engine_process() {
    if (engine_pid > 0) {
        kill(engine_pid, SIGTERM);
        engine_pid = -1;
    }
    engine_active = 0;
}

void send_to_engine(const char *cmd) {
    if (!engine_active) return;
    write(engine_stdin_pipe[1], cmd, strlen(cmd));
    write(engine_stdin_pipe[1], "\n", 1);
}

// Reads Engine streams (Non-blocking)
int read_from_engine(char *out_move_str, int max_len) {
    if (!engine_active) return 0;

    static char engine_buffer[8192];
    static int engine_buf_len = 0;

    char temp[2048];
    int n = read(engine_stdout_pipe[0], temp, sizeof(temp) - 1);
    if (n > 0) {
        temp[n] = '\0';
        if (engine_buf_len + n < (int)sizeof(engine_buffer) - 1) {
            memcpy(engine_buffer + engine_buf_len, temp, n);
            engine_buf_len += n;
            engine_buffer[engine_buf_len] = '\0';
        } else {
            engine_buf_len = 0;
            engine_buffer[0] = '\0';
        }
    }

    char *line_start = engine_buffer;
    char *newline;
    int found_bestmove = 0;

    while ((newline = strchr(line_start, '\n')) != NULL) {
        *newline = '\0';
        char *line = line_start;

        if (strncmp(line, "bestmove ", 9) == 0) {
            char move_str[32];
            if (sscanf(line, "bestmove %s", move_str) == 1) {
                strncpy(out_move_str, move_str, max_len);
                found_bestmove = 1;
            }
        }
        line_start = newline + 1;
    }

    int consumed = line_start - engine_buffer;
    if (consumed > 0) {
        memmove(engine_buffer, line_start, engine_buf_len - consumed);
        engine_buf_len -= consumed;
        engine_buffer[engine_buf_len] = '\0';
    }

    return found_bestmove;
}

void trigger_engine_search() {
    char cmd[16384] = "position startpos moves";
    for (int i = 1; i < history_count; i++) {
        char m_str[16];
        int f_col = history[i].last_move.from % 8;
        int f_row = 8 - (history[i].last_move.from / 8);
        int t_col = history[i].last_move.to % 8;
        int t_row = 8 - (history[i].last_move.to / 8);

        if (history[i].last_move.promotion != 0) {
            char p_char = 'q';
            if (history[i].last_move.promotion == ROOK) p_char = 'r';
            else if (history[i].last_move.promotion == BISHOP) p_char = 'b';
            else if (history[i].last_move.promotion == KNIGHT) p_char = 'n';
            sprintf(m_str, "%c%d%c%d%c", 'a' + f_col, f_row, 'a' + t_col, t_row, p_char);
        } else {
            sprintf(m_str, "%c%d%c%d", 'a' + f_col, f_row, 'a' + t_col, t_row);
        }

        strcat(cmd, " ");
        strcat(cmd, m_str);
    }
    send_to_engine(cmd);

    char go_cmd[128];
    if (search_limit_type == LIMIT_DEPTH) {
        sprintf(go_cmd, "go depth %d", search_limit_value);
    } else if (search_limit_type == LIMIT_NODES) {
        sprintf(go_cmd, "go nodes %d", search_limit_value);
    } else {
        sprintf(go_cmd, "go movetime %d", search_limit_value);
    }
    send_to_engine(go_cmd);
}

// Convert algebraic UCI coordinate string into Move object
int parse_uci_move(const GameState *state, const char *move_str, Move *move) {
    if (strlen(move_str) < 4) return 0;
    int f_col = move_str[0] - 'a';
    int f_row = 8 - (move_str[1] - '0');
    int t_col = move_str[2] - 'a';
    int t_row = 8 - (move_str[3] - '0');

    if (!on_board(f_row, f_col) || !on_board(t_row, t_col)) return 0;

    move->from = f_row * 8 + f_col;
    move->to = t_row * 8 + t_col;
    move->promotion = 0;

    if (strlen(move_str) >= 5) {
        char p_char = move_str[4];
        if (p_char == 'q') move->promotion = QUEEN;
        else if (p_char == 'r') move->promotion = ROOK;
        else if (p_char == 'b') move->promotion = BISHOP;
        else if (p_char == 'n') move->promotion = KNIGHT;
    }

    Move legal[256];
    int count = 0;
    get_legal_moves(state, legal, &count);
    for (int i = 0; i < count; i++) {
        if (legal[i].from == move->from && legal[i].to == move->to) {
            if (move->promotion == 0 || legal[i].promotion == move->promotion) {
                *move = legal[i];
                return 1;
            }
        }
    }
    return 0;
}

// Dynamically determine ANSI background code of specific square index
int get_square_bg(int r, int c, int cursor, int selected, const int *targets, int num_targets, const GameState *state) {
    int idx = r * 8 + c;

    // Red Highlight: King in Check
    int piece = state->board[idx];
    if ((piece & PIECE_MASK) == KING && ((piece & COLOR_MASK) == state->active_color)) {
        if (in_check(state, state->active_color)) {
            return 196; // Crimson Red
        }
    }

    // Cyan highlight: Cursor active
    if (idx == cursor) {
        return 38; 
    }

    // Yellow highlight: Active Selected piece
    if (idx == selected) {
        return 220; 
    }

    // Shaded Green highlight: Legal moves from selected piece
    for (int i = 0; i < num_targets; i++) {
        if (targets[i] == idx) {
            return ((r + c) % 2 == 0) ? 150 : 108;
        }
    }

    // Soft Blue highlight: Previous move (From and To)
    if (state->has_last_move) {
        if (idx == state->last_move.from || idx == state->last_move.to) {
            return ((r + c) % 2 == 0) ? 111 : 67;
        }
    }

    // Standard Squares
    return ((r + c) % 2 == 0) ? 252 : 239;
}

// Terminal wrap drawing helper
void draw_pgn_box(const char *pgn_str) {
    int width = 50;
    printf("┌──────────────────────────────────────────────────┐\n");
    printf("│ PGN History:                                     │\n");

    const char *p = pgn_str;
    char line[256];
    int line_idx = 0;

    while (*p) {
        char word[32];
        int w_idx = 0;
        while (*p && *p != ' ' && w_idx < 31) {
            word[w_idx++] = *p++;
        }
        word[w_idx] = '\0';
        if (*p == ' ') p++;

        if (line_idx + strlen(word) + (line_idx > 0 ? 1 : 0) >= (size_t)(width - 4)) {
            line[line_idx] = '\0';
            printf("│ %-48s │\n", line);
            line_idx = 0;
        }

        if (line_idx > 0) {
            line[line_idx++] = ' ';
        }
        strcpy(line + line_idx, word);
        line_idx += strlen(word);
    }

    if (line_idx > 0) {
        line[line_idx] = '\0';
        printf("│ %-48s │\n", line);
    } else if (strlen(pgn_str) == 0) {
        printf("│ (No moves played yet)                            │\n");
    }
    printf("└──────────────────────────────────────────────────┘\n");
}

// Smooth screen flush without flicker
void render_screen() {
    printf("\033[H"); // Cursor to Home position

    printf("\033[1;36m─── TERMINAL CHESS GUI ───\033[0m\n\n");

    GameState *state = &history[history_count - 1];

    // Draw Chessboard on Left (Height: 3 terminal rows per square, Width: 5 characters per square)
    for (int r = 0; r < 8; r++) {
        for (int l = 0; l < 3; l++) {
            if (l == 1) {
                printf(" %d  ", 8 - r);
            } else {
                printf("    ");
            }

            for (int c = 0; c < 8; c++) {
                int idx = r * 8 + c;
                int bg = get_square_bg(r, c, cursor_sq, selected_sq, legal_targets, num_targets, state);

                printf("\033[48;5;%dm", bg);

                int p = state->board[idx];
                if (l == 1) {
                    if (p != EMPTY) {
                        int fg = ((p & COLOR_MASK) == WHITE) ? 255 : 16;
                        printf("\033[38;5;%dm  %s  ", fg, piece_unicode(p));
                    } else {
                        printf("     ");
                    }
                } else {
                    printf("     ");
                }
            }
            printf("\033[0m\n");
        }
    }
    printf("       a    b    c    d    e    f    g    h\n\n");

    // Dynamic Game State Info Area
    printf("\033[1mSide to Move:\033[0m %s\n", (state->active_color == WHITE) ? "White ♔" : "Black ♚");

    Move moves[256];
    int count = 0;
    get_legal_moves(state, moves, &count);

    if (count == 0) {
        if (in_check(state, state->active_color)) {
            printf("\033[1;31m🏆 CHECKMATE! %s wins!\033[0m\n", (state->active_color == WHITE) ? "Black" : "White");
        } else {
            printf("\033[1;33m🤝 STALEMATE / DRAW!\033[0m\n");
        }
    } else if (in_check(state, state->active_color)) {
        printf("\033[1;31m⚠️  CHECK!\033[0m\n");
    } else {
        printf("\n");
    }

    printf("\n");
    char pgn_str[4096];
    build_pgn_string(pgn_str, sizeof(pgn_str));
    draw_pgn_box(pgn_str);

    printf("\n\033[1mEngine Integration:\033[0m\n");
    printf("  Opponent: %s\n", (game_mode == 0) ? "UCI Engine" : "Self Play (Local)");
    printf("  Binary:   %s (%s)\n", engine_path, engine_active ? "\033[1;32mConnected\033[0m" : "\033[1;31mOffline (Standard Self-Play Mode)\033[0m");
    printf("  Search Control: ");
    if (search_limit_type == LIMIT_TIME) {
        printf("Time limit (%d ms)", search_limit_value);
    } else if (search_limit_type == LIMIT_DEPTH) {
        printf("Depth limit (%d plies)", search_limit_value);
    } else {
        printf("Node search limit (%d nodes)", search_limit_value);
    }
    printf(" [Press 't' to change]\n");

    printf("\n\033[1mControls:\033[0m\n");
    printf("  Arrows / WASD : Navigate Cursor  | Space / Enter : Select / Move Piece\n");
    printf("  'u'           : Undo Move        | 'e'           : Switch Game Mode (Vs/Self)\n");
    printf("  't'           : Engine Limits    | 'r'           : Restart Match\n");
    printf("  'q'           : Exit GUI\n");

    if (engine_is_thinking) {
        printf("\n\033[5;32mEngine is calculating...\033[0m\n");
    } else {
        printf("\n\n");
    }

    fflush(stdout);
}

void change_engine_settings() {
    int setting_active = 1;
    int selected_option = 0;

    while (setting_active) {
        printf("\033[H\033[2J");
        printf("\033[1;36m─── ENGINE SETTINGS MENU ───\033[0m\n\n");
        printf(" %s  1. Limit Method : %s\n", (selected_option == 0) ? "▶" : " ",
               (search_limit_type == LIMIT_TIME) ? "Time Constraint" : (search_limit_type == LIMIT_DEPTH) ? "Search Depth" : "Node count");
        printf(" %s  2. Limit Value  : %d\n", (selected_option == 1) ? "▶" : " ", search_limit_value);
        printf(" %s  3. Binary Path  : %s\n", (selected_option == 2) ? "▶" : " ", engine_path);
        printf(" %s  4. Return to Match\n", (selected_option == 3) ? "▶" : " ");
        printf("\nUse UP/DOWN (or W/S) to navigate, SPACE/ENTER to change.\n");
        fflush(stdout);

        char c;
        int n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) continue;

        if (c == '\033') {
            char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                if (seq[0] == '[') {
                    if (seq[1] == 'A' && selected_option > 0) selected_option--;
                    if (seq[1] == 'B' && selected_option < 3) selected_option++;
                }
            }
        } else if (c == 'w' || c == 'W') {
            if (selected_option > 0) selected_option--;
        } else if (c == 's' || c == 'S') {
            if (selected_option < 3) selected_option++;
        } else if (c == ' ' || c == '\n' || c == '\r') {
            if (selected_option == 0) {
                search_limit_type = (LimitType)((search_limit_type + 1) % 3);
                if (search_limit_type == LIMIT_TIME) search_limit_value = 1000;
                else if (search_limit_type == LIMIT_DEPTH) search_limit_value = 10;
                else search_limit_value = 100000;
            } else if (selected_option == 1) {
                disable_raw_mode();
                printf("\nEnter new limit value: ");
                fflush(stdout);
                char input_val[64];
                if (fgets(input_val, sizeof(input_val), stdin)) {
                    search_limit_value = atoi(input_val);
                }
                enable_raw_mode();
            } else if (selected_option == 2) {
                disable_raw_mode();
                printf("\nEnter custom executable path to Engine: ");
                fflush(stdout);
                char input_path[512];
                if (fgets(input_path, sizeof(input_path), stdin)) {
                    input_path[strcspn(input_path, "\r\n")] = '\0';
                    if (strlen(input_path) > 0) {
                        strcpy(engine_path, input_path);
                        stop_engine_process();
                        start_engine_process(engine_path);
                        send_to_engine("uci");
                        send_to_engine("isready");
                    }
                }
                enable_raw_mode();
            } else if (selected_option == 3) {
                setting_active = 0;
            }
        }
    }
    printf("\033[H\033[2J");
}

void handle_selection() {
    GameState *state = &history[history_count - 1];

    if (engine_is_thinking) return;
    if (game_mode == 0 && state->active_color != human_color) return;

    if (selected_sq == -1) {
        int piece = state->board[cursor_sq];
        if (piece != EMPTY && (piece & COLOR_MASK) == state->active_color) {
            selected_sq = cursor_sq;
            Move legal[256];
            int count = 0;
            get_legal_moves(state, legal, &count);
            num_targets = 0;
            for (int i = 0; i < count; i++) {
                if (legal[i].from == selected_sq) {
                    legal_targets[num_targets++] = legal[i].to;
                }
            }
        }
    } else {
        int is_valid = 0;
        Move selected_move;
        Move legal[256];
        int count = 0;
        get_legal_moves(state, legal, &count);
        for (int i = 0; i < count; i++) {
            if (legal[i].from == selected_sq && legal[i].to == cursor_sq) {
                selected_move = legal[i];
                is_valid = 1;
                break;
            }
        }

        if (is_valid) {
            if ((state->board[selected_sq] & PIECE_MASK) == PAWN) {
                int r = cursor_sq / 8;
                if (r == 0 || r == 7) {
                    selected_move.promotion = QUEEN; // Queen auto-promotion
                }
            }

            GameState next;
            make_move(state, &next, selected_move);
            next.last_move = selected_move;
            next.has_last_move = 1;

            history[history_count++] = next;

            selected_sq = -1;
            num_targets = 0;
        } else {
            int piece = state->board[cursor_sq];
            if (piece != EMPTY && (piece & COLOR_MASK) == state->active_color) {
                selected_sq = cursor_sq;
                num_targets = 0;
                for (int i = 0; i < count; i++) {
                    if (legal[i].from == selected_sq) {
                        legal_targets[num_targets++] = legal[i].to;
                    }
                }
            } else {
                selected_sq = -1;
                num_targets = 0;
            }
        }
    }
}

void handle_input() {
    char c;
    int n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return;

    if (c == '\033') { // Parse ESC keypresses (Arrow codes)
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
            if (seq[0] == '[') {
                int r = cursor_sq / 8;
                int col = cursor_sq % 8;
                switch (seq[1]) {
                    case 'A': if (r > 0) r--; break; // UP
                    case 'B': if (r < 7) r++; break; // DOWN
                    case 'C': if (col < 7) col++; break; // RIGHT
                    case 'D': if (col > 0) col--; break; // LEFT
                }
                cursor_sq = r * 8 + col;
            }
        }
    } else if (c == 'w' || c == 'W') {
        int r = cursor_sq / 8; if (r > 0) r--; cursor_sq = r * 8 + (cursor_sq % 8);
    } else if (c == 's' || c == 'S') {
        int r = cursor_sq / 8; if (r < 7) r++; cursor_sq = r * 8 + (cursor_sq % 8);
    } else if (c == 'a' || c == 'A') {
        int col = cursor_sq % 8; if (col > 0) col--; cursor_sq = (cursor_sq / 8) * 8 + col;
    } else if (c == 'd' || c == 'D') {
        int col = cursor_sq % 8; if (col < 7) col++; cursor_sq = (cursor_sq / 8) * 8 + col;
    } else if (c == ' ' || c == '\n' || c == '\r') {
        handle_selection();
    } else if (c == 'u' || c == 'U') {
        if (history_count > 1) {
            if (game_mode == 0 && history_count > 2) {
                history_count -= 2; // Undo both human & engine moves
            } else {
                history_count--;
            }
            selected_sq = -1;
            num_targets = 0;
        }
    } else if (c == 't' || c == 'T') {
        change_engine_settings();
    } else if (c == 'r' || c == 'R') {
        history_count = 0;
        load_fen(&history[history_count++], "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        selected_sq = -1;
        num_targets = 0;
    } else if (c == 'e' || c == 'E') {
        game_mode = (game_mode == 0) ? 1 : 0;
    } else if (c == 'q' || c == 'Q') {
        disable_raw_mode();
        stop_engine_process();
        printf("\033[2J\033[HBye!\n");
        exit(0);
    }
}

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // Show terminal cursor
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide standard terminal cursor
}

int main() {
    enable_raw_mode();

    // Standard starting FEN
    load_fen(&history[history_count++], "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

    // Dynamic Engine discovery
    if (!start_engine_process(engine_path)) {
        // Fallbacks for Homebrew setups and paths on macOS
        if (!start_engine_process("/opt/homebrew/bin/stockfish")) {
            start_engine_process("/usr/local/bin/stockfish");
        }
    }

    if (engine_active) {
        send_to_engine("uci");
        send_to_engine("isready");
    }

    printf("\033[2J"); // Initial screen refresh clear

    while (1) {
        GameState *state = &history[history_count - 1];

        // Trigger engine thoughts on its turn
        if (game_mode == 0 && state->active_color != human_color && !engine_is_thinking && engine_active) {
            engine_is_thinking = 1;
            trigger_engine_search();
        }

        render_screen();

        // Multiplex Keyboard and Pipe reads with select()
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        int max_fd = STDIN_FILENO;

        if (engine_active) {
            FD_SET(engine_stdout_pipe[0], &readfds);
            if (engine_stdout_pipe[0] > max_fd) {
                max_fd = engine_stdout_pipe[0];
            }
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 20000; // 20 ms resolution loop

        int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv);

        if (activity > 0) {
            if (FD_ISSET(STDIN_FILENO, &readfds)) {
                handle_input();
            }
            if (engine_active && FD_ISSET(engine_stdout_pipe[0], &readfds)) {
                char bestmove[32];
                if (read_from_engine(bestmove, sizeof(bestmove))) {
                    Move engine_move;
                    if (parse_uci_move(state, bestmove, &engine_move)) {
                        GameState next;
                        make_move(state, &next, engine_move);
                        next.last_move = engine_move;
                        next.has_last_move = 1;
                        history[history_count++] = next;
                    }
                    engine_is_thinking = 0;
                }
            }
        }
    }

    return 0;
}
