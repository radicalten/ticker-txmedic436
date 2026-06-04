#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <termios.h>
#include <signal.h>

#define MAX_HIST 1024
#define WHITE 1
#define BLACK 2

// Piece Enumeration
enum {
    EMPTY = 0,
    W_PAWN = 1, W_KNIGHT = 2, W_BISHOP = 3, W_ROOK = 4, W_QUEEN = 5, W_KING = 6,
    B_PAWN = 9, B_KNIGHT = 10, B_BISHOP = 11, B_ROOK = 12, B_QUEEN = 13, B_KING = 14
};

// Coordinate mapping macros
#define ROW(sq) ((sq) / 8)
#define COL(sq) ((sq) % 8)
#define SQ(r, c) ((r) * 8 + (c))

typedef struct {
    int from;
    int to;
    int promotion;
} Move;

typedef struct {
    int board[64];
    int turn;
    int castling; // bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep_sq;    // en passant square (-1 if none)
    int halfmove;
    int fullmove;
    Move last_move;
    char san[16];   // SAN notation for this move (e.g., Nf3)
    char coord[6];  // UCI notation for this move (e.g., g1f3)
} BoardState;

// Global Game Variables
int board[64];
int turn;
int castling;
int ep_sq;
int halfmove;
int fullmove;
Move last_move;

BoardState history[MAX_HIST];
int hist_count = 0;

int cursor_x = 4;
int cursor_y = 6;
int selected_square = -1;

// Time Control configurations
int tc_type = 2;     // 0 = depth, 1 = nodes, 2 = movetime
int tc_value = 1000; // default value

// Engine interface variables
int engine_in = -1;
int engine_out = -1;
pid_t engine_pid = -1;
int engine_active = 0;
int engine_thinking = 0;

struct termios orig_termios;

// Helper function declarations
int get_color(int piece) { return (piece == EMPTY) ? 0 : ((piece < 8) ? WHITE : BLACK); }
int get_type(int piece) { return piece & 7; }
int find_king(int color);
int is_attacked(int sq, int by_color);
void make_move(Move m);
void undo_move();
int generate_legal_moves(int color, Move *moves);
void generate_pseudo_moves(int color, Move *moves, int *count);

const int initial_board[64] = {
    B_ROOK, B_KNIGHT, B_BISHOP, B_QUEEN, B_KING, B_BISHOP, B_KNIGHT, B_ROOK,
    B_PAWN, B_PAWN,   B_PAWN,   B_PAWN,  B_PAWN, B_PAWN,   B_PAWN,   B_PAWN,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    W_PAWN, W_PAWN,   W_PAWN,   W_PAWN,  W_PAWN, W_PAWN,   W_PAWN,   W_PAWN,
    W_ROOK, W_KNIGHT, W_BISHOP, W_QUEEN, W_KING, W_BISHOP, W_KNIGHT, W_ROOK
};

Move active_legal_moves[256];
int active_legal_count = 0;

int is_legal_target(int sq) {
    if (selected_square == -1) return 0;
    for (int i = 0; i < active_legal_count; i++) {
        if (active_legal_moves[i].to == sq) return 1;
    }
    return 0;
}

// Terminal manipulation and Raw Mode
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // Show cursor
    fflush(stdout);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
    fflush(stdout);
}

