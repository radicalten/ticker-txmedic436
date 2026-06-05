#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>

/* Chess Logic Constants */
#define EMPTY 0
#define PAWN 1
#define KNIGHT 2
#define BISHOP 3
#define ROOK 4
#define QUEEN 5
#define KING 6

#define WHITE 8
#define BLACK 16
#define COLOR_MASK (WHITE | BLACK)
#define PIECE_MASK 7

/* Engine State Definitions */
#define ENGINE_IDLE 0
#define ENGINE_THINKING 1

/* Struct declarations */
typedef struct {
    int from;
    int to;
    int promotion;
} Move;

typedef struct {
    int board[64];
    int turn;       // WHITE or BLACK
    int castling;  // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep_sq;     // -1 if none, otherwise 0-63
    int halfmove;
    int fullmove;
} BoardState;

/* Global State */
volatile sig_atomic_t running = 1;
struct termios orig_termios;

BoardState history[1024];
Move moves_log[1024];
int history_len = 1;

Move legal_moves[256];
int legal_moves_count = 0;

int cursor_r = 6;
int cursor_c = 4;
int selected_sq = -1;

/* Settings */
int player_side = WHITE; // WHITE, BLACK, BOTH (Pass-and-play), NONE (AI vs AI)
int flipped = 0;         // 1 if black at bottom
int time_control_type = 0; // 0=Depth, 1=Nodes, 2=Time
int depth_limit = 8;
int nodes_limit = 20000;
int time_limit_ms = 1500;

/* Engine process communication descriptors */
int engine_in[2] = {-1, -1};
int engine_out[2] = {-1, -1};
pid_t engine_pid = -1;
int engine_state = ENGINE_IDLE;
char engine_buf[8192];
int engine_buf_len = 0;

/* Beautiful Unicode Chess Pieces */
const char* piece_symbols[] = {
    " ", "♟", "♞", "♝", "♜", "♛", "♚"
};

/* Known macOS stockfish install coordinates */
const char *engine_paths[] = {
    "/opt/homebrew/bin/stockfish",
    "/usr/local/bin/stockfish",
    "stockfish"
};

/* Output Double-Buffer variables */
char out_buf[32768];
int out_ptr = 0;

void out_str(const char *str) {
    int len = strlen(str);
    if (out_ptr + len < (int)sizeof(out_buf)) {
        memcpy(out_buf + out_ptr, str, len);
        out_ptr += len;
    }
}

void out_fmt(const char *fmt, ...) {
    char temp[2048];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(temp, sizeof(temp), fmt, args);
    va_end(args);
    if (out_ptr + len < (int)sizeof(out_buf)) {
        memcpy(out_buf + out_ptr, temp, len);
        out_ptr += len;
    }
}

void flush_out() {
    write(STDOUT_FILENO, out_buf, out_ptr);
    out_ptr = 0;
}

/* Helpers */
int is_valid_coord(int r, int c) {
    return (r >= 0 && r < 8 && c >= 0 && c < 8);
}

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/* Check if square is attacked by opponent */
int is_square_attacked(BoardState *state, int sq, int attacker_color) {
    int r = sq / 8, c = sq % 8;

    // Knight attacks
    int kn_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int kn_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn_r[i], nc = c + kn_c[i];
        if (is_valid_coord(nr, nc)) {
            int p = state->board[nr * 8 + nc];
            if (p && (p & COLOR_MASK) == attacker_color && (p & PIECE_MASK) == KNIGHT)
                return 1;
        }
    }

    // King attacks
    int k_r[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int k_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + k_r[i], nc = c + k_c[i];
        if (is_valid_coord(nr, nc)) {
            int p = state->board[nr * 8 + nc];
            if (p && (p & COLOR_MASK) == attacker_color && (p & PIECE_MASK) == KING)
                return 1;
        }
    }

    // Pawn attacks
    int p_row = (attacker_color == WHITE) ? r + 1 : r - 1;
    int p_cols[] = {c - 1, c + 1};
    for (int i = 0; i < 2; i++) {
        int pc = p_cols[i];
        if (is_valid_coord(p_row, pc)) {
            int p = state->board[p_row * 8 + pc];
            if (p && (p & COLOR_MASK) == attacker_color && (p & PIECE_MASK) == PAWN)
                return 1;
        }
    }

    // Sliders Orthogonal (Rook & Queen)
    int r_dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int d = 0; d < 4; d++) {
        int nr = r, nc = c;
        while (1) {
            nr += r_dirs[d][0];
            nc += r_dirs[d][1];
            if (!is_valid_coord(nr, nc)) break;
            int p = state->board[nr * 8 + nc];
            if (p) {
                if ((p & COLOR_MASK) == attacker_color && ((p & PIECE_MASK) == ROOK || (p & PIECE_MASK) == QUEEN))
                    return 1;
                break;
            }
        }
    }

    // Sliders Diagonal (Bishop & Queen)
    int b_dirs[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    for (int d = 0; d < 4; d++) {
        int nr = r, nc = c;
        while (1) {
            nr += b_dirs[d][0];
            nc += b_dirs[d][1];
            if (!is_valid_coord(nr, nc)) break;
            int p = state->board[nr * 8 + nc];
            if (p) {
                if ((p & COLOR_MASK) == attacker_color && ((p & PIECE_MASK) == BISHOP || (p & PIECE_MASK) == QUEEN))
                    return 1;
                break;
            }
        }
    }
    return 0;
}

