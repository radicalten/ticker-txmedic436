#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/wait.h>

// Piece representations
#define EMPTY 0
#define WP 1
#define WN 2
#define WB 3
#define WR 4
#define WQ 5
#define WK 6
#define BP 9
#define BN 10
#define BB 11
#define BR 12
#define BQ 13
#define BK 14

#define COLOR_WHITE 0
#define COLOR_BLACK 1

// Color maps
#define BG_LIGHT     "48;5;187"
#define BG_DARK      "48;5;137"
#define BG_CURSOR    "48;5;214" // Orange
#define BG_SELECTED  "48;5;220" // Yellow
#define BG_LEGAL     "48;5;108" // Shaded Green
#define BG_PREV      "48;5;67"  // Soft Blue
#define BG_CHECK     "48;5;167" // Light Red

#define FG_WHITE     "38;5;231"
#define FG_BLACK     "38;5;232"

const char* unicode_pieces[] = {
    " ", "♙", "♘", "♗", "♖", "♕", "♔", "", "", "♟", "♞", "♝", "♜", "♛", "♚"
};

typedef struct {
    int from;
    int to;
    int promo; // Piece type to promote to
} Move;

typedef struct {
    uint8_t board[64];
    int active_color;
    int castling;   // 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep_square;  // En passant target square (-1 if none)
    int last_from;  // For visual blue highlights
    int last_to;
} GameState;

// Global engine pipes
int engine_in = -1;
int engine_out = -1;
pid_t engine_pid = -1;
char engine_path[256] = "stockfish";

// Engine configurations
int pvp_mode = 0;
int tc_mode = 1;     // 0 = Movetime, 1 = Depth, 2 = Nodes
int tc_val = 8;      // Default to depth 8

// Game variables
GameState history[2048];
Move move_history[2048];
char pgn_list[2048][12];
int history_count = 0;

struct termios orig_termios;

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int get_color(int piece) {
    if (piece == EMPTY) return -1;
    return (piece & 8) ? COLOR_BLACK : COLOR_WHITE;
}

// Minimalistic clean display drawer
void init_game() {
    const uint8_t initial_board[64] = {
        BR, BN, BB, BQ, BK, BB, BN, BR,
        BP, BP, BP, BP, BP, BP, BP, BP,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        WP, WP, WP, WP, WP, WP, WP, WP,
        WR, WN, WB, WQ, WK, WB, WN, WR
    };
    memcpy(history[0].board, initial_board, 64);
    history[0].active_color = COLOR_WHITE;
    history[0].castling = 1 | 2 | 4 | 8;
    history[0].ep_square = -1;
    history[0].last_from = -1;
    history[0].last_to = -1;
    history_count = 1;
}

