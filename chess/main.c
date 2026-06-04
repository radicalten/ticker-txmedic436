#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <signal.h>

// Piece representations
#define EMPTY 0
#define W_PAWN 1
#define W_KNIGHT 2
#define W_BISHOP 3
#define W_ROOK 4
#define W_QUEEN 5
#define W_KING 6
#define B_PAWN 9
#define B_KNIGHT 10
#define B_BISHOP 11
#define B_ROOK 12
#define B_QUEEN 13
#define B_KING 14

#define SQ(r, c) ((r) * 8 + (c))
#define ROW(sq) ((sq) / 8)
#define COL(sq) ((sq) % 8)
#define IS_VALID(r, c) ((r) >= 0 && (r) < 8 && (c) >= 0 && (c) < 8)
#define COLOR(p) ((p) == EMPTY ? -1 : ((p) < 8 ? 0 : 1)) // 0: White, 1: Black

typedef struct {
    int from;
    int to;
    int promo;
} Move;

typedef struct {
    int board[64];
    int side;      // 0: White, 1: Black
    int castle;    // Bitmask: 1:WK, 2:WQ, 4:BK, 8:BQ
    int ep;        // En Passant square index (-1 if none)
    int halfmove;
    int fullmove;
} BoardState;

// UCI Time Controls
typedef enum { TC_DEPTH, TC_NODES, TC_TIME } TCType;
typedef struct {
    TCType type;
    int value;
} TimeControl;

#define MAX_HISTORY 2048
BoardState board_history[MAX_HISTORY];
int board_history_count = 0;
char uci_history[MAX_HISTORY][10];
char san_history[MAX_HISTORY][16];
int history_count = 0;

TimeControl tc = { TC_DEPTH, 10 }; // Default Search Depth = 10
struct termios orig_termios;
int to_engine[2], from_engine[2];
pid_t engine_pid = -1;
char engine_buf[4096];
int engine_buf_len = 0;

// Unicode piece icons for GUI rendering
const char* piece_symbols[] = {
    " ", "♙", "♘", "♗", "♖", "♕", "♔", " ", " ", "♟", "♞", "♝", "♜", "♛", "♚"
};

void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void init_board(BoardState *state) {
    memset(state, 0, sizeof(BoardState));
    int back_rank[8] = {W_ROOK, W_KNIGHT, W_BISHOP, W_QUEEN, W_KING, W_BISHOP, W_KNIGHT, W_ROOK};
    for (int i = 0; i < 8; i++) {
        state->board[SQ(0, i)] = back_rank[i] + 8; // Black back rank
        state->board[SQ(1, i)] = B_PAWN;
        state->board[SQ(6, i)] = W_PAWN;
        state->board[SQ(7, i)] = back_rank[i];     // White back rank
    }
    state->side = 0;
    state->castle = 15;
    state->ep = -1;
    state->halfmove = 0;
    state->fullmove = 1;
}

// Check if a square is attacked by a given opponent color
int is_square_attacked(BoardState *state, int sq, int attacker_color) {
    int r = ROW(sq), c = COL(sq);

    // Knight Attacks
    int knight_offsets[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + knight_offsets[i][0], nc = c + knight_offsets[i][1];
        if (IS_VALID(nr, nc)) {
            int p = state->board[SQ(nr, nc)];
            if (p != EMPTY && COLOR(p) == attacker_color && (p == W_KNIGHT || p == B_KNIGHT)) return 1;
        }
    }

    // King Attacks
    int king_offsets[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + king_offsets[i][0], nc = c + king_offsets[i][1];
        if (IS_VALID(nr, nc)) {
            int p = state->board[SQ(nr, nc)];
            if (p != EMPTY && COLOR(p) == attacker_color && (p == W_KING || p == B_KING)) return 1;
        }
    }

    // Pawn Attacks
    int pawn_r = (attacker_color == 0) ? r + 1 : r - 1;
    int pawn_p = (attacker_color == 0) ? W_PAWN : B_PAWN;
    if (IS_VALID(pawn_r, c - 1) && state->board[SQ(pawn_r, c - 1)] == pawn_p) return 1;
    if (IS_VALID(pawn_r, c + 1) && state->board[SQ(pawn_r, c + 1)] == pawn_p) return 1;

    // Sliding Attacks: Rooks/Queens
    int rook_dirs[4][2] = {{-1,0}, {1,0}, {0,-1}, {0,1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += rook_dirs[i][0]; nc += rook_dirs[i][1];
            if (!IS_VALID(nr, nc)) break;
            int p = state->board[SQ(nr, nc)];
            if (p != EMPTY) {
                if (COLOR(p) == attacker_color && (p == W_ROOK || p == B_ROOK || p == W_QUEEN || p == B_QUEEN)) return 1;
                break;
            }
        }
    }

    // Sliding Attacks: Bishops/Queens
    int bishop_dirs[4][2] = {{-1,-1}, {-1,1}, {1,-1}, {1,1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += bishop_dirs[i][0]; nc += bishop_dirs[i][1];
            if (!IS_VALID(nr, nc)) break;
            int p = state->board[SQ(nr, nc)];
            if (p != EMPTY) {
                if (COLOR(p) == attacker_color && (p == W_BISHOP || p == B_BISHOP || p == W_QUEEN || p == B_QUEEN)) return 1;
                break;
            }
        }
    }
    return 0;
}