// Chess Rules & Move Generation Implementation
int is_attacked(int sq, int by_color) {
    int r = ROW(sq), c = COL(sq);

    // Knight attacks
    int k_off[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + k_off[i][0], nc = c + k_off[i][1];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int p = board[SQ(nr, nc)];
            if (get_color(p) == by_color && get_type(p) == W_KNIGHT) return 1;
        }
    }

    // King attacks
    int ki_off[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + ki_off[i][0], nc = c + ki_off[i][1];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int p = board[SQ(nr, nc)];
            if (get_color(p) == by_color && get_type(p) == W_KING) return 1;
        }
    }

    // Pawn attacks
    if (by_color == WHITE) {
        if (r < 7) {
            if (c > 0 && board[SQ(r + 1, c - 1)] == W_PAWN) return 1;
            if (c < 7 && board[SQ(r + 1, c + 1)] == W_PAWN) return 1;
        }
    } else {
        if (r > 0) {
            if (c > 0 && board[SQ(r - 1, c - 1)] == B_PAWN) return 1;
            if (c < 7 && board[SQ(r - 1, c + 1)] == B_PAWN) return 1;
        }
    }

    // Diagonal Sliding (Bishop/Queen)
    int diag[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += diag[i][0]; nc += diag[i][1];
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            int p = board[SQ(nr, nc)];
            if (p != EMPTY) {
                if (get_color(p) == by_color && (get_type(p) == W_BISHOP || get_type(p) == W_QUEEN)) return 1;
                break;
            }
        }
    }

    // Orthogonal Sliding (Rook/Queen)
    int orth[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += orth[i][0]; nc += orth[i][1];
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            int p = board[SQ(nr, nc)];
            if (p != EMPTY) {
                if (get_color(p) == by_color && (get_type(p) == W_ROOK || get_type(p) == W_QUEEN)) return 1;
                break;
            }
        }
    }
    return 0;
}

void add_move(Move *moves, int *count, int from, int to, int promo) {
    moves[*count].from = from;
    moves[*count].to = to;
    moves[*count].promotion = promo;
    (*count)++;
}