// Pseudo-legal move generator
int get_pseudo_moves(GameState* state, int from, Move* moves) {
    int count = 0;
    int piece = state->board[from];
    if (piece == EMPTY) return 0;
    int color = get_color(piece);
    int r = from / 8, c = from % 8;

    if (piece == WP) {
        int to = from - 8;
        if (to >= 0 && state->board[to] == EMPTY) {
            if (to / 8 == 0) {
                moves[count++] = (Move){from, to, WQ};
                moves[count++] = (Move){from, to, WR};
            } else {
                moves[count++] = (Move){from, to, EMPTY};
                if (r == 6 && state->board[from - 16] == EMPTY)
                    moves[count++] = (Move){from, from - 16, EMPTY};
            }
        }
        int targets[] = {from - 9, from - 7};
        for (int i = 0; i < 2; i++) {
            int t = targets[i];
            if (t >= 0 && abs((t % 8) - c) == 1) {
                if (state->board[t] != EMPTY && get_color(state->board[t]) == COLOR_BLACK) {
                    if (t / 8 == 0) {
                        moves[count++] = (Move){from, t, WQ};
                    } else {
                        moves[count++] = (Move){from, t, EMPTY};
                    }
                } else if (t == state->ep_square) {
                    moves[count++] = (Move){from, t, EMPTY};
                }
            }
        }
    } else if (piece == BP) {
        int to = from + 8;
        if (to < 64 && state->board[to] == EMPTY) {
            if (to / 8 == 7) {
                moves[count++] = (Move){from, to, BQ};
                moves[count++] = (Move){from, to, BR};
            } else {
                moves[count++] = (Move){from, to, EMPTY};
                if (r == 1 && state->board[from + 16] == EMPTY)
                    moves[count++] = (Move){from, from + 16, EMPTY};
            }
        }
        int targets[] = {from + 7, from + 9};
        for (int i = 0; i < 2; i++) {
            int t = targets[i];
            if (t < 64 && abs((t % 8) - c) == 1) {
                if (state->board[t] != EMPTY && get_color(state->board[t]) == COLOR_WHITE) {
                    if (t / 8 == 7) {
                        moves[count++] = (Move){from, t, BQ};
                    } else {
                        moves[count++] = (Move){from, t, EMPTY};
                    }
                } else if (t == state->ep_square) {
                    moves[count++] = (Move){from, t, EMPTY};
                }
            }
        }
    } else if (piece == WN || piece == BN) {
        int r_off[] = {-2, -2, -1, -1, 1, 1, 2, 2};
        int c_off[] = {-1, 1, -2, 2, -2, 2, -1, 1};
        for (int i = 0; i < 8; i++) {
            int nr = r + r_off[i], nc = c + c_off[i];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int to = nr * 8 + nc;
                if (state->board[to] == EMPTY || get_color(state->board[to]) != color)
                    moves[count++] = (Move){from, to, EMPTY};
            }
        }
    } else if (piece == WK || piece == BK) {
        int r_off[] = {-1, -1, -1, 0, 0, 1, 1, 1};
        int c_off[] = {-1, 0, 1, -1, 1, -1, 0, 1};
        for (int i = 0; i < 8; i++) {
            int nr = r + r_off[i], nc = c + c_off[i];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int to = nr * 8 + nc;
                if (state->board[to] == EMPTY || get_color(state->board[to]) != color)
                    moves[count++] = (Move){from, to, EMPTY};
            }
        }
    } else {
        int diag = (piece == WB || piece == BB || piece == WQ || piece == BQ);
        int orth = (piece == WR || piece == BR || piece == WQ || piece == BQ);
        int dirs[8][2], nd = 0;
        if (orth) {
            dirs[nd][0] = 0; dirs[nd][1] = 1; nd++;
            dirs[nd][0] = 0; dirs[nd][1] = -1; nd++;
            dirs[nd][0] = 1; dirs[nd][1] = 0; nd++;
            dirs[nd][0] = -1; dirs[nd][1] = 0; nd++;
        }
        if (diag) {
            dirs[nd][0] = 1; dirs[nd][1] = 1; nd++;
            dirs[nd][0] = 1; dirs[nd][1] = -1; nd++;
            dirs[nd][0] = -1; dirs[nd][1] = 1; nd++;
            dirs[nd][0] = -1; dirs[nd][1] = -1; nd++;
        }
        for (int d = 0; d < nd; d++) {
            int nr = r, nc = c;
            while (1) {
                nr += dirs[d][0]; nc += dirs[d][1];
                if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
                int to = nr * 8 + nc;
                if (state->board[to] == EMPTY) {
                    moves[count++] = (Move){from, to, EMPTY};
                } else {
                    if (get_color(state->board[to]) != color)
                        moves[count++] = (Move){from, to, EMPTY};
                    break;
                }
            }
        }
    }
    return count;
}