int find_king(BoardState *state, int color) {
    int target = (color == 0) ? W_KING : B_KING;
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == target) return i;
    }
    return -1;
}

int is_in_check(BoardState *state, int color) {
    int king_sq = find_king(state, color);
    if (king_sq == -1) return 0;
    return is_square_attacked(state, king_sq, 1 - color);
}

// Generate pseudo-legal moves for a designated square
int generate_moves(BoardState *state, int sq, Move *moves) {
    int r = ROW(sq), c = COL(sq);
    int p = state->board[sq];
    if (p == EMPTY || COLOR(p) != state->side) return 0;

    int count = 0;
    int color = state->side;
    int opp = 1 - color;

    if (p == W_PAWN || p == B_PAWN) {
        int dir = (color == 0) ? -1 : 1;
        int start_row = (color == 0) ? 6 : 1;
        int promo_row = (color == 0) ? 0 : 7;

        // One step forward
        int nr = r + dir;
        if (IS_VALID(nr, c) && state->board[SQ(nr, c)] == EMPTY) {
            if (nr == promo_row) {
                moves[count++] = (Move){sq, SQ(nr, c), (color == 0) ? W_QUEEN : B_QUEEN};
                moves[count++] = (Move){sq, SQ(nr, c), (color == 0) ? W_ROOK : B_ROOK};
                moves[count++] = (Move){sq, SQ(nr, c), (color == 0) ? W_BISHOP : B_BISHOP};
                moves[count++] = (Move){sq, SQ(nr, c), (color == 0) ? W_KNIGHT : B_KNIGHT};
            } else {
                moves[count++] = (Move){sq, SQ(nr, c), 0};
            }
            // Double step forward
            if (r == start_row) {
                int nnr = r + 2 * dir;
                if (state->board[SQ(nnr, c)] == EMPTY) {
                    moves[count++] = (Move){sq, SQ(nnr, c), 0};
                }
            }
        }

        // Standard and En Passant Captures
        int dc_opts[2] = {-1, 1};
        for (int i = 0; i < 2; i++) {
            int nc = c + dc_opts[i];
            if (IS_VALID(nr, nc)) {
                int tp = state->board[SQ(nr, nc)];
                if (tp != EMPTY && COLOR(tp) == opp) {
                    if (nr == promo_row) {
                        moves[count++] = (Move){sq, SQ(nr, nc), (color == 0) ? W_QUEEN : B_QUEEN};
                        moves[count++] = (Move){sq, SQ(nr, nc), (color == 0) ? W_ROOK : B_ROOK};
                        moves[count++] = (Move){sq, SQ(nr, nc), (color == 0) ? W_BISHOP : B_BISHOP};
                        moves[count++] = (Move){sq, SQ(nr, nc), (color == 0) ? W_KNIGHT : B_KNIGHT};
                    } else {
                        moves[count++] = (Move){sq, SQ(nr, nc), 0};
                    }
                } else if (state->ep == SQ(nr, nc)) {
                    moves[count++] = (Move){sq, SQ(nr, nc), 0};
                }
            }
        }
    }
    else if (p == W_KNIGHT || p == B_KNIGHT) {
        int knight_offsets[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
        for (int i = 0; i < 8; i++) {
            int nr = r + knight_offsets[i][0], nc = c + knight_offsets[i][1];
            if (IS_VALID(nr, nc)) {
                int tp = state->board[SQ(nr, nc)];
                if (tp == EMPTY || COLOR(tp) == opp) moves[count++] = (Move){sq, SQ(nr, nc), 0};
            }
        }
    }
    else if (p == W_KING || p == B_KING) {
        int king_offsets[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
        for (int i = 0; i < 8; i++) {
            int nr = r + king_offsets[i][0], nc = c + king_offsets[i][1];
            if (IS_VALID(nr, nc)) {
                int tp = state->board[SQ(nr, nc)];
                if (tp == EMPTY || COLOR(tp) == opp) moves[count++] = (Move){sq, SQ(nr, nc), 0};
            }
        }
        // Castling Rules
        if (color == 0) {
            if ((state->castle & 1) && state->board[SQ(7, 5)] == EMPTY && state->board[SQ(7, 6)] == EMPTY) {
                if (!is_square_attacked(state, SQ(7, 4), 1) && !is_square_attacked(state, SQ(7, 5), 1) && !is_square_attacked(state, SQ(7, 6), 1)) {
                    moves[count++] = (Move){sq, SQ(7, 6), 0};
                }
            }
            if ((state->castle & 2) && state->board[SQ(7, 3)] == EMPTY && state->board[SQ(7, 2)] == EMPTY && state->board[SQ(7, 1)] == EMPTY) {
                if (!is_square_attacked(state, SQ(7, 4), 1) && !is_square_attacked(state, SQ(7, 3), 1) && !is_square_attacked(state, SQ(7, 2), 1)) {
                    moves[count++] = (Move){sq, SQ(7, 2), 0};
                }
            }
        } else {
            if ((state->castle & 4) && state->board[SQ(0, 5)] == EMPTY && state->board[SQ(0, 6)] == EMPTY) {
                if (!is_square_attacked(state, SQ(0, 4), 0) && !is_square_attacked(state, SQ(0, 5), 0) && !is_square_attacked(state, SQ(0, 6), 0)) {
                    moves[count++] = (Move){sq, SQ(0, 6), 0};
                }
            }
            if ((state->castle & 8) && state->board[SQ(0, 3)] == EMPTY && state->board[SQ(0, 2)] == EMPTY && state->board[SQ(0, 1)] == EMPTY) {
                if (!is_square_attacked(state, SQ(0, 4), 0) && !is_square_attacked(state, SQ(0, 3), 0) && !is_square_attacked(state, SQ(0, 2), 0)) {
                    moves[count++] = (Move){sq, SQ(0, 2), 0};
                }
            }
        }
    }
    else { // Sliding Pieces: Bishop, Rook, Queen
        int dirs[8][2], dir_count = 0;
        if (p == W_ROOK || p == B_ROOK || p == W_QUEEN || p == B_QUEEN) {
            int r_dirs[4][2] = {{-1,0}, {1,0}, {0,-1}, {0,1}};
            for (int i = 0; i < 4; i++) { dirs[dir_count][0] = r_dirs[i][0]; dirs[dir_count++][1] = r_dirs[i][1]; }
        }
        if (p == W_BISHOP || p == B_BISHOP || p == W_QUEEN || p == B_QUEEN) {
            int b_dirs[4][2] = {{-1,-1}, {-1,1}, {1,-1}, {1,1}};
            for (int i = 0; i < 4; i++) { dirs[dir_count][0] = b_dirs[i][0]; dirs[dir_count++][1] = b_dirs[i][1]; }
        }
        for (int d = 0; d < dir_count; d++) {
            int nr = r, nc = c;
            while (1) {
                nr += dirs[d][0]; nc += dirs[d][1];
                if (!IS_VALID(nr, nc)) break;
                int tp = state->board[SQ(nr, nc)];
                if (tp == EMPTY) {
                    moves[count++] = (Move){sq, SQ(nr, nc), 0};
                } else {
                    if (COLOR(tp) == opp) moves[count++] = (Move){sq, SQ(nr, nc), 0};
                    break;
                }
            }
        }
    }
    return count;
}

void make_move(BoardState *prev, Move m, BoardState *next) {
    *next = *prev;
    int p = next->board[m.from];
    next->board[m.from] = EMPTY;

    // En Passant Capture
    if ((p == W_PAWN || p == B_PAWN) && m.to == prev->ep) {
        int captured_sq = SQ(ROW(m.from), COL(m.to));
        next->board[captured_sq] = EMPTY;
    }

    // Set destination piece (handle promotion)
    next->board[m.to] = (m.promo != EMPTY) ? m.promo : p;

    // Castling updates
    if (p == W_KING) {
        next->castle &= ~3;
        if (m.from == 60) {
            if (m.to == 62) { next->board[61] = W_ROOK; next->board[63] = EMPTY; }
            if (m.to == 58) { next->board[59] = W_ROOK; next->board[56] = EMPTY; }
        }
    } else if (p == B_KING) {
        next->castle &= ~12;
        if (m.from == 4) {
            if (m.to == 6) { next->board[5] = B_ROOK; next->board[7] = EMPTY; }
            if (m.to == 2) { next->board[3] = B_ROOK; next->board[0] = EMPTY; }
        }
    }

    // Rook Movement/Capture updates castling rights
    if (m.from == 56 || m.to == 56) next->castle &= ~2;
    if (m.from == 63 || m.to == 63) next->castle &= ~1;
    if (m.from == 0 || m.to == 0)   next->castle &= ~8;
    if (m.from == 7 || m.to == 7)   next->castle &= ~4;

    // Manage En Passant status
    if ((p == W_PAWN || p == B_PAWN) && abs(ROW(m.from) - ROW(m.to)) == 2) {
        next->ep = SQ((ROW(m.from) + ROW(m.to)) / 2, COL(m.from));
    } else {
        next->ep = -1;
    }

    if (p == W_PAWN || p == B_PAWN || prev->board[m.to] != EMPTY) {
        next->halfmove = 0;
    } else {
        next->halfmove++;
    }

    if (next->side == 1) next->fullmove++;
    next->side = 1 - prev->side;
}

// Filters out pseudo-legal moves that put/leave the King in check
int get_legal_moves(BoardState *state, int sq, Move *legal_moves) {
    Move pseudo[64];
    int pseudo_count = generate_moves(state, sq, pseudo);
    int legal_count = 0;
    for (int i = 0; i < pseudo_count; i++) {
        BoardState temp;
        make_move(state, pseudo[i], &temp);
        if (!is_in_check(&temp, state->side)) {
            legal_moves[legal_count++] = pseudo[i];
        }
    }
    return legal_count;
}

int parse_uci_move(BoardState *state, const char *str, Move *m) {
    if (strlen(str) < 4) return 0;
    int f_col = str[0] - 'a', f_row = '8' - str[1];
    int t_col = str[2] - 'a', t_row = '8' - str[3];
    if (!IS_VALID(f_row, f_col) || !IS_VALID(t_row, t_col)) return 0;

    m->from = SQ(f_row, f_col);
    m->to = SQ(t_row, t_col);
    m->promo = 0;

    if (strlen(str) >= 5) {
        char p_char = str[4];
        int color = state->side;
        if (p_char == 'q') m->promo = (color == 0) ? W_QUEEN : B_QUEEN;
        else if (p_char == 'r') m->promo = (color == 0) ? W_ROOK : B_ROOK;
        else if (p_char == 'b') m->promo = (color == 0) ? W_BISHOP : B_BISHOP;
        else if (p_char == 'n') m->promo = (color == 0) ? W_KNIGHT : B_KNIGHT;
    }

    Move legal[64];
    int count = get_legal_moves(state, m->from, legal);
    for (int i = 0; i < count; i++) {
        if (legal[i].to == m->to) {
            if (m->promo != 0 && legal[i].promo != m->promo) continue;
            *m = legal[i];
            return 1;
        }
    }
    return 0;
}

// Translates chess moves into standard algebraic notation (SAN) for output representation
void get_move_san(BoardState *prev, Move m, char *san) {
    int piece = prev->board[m.from];
    int p_type = piece & 7;
    int color = prev->side;

    if (p_type == W_KING && abs(COL(m.from) - COL(m.to)) == 2) {
        if (COL(m.to) == 6) strcpy(san, "O-O");
        else strcpy(san, "O-O-O");
        goto append_check;
    }

    int ptr = 0;
    if (p_type != W_PAWN) {
        char p_chars[] = "  NBRQK";
        san[ptr++] = p_chars[p_type];

        // Disambiguate piece origins if multiple can move to the target
        int disambiguate_file = 0, disambiguate_rank = 0, count = 0;
        for (int i = 0; i < 64; i++) {
            if (i != m.from && prev->board[i] == piece) {
                Move legal[64];
                int leg_count = get_legal_moves(prev, i, legal);
                for (int j = 0; j < leg_count; j++) {
                    if (legal[j].to == m.to) {
                        count++;
                        if (COL(i) == COL(m.from)) disambiguate_rank = 1;
                        else disambiguate_file = 1;
                    }
                }
            }
        }
        if (count > 0) {
            if (disambiguate_file || !disambiguate_rank) san[ptr++] = 'a' + COL(m.from);
            if (disambiguate_rank) san[ptr++] = '8' - ROW(m.from);
        }
    } else {
        if (prev->board[m.to] != EMPTY || m.to == prev->ep) san[ptr++] = 'a' + COL(m.from);
    }

    if (prev->board[m.to] != EMPTY || (p_type == W_PAWN && m.to == prev->ep)) {
        san[ptr++] = 'x';
    }

    san[ptr++] = 'a' + COL(m.to);
    san[ptr++] = '8' - ROW(m.to);

    if (m.promo != 0) {
        san[ptr++] = '=';
        char p_chars[] = "  NBRQK";
        san[ptr++] = p_chars[m.promo & 7];
    }
    san[ptr] = '\0';

append_check:;
    BoardState next;
    make_move(prev, m, &next);
    if (is_in_check(&next, next.side)) {
        int has_moves = 0;
        for (int i = 0; i < 64; i++) {
            Move legal[64];
            if (get_legal_moves(&next, i, legal) > 0) { has_moves = 1; break; }
        }
        if (!has_moves) strcat(san, "#");
        else strcat(san, "+");
    }
}

void send_to_engine(const char *cmd) {
    write(to_engine[1], cmd, strlen(cmd));
    write(to_engine[1], "\n", 1);
}

int get_engine_line(char *line, int max_len) {
    char c;
    while (read(from_engine[0], &c, 1) > 0) {
        if (c == '\n') {
            engine_buf[engine_buf_len] = '\0';
            strncpy(line, engine_buf, max_len);
            engine_buf_len = 0;
            return 1;
        } else if (c != '\r') {
            if (engine_buf_len < (int)sizeof(engine_buf) - 2) {
                engine_buf[engine_buf_len++] = c;
            }
        }
    }
    return 0;
}

void draw_board(BoardState *state, int cursor_sq, int selected_sq, Move *legal_moves, int num_legal_moves) {
    printf("\033[H"); // Reset cursor to top-left
    printf("\n   \033[1;36m=== TERMINAL CHESS GUI ===\033[0m\n\n");

    int in_check = is_in_check(state, state->side);
    int king_sq = find_king(state, state->side);

    for (int r = 0; r < 8; r++) {
        printf("  %d ", 8 - r);
        for (int c = 0; c < 8; c++) {
            int sq = SQ(r, c);
            int piece = state->board[sq];

            int is_cursor = (sq == cursor_sq);
            int is_selected = (sq == selected_sq);
            int is_legal = 0;
            for (int i = 0; i < num_legal_moves; i++) {
                if (legal_moves[i].to == sq) { is_legal = 1; break; }
            }
            int is_check = (in_check && sq == king_sq);

            // Set square backgrounds
            if (is_cursor) {
                printf("\033[48;5;201m"); // Magenta cursor
            } else if (is_check) {
                printf("\033[48;5;196m"); // Red highlight for check
            } else if (is_selected) {
                printf("\033[48;5;220m"); // Gold selected piece
            } else if (is_legal) {
                printf("\033[48;5;40m");  // Green target paths
            } else {
                if ((r + c) % 2 == 0) printf("\033[48;5;253m"); // Light Square
                else printf("\033[48;5;241m");                  // Dark Square
            }

            // Set piece colors
            if (piece != EMPTY) {
                if (COLOR(piece) == 0) printf("\033[1;34m"); // Blue for White pieces
                else printf("\033[1;31m");                  // Red for Black pieces
                printf(" %s ", piece_symbols[piece]);
            } else {
                printf("   ");
            }
            printf("\033[0m");
        }
        printf("\n");
    }
    printf("     A  B  C  D  E  F  G  H\n\n");
}

int get_promotion_piece(int color) {
    printf("\033[KPromote pawn to: (q)ueen, (r)ook, (b)ishop, (k)night: ");
    fflush(stdout);
    while (1) {
        char ch;
        int r = read(STDIN_FILENO, &ch, 1);
        if (r > 0) {
            if (ch == 'q' || ch == 'Q') return (color == 0) ? W_QUEEN : B_QUEEN;
            if (ch == 'r' || ch == 'R') return (color == 0) ? W_ROOK : B_ROOK;
            if (ch == 'b' || ch == 'B') return (color == 0) ? W_BISHOP : B_BISHOP;
            if (ch == 'n' || ch == 'N') return (color == 0) ? W_KNIGHT : B_KNIGHT;
        }
        usleep(10000);
    }
}

void show_config_menu() {
    printf("\033[H\033[J");
    printf("\n\033[1;33m=== Time Control Settings ===\033[0m\n\n");
    printf(" 1. Search Depth (current: %s)\n", (tc.type == TC_DEPTH) ? "active" : "inactive");
    printf(" 2. Search Nodes (current: %s)\n", (tc.type == TC_NODES) ? "active" : "inactive");
    printf(" 3. Move Time    (current: %s)\n", (tc.type == TC_TIME) ? "active" : "inactive");
    printf(" 4. Back to Game\n\n");
    printf("Select option (1-4): ");
    fflush(stdout);

    while (1) {
        char ch;
        if (read(STDIN_FILENO, &ch, 1) > 0) {
            if (ch == '1') {
                printf("\nEnter search depth limit (1-20): "); fflush(stdout);
                disableRawMode();
                int d;
                if (scanf("%d", &d) == 1 && d >= 1 && d <= 20) { tc.type = TC_DEPTH; tc.value = d; }
                enableRawMode(); break;
            } else if (ch == '2') {
                printf("\nEnter node threshold limit (e.g., 50000): "); fflush(stdout);
                disableRawMode();
                int n;
                if (scanf("%d", &n) == 1 && n >= 100) { tc.type = TC_NODES; tc.value = n; }
                enableRawMode(); break;
            } else if (ch == '3') {
                printf("\nEnter time limit in milliseconds (e.g., 2000): "); fflush(stdout);
                disableRawMode();
                int t;
                if (scanf("%d", &t) == 1 && t >= 100) { tc.type = TC_TIME; tc.value = t; }
                enableRawMode(); break;
            } else if (ch == '4' || ch == 27) {
                break;
            }
        }
        usleep(10000);
    }
}

int main() {
    char engine_path[512] = "stockfish";
    printf("Enter path to UCI engine (or press Enter for standard 'stockfish'): ");
    fflush(stdout);
    char input[512];
    if (fgets(input, sizeof(input), stdin)) {
        input[strcspn(input, "\n")] = 0;
        if (strlen(input) > 0) strcpy(engine_path, input);
    }

    if (pipe(to_engine) < 0 || pipe(from_engine) < 0) {
        perror("Pipe configuration failed");
        return 1;
    }

    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[0]); close(to_engine[1]);
        close(from_engine[0]); close(from_engine[1]);
        char *args[] = {engine_path, NULL};
        execvp(args[0], args);
        perror("Failed to launch engine!");
        exit(1);
    }

    close(to_engine[0]);
    close(from_engine[1]);
    fcntl(from_engine[0], F_SETFL, O_NONBLOCK);

    // Synchronize with UCI Engine
    send_to_engine("uci");
    char uci_resp[2048];
    int ok = 0;
    for (int i = 0; i < 40; i++) {
        usleep(100000);
        while (get_engine_line(uci_resp, sizeof(uci_resp))) {
            if (strcmp(uci_resp, "uciok") == 0) ok = 1;
        }
        if (ok) break;
    }
    if (!ok) fprintf(stderr, "Warning: UCI initialization failed.\n");

    send_to_engine("isready");
    ok = 0;
    for (int i = 0; i < 40; i++) {
        usleep(100000);
        while (get_engine_line(uci_resp, sizeof(uci_resp))) {
            if (strcmp(uci_resp, "readyok") == 0) ok = 1;
        }
        if (ok) break;
    }

    enableRawMode();
    printf("\033[?25l\033[H\033[J"); // Hide real cursor, clear screen

    BoardState state;
    init_board(&state);

    int cursor_sq = 52; // Defaults to white e2 pawn
    int selected_sq = -1;
    Move legal_moves[64];
    int num_legal_moves = 0;

    int running = 1;
    while (running) {
        draw_board(&state, cursor_sq, selected_sq, legal_moves, num_legal_moves);

        // Sidebar / State information
        printf(" Turn: %s\n", (state.side == 0) ? "\033[1;34mWhite (User)\033[0m" : "\033[1;31mBlack (Engine)\033[0m");
        printf(" Control: [Arrows/WASD] Navigate | [Space/Enter] Interact\n");
        printf(" Global:  [U] Undo Last Turn   | [C] Time Options | [Q] Exit Game\n");
        printf(" Mode: ");
        if (tc.type == TC_DEPTH) printf("Depth Limit: %d\n", tc.value);
        else if (tc.type == TC_NODES) printf("Max Nodes: %d\n", tc.value);
        else printf("Maximum Move Duration: %d ms\n", tc.value);

        printf("\n Played Moves History:\n ");
        for (int i = 0; i < history_count; i++) {
            if (i % 2 == 0) printf(" %d. %s", (i / 2) + 1, san_history[i]);
            else printf(" %s   ", san_history[i]);
        }
        printf("\n\n");
        fflush(stdout);

        // Engine Move Handler
        if (state.side == 1) {
            char cmd[8192] = "position startpos";
            if (history_count > 0) {
                strcat(cmd, " moves");
                for (int i = 0; i < history_count; i++) {
                    strcat(cmd, " ");
                    strcat(cmd, uci_history[i]);
                }
            }
            send_to_engine(cmd);

            char go_cmd[128];
            if (tc.type == TC_DEPTH) sprintf(go_cmd, "go depth %d", tc.value);
            else if (tc.type == TC_NODES) sprintf(go_cmd, "go nodes %d", tc.value);
            else sprintf(go_cmd, "go movetime %d", tc.value);
            send_to_engine(go_cmd);

            char line[2048];
            int thinking = 1;
            int spin_idx = 0;
            char spinner[] = "/-\\|";
            while (thinking) {
                if (get_engine_line(line, sizeof(line))) {
                    if (strncmp(line, "bestmove", 8) == 0) {
                        char move_str[16];
                        sscanf(line, "bestmove %s", move_str);
                        Move m;
                        if (parse_uci_move(&state, move_str, &m)) {
                            get_move_san(&state, m, san_history[history_count]);

                            char promo_char = '\0';
                            if (m.promo != 0) {
                                int pt = m.promo & 7;
                                if (pt == W_QUEEN || pt == B_QUEEN) promo_char = 'q';
                                else if (pt == W_ROOK || pt == B_ROOK) promo_char = 'r';
                                else if (pt == W_BISHOP || pt == B_BISHOP) promo_char = 'b';
                                else if (pt == W_KNIGHT || pt == B_KNIGHT) promo_char = 'n';
                            }
                            if (promo_char) {
                                sprintf(uci_history[history_count], "%c%d%c%d%c", 'a' + COL(m.from), 8 - ROW(m.from), 'a' + COL(m.to), 8 - ROW(m.to), promo_char);
                            } else {
                                sprintf(uci_history[history_count], "%c%d%c%d", 'a' + COL(m.from), 8 - ROW(m.from), 'a' + COL(m.to), 8 - ROW(m.to));
                            }

                            board_history[board_history_count++] = state;
                            BoardState next;
                            make_move(&state, m, &next);
                            state = next;
                            history_count++;
                        }
                        thinking = 0;
                    }
                }
                printf("\r \033[1;32mEngine is calculating... %c\033[0m   ", spinner[spin_idx]);
                fflush(stdout);
                spin_idx = (spin_idx + 1) % 4;
                usleep(60000);
            }
            continue;
        }

        // Terminal Checkmate / Stalemate Verification
        int user_has_moves = 0;
        for (int i = 0; i < 64; i++) {
            Move m[64];
            if (get_legal_moves(&state, i, m) > 0) { user_has_moves = 1; break; }
        }
        if (!user_has_moves) {
            if (is_in_check(&state, state.side)) printf("\r\033[1;31m CHECKMATE! Engine wins.\033[0m\n\n");
            else printf("\r\033[1;33m STALEMATE! Game is a draw.\033[0m\n\n");
            disableRawMode();
            printf("\033[?25h"); // Show cursor
            return 0;
        }

        char c;
        int nread = read(STDIN_FILENO, &c, 1);
        if (nread <= 0) { usleep(10000); continue; }

        if (c == 'q' || c == 'Q') {
            running = 0;
        }
        else if (c == 'u' || c == 'U') {
            // Revert last two full actions (Your move + Engine response)
            if (board_history_count >= 2) {
                board_history_count -= 2;
                state = board_history[board_history_count];
                history_count -= 2;
                selected_sq = -1;
                num_legal_moves = 0;
                printf("\033[H\033[J");
            }
        }
        else if (c == 'c' || c == 'C') {
            disableRawMode();
            show_config_menu();
            enableRawMode();
            printf("\033[H\033[J");
        }
        else if (c == 'w' || c == 'W') { if (cursor_sq >= 8) cursor_sq -= 8; }
        else if (c == 's' || c == 'S') { if (cursor_sq < 56) cursor_sq += 8; }
        else if (c == 'a' || c == 'A') { if (cursor_sq % 8 > 0) cursor_sq--; }
        else if (c == 'd' || c == 'D') { if (cursor_sq % 8 < 7) cursor_sq++; }
        else if (c == '\033') { // Arrow keys escape interpreter
            char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                if (seq[0] == '[') {
                    switch (seq[1]) {
                        case 'A': if (cursor_sq >= 8) cursor_sq -= 8; break; // Up
                        case 'B': if (cursor_sq < 56) cursor_sq += 8; break; // Down
                        case 'C': if (cursor_sq % 8 < 7) cursor_sq++; break; // Right
                        case 'D': if (cursor_sq % 8 > 0) cursor_sq--; break; // Left
                    }
                }
            }
        }
        else if (c == ' ' || c == '\n' || c == '\r') {
            if (selected_sq == -1) {
                if (state.board[cursor_sq] != EMPTY && COLOR(state.board[cursor_sq]) == state.side) {
                    selected_sq = cursor_sq;
                    num_legal_moves = get_legal_moves(&state, selected_sq, legal_moves);
                }
            } else {
                int target_move_idx = -1;
                for (int i = 0; i < num_legal_moves; i++) {
                    if (legal_moves[i].to == cursor_sq) { target_move_idx = i; break; }
                }

                if (target_move_idx != -1) {
                    Move m = legal_moves[target_move_idx];
                    int p_type = state.board[m.from] & 7;
                    if (p_type == W_PAWN && ROW(m.to) == 0) m.promo = get_promotion_piece(state.side);

                    get_move_san(&state, m, san_history[history_count]);

                    char promo_char = '\0';
                    if (m.promo != 0) {
                        int pt = m.promo & 7;
                        if (pt == W_QUEEN || pt == B_QUEEN) promo_char = 'q';
                        else if (pt == W_ROOK || pt == B_ROOK) promo_char = 'r';
                        else if (pt == W_BISHOP || pt == B_BISHOP) promo_char = 'b';
                        else if (pt == W_KNIGHT || pt == B_KNIGHT) promo_char = 'n';
                    }
                    if (promo_char) {
                        sprintf(uci_history[history_count], "%c%d%c%d%c", 'a' + COL(m.from), 8 - ROW(m.from), 'a' + COL(m.to), 8 - ROW(m.to), promo_char);
                    } else {
                        sprintf(uci_history[history_count], "%c%d%c%d", 'a' + COL(m.from), 8 - ROW(m.from), 'a' + COL(m.to), 8 - ROW(m.to));
                    }

                    board_history[board_history_count++] = state;
                    BoardState next;
                    make_move(&state, m, &next);
                    state = next;
                    history_count++;

                    selected_sq = -1;
                    num_legal_moves = 0;
                    printf("\033[H\033[J");
                } else {
                    if (state.board[cursor_sq] != EMPTY && COLOR(state.board[cursor_sq]) == state.side) {
                        selected_sq = cursor_sq;
                        num_legal_moves = get_legal_moves(&state, selected_sq, legal_moves);
                    } else {
                        selected_sq = -1;
                        num_legal_moves = 0;
                    }
                }
            }
        }
    }

    disableRawMode();
    printf("\033[?25h\n"); // Show real cursor
    if (engine_pid > 0) {
        send_to_engine("quit");
        kill(engine_pid, SIGTERM);
    }
    return 0;
}