/* Pseudo-Legal Move Generator */
void get_pseudo_moves(BoardState *state, Move *moves, int *count) {
    int turn = state->turn;
    int opp = (turn == WHITE) ? BLACK : WHITE;

    for (int s = 0; s < 64; s++) {
        int piece = state->board[s];
        if (!piece || (piece & COLOR_MASK) != turn) continue;

        int type = piece & PIECE_MASK;
        int r = s / 8, c = s % 8;

        if (type == PAWN) {
            int dir = (turn == WHITE) ? -1 : 1;
            int start_row = (turn == WHITE) ? 6 : 1;
            int promo_row = (turn == WHITE) ? 0 : 7;

            // Step 1
            int tr = r + dir, tc = c;
            if (is_valid_coord(tr, tc) && state->board[tr * 8 + tc] == EMPTY) {
                if (tr == promo_row) {
                    moves[(*count)++] = (Move){s, tr * 8 + tc, QUEEN};
                    moves[(*count)++] = (Move){s, tr * 8 + tc, ROOK};
                    moves[(*count)++] = (Move){s, tr * 8 + tc, BISHOP};
                    moves[(*count)++] = (Move){s, tr * 8 + tc, KNIGHT};
                } else {
                    moves[(*count)++] = (Move){s, tr * 8 + tc, 0};
                    // Step 2
                    if (r == start_row) {
                        int t2r = r + 2 * dir;
                        if (state->board[t2r * 8 + tc] == EMPTY) {
                            moves[(*count)++] = (Move){s, t2r * 8 + tc, 0};
                        }
                    }
                }
            }
            // Capture
            int c_cols[] = {c - 1, c + 1};
            for (int i = 0; i < 2; i++) {
                int col = c_cols[i];
                if (is_valid_coord(tr, col)) {
                    int dest_sq = tr * 8 + col;
                    int dest_p = state->board[dest_sq];
                    if (dest_p && (dest_p & COLOR_MASK) == opp) {
                        if (tr == promo_row) {
                            moves[(*count)++] = (Move){s, dest_sq, QUEEN};
                            moves[(*count)++] = (Move){s, dest_sq, ROOK};
                            moves[(*count)++] = (Move){s, dest_sq, BISHOP};
                            moves[(*count)++] = (Move){s, dest_sq, KNIGHT};
                        } else {
                            moves[(*count)++] = (Move){s, dest_sq, 0};
                        }
                    } else if (dest_sq == state->ep_sq) {
                        moves[(*count)++] = (Move){s, dest_sq, 0};
                    }
                }
            }
        } else if (type == KNIGHT) {
            int kn_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
            int kn_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
            for (int i = 0; i < 8; i++) {
                int tr = r + kn_r[i], tc = c + kn_c[i];
                if (is_valid_coord(tr, tc)) {
                    int dest_p = state->board[tr * 8 + tc];
                    if (!dest_p || (dest_p & COLOR_MASK) == opp) {
                        moves[(*count)++] = (Move){s, tr * 8 + tc, 0};
                    }
                }
            }
        } else if (type == KING) {
            int k_r[] = {-1, -1, -1, 0, 0, 1, 1, 1};
            int k_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
            for (int i = 0; i < 8; i++) {
                int tr = r + k_r[i], tc = c + k_c[i];
                if (is_valid_coord(tr, tc)) {
                    int dest_p = state->board[tr * 8 + tc];
                    if (!dest_p || (dest_p & COLOR_MASK) == opp) {
                        moves[(*count)++] = (Move){s, tr * 8 + tc, 0};
                    }
                }
            }
            // Castling
            if (turn == WHITE) {
                if ((state->castling & 1) && state->board[61] == EMPTY && state->board[62] == EMPTY) {
                    if (!is_square_attacked(state, 60, opp) && !is_square_attacked(state, 61, opp) && !is_square_attacked(state, 62, opp))
                        moves[(*count)++] = (Move){60, 62, 0};
                }
                if ((state->castling & 2) && state->board[59] == EMPTY && state->board[58] == EMPTY && state->board[57] == EMPTY) {
                    if (!is_square_attacked(state, 60, opp) && !is_square_attacked(state, 59, opp) && !is_square_attacked(state, 58, opp))
                        moves[(*count)++] = (Move){60, 58, 0};
                }
            } else {
                if ((state->castling & 4) && state->board[5] == EMPTY && state->board[6] == EMPTY) {
                    if (!is_square_attacked(state, 4, opp) && !is_square_attacked(state, 5, opp) && !is_square_attacked(state, 6, opp))
                        moves[(*count)++] = (Move){4, 6, 0};
                }
                if ((state->castling & 8) && state->board[3] == EMPTY && state->board[2] == EMPTY && state->board[1] == EMPTY) {
                    if (!is_square_attacked(state, 4, opp) && !is_square_attacked(state, 3, opp) && !is_square_attacked(state, 2, opp))
                        moves[(*count)++] = (Move){4, 2, 0};
                }
            }
        } else {
            // Sliders (Bishop, Rook, Queen)
            int dirs[8][2];
            int d_count = 0;
            if (type == ROOK || type == QUEEN) {
                int r_dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (int i = 0; i < 4; i++) { dirs[d_count][0] = r_dirs[i][0]; dirs[d_count][1] = r_dirs[i][1]; d_count++; }
            }
            if (type == BISHOP || type == QUEEN) {
                int b_dirs[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
                for (int i = 0; i < 4; i++) { dirs[d_count][0] = b_dirs[i][0]; dirs[d_count][1] = b_dirs[i][1]; d_count++; }
            }
            for (int d = 0; d < d_count; d++) {
                int tr = r, tc = c;
                while (1) {
                    tr += dirs[d][0];
                    tc += dirs[d][1];
                    if (!is_valid_coord(tr, tc)) break;
                    int dest_p = state->board[tr * 8 + tc];
                    if (!dest_p) {
                        moves[(*count)++] = (Move){s, tr * 8 + tc, 0};
                    } else {
                        if ((dest_p & COLOR_MASK) == opp) {
                            moves[(*count)++] = (Move){s, tr * 8 + tc, 0};
                        }
                        break;
                    }
                }
            }
        }
    }
}

/* Create Deep Copy and Apply Move State */
void make_move(BoardState *next, const BoardState *prev, Move m) {
    *next = *prev;

    int piece = next->board[m.from];
    int color = piece & COLOR_MASK;
    int type = piece & PIECE_MASK;

    next->board[m.from] = EMPTY;

    // En Passant Capture
    if (type == PAWN && m.to == prev->ep_sq) {
        int cap_sq = (color == WHITE) ? m.to + 8 : m.to - 8;
        next->board[cap_sq] = EMPTY;
    }

    // Set next En Passant square
    next->ep_sq = -1;
    if (type == PAWN && abs(m.to - m.from) == 16) {
        next->ep_sq = (m.from + m.to) / 2;
    }

    // Castling updates
    if (type == KING) {
        if (m.from == 60 && m.to == 62) { next->board[61] = WHITE | ROOK; next->board[63] = EMPTY; }
        else if (m.from == 60 && m.to == 58) { next->board[59] = WHITE | ROOK; next->board[56] = EMPTY; }
        else if (m.from == 4 && m.to == 6) { next->board[5] = BLACK | ROOK; next->board[7] = EMPTY; }
        else if (m.from == 4 && m.to == 2) { next->board[3] = BLACK | ROOK; next->board[0] = EMPTY; }
    }

    // Revoke Castling rights
    if (type == KING) {
        if (color == WHITE) next->castling &= ~3;
        else next->castling &= ~12;
    }
    if (type == ROOK) {
        if (m.from == 56) next->castling &= ~2;
        if (m.from == 63) next->castling &= ~1;
        if (m.from == 0)  next->castling &= ~8;
        if (m.from == 7)  next->castling &= ~4;
    }
    // Corner rook captures
    if (m.to == 56) next->castling &= ~2;
    if (m.to == 63) next->castling &= ~1;
    if (m.to == 0)  next->castling &= ~8;
    if (m.to == 7)  next->castling &= ~4;

    // Place & Promote
    if (m.promotion) {
        next->board[m.to] = color | m.promotion;
    } else {
        next->board[m.to] = piece;
    }

    next->turn = (color == WHITE) ? BLACK : WHITE;
    if (type == PAWN || prev->board[m.to] != EMPTY) {
        next->halfmove = 0;
    } else {
        next->halfmove = prev->halfmove + 1;
    }
    if (color == BLACK) {
        next->fullmove = prev->fullmove + 1;
    }
}

/* Evaluate moves to filter King checks */
int get_legal_moves(BoardState *state, Move *legal_m) {
    Move pseudo[256];
    int p_count = 0;
    get_pseudo_moves(state, pseudo, &p_count);

    int l_count = 0;
    for (int i = 0; i < p_count; i++) {
        BoardState temp;
        make_move(&temp, state, pseudo[i]);
        int k_sq = -1;
        int us = state->turn;
        for (int s = 0; s < 64; s++) {
            if (temp.board[s] == (us | KING)) {
                k_sq = s;
                break;
            }
        }
        if (k_sq != -1) {
            int opp = (us == WHITE) ? BLACK : WHITE;
            if (!is_square_attacked(&temp, k_sq, opp)) {
                legal_m[l_count++] = pseudo[i];
            }
        }
    }
    return l_count;
}

/* Start Background UCI Process */
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
    } else {
        close(engine_in[0]);
        close(engine_out[1]);
        fcntl(engine_out[0], F_SETFL, O_NONBLOCK);
    }
}