int is_square_attacked(int sq, int attacker_color, const uint8_t* board) {
    int r = sq / 8, c = sq % 8;
    if (attacker_color == COLOR_WHITE) {
        if (r < 7) {
            if (c > 0 && board[(r + 1) * 8 + c - 1] == WP) return 1;
            if (c < 7 && board[(r + 1) * 8 + c + 1] == WP) return 1;
        }
    } else {
        if (r > 0) {
            if (c > 0 && board[(r - 1) * 8 + c - 1] == BP) return 1;
            if (c < 7 && board[(r - 1) * 8 + c + 1] == BP) return 1;
        }
    }
    int kn_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int kn_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    int target_knight = (attacker_color == COLOR_WHITE) ? WN : BN;
    for (int i = 0; i < 8; i++) {
        int nr = r + kn_r[i], nc = c + kn_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (board[nr * 8 + nc] == target_knight) return 1;
        }
    }
    int target_king = (attacker_color == COLOR_WHITE) ? WK : BK;
    int k_r[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int k_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + k_r[i], nc = c + k_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (board[nr * 8 + nc] == target_king) return 1;
        }
    }
    int target_rook = (attacker_color == COLOR_WHITE) ? WR : BR;
    int target_queen = (attacker_color == COLOR_WHITE) ? WQ : BQ;
    int r_dirs[4][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};
    for (int d = 0; d < 4; d++) {
        int nr = r, nc = c;
        while (1) {
            nr += r_dirs[d][0]; nc += r_dirs[d][1];
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            int p = board[nr * 8 + nc];
            if (p != EMPTY) {
                if (p == target_rook || p == target_queen) return 1;
                break;
            }
        }
    }
    int target_bishop = (attacker_color == COLOR_WHITE) ? WB : BB;
    int b_dirs[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    for (int d = 0; d < 4; d++) {
        int nr = r, nc = c;
        while (1) {
            nr += b_dirs[d][0]; nc += b_dirs[d][1];
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            int p = board[nr * 8 + nc];
            if (p != EMPTY) {
                if (p == target_bishop || p == target_queen) return 1;
                break;
            }
        }
    }
    return 0;
}

int is_in_check(int color, const uint8_t* board) {
    int king_val = (color == COLOR_WHITE) ? WK : BK;
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (board[i] == king_val) { king_sq = i; break; }
    }
    if (king_sq == -1) return 0;
    return is_square_attacked(king_sq, 1 - color, board);
}

void make_move(GameState* current, Move m, GameState* next) {
    memcpy(next, current, sizeof(GameState));
    int piece = next->board[m.from];
    next->board[m.to] = piece;
    next->board[m.from] = EMPTY;

    if (m.promo != EMPTY) {
        next->board[m.to] = m.promo;
    }

    // Castling execution
    if (piece == WK && m.from == 60) {
        if (m.to == 62) { next->board[61] = WR; next->board[63] = EMPTY; }
        else if (m.to == 58) { next->board[59] = WR; next->board[56] = EMPTY; }
    } else if (piece == BK && m.from == 4) {
        if (m.to == 6) { next->board[5] = BR; next->board[7] = EMPTY; }
        else if (m.to == 2) { next->board[3] = BR; next->board[0] = EMPTY; }
    }

    // En Passant execution
    if ((piece == WP || piece == BP) && m.to == current->ep_square) {
        if (piece == WP) next->board[m.to + 8] = EMPTY;
        else next->board[m.to - 8] = EMPTY;
    }

    // Set En Passant target
    if (piece == WP && m.from - m.to == 16) next->ep_square = m.from - 8;
    else if (piece == BP && m.to - m.from == 16) next->ep_square = m.from + 8;
    else next->ep_square = -1;

    // Update castling rights
    if (m.from == 60) next->castling &= ~3;
    if (m.from == 4)  next->castling &= ~12;
    if (m.from == 56 || m.to == 56) next->castling &= ~2;
    if (m.from == 63 || m.to == 63) next->castling &= ~1;
    if (m.from == 0  || m.to == 0)  next->castling &= ~8;
    if (m.from == 7  || m.to == 7)  next->castling &= ~4;

    next->last_from = m.from;
    next->last_to = m.to;
    next->active_color = 1 - current->active_color;
}