void generate_pseudo_moves(int color, Move *moves, int *count) {
    *count = 0;
    for (int from = 0; from < 64; from++) {
        int p = board[from];
        if (get_color(p) != color) continue;

        int r = ROW(from), c = COL(from);
        int type = get_type(p);

        if (type == W_PAWN) {
            int dir = (color == WHITE) ? -1 : 1;
            int start_row = (color == WHITE) ? 6 : 1;
            int promo_row = (color == WHITE) ? 0 : 7;

            // Single step
            int to1 = SQ(r + dir, c);
            if (to1 >= 0 && to1 < 64 && board[to1] == EMPTY) {
                if (ROW(to1) == promo_row) {
                    add_move(moves, count, from, to1, (color == WHITE) ? W_QUEEN : B_QUEEN);
                    add_move(moves, count, from, to1, (color == WHITE) ? W_ROOK : B_ROOK);
                    add_move(moves, count, from, to1, (color == WHITE) ? W_BISHOP : B_BISHOP);
                    add_move(moves, count, from, to1, (color == WHITE) ? W_KNIGHT : B_KNIGHT);
                } else {
                    add_move(moves, count, from, to1, 0);
                    // Double step
                    if (r == start_row) {
                        int to2 = SQ(r + 2 * dir, c);
                        if (board[to2] == EMPTY) add_move(moves, count, from, to2, 0);
                    }
                }
            }
            // Captures
            int cap_cols[2] = {c - 1, c + 1};
            for (int i = 0; i < 2; i++) {
                int nc = cap_cols[i];
                if (nc >= 0 && nc < 8) {
                    int to_cap = SQ(r + dir, nc);
                    if (board[to_cap] != EMPTY && get_color(board[to_cap]) != color) {
                        if (ROW(to_cap) == promo_row) {
                            add_move(moves, count, from, to_cap, (color == WHITE) ? W_QUEEN : B_QUEEN);
                            add_move(moves, count, from, to_cap, (color == WHITE) ? W_ROOK : B_ROOK);
                            add_move(moves, count, from, to_cap, (color == WHITE) ? W_BISHOP : B_BISHOP);
                            add_move(moves, count, from, to_cap, (color == WHITE) ? W_KNIGHT : B_KNIGHT);
                        } else {
                            add_move(moves, count, from, to_cap, 0);
                        }
                    }
                    if (to_cap == ep_sq) {
                        add_move(moves, count, from, to_cap, 0);
                    }
                }
            }
        } else if (type == W_KNIGHT) {
            int off[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
            for (int i = 0; i < 8; i++) {
                int nr = r + off[i][0], nc = c + off[i][1];
                if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                    int to = SQ(nr, nc);
                    if (board[to] == EMPTY || get_color(board[to]) != color) add_move(moves, count, from, to, 0);
                }
            }
        } else if (type == W_KING) {
            int off[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
            for (int i = 0; i < 8; i++) {
                int nr = r + off[i][0], nc = c + off[i][1];
                if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                    int to = SQ(nr, nc);
                    if (board[to] == EMPTY || get_color(board[to]) != color) add_move(moves, count, from, to, 0);
                }
            }
            // Castling
            if (color == WHITE && from == 60) {
                if ((castling & 1) && board[61] == EMPTY && board[62] == EMPTY) {
                    if (!is_attacked(60, BLACK) && !is_attacked(61, BLACK) && !is_attacked(62, BLACK)) add_move(moves, count, 60, 62, 0);
                }
                if ((castling & 2) && board[59] == EMPTY && board[58] == EMPTY && board[57] == EMPTY) {
                    if (!is_attacked(60, BLACK) && !is_attacked(59, BLACK) && !is_attacked(58, BLACK)) add_move(moves, count, 60, 58, 0);
                }
            } else if (color == BLACK && from == 4) {
                if ((castling & 4) && board[5] == EMPTY && board[6] == EMPTY) {
                    if (!is_attacked(4, WHITE) && !is_attacked(5, WHITE) && !is_attacked(6, WHITE)) add_move(moves, count, 4, 6, 0);
                }
                if ((castling & 8) && board[3] == EMPTY && board[2] == EMPTY && board[1] == EMPTY) {
                    if (!is_attacked(4, WHITE) && !is_attacked(3, WHITE) && !is_attacked(2, WHITE)) add_move(moves, count, 4, 2, 0);
                }
            }
        } else {
            // Sliders: Bishops, Rooks, Queens
            int is_diag = (type == W_BISHOP || type == W_QUEEN);
            int is_orth = (type == W_ROOK || type == W_QUEEN);
            int dirs[8][2], dc = 0;
            if (is_diag) {
                int d[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
                for (int i=0; i<4; i++) { dirs[dc][0] = d[i][0]; dirs[dc][1] = d[i][1]; dc++; }
            }
            if (is_orth) {
                int o[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
                for (int i=0; i<4; i++) { dirs[dc][0] = o[i][0]; dirs[dc][1] = o[i][1]; dc++; }
            }
            for (int d = 0; d < dc; d++) {
                int nr = r, nc = c;
                while (1) {
                    nr += dirs[d][0]; nc += dirs[d][1];
                    if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
                    int to = SQ(nr, nc);
                    if (board[to] == EMPTY) {
                        add_move(moves, count, from, to, 0);
                    } else {
                        if (get_color(board[to]) != color) add_move(moves, count, from, to, 0);
                        break;
                    }
                }
            }
        }
    }
}

int generate_legal_moves(int color, Move *moves) {
    Move pseudo[256];
    int p_count = 0;
    generate_pseudo_moves(color, pseudo, &p_count);

    int l_count = 0;
    for (int i = 0; i < p_count; i++) {
        // Run test simulation on mock history push/pull
        int pre_hist = hist_count;
        make_move(pseudo[i]);
        int king = find_king(color);
        if (king != -1 && !is_attacked(king, 3 - color)) {
            moves[l_count++] = pseudo[i];
        }
        undo_move();
    }
    return l_count;
}

int find_king(int color) {
    int target = (color == WHITE) ? W_KING : B_KING;
    for (int i = 0; i < 64; i++) {
        if (board[i] == target) return i;
    }
    return -1;
}

void make_move(Move m) {
    if (hist_count < MAX_HIST) {
        history[hist_count].turn = turn;
        history[hist_count].castling = castling;
        history[hist_count].ep_sq = ep_sq;
        history[hist_count].halfmove = halfmove;
        history[hist_count].fullmove = fullmove;
        history[hist_count].last_move = last_move;
        memcpy(history[hist_count].board, board, sizeof(board));
        hist_count++;
    }

    int p = board[m.from];
    int target = board[m.to];
    int next_ep = -1;

    if (get_type(p) == W_PAWN || target != EMPTY) halfmove = 0;
    else halfmove++;

    // Castling mechanics
    if (get_type(p) == W_KING) {
        if (m.from == 60) {
            if (m.to == 62) { board[61] = W_ROOK; board[63] = EMPTY; }
            else if (m.to == 58) { board[59] = W_ROOK; board[56] = EMPTY; }
            castling &= ~3;
        } else if (m.from == 4) {
            if (m.to == 6) { board[5] = B_ROOK; board[7] = EMPTY; }
            else if (m.to == 2) { board[3] = B_ROOK; board[0] = EMPTY; }
            castling &= ~12;
        }
    }

    // Rook movements/captures modifying castling rights
    if (m.from == 56) castling &= ~2;
    if (m.from == 63) castling &= ~1;
    if (m.from == 0)  castling &= ~8;
    if (m.from == 7)  castling &= ~4;
    if (m.to == 56) castling &= ~2;
    if (m.to == 63) castling &= ~1;
    if (m.to == 0)  castling &= ~8;
    if (m.to == 7)  castling &= ~4;

    // En Passant execution
    if (get_type(p) == W_PAWN && m.to == ep_sq) {
        if (turn == WHITE) board[m.to + 8] = EMPTY;
        else board[m.to - 8] = EMPTY;
    }

    // Generate upcoming Ep Square
    if (get_type(p) == W_PAWN) {
        if (m.to - m.from == -16) next_ep = m.from - 8;
        else if (m.to - m.from == 16) next_ep = m.from + 8;
    }

    board[m.to] = board[m.from];
    board[m.from] = EMPTY;

    // Promotion replacement
    if (get_type(p) == W_PAWN && (ROW(m.to) == 0 || ROW(m.to) == 7)) {
        board[m.to] = (m.promotion != EMPTY) ? m.promotion : ((turn == WHITE) ? W_QUEEN : B_QUEEN);
    }

    ep_sq = next_ep;
    last_move = m;

    if (turn == BLACK) {
        fullmove++;
        turn = WHITE;
    } else {
        turn = BLACK;
    }
}

void undo_move() {
    if (hist_count > 0) {
        hist_count--;
        turn = history[hist_count].turn;
        castling = history[hist_count].castling;
        ep_sq = history[hist_count].ep_sq;
        halfmove = history[hist_count].halfmove;
        fullmove = history[hist_count].fullmove;
        last_move = history[hist_count].last_move;
        memcpy(board, history[hist_count].board, sizeof(board));
    }
}

// UCI coordinates conversion
void get_coord(Move m, char *buf) {
    int f_col = COL(m.from), f_row = ROW(m.from);
    int t_col = COL(m.to),   t_row = ROW(m.to);
    int p = 0;
    buf[p++] = 'a' + f_col;
    buf[p++] = '8' - f_row;
    buf[p++] = 'a' + t_col;
    buf[p++] = '8' - t_row;
    if (m.promotion != EMPTY) {
        char symbols[] = "  nbrq";
        buf[p++] = symbols[get_type(m.promotion)];
    }
    buf[p] = '\0';
}

// SAN generator for precise PGN logs
void generate_san(Move m, char *buf) {
    int p = board[m.from];
    int type = get_type(p);
    int color = get_color(p);

    if (type == W_KING) {
        if (m.from == 60 && m.to == 62) { strcpy(buf, "O-O"); return; }
        if (m.from == 60 && m.to == 58) { strcpy(buf, "O-O-O"); return; }
        if (m.from ==  4 && m.to ==  6) { strcpy(buf, "O-O"); return; }
        if (m.from ==  4 && m.to ==  2) { strcpy(buf, "O-O-O"); return; }
    }

    int ptr = 0;
    if (type == W_PAWN) {
        if (COL(m.from) != COL(m.to)) {
            buf[ptr++] = 'a' + COL(m.from);
            buf[ptr++] = 'x';
        }
        buf[ptr++] = 'a' + COL(m.to);
        buf[ptr++] = '8' - ROW(m.to);
        if (ROW(m.to) == 0 || ROW(m.to) == 7) {
            buf[ptr++] = '=';
            char chars[] = " PNBRQK";
            buf[ptr++] = chars[(m.promotion != EMPTY) ? get_type(m.promotion) : W_QUEEN];
        }
    } else {
        char chars[] = " PNBRQK";
        buf[ptr++] = chars[type];

        // Search disambiguating criteria
        Move list[256];
        int count = generate_legal_moves(color, list);
        int same_type = 0, share_file = 0, share_rank = 0;
        for (int i = 0; i < count; i++) {
            if (list[i].to == m.to && list[i].from != m.from && get_type(board[list[i].from]) == type) {
                same_type++;
                if (COL(list[i].from) == COL(m.from)) share_file = 1;
                if (ROW(list[i].from) == ROW(m.from)) share_rank = 1;
            }
        }
        if (same_type > 0) {
            if (!share_file) buf[ptr++] = 'a' + COL(m.from);
            else if (!share_rank) buf[ptr++] = '8' - ROW(m.from);
            else {
                buf[ptr++] = 'a' + COL(m.from);
                buf[ptr++] = '8' - ROW(m.from);
            }
        }
        if (board[m.to] != EMPTY) buf[ptr++] = 'x';
        buf[ptr++] = 'a' + COL(m.to);
        buf[ptr++] = '8' - ROW(m.to);
    }

    // Determine Check(+) or Checkmate(#) status post-execution
    make_move(m);
    int ch = is_attacked(find_king(turn), 3 - turn);
    Move op[256];
    int op_count = generate_legal_moves(turn, op);
    undo_move();

    if (ch) {
        if (op_count == 0) buf[ptr++] = '#';
        else buf[ptr++] = '+';
    }
    buf[ptr] = '\0';
}

// UCI Engine connection using POSIX pipes
int start_engine(const char *path) {
    int to_eng[2], from_eng[2];
    if (pipe(to_eng) != 0 || pipe(from_eng) != 0) return 0;

    engine_pid = fork();
    if (engine_pid < 0) return 0;
    if (engine_pid == 0) {
        dup2(to_eng[0], STDIN_FILENO);
        dup2(from_eng[1], STDOUT_FILENO);
        close(to_eng[1]);
        close(from_eng[0]);
        char *args[] = {(char *)path, NULL};
        execvp(path, args);
        exit(1);
    }
    close(to_eng[0]);
    close(from_eng[1]);
    engine_in = to_eng[1];
    engine_out = from_eng[0];

    // Set non-blocking descriptors
    int flags = fcntl(engine_out, F_GETFL, 0);
    fcntl(engine_out, F_SETFL, flags | O_NONBLOCK);

    engine_active = 1;
    write(engine_in, "uci\n", 4);
    usleep(100000);
    write(engine_in, "isready\n", 8);
    return 1;
}

void send_to_engine(const char *cmd) {
    if (engine_active) write(engine_in, cmd, strlen(cmd));
}

int read_from_engine(char *buf, int max_len) {
    if (!engine_active) return 0;
    int total = 0;
    while (total < max_len - 1) {
        char c;
        int n = read(engine_out, &c, 1);
        if (n <= 0) break;
        buf[total++] = c;
        if (c == '\n') break;
    }
    buf[total] = '\0';
    return total;
}

// Color and layout styles
const char *get_bg_color(int r, int c) {
    int idx = r * 8 + c;
    if (cursor_x == c && cursor_y == r) return "\033[48;5;214m"; // Orange (Cursor)
    if (selected_square == idx) return "\033[48;5;75m";         // Blue (Selected piece)
    if (is_legal_target(idx)) return "\033[48;5;71m";           // Green (Legal Target)
    if (hist_count > 0) {
        if (last_move.from == idx || last_move.to == idx) return "\033[48;5;186m"; // Gold (Last Move)
    }
    return ((r + c) % 2 == 0) ? "\033[48;5;253m" : "\033[48;5;242m"; // Alternating squares
}

const char *get_fg_color(int p) {
    return (get_color(p) == WHITE) ? "\033[1;38;5;231m" : "\033[1;38;5;196m"; // High contrast red & white
}

void draw_sidebar(int r, int line) {
    int idx = r * 3 + line;
    printf("   │ ");
    switch (idx) {
        case 0:  printf("\033[1;35m--- STATUS ---\033[0m"); break;
        case 1:
            if (turn == WHITE) printf("Active Turn: \033[1;32mWHITE (Player)\033[0m");
            else printf("Active Turn: \033[1;31mBLACK (Engine)\033[0m");
            break;
        case 2:
            printf("Castling: ");
            printf("%c", (castling & 1) ? 'K' : '-');
            printf("%c", (castling & 2) ? 'Q' : '-');
            printf("%c", (castling & 4) ? 'k' : '-');
            printf("%c", (castling & 8) ? 'q' : '-');
            break;
        case 3:
            if (ep_sq != -1) printf("En Passant Sq: %c%d", 'a' + COL(ep_sq), 8 - ROW(ep_sq));
            else printf("En Passant Sq: -");
            break;
        case 4:  printf("Fullmoves: %d (Half: %d)", fullmove, halfmove); break;
        case 6:  printf("\033[1;35m--- ENGINE ---\033[0m"); break;
        case 7:
            if (engine_active) printf("Connection: \033[1;32mActive\033[0m");
            else printf("Connection: \033[1;30mInactive (Manual Play)\033[0m");
            break;
        case 8:
            if (tc_type == 0) printf("Time Control: Depth %d", tc_value);
            else if (tc_type == 1) printf("Time Control: Nodes %d", tc_value);
            else printf("Time Control: %d ms", tc_value);
            break;
        case 9:
            if (engine_thinking) printf("Engine State: \033[1;33mThinking...\033[0m");
            else printf("Engine State: Idle");
            break;
        case 11: printf("\033[1;35m--- PGN LOGS ---\033[0m"); break;
        default:
            if (idx >= 12 && idx < 24) {
                int line_num = idx - 12;
                int base_mv = line_num * 3 + 1;
                int written = 0;
                for (int m = 0; m < 3; m++) {
                    int mv_idx = base_mv + m;
                    int w_hist = (mv_idx - 1) * 2;
                    int b_hist = w_hist + 1;
                    if (w_hist < hist_count) {
                        if (!written) { printf("%3d. ", mv_idx); written = 1; }
                        printf("%s ", history[w_hist].san);
                        if (b_hist < hist_count) {
                            printf("%s  ", history[b_hist].san);
                        }
                    }
                }
            }
            break;
    }
}

void draw_interface() {
    printf("\033[H"); // Reset cursor to top left
    printf("\033[1;36m========================================================\033[0m\n");
    printf("\033[1;36m                CHESS UCI TERMINAL GUI                  \033[0m\n");
    printf("\033[1;36m========================================================\033[0m\n\n");

    for (int r = 0; r < 8; r++) {
        for (int line = 0; line < 3; line++) {
            if (line == 1) printf("  %d  ", 8 - r);
            else printf("     ");

            for (int c = 0; c < 8; c++) {
                const char *bg = get_bg_color(r, c);
                printf("%s", bg);
                if (line == 1) {
                    int p = board[SQ(r, c)];
                    if (p != EMPTY) {
                        char symbols[] = " PNBRQK";
                        printf("  %s%c%s  ", get_fg_color(p), symbols[get_type(p)], bg);
                    } else {
                        printf("     ");
                    }
                } else {
                    printf("     ");
                }
            }
            printf("\033[0m");
            draw_sidebar(r, line);
            printf("\n");
        }
    }
    printf("\n     ");
    for (int c = 0; c < 8; c++) printf("  %c  ", 'a' + c);
    printf("\n\n");
    printf("\033[1;33mControls:\033[0m [Arrows/WASD] Navigate | [Space/Enter] Select/Execute\n");
    printf("          [U] Undo Last Turn   | [C] Change Time Controls  | [Q] Exit Game\n");
}

int prompt_promotion(int color) {
    disable_raw_mode();
    printf("\n\033[1;33mPromote pawn to: (Q)ueen, (R)ook, (B)ishop, (K)night: \033[0m");
    fflush(stdout);
    char ch;
    while (1) {
        ch = getchar();
        if (ch == 'q' || ch == 'Q') { enable_raw_mode(); return (color == WHITE) ? W_QUEEN : B_QUEEN; }
        if (ch == 'r' || ch == 'R') { enable_raw_mode(); return (color == WHITE) ? W_ROOK : B_ROOK; }
        if (ch == 'b' || ch == 'B') { enable_raw_mode(); return (color == WHITE) ? W_BISHOP : B_BISHOP; }
        if (ch == 'n' || ch == 'N') { enable_raw_mode(); return (color == WHITE) ? W_KNIGHT : B_KNIGHT; }
    }
}

void prompt_time_control() {
    disable_raw_mode();
    printf("\n\033[1;36m=== CONFIG TIME CONTROLS ===\033[0m\n");
    printf("1. Search Depth (moves)\n");
    printf("2. Engine Search Nodes limit\n");
    printf("3. Move Time (milliseconds limit)\n");
    printf("Selection (1-3): ");
    fflush(stdout);

    char type_ch = getchar();
    int type = -1;
    if (type_ch == '1') type = 0;
    else if (type_ch == '2') type = 1;
    else if (type_ch == '3') type = 2;

    if (type != -1) {
        printf("Enter Limit Target: ");
        fflush(stdout);
        int target;
        if (scanf("%d", &target) == 1) {
            tc_type = type;
            tc_value = target;
        }
    }
    int extra;
    while ((extra = getchar()) != '\n' && extra != EOF);
    enable_raw_mode();
}

int main() {
    memcpy(board, initial_board, sizeof(board));
    turn = WHITE;
    castling = 15;
    ep_sq = -1;
    halfmove = 0;
    fullmove = 1;
    hist_count = 0;

    printf("\033[2J\033[H");
    printf("\033[1;36m=== CHESS TERMINAL ENGINE GUI ===\033[0m\n");
    printf("Specify UCI Engine Absolute Path (e.g. /opt/homebrew/bin/stockfish)\n");
    printf("Or press [Enter] directly to transition to manual, two-player mode:\n> ");
    fflush(stdout);

    char path_buf[1024];
    if (fgets(path_buf, sizeof(path_buf), stdin)) {
        path_buf[strcspn(path_buf, "\n")] = 0;
        if (strlen(path_buf) > 0) {
            if (!start_engine(path_buf)) {
                printf("\033[1;31mCould not establish interface at '%s'. Starting Manual Mode.\033[0m\n", path_buf);
                sleep(2);
            }
        }
    }

    enable_raw_mode();
    int active = 1;
    draw_interface();

    while (active) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);

        int max_fd = STDIN_FILENO;
        if (engine_active) {
            FD_SET(engine_out, &read_fds);
            if (engine_out > max_fd) max_fd = engine_out;
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;

        int state = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (state < 0) continue;

        // Process incoming Engine responses
        if (engine_active && FD_ISSET(engine_out, &read_fds)) {
            char line[2048];
            int size = read_from_engine(line, sizeof(line));
            if (size > 0 && strncmp(line, "bestmove", 8) == 0) {
                char mv_str[10];
                if (sscanf(line, "bestmove %s", mv_str) == 1) {
                    int f_col = mv_str[0] - 'a', f_row = '8' - mv_str[1];
                    int t_col = mv_str[2] - 'a', t_row = '8' - mv_str[3];

                    Move m;
                    m.from = SQ(f_row, f_col);
                    m.to = SQ(t_row, t_col);
                    m.promotion = EMPTY;

                    if (strlen(mv_str) == 5) {
                        char promo_char = mv_str[4];
                        if (promo_char == 'q') m.promotion = (turn == WHITE) ? W_QUEEN : B_QUEEN;
                        if (promo_char == 'r') m.promotion = (turn == WHITE) ? W_ROOK : B_ROOK;
                        if (promo_char == 'b') m.promotion = (turn == WHITE) ? W_BISHOP : B_BISHOP;
                        if (promo_char == 'n') m.promotion = (turn == WHITE) ? W_KNIGHT : B_KNIGHT;
                    }

                    char san_buf[16], coord_buf[6];
                    generate_san(m, san_buf);
                    get_coord(m, coord_buf);

                    make_move(m);

                    strcpy(history[hist_count - 1].san, san_buf);
                    strcpy(history[hist_count - 1].coord, coord_buf);

                    engine_thinking = 0;
                    draw_interface();
                }
            }
        }

        // Process Player Keyboard events
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            char key;
            if (read(STDIN_FILENO, &key, 1) == 1) {
                if (key == '\033') { // Escape Sequence Parsing
                    char escape1, escape2;
                    if (read(STDIN_FILENO, &escape1, 1) == 1 && read(STDIN_FILENO, &escape2, 1) == 1) {
                        if (escape1 == '[') {
                            if (escape2 == 'A' && cursor_y > 0) cursor_y--;
                            else if (escape2 == 'B' && cursor_y < 7) cursor_y++;
                            else if (escape2 == 'C' && cursor_x < 7) cursor_x++;
                            else if (escape2 == 'D' && cursor_x > 0) cursor_x--;
                        }
                    }
                } else if (key == 'w' || key == 'W') { if (cursor_y > 0) cursor_y--; }
                else if (key == 's' || key == 'S') { if (cursor_y < 7) cursor_y++; }
                else if (key == 'a' || key == 'A') { if (cursor_x > 0) cursor_x--; }
                else if (key == 'd' || key == 'D') { if (cursor_x < 7) cursor_x++; }
                else if (key == ' ' || key == '\n') {
                    if (!engine_thinking) {
                        int index = SQ(cursor_y, cursor_x);
                        if (selected_square == -1) {
                            if (board[index] != EMPTY && get_color(board[index]) == turn) {
                                selected_square = index;
                                Move moves[256];
                                int full_c = generate_legal_moves(turn, moves);
                                active_legal_count = 0;
                                for (int i = 0; i < full_c; i++) {
                                    if (moves[i].from == selected_square) {
                                        active_legal_moves[active_legal_count++] = moves[i];
                                    }
                                }
                            }
                        } else {
                            if (is_legal_target(index)) {
                                Move selection;
                                for (int i = 0; i < active_legal_count; i++) {
                                    if (active_legal_moves[i].to == index) { selection = active_legal_moves[i]; break; }
                                }
                                if (get_type(board[selected_square]) == W_PAWN && (ROW(index) == 0 || ROW(index) == 7)) {
                                    selection.promotion = prompt_promotion(turn);
                                }

                                char san_buf[16], coord_buf[6];
                                generate_san(selection, san_buf);
                                get_coord(selection, coord_buf);

                                make_move(selection);

                                strcpy(history[hist_count - 1].san, san_buf);
                                strcpy(history[hist_count - 1].coord, coord_buf);

                                selected_square = -1;
                                active_legal_count = 0;

                                if (engine_active && turn == BLACK) {
                                    engine_thinking = 1;
                                    draw_interface();

                                    char play_string[8192] = "position startpos moves";
                                    for (int i = 0; i < hist_count; i++) {
                                        strcat(play_string, " ");
                                        strcat(play_string, history[i].coord);
                                    }
                                    strcat(play_string, "\n");
                                    send_to_engine(play_string);

                                    char search_string[128];
                                    if (tc_type == 0) sprintf(search_string, "go depth %d\n", tc_value);
                                    else if (tc_type == 1) sprintf(search_string, "go nodes %d\n", tc_value);
                                    else sprintf(search_string, "go movetime %d\n", tc_value);
                                    send_to_engine(search_string);
                                }
                            } else if (board[index] != EMPTY && get_color(board[index]) == turn) {
                                selected_square = index;
                                Move moves[256];
                                int full_c = generate_legal_moves(turn, moves);
                                active_legal_count = 0;
                                for (int i = 0; i < full_c; i++) {
                                    if (moves[i].from == selected_square) {
                                        active_legal_moves[active_legal_count++] = moves[i];
                                    }
                                }
                            } else {
                                selected_square = -1;
                                active_legal_count = 0;
                            }
                        }
                    }
                } else if (key == 'u' || key == 'U') {
                    if (!engine_thinking) {
                        if (engine_active) {
                            undo_move(); // Undo engine move
                            undo_move(); // Undo player move
                        } else {
                            undo_move(); // Local play single undo
                        }
                        selected_square = -1;
                        active_legal_count = 0;
                    }
                } else if (key == 'c' || key == 'C') {
                    if (!engine_thinking) prompt_time_control();
                } else if (key == 'q' || key == 'Q') {
                    active = 0;
                }
                if (active) draw_interface();
            }
        }
    }

    if (engine_active) {
        send_to_engine("quit\n");
        close(engine_in);
        close(engine_out);
        kill(engine_pid, SIGTERM);
    }
    return 0;
}