void init_uci() {
    if (engine_pid == -1) return;
    write(engine_in[1], "uci\nisready\n", 12);
}

/* Convert string coordinate representation to Move object */
Move str_to_move(const char *str, BoardState *state) {
    Move m = {0, 0, 0};
    int fc = str[0] - 'a';
    int fr = '8' - str[1];
    int tc = str[2] - 'a';
    int tr = '8' - str[3];
    m.from = fr * 8 + fc;
    m.to = tr * 8 + tc;
    if (str[4] == 'q') m.promotion = QUEEN;
    else if (str[4] == 'r') m.promotion = ROOK;
    else if (str[4] == 'b') m.promotion = BISHOP;
    else if (str[4] == 'n') m.promotion = KNIGHT;
    return m;
}

void move_to_str(Move m, char *str) {
    int f_r = m.from / 8, f_c = m.from % 8;
    int t_r = m.to / 8, t_c = m.to % 8;
    sprintf(str, "%c%c%c%c", 'a' + f_c, '8' - f_r, 'a' + t_c, '8' - t_r);
    if (m.promotion) {
        char p_char = 'q';
        if (m.promotion == ROOK) p_char = 'r';
        else if (m.promotion == BISHOP) p_char = 'b';
        else if (m.promotion == KNIGHT) p_char = 'n';
        sprintf(str + 4, "%c", p_char);
    }
}