int get_legal_moves(GameState* state, int from, Move* legal_moves) {
    Move temp_moves[128];
    int count = get_pseudo_moves(state, from, temp_moves);
    int legal_count = 0;
    for (int i = 0; i < count; i++) {
        GameState next;
        make_move(state, temp_moves[i], &next);
        if (!is_in_check(state->active_color, next.board)) {
            legal_moves[legal_count++] = temp_moves[i];
        }
    }
    // Add castling checks specifically if King is not in check
    int piece = state->board[from];
    int color = state->active_color;
    if (piece == WK && from == 60 && !is_in_check(COLOR_WHITE, state->board)) {
        if ((state->castling & 1) && state->board[61] == EMPTY && state->board[62] == EMPTY) {
            if (!is_square_attacked(61, COLOR_BLACK, state->board) && !is_square_attacked(62, COLOR_BLACK, state->board)) {
                legal_moves[legal_count++] = (Move){60, 62, EMPTY};
            }
        }
        if ((state->castling & 2) && state->board[59] == EMPTY && state->board[58] == EMPTY && state->board[57] == EMPTY) {
            if (!is_square_attacked(59, COLOR_BLACK, state->board) && !is_square_attacked(58, COLOR_BLACK, state->board)) {
                legal_moves[legal_count++] = (Move){60, 58, EMPTY};
            }
        }
    } else if (piece == BK && from == 4 && !is_in_check(COLOR_BLACK, state->board)) {
        if ((state->castling & 4) && state->board[5] == EMPTY && state->board[6] == EMPTY) {
            if (!is_square_attacked(5, COLOR_WHITE, state->board) && !is_square_attacked(6, COLOR_WHITE, state->board)) {
                legal_moves[legal_count++] = (Move){4, 6, EMPTY};
            }
        }
        if ((state->castling & 8) && state->board[3] == EMPTY && state->board[2] == EMPTY && state->board[1] == EMPTY) {
            if (!is_square_attacked(3, COLOR_WHITE, state->board) && !is_square_attacked(2, COLOR_WHITE, state->board)) {
                legal_moves[legal_count++] = (Move){4, 2, EMPTY};
            }
        }
    }
    return legal_count;
}

void get_move_pgn(GameState* state, Move m, char* out) {
    int piece = state->board[m.from];
    int target = state->board[m.to];
    int p_type = piece & 7;
    int len = 0;

    if (p_type == WK && m.from == 60 && m.to == 62) { strcpy(out, "O-O"); return; }
    if (p_type == WK && m.from == 60 && m.to == 58) { strcpy(out, "O-O-O"); return; }
    if (p_type == BK && m.from == 4 && m.to == 6) { strcpy(out, "O-O"); return; }
    if (p_type == BK && m.from == 4 && m.to == 2) { strcpy(out, "O-O-O"); return; }

    if (p_type != WP && p_type != BP) {
        const char* piece_chars = "  NBRQK";
        out[len++] = piece_chars[p_type];
    } else if (target != EMPTY || m.to == state->ep_square) {
        out[len++] = 'a' + (m.from % 8);
    }
    if (target != EMPTY || m.to == state->ep_square) {
        out[len++] = 'x';
    }
    out[len++] = 'a' + (m.to % 8);
    out[len++] = '1' + (7 - (m.to / 8));

    if (m.promo != EMPTY) {
        out[len++] = '=';
        const char* piece_chars = "  NBRQK";
        out[len++] = piece_chars[m.promo & 7];
    }
    out[len] = '\0';
}

void render_square(int r, int c, int cursor_r, int cursor_c, int sel_sq, Move* legal_moves, int num_legal, GameState* state) {
    int sq = r * 8 + c;
    const char* bg = ((r + c) % 2 == 0) ? BG_LIGHT : BG_DARK;

    if (r == cursor_r && c == cursor_c) {
        bg = BG_CURSOR;
    } else if (state->board[sq] == (state->active_color == COLOR_WHITE ? WK : BK) && is_in_check(state->active_color, state->board)) {
        bg = BG_CHECK;
    } else if (sq == sel_sq) {
        bg = BG_SELECTED;
    } else {
        int is_legal = 0;
        for (int i = 0; i < num_legal; i++) {
            if (legal_moves[i].to == sq) { is_legal = 1; break; }
        }
        if (is_legal) {
            bg = BG_LEGAL;
        } else if (sq == state->last_from || sq == state->last_to) {
            bg = BG_PREV;
        }
    }
    int piece = state->board[sq];
    const char* fg = (get_color(piece) == COLOR_WHITE) ? FG_WHITE : FG_BLACK;
    printf("\x1b[%s;%sm %s \x1b[0m", fg, bg, unicode_pieces[piece]);
}

void draw_screen(GameState* state, int cursor_r, int cursor_c, int sel_sq, Move* legal_moves, int num_legal, int total_moves, int engine_thinking) {
    printf("\x1b[H"); // Put cursor at home coordinates
    printf("\x1b[1;36m  === TERMINAL CHESS GUI ===\x1b[0m\n\n");

    for (int r = 0; r < 8; r++) {
        printf("  %d ", 8 - r);
        for (int c = 0; c < 8; c++) {
            render_square(r, c, cursor_r, cursor_c, sel_sq, legal_moves, num_legal, state);
        }

        // Side information panel
        if (r == 0) {
            printf("  | \x1b[1mSETTINGS & STATUS\x1b[0m");
        } else if (r == 1) {
            printf("  | Mode: %s", pvp_mode ? "Player vs Player" : "vs Engine");
        } else if (r == 2) {
            const char* modes[] = {"MoveTime", "Depth", "Nodes"};
            printf("  | TC Limit: %d (%s)", tc_val, modes[tc_mode]);
        } else if (r == 3) {
            printf("  | Turn: %s", (state->active_color == COLOR_WHITE) ? "\x1b[1;37mWhite\x1b[0m" : "\x1b[1;33mBlack\x1b[0m");
        } else if (r == 4) {
            printf("  | Status: %s", engine_thinking ? "\x1b[1;31mThinking...\x1b[0m" : "Ready");
        } else if (r == 5) {
            printf("  | --------------------");
        } else {
            int start_line = r - 6; // Display last 2 full turns
            int total_full_lines = (total_moves + 1) / 2;
            int scroll_offset = (total_full_lines > 2) ? (total_full_lines - 2) : 0;
            int line = start_line + scroll_offset;
            int move_idx = line * 2;
            if (move_idx < total_moves) {
                printf("  | %2d. %-7s %-7s", line + 1, pgn_list[move_idx], (move_idx + 1 < total_moves) ? pgn_list[move_idx + 1] : "");
            } else {
                printf("  |                     ");
            }
        }
        printf("\n");
    }
    printf("     a  b  c  d  e  f  g  h\n\n");
    printf("\x1b[1m CONTROLS:\x1b[0m\n");
    printf(" [WASD/Arrows] Navigate  [Space/Enter] Select/Move\n");
    printf(" [U] Undo                [T] Time Control Type\n");
    printf(" [C] Change Limit Value  [M] Change Mode (PvP/Engine)\n");
    printf(" [Q] Quit Game\n\n");
}

void start_engine() {
    int to_engine[2], from_engine[2];
    if (pipe(to_engine) < 0 || pipe(from_engine) < 0) return;
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[1]);
        close(from_engine[0]);
        execlp(engine_path, engine_path, NULL);
        exit(1);
    }
    close(to_engine[0]);
    close(from_engine[1]);
    engine_in = to_engine[1];
    engine_out = from_engine[0];
    fcntl(engine_out, F_SETFL, O_NONBLOCK);
    write(engine_in, "uci\n", 4);
    write(engine_in, "isready\n", 8);
}