/* UI Renderer */
void render_all() {
    out_ptr = 0;
    out_str("\033[H"); // Cursor to home

    out_str("  \033[1;36m┌──────────────────────────────────────────────────────────────┐\033[0m\n");
    out_str("  \033[1;36m│                     CHESS TERMINAL GUI                       │\033[0m\n");
    out_str("  \033[1;36m└──────────────────────────────────────────────────────────────┘\033[0m\n\n");

    BoardState *current = &history[history_len - 1];

    // Determine King Check condition
    int k_sq = -1;
    int opp = (current->turn == WHITE) ? BLACK : WHITE;
    for (int s = 0; s < 64; s++) {
        if (current->board[s] == (current->turn | KING)) { k_sq = s; break; }
    }
    int check_state = (k_sq != -1) && is_square_attacked(current, k_sq, opp);

    // Header Coordinates
    out_str("       ");
    for (int c = 0; c < 8; c++) {
        int file = flipped ? (7 - c) : c;
        out_fmt("   %c  ", 'A' + file);
    }
    out_str("\n     ┌────────────────────────────────────────────────┐\n");

    for (int r_idx = 0; r_idx < 8; r_idx++) {
        int r = flipped ? (7 - r_idx) : r_idx;
        out_fmt("  %d  │", 8 - r);

        for (int c_idx = 0; c_idx < 8; c_idx++) {
            int c = flipped ? (7 - c_idx) : c_idx;
            int sq = r * 8 + c;

            // Square Coloring
            int is_dark = ((r + c) % 2) == 1;
            const char *bg = is_dark ? "\033[48;5;95m" : "\033[48;5;223m";

            // Overlap Highlight Colors
            if (sq == cursor_r * 8 + cursor_c) {
                bg = "\033[48;5;214m"; // active cursor: Orange
            } else if (sq == selected_sq) {
                bg = "\033[48;5;220m"; // selected piece: Yellow
            } else {
                // Last Move Highlight
                if (history_len > 1) {
                    Move last = moves_log[history_len - 2];
                    if (sq == last.from || sq == last.to) {
                        bg = is_dark ? "\033[48;5;101m" : "\033[48;5;144m"; // Muted Olive
                    }
                }
                // Legal move highlights
                if (selected_sq != -1) {
                    for (int l = 0; l < legal_moves_count; l++) {
                        if (legal_moves[l].from == selected_sq && legal_moves[l].to == sq) {
                            bg = is_dark ? "\033[48;5;66m" : "\033[48;5;117m"; // Muted Blue
                        }
                    }
                }
                // Red Alert Check Highlight
                if (check_state && sq == k_sq) {
                    bg = "\033[48;5;124m"; // Dark Red
                }
            }

            int piece = current->board[sq];
            int p_type = piece & PIECE_MASK;
            int p_color = piece & COLOR_MASK;

            const char *fg = "";
            if (p_color == WHITE) fg = "\033[38;5;231;1m";
            else if (p_color == BLACK) fg = "\033[38;5;16;1m";

            out_fmt("%s%s  %s  \033[0m", bg, fg, piece_symbols[p_type]);
        }
        out_fmt("│  %d\n", 8 - r);
    }
    out_str("     └────────────────────────────────────────────────┘\n");
    out_str("       ");
    for (int c = 0; c < 8; c++) {
        int file = flipped ? (7 - c) : c;
        out_fmt("   %c  ", 'A' + file);
    }
    out_str("\n\n");
    out_str("   \033[1;30mMove: Arrow/WASD | Select/Submit: Space/Enter\033[0m\n");

    // Side Controls & Info panel
    int side_row = 4;
    #define PRINT_S(fmt, ...) out_fmt("\033[%d;58H" fmt, side_row++, ##__VA_ARGS__)

    PRINT_S("\033[1;33m[ ENGINE STATUS ]\033[0m");
    if (engine_pid == -1) {
        PRINT_S("Engine: \033[1;31mUnavailable (Pass-and-Play)\033[0m");
    } else {
        PRINT_S("Engine: \033[1;32mUCI Online (Stockfish)\033[0m");
        PRINT_S("Status: %s", (engine_state == ENGINE_THINKING) ? "\033[1;5;32mTHINKING...\033[0m" : "IDLE");
    }

    side_row++;
    PRINT_S("\033[1;33m[ OPTIONS & CONTROLS ]\033[0m");
    PRINT_S(" \033[1;35mP\033[0m. Player Side: \033[1;37m%s\033[0m",
            (player_side == WHITE) ? "WHITE (You)" :
            (player_side == BLACK) ? "BLACK (You)" :
            (player_side == BOTH) ? "BOTH (Pass & Play)" : "NONE (Engine vs Engine)");
    PRINT_S(" \033[1;35mF\033[0m. Flip Board:  \033[1;37m%s\033[0m", flipped ? "ON" : "OFF");

    const char* tc_str = (time_control_type == 0) ? "DEPTH" : (time_control_type == 1) ? "NODES" : "TIME";
    int tc_val = (time_control_type == 0) ? depth_limit : (time_control_type == 1) ? nodes_limit : time_limit_ms;
    PRINT_S(" \033[1;35mT\033[0m. Time Control: \033[1;37m%s (%d)\033[0m", tc_str, tc_val);
    PRINT_S(" \033[1;35mU\033[0m. Undo Move");
    PRINT_S(" \033[1;35mR\033[0m. Reset Game");
    PRINT_S(" \033[1;35mQ\033[0m. Quit Game");

    side_row++;
    PRINT_S("\033[1;33m[ STATUS METRICS ]\033[0m");
    PRINT_S(" Turn: %s", (current->turn == WHITE) ? "\033[1;37mWHITE\033[0m" : "\033[1;30mBLACK\033[0m");
    PRINT_S(" Fullmoves Played: %d", current->fullmove);

    Move test_m[256];
    int checkmate_detect = get_legal_moves(current, test_m);
    if (checkmate_detect == 0) {
        if (check_state) {
            PRINT_S("\033[1;41;37m  ### CHECKMATE! %s WINS ###  \033[0m", (current->turn == WHITE) ? "BLACK" : "WHITE");
        } else {
            PRINT_S("\033[1;43;30m   ### DRAW (STALEMATE) ###   \033[0m");
        }
    } else if (check_state) {
        PRINT_S("\033[1;41;37m     KING IN CHECK!     \033[0m");
    } else {
        PRINT_S("                         ");
    }

    side_row++;
    PRINT_S("\033[1;33m[ HISTORIC MOVES ]\033[0m");
    int start_print = (history_len > 9) ? (history_len - 9) : 1;
    for (int i = 0; i < 8; i++) {
        int idx = start_print + i;
        if (idx < history_len) {
            char mv[16];
            move_to_str(moves_log[idx - 1], mv);
            int num = (idx + 1) / 2;
            if (idx % 2 != 0) {
                PRINT_S(" %3d. %-8s          ", num, mv);
            } else {
                char prev_mv[16];
                move_to_str(moves_log[idx - 2], prev_mv);
                side_row--;
                PRINT_S(" %3d. %-8s %-8s", num, prev_mv, mv);
            }
        } else {
            PRINT_S("                          ");
        }
    }
    #undef PRINT_S
    flush_out();
}

/* Prompt Pawn promotion UI options */
int prompt_promotion() {
    printf("\033[24;1H\033[1;33mPROMOTION! Select Option: Q=Queen, R=Rook, B=Bishop, N=Knight: \033[0m");
    fflush(stdout);
    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) > 0) {
            if (c == 'q' || c == 'Q') return QUEEN;
            if (c == 'r' || c == 'R') return ROOK;
            if (c == 'b' || c == 'B') return BISHOP;
            if (c == 'n' || c == 'N') return KNIGHT;
        }
    }
}

/* Trigger search constraints for Engine */
void trigger_engine() {
    if (engine_pid == -1 || engine_state == ENGINE_THINKING) return;

    BoardState *current = &history[history_len - 1];
    Move test[256];
    if (get_legal_moves(current, test) == 0) return;

    int active_ai = 0;
    if (player_side == NONE) active_ai = 1;
    else if (player_side == WHITE && current->turn == BLACK) active_ai = 1;
    else if (player_side == BLACK && current->turn == WHITE) active_ai = 1;

    if (active_ai) {
        engine_state = ENGINE_THINKING;
        memset(engine_buf, 0, sizeof(engine_buf));
        engine_buf_len = 0;

        char pos_cmd[8192] = "position startpos moves";
        for (int i = 0; i < history_len - 1; i++) {
            char temp[12];
            move_to_str(moves_log[i], temp);
            strcat(pos_cmd, " ");
            strcat(pos_cmd, temp);
        }
        strcat(pos_cmd, "\n");
        write(engine_in[1], pos_cmd, strlen(pos_cmd));

        char go_cmd[128];
        if (time_control_type == 0) {
            sprintf(go_cmd, "go depth %d\n", depth_limit);
        } else if (time_control_type == 1) {
            sprintf(go_cmd, "go nodes %d\n", nodes_limit);
        } else {
            sprintf(go_cmd, "go movetime %d\n", time_limit_ms);
        }
        write(engine_in[1], go_cmd, strlen(go_cmd));
    }
}