void send_engine_position() {
    char cmd[8192];
    int len = sprintf(cmd, "position startpos");
    if (history_count > 1) {
        len += sprintf(cmd + len, " moves");
        for (int i = 0; i < history_count - 1; i++) {
            char promo_c = '\0';
            Move m = move_history[i];
            if (m.promo != EMPTY) {
                int p = m.promo & 7;
                if (p == WN || p == BN) promo_c = 'n';
                if (p == WB || p == BB) promo_c = 'b';
                if (p == WR || p == BR) promo_c = 'r';
                if (p == WQ || p == BQ) promo_c = 'q';
            }
            if (promo_c) {
                len += sprintf(cmd + len, " %c%d%c%d%c", 'a' + (m.from % 8), 8 - (m.from / 8), 'a' + (m.to % 8), 8 - (m.to / 8), promo_c);
            } else {
                len += sprintf(cmd + len, " %c%d%c%d", 'a' + (m.from % 8), 8 - (m.from / 8), 'a' + (m.to % 8), 8 - (m.to / 8));
            }
        }
    }
    sprintf(cmd + len, "\n");
    write(engine_in, cmd, strlen(cmd));

    char go_cmd[128];
    if (tc_mode == 0) {
        sprintf(go_cmd, "go movetime %d\n", tc_val);
    } else if (tc_mode == 1) {
        sprintf(go_cmd, "go depth %d\n", tc_val);
    } else {
        sprintf(go_cmd, "go nodes %d\n", tc_val);
    }
    write(engine_in, go_cmd, strlen(go_cmd));
}

int read_key() {
    char c;
    int n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return 0;
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return '\x1b';
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 'W'; // UP
                case 'B': return 'S'; // DOWN
                case 'C': return 'D'; // RIGHT
                case 'D': return 'A'; // LEFT
            }
        }
        return '\x1b';
    }
    return toupper(c);
}