/* Asynchronously capture engine outputs */
void update_engine() {
    if (engine_pid == -1) return;

    char temp[1024];
    int n;
    while ((n = read(engine_out[0], temp, sizeof(temp) - 1)) > 0) {
        temp[n] = '\0';
        if (engine_buf_len + n < (int)sizeof(engine_buf)) {
            memcpy(engine_buf + engine_buf_len, temp, n);
            engine_buf_len += n;
            engine_buf[engine_buf_len] = '\0';
        }

        char *line_start = engine_buf;
        char *line_end;
        while ((line_end = strchr(line_start, '\n')) != NULL) {
            *line_end = '\0';
            if (strncmp(line_start, "bestmove ", 9) == 0) {
                char mv_str[16];
                sscanf(line_start + 9, "%s", mv_str);

                BoardState *curr = &history[history_len - 1];
                Move m = str_to_move(mv_str, curr);

                make_move(&history[history_len], curr, m);
                moves_log[history_len - 1] = m;
                history_len++;

                engine_state = ENGINE_IDLE;
                selected_sq = -1;
                legal_moves_count = get_legal_moves(&history[history_len - 1], legal_moves);

                render_all();
            }
            line_start = line_end + 1;
        }
        int read_bytes = line_start - engine_buf;
        if (read_bytes > 0) {
            memmove(engine_buf, line_start, engine_buf_len - read_bytes);
            engine_buf_len -= read_bytes;
            engine_buf[engine_buf_len] = '\0';
        }
    }
}

/* Interactive human input controls mapping */
void handle_input() {
    char c;
    if (read(STDIN_FILENO, &c, 1) <= 0) return;

    if (c == '\033') { // Escape Sequence (Arrows)
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return;
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return;
        if (seq[0] == '[') {
            int dr = 0, dc = 0;
            if (seq[1] == 'A') dr = -1;
            else if (seq[1] == 'B') dr = 1;
            else if (seq[1] == 'C') dc = 1;
            else if (seq[1] == 'D') dc = -1;

            if (flipped) { dr = -dr; dc = -dc; }
            cursor_r = (cursor_r + dr < 0) ? 0 : (cursor_r + dr > 7) ? 7 : cursor_r + dr;
            cursor_c = (cursor_c + dc < 0) ? 0 : (cursor_c + dc > 7) ? 7 : cursor_c + dc;
        }
    } else if (c == 'w' || c == 'W') { cursor_r = (cursor_r - 1 < 0) ? 0 : cursor_r - 1; }
      else if (c == 's' || c == 'S') { cursor_r = (cursor_r + 1 > 7) ? 7 : cursor_r + 1; }
      else if (c == 'a' || c == 'A') { cursor_c = (cursor_c - 1 < 0) ? 0 : cursor_c - 1; }
      else if (c == 'd' || c == 'D') { cursor_c = (cursor_c + 1 > 7) ? 7 : cursor_c + 1; }
      else if (c == ' ' || c == '\n') {
          BoardState *curr = &history[history_len - 1];
          int sq = cursor_r * 8 + cursor_c;

          if (selected_sq == -1) {
              int piece = curr->board[sq];
              if (piece && (piece & COLOR_MASK) == curr->turn) {
                  selected_sq = sq;
              }
          } else {
              if (sq == selected_sq) {
                  selected_sq = -1;
              } else {
                  int match_idx = -1;
                  int matches[4], match_cnt = 0;
                  for (int i = 0; i < legal_moves_count; i++) {
                      if (legal_moves[i].from == selected_sq && legal_moves[i].to == sq) {
                          matches[match_cnt++] = i;
                      }
                  }
                  if (match_cnt > 0) {
                      match_idx = matches[0];
                      if (legal_moves[match_idx].promotion != 0) {
                          int choice = prompt_promotion();
                          for (int i = 0; i < match_cnt; i++) {
                              if (legal_moves[matches[i]].promotion == choice) {
                                  match_idx = matches[i];
                                  break;
                              }
                          }
                      }
                      make_move(&history[history_len], curr, legal_moves[match_idx]);
                      moves_log[history_len - 1] = legal_moves[match_idx];
                      history_len++;

                      selected_sq = -1;
                      legal_moves_count = get_legal_moves(&history[history_len - 1], legal_moves);
                  } else {
                      int piece = curr->board[sq];
                      if (piece && (piece & COLOR_MASK) == curr->turn) selected_sq = sq;
                      else selected_sq = -1;
                  }
              }
          }
      } else if (c == 'p' || c == 'P') {
          if (player_side == WHITE) player_side = BLACK;
          else if (player_side == BLACK) player_side = BOTH;
          else if (player_side == BOTH) player_side = NONE;
          else player_side = WHITE;
          selected_sq = -1;
      } else if (c == 'f' || c == 'F') {
          flipped = !flipped;
      } else if (c == 't' || c == 'T') {
          if (time_control_type == 0) {
              if (depth_limit == 6) depth_limit = 8;
              else if (depth_limit == 8) depth_limit = 10;
              else { depth_limit = 6; time_control_type = 1; }
          } else if (time_control_type == 1) {
              if (nodes_limit == 20000) nodes_limit = 50000;
              else { nodes_limit = 20000; time_control_type = 2; }
          } else {
              if (time_limit_ms == 1500) time_limit_ms = 3000;
              else { time_limit_ms = 1500; time_control_type = 0; }
          }
      } else if (c == 'u' || c == 'U') {
          int limit = (player_side == WHITE || player_side == BLACK) ? 2 : 1;
          for (int i = 0; i < limit; i++) {
              if (history_len > 1) { history_len--; }
          }
          selected_sq = -1;
          legal_moves_count = get_legal_moves(&history[history_len - 1], legal_moves);
          if (engine_pid != -1) { write(engine_in[1], "stop\n", 5); engine_state = ENGINE_IDLE; }
      } else if (c == 'r' || c == 'R') {
          history_len = 1;
          selected_sq = -1;
          legal_moves_count = get_legal_moves(&history[0], legal_moves);
          if (engine_pid != -1) { write(engine_in[1], "ucinewgame\n", 11); engine_state = ENGINE_IDLE; }
      } else if (c == 'q' || c == 'Q') {
          running = 0;
      }
}