int main() {
    init_game();
    enable_raw_mode();
    start_engine();

    int cursor_r = 6, cursor_c = 4;
    int sel_sq = -1;
    Move legal_moves[128];
    int num_legal = 0;
    int engine_thinking = 0;

    char line_buf[4096];
    int line_len = 0;

    // Clear display once initially
    printf("\x1b[2J");

    while (1) {
        GameState* current = &history[history_count - 1];

        // Trigger engine move execution if applicable
        if (!pvp_mode && current->active_color == COLOR_BLACK && !engine_thinking) {
            send_engine_position();
            engine_thinking = 1;
        }

        draw_screen(current, cursor_r, cursor_c, sel_sq, legal_moves, num_legal, history_count - 1, engine_thinking);

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        int max_fd = STDIN_FILENO;
        if (engine_thinking && engine_out != -1) {
            FD_SET(engine_out, &fds);
            if (engine_out > max_fd) max_fd = engine_out;
        }

        struct timeval tv = {0, 15000}; // 15ms frame cycle
        int active_fds = select(max_fd + 1, &fds, NULL, NULL, &tv);

        if (active_fds > 0) {
            // Asynchronous engine reading
            if (engine_thinking && engine_out != -1 && FD_ISSET(engine_out, &fds)) {
                char c;
                while (read(engine_out, &c, 1) > 0) {
                    if (c == '\n') {
                        line_buf[line_len] = '\0';
                        line_len = 0;
                        if (strncmp(line_buf, "bestmove ", 9) == 0) {
                            char best[16];
                            sscanf(line_buf, "bestmove %s", best);
                            if (strcmp(best, "(none)") != 0) {
                                int from = (best[0] - 'a') + (8 - (best[1] - '0')) * 8;
                                int to = (best[2] - 'a') + (8 - (best[3] - '0')) * 8;
                                int promo = EMPTY;
                                if (best[4] != '\0' && !isspace(best[4])) {
                                    if (best[4] == 'q') promo = BQ;
                                    else if (best[4] == 'r') promo = BR;
                                    else if (best[4] == 'b') promo = BB;
                                    else if (best[4] == 'n') promo = BN;
                                }
                                Move m = {from, to, promo};
                                get_move_pgn(current, m, pgn_list[history_count - 1]);
                                make_move(current, m, &history[history_count]);
                                move_history[history_count - 1] = m;
                                history_count++;
                            }
                            engine_thinking = 0;
                            break;
                        }
                    } else if (line_len < 4095) {
                        line_buf[line_len++] = c;
                    }
                }
            }

            // User input handling
            if (FD_ISSET(STDIN_FILENO, &fds)) {
                int key = read_key();
                if (key == 'Q') {
                    break;
                } else if (key == 'W' && cursor_r > 0) {
                    cursor_r--;
                } else if (key == 'S' && cursor_r < 7) {
                    cursor_r++;
                } else if (key == 'A' && cursor_c > 0) {
                    cursor_c--;
                } else if (key == 'D' && cursor_c < 7) {
                    cursor_c++;
                } else if (key == 'U') {
                    // Undo support
                    int undos_needed = pvp_mode ? 1 : 2;
                    if (history_count > undos_needed) {
                        history_count -= undos_needed;
                        sel_sq = -1;
                        num_legal = 0;
                        engine_thinking = 0;
                        printf("\x1b[2J"); // Complete redraw
                    }
                } else if (key == 'T') {
                    tc_mode = (tc_mode + 1) % 3;
                } else if (key == 'M') {
                    pvp_mode = !pvp_mode;
                    sel_sq = -1;
                    num_legal = 0;
                    engine_thinking = 0;
                } else if (key == 'C') {
                    disable_raw_mode();
                    printf("\nEnter limit value (current: %d): ", tc_val);
                    char input[32];
                    if (fgets(input, sizeof(input), stdin)) {
                        int v = atoi(input);
                        if (v > 0) tc_val = v;
                    }
                    enable_raw_mode();
                    printf("\x1b[2J");
                } else if (key == ' ' || key == '\n' || key == '\r') {
                    if (engine_thinking) continue;
                    int sq = cursor_r * 8 + cursor_c;
                    if (sel_sq == -1) {
                        if (current->board[sq] != EMPTY && get_color(current->board[sq]) == current->active_color) {
                            sel_sq = sq;
                            num_legal = get_legal_moves(current, sel_sq, legal_moves);
                        }
                    } else {
                        int selected_move_idx = -1;
                        for (int i = 0; i < num_legal; i++) {
                            if (legal_moves[i].to == sq) { selected_move_idx = i; break; }
                        }
                        if (selected_move_idx != -1) {
                            Move m = legal_moves[selected_move_idx];
                            // Check Pawn Promotion overlay triggers
                            if ((current->board[m.from] == WP && m.to / 8 == 0) || (current->board[m.from] == BP && m.to / 8 == 7)) {
                                disable_raw_mode();
                                printf("\nPromote to: [Q]ueen, [R]ook, [B]ishop, [K]night: ");
                                char ans[16];
                                if (fgets(ans, sizeof(ans), stdin)) {
                                    char a = toupper(ans[0]);
                                    if (a == 'R') m.promo = (current->active_color == COLOR_WHITE) ? WR : BR;
                                    else if (a == 'B') m.promo = (current->active_color == COLOR_WHITE) ? WB : BB;
                                    else if (a == 'K') m.promo = (current->active_color == COLOR_WHITE) ? WN : BN;
                                    else m.promo = (current->active_color == COLOR_WHITE) ? WQ : BQ;
                                }
                                enable_raw_mode();
                                printf("\x1b[2J");
                            }
                            get_move_pgn(current, m, pgn_list[history_count - 1]);
                            make_move(current, m, &history[history_count]);
                            move_history[history_count - 1] = m;
                            history_count++;
                            sel_sq = -1;
                            num_legal = 0;
                        } else {
                            if (current->board[sq] != EMPTY && get_color(current->board[sq]) == current->active_color) {
                                sel_sq = sq;
                                num_legal = get_legal_moves(current, sel_sq, legal_moves);
                            } else {
                                sel_sq = -1;
                                num_legal = 0;
                            }
                        }
                    }
                }
            }
        }
    }

    if (engine_pid > 0) {
        kill(engine_pid, SIGTERM);
        waitpid(engine_pid, NULL, 0);
    }
    disable_raw_mode();
    printf("\x1b[2J\x1b[HGoodbye!\n");
    return 0;
}