void init_board(BoardState *state) {
    memset(state, 0, sizeof(BoardState));
    int order[] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
    for (int i = 0; i < 8; i++) {
        state->board[0 * 8 + i] = BLACK | order[i];
        state->board[1 * 8 + i] = BLACK | PAWN;
        state->board[6 * 8 + i] = WHITE | PAWN;
        state->board[7 * 8 + i] = WHITE | order[i];
    }
    state->turn = WHITE;
    state->castling = 15;
    state->ep_sq = -1;
    state->halfmove = 0;
    state->fullmove = 1;
}

void clean_up() {
    if (engine_pid != -1) {
        write(engine_in[1], "quit\n", 5);
        close(engine_in[1]);
        close(engine_out[0]);
        kill(engine_pid, SIGTERM);
    }
    printf("\033[?25h");      // Show cursor
    printf("\033[?1049l");     // Exit alternate screen buffer
    fflush(stdout);
    disable_raw_mode();
}

void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

int main() {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    init_board(&history[0]);
    legal_moves_count = get_legal_moves(&history[0], legal_moves);

    // Dynamic Engine Selection Lookups
    for (int i = 0; i < 3; i++) {
        if (i < 2 && access(engine_paths[i], X_OK) == 0) {
            start_engine(engine_paths[i]);
            break;
        } else if (i == 2) {
            start_engine(engine_paths[i]); // Path variable check fallback
        }
    }
    init_uci();

    enable_raw_mode();
    printf("\033[?1049h"); // Alternate screen buffer redirect
    printf("\033[?25l");   // Hide default cursor
    fflush(stdout);

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[1].fd = (engine_pid != -1) ? engine_out[0] : -1;
    fds[1].events = POLLIN;

    render_all();

    while (running) {
        trigger_engine();

        // High performance POSIX multiplex waiting
        int poller = poll(fds, 2, 50);
        if (poller > 0) {
            if (fds[0].revents & POLLIN) {
                handle_input();
                render_all();
            }
            if (fds[1].revents & POLLIN) {
                update_engine();
            }
        } else {
            // Clock ticking and updates during idle waits
            render_all();
        }
    }

    clean_up();
    return 0;
}
