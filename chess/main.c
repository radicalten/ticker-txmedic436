#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>

/* --- Game Constants & Structures --- */
#define EMPTY 0
#define PAWN 1
#define KNIGHT 2
#define BISHOP 3
#define ROOK 4
#define QUEEN 5
#define KING 6

#define NONE 0
#define WHITE 1
#define BLACK 2

typedef struct {
    int piece; // EMPTY, PAWN, KNIGHT, etc.
    int color; // NONE, WHITE, BLACK
} Square;

typedef struct {
    Square board[64];
    int turn;       // WHITE, BLACK
    int castle;     // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep;         // En passant target square index (-1 if none)
    int halfmove;
    int fullmove;
} BoardState;

typedef struct {
    int from;
    int to;
    int promotion;  // Piece type if promotion, otherwise 0
} Move;

typedef struct {
    Move m;
    char san[16];
} PlayedMove;

/* --- Global State --- */
BoardState state;
Move current_legal_moves[256];
int current_legal_count = 0;

PlayedMove move_history[1024];
BoardState state_history[1024];
int history_len = 0;

int game_over_status = 0; // 0: Active, 1: White Wins, 2: Black Wins, 3: Draw

// Terminal handling
struct termios orig_termios;

// UCI Engine variables
int engine_enabled = 0;
int human_color = WHITE;
int engine_pid = -1;
int to_engine[2];
int from_engine[2];
char engine_buf[4096];
int engine_buf_len = 0;
int is_engine_thinking = 0;

// Engine Search Options
enum TC_Mode { TC_DEPTH, TC_NODES, TC_MOVETIME };
enum TC_Mode current_tc_mode = TC_DEPTH;
int tc_depth_value = 8;
int tc_nodes_value = 50000;
int tc_movetime_value = 1000; // in milliseconds

// Unicode representations
const char* piece_symbols[3][7] = {
    {" ", " ", " ", " ", " ", " ", " "},            // NONE
    {" ", "♙", "♘", "♗", "♖", "♕", "♔"},            // WHITE
    {" ", "♟", "♞", "♝", "♜", "♛", "♚"}             // BLACK
};

/* --- Terminal Settings --- */
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // Show cursor
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; // 100ms non-blocking read timeout
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void sig_handler(int sig) {
    disable_raw_mode();
    if (engine_pid != -1) {
        kill(engine_pid, SIGTERM);
    }
    exit(0);
}

/* --- Chess Logic & Move Generator --- */
void init_board(BoardState *b) {
    memset(b, 0, sizeof(BoardState));
    int back_row[] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
    for (int col = 0; col < 8; col++) {
        b->board[col] = (Square){back_row[col], BLACK};
        b->board[8 + col] = (Square){PAWN, BLACK};
        for (int r = 2; r < 6; r++) {
            b->board[r * 8 + col] = (Square){EMPTY, NONE};
        }
        b->board[48 + col] = (Square){PAWN, WHITE};
        b->board[56 + col] = (Square){back_row[col], WHITE};
    }
    b->turn = WHITE;
    b->castle = 15; // WK=1, WQ=2, BK=4, BQ=8
    b->ep = -1;
    b->halfmove = 0;
    b->fullmove = 1;
}

int is_square_attacked(BoardState *b, int sq, int attacker_color) {
    int r = sq / 8, c = sq % 8;

    // Knight attacks
    int kn_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int kn_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn_r[i], nc = c + kn_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            Square s = b->board[nr * 8 + nc];
            if (s.piece == KNIGHT && s.color == attacker_color) return 1;
        }
    }

    // Sliding attacks (Bishop, Rook, Queen)
    int dirs[8][2] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1},       // Orthogonals
        {-1, -1}, {-1, 1}, {1, -1}, {1, 1}      // Diagonals
    };
    for (int d = 0; d < 8; d++) {
        int step = 1;
        while (1) {
            int nr = r + dirs[d][0] * step;
            int nc = c + dirs[d][1] * step;
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            Square s = b->board[nr * 8 + nc];
            if (s.piece != EMPTY) {
                if (s.color == attacker_color) {
                    if (d < 4 && (s.piece == ROOK || s.piece == QUEEN)) return 1;
                    if (d >= 4 && (s.piece == BISHOP || s.piece == QUEEN)) return 1;
                }
                break;
            }
            step++;
        }
    }

    // King attacks
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = r + dr, nc = c + dc;
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                Square s = b->board[nr * 8 + nc];
                if (s.piece == KING && s.color == attacker_color) return 1;
            }
        }
    }

    // Pawn attacks
    int p_dir = (attacker_color == WHITE) ? 1 : -1;
    int p_cols[] = {c - 1, c + 1};
    for (int i = 0; i < 2; i++) {
        int nc = p_cols[i];
        int nr = r + p_dir;
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            Square s = b->board[nr * 8 + nc];
            if (s.piece == PAWN && s.color == attacker_color) return 1;
        }
    }
    return 0;
}

int is_in_check(BoardState *b, int color) {
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (b->board[i].piece == KING && b->board[i].color == color) {
            king_sq = i;
            break;
        }
    }
    if (king_sq == -1) return 0;
    return is_square_attacked(b, king_sq, (color == WHITE) ? BLACK : WHITE);
}

void make_move(BoardState *next, const BoardState *prev, Move m) {
    *next = *prev;
    Square p = next->board[m.from];
    next->board[m.from] = (Square){EMPTY, NONE};

    // Castling mechanics
    if (p.piece == KING) {
        if (p.color == WHITE) {
            next->castle &= ~1; next->castle &= ~2;
            if (m.from == 60) {
                if (m.to == 62) { next->board[61] = next->board[63]; next->board[63] = (Square){EMPTY, NONE}; }
                else if (m.to == 58) { next->board[59] = next->board[56]; next->board[56] = (Square){EMPTY, NONE}; }
            }
        } else {
            next->castle &= ~4; next->castle &= ~8;
            if (m.from == 4) {
                if (m.to == 6) { next->board[5] = next->board[7]; next->board[7] = (Square){EMPTY, NONE}; }
                else if (m.to == 2) { next->board[3] = next->board[0]; next->board[0] = (Square){EMPTY, NONE}; }
            }
        }
    }

    // Disable castling rights if Rooks move or get captured
    if (p.piece == ROOK) {
        if (m.from == 56) next->castle &= ~2;
        if (m.from == 63) next->castle &= ~1;
        if (m.from == 0)  next->castle &= ~8;
        if (m.from == 7)  next->castle &= ~4;
    }
    if (m.to == 56) next->castle &= ~2;
    if (m.to == 63) next->castle &= ~1;
    if (m.to == 0)  next->castle &= ~8;
    if (m.to == 7)  next->castle &= ~4;

    // En Passant capture
    if (p.piece == PAWN && m.to == prev->ep) {
        int victim = (p.color == WHITE) ? (m.to + 8) : (m.to - 8);
        next->board[victim] = (Square){EMPTY, NONE};
    }

    // Set En Passant target square
    next->ep = -1;
    if (p.piece == PAWN && abs(m.from - m.to) == 16) {
        next->ep = (p.color == WHITE) ? (m.from - 8) : (m.from + 8);
    }

    // Apply Move/Promotion
    if (m.promotion) {
        next->board[m.to] = (Square){m.promotion, p.color};
    } else {
        next->board[m.to] = p;
    }

    next->turn = (prev->turn == WHITE) ? BLACK : WHITE;
    if (p.piece == PAWN || prev->board[m.to].piece != EMPTY) {
        next->halfmove = 0;
    } else {
        next->halfmove++;
    }
    if (prev->turn == BLACK) next->fullmove++;
}

void generate_pseudo_moves(BoardState *b, int from, Move *moves, int *count) {
    Square p = b->board[from];
    if (p.piece == EMPTY || p.color != b->turn) return;

    int r = from / 8, c = from % 8;

    if (p.piece == PAWN) {
        int dir = (p.color == WHITE) ? -1 : 1;
        int start_r = (p.color == WHITE) ? 6 : 1;
        int promo_r = (p.color == WHITE) ? 0 : 7;

        // One-step forward
        int nr = r + dir;
        if (nr >= 0 && nr < 8 && b->board[nr * 8 + c].piece == EMPTY) {
            if (nr == promo_r) {
                moves[(*count)++] = (Move){from, nr * 8 + c, QUEEN};
                moves[(*count)++] = (Move){from, nr * 8 + c, ROOK};
                moves[(*count)++] = (Move){from, nr * 8 + c, BISHOP};
                moves[(*count)++] = (Move){from, nr * 8 + c, KNIGHT};
            } else {
                moves[(*count)++] = (Move){from, nr * 8 + c, 0};
                // Two-step forward
                if (r == start_r && b->board[(r + 2 * dir) * 8 + c].piece == EMPTY) {
                    moves[(*count)++] = (Move){from, (r + 2 * dir) * 8 + c, 0};
                }
            }
        }

        // Diagonal captures
        int capture_cols[] = {c - 1, c + 1};
        for (int i = 0; i < 2; i++) {
            int nc = capture_cols[i];
            if (nc >= 0 && nc < 8) {
                int target = nr * 8 + nc;
                if (b->board[target].color == ((p.color == WHITE) ? BLACK : WHITE) || target == b->ep) {
                    if (nr == promo_r) {
                        moves[(*count)++] = (Move){from, target, QUEEN};
                        moves[(*count)++] = (Move){from, target, ROOK};
                        moves[(*count)++] = (Move){from, target, BISHOP};
                        moves[(*count)++] = (Move){from, target, KNIGHT};
                    } else {
                        moves[(*count)++] = (Move){from, target, 0};
                    }
                }
            }
        }
    }
    else if (p.piece == KNIGHT) {
        int kn_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
        int kn_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
        for (int i = 0; i < 8; i++) {
            int nr = r + kn_r[i], nc = c + kn_c[i];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int target = nr * 8 + nc;
                if (b->board[target].color != p.color) {
                    moves[(*count)++] = (Move){from, target, 0};
                }
            }
        }
    }
    else if (p.piece == KING) {
        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                if (dr == 0 && dc == 0) continue;
                int nr = r + dr, nc = c + dc;
                if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                    int target = nr * 8 + nc;
                    if (b->board[target].color != p.color) {
                        moves[(*count)++] = (Move){from, target, 0};
                    }
                }
            }
        }

        // Castling
        int enemy = (p.color == WHITE) ? BLACK : WHITE;
        if (p.color == WHITE) {
            if ((b->castle & 1) && b->board[61].piece == EMPTY && b->board[62].piece == EMPTY) {
                if (!is_square_attacked(b, 60, enemy) && !is_square_attacked(b, 61, enemy) && !is_square_attacked(b, 62, enemy)) {
                    moves[(*count)++] = (Move){60, 62, 0};
                }
            }
            if ((b->castle & 2) && b->board[59].piece == EMPTY && b->board[58].piece == EMPTY && b->board[57].piece == EMPTY) {
                if (!is_square_attacked(b, 60, enemy) && !is_square_attacked(b, 59, enemy) && !is_square_attacked(b, 58, enemy)) {
                    moves[(*count)++] = (Move){60, 58, 0};
                }
            }
        } else {
            if ((b->castle & 4) && b->board[5].piece == EMPTY && b->board[6].piece == EMPTY) {
                if (!is_square_attacked(b, 4, enemy) && !is_square_attacked(b, 5, enemy) && !is_square_attacked(b, 6, enemy)) {
                    moves[(*count)++] = (Move){4, 6, 0};
                }
            }
            if ((b->castle & 8) && b->board[3].piece == EMPTY && b->board[2].piece == EMPTY && b->board[1].piece == EMPTY) {
                if (!is_square_attacked(b, 4, enemy) && !is_square_attacked(b, 3, enemy) && !is_square_attacked(b, 2, enemy)) {
                    moves[(*count)++] = (Move){4, 2, 0};
                }
            }
        }
    }
    else {
        // Sliding Pieces (Bishop, Rook, Queen)
        int start_d = (p.piece == BISHOP) ? 4 : 0;
        int end_d = (p.piece == ROOK) ? 4 : 8;
        int dirs[8][2] = {
            {-1, 0}, {1, 0}, {0, -1}, {0, 1},
            {-1, -1}, {-1, 1}, {1, -1}, {1, 1}
        };
        for (int d = start_d; d < end_d; d++) {
            int step = 1;
            while (1) {
                int nr = r + dirs[d][0] * step;
                int nc = c + dirs[d][1] * step;
                if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
                int target = nr * 8 + nc;
                if (b->board[target].piece == EMPTY) {
                    moves[(*count)++] = (Move){from, target, 0};
                } else {
                    if (b->board[target].color != p.color) {
                        moves[(*count)++] = (Move){from, target, 0};
                    }
                    break;
                }
                step++;
            }
        }
    }
}

void generate_legal_moves(BoardState *b, Move *moves, int *count) {
    *count = 0;
    Move pseudo[256];
    int pseudo_count = 0;
    for (int i = 0; i < 64; i++) {
        if (b->board[i].color == b->turn) {
            generate_pseudo_moves(b, i, pseudo, &pseudo_count);
        }
    }
    for (int i = 0; i < pseudo_count; i++) {
        BoardState next;
        make_move(&next, b, pseudo[i]);
        if (!is_in_check(&next, b->turn)) {
            moves[(*count)++] = pseudo[i];
        }
    }
}

/* --- SAN PGN Formatting Engine --- */
void get_san(BoardState *b, Move m, char *san) {
    int is_cap = (b->board[m.to].piece != EMPTY) || (b->board[m.from].piece == PAWN && m.to == b->ep);
    int piece = b->board[m.from].piece;

    if (piece == KING) {
        if (m.from == 60) {
            if (m.to == 62) { strcpy(san, "O-O"); goto post_process; }
            if (m.to == 58) { strcpy(san, "O-O-O"); goto post_process; }
        }
        if (m.from == 4) {
            if (m.to == 6) { strcpy(san, "O-O"); goto post_process; }
            if (m.to == 2) { strcpy(san, "O-O-O"); goto post_process; }
        }
    }

    int len = 0;
    if (piece == PAWN) {
        if (is_cap) {
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
        char p_char = ' ';
        if (piece == KNIGHT) p_char = 'N';
        else if (piece == BISHOP) p_char = 'B';
        else if (piece == ROOK) p_char = 'R';
        else if (piece == QUEEN) p_char = 'Q';
        else if (piece == KING) p_char = 'K';

        san[len++] = p_char;

        // Disambiguation
        Move legal[256];
        int legal_count = 0;
        generate_legal_moves(b, legal, &legal_count);
        int duplicate = 0, same_file = 0, same_rank = 0;
        for (int i = 0; i < legal_count; i++) {
            if (legal[i].from != m.from && legal[i].to == m.to && b->board[legal[i].from].piece == piece) {
                duplicate = 1;
                if (legal[i].from % 8 == m.from % 8) same_file = 1;
                if (legal[i].from / 8 == m.from / 8) same_rank = 1;
            }
        }
        if (duplicate) {
            if (!same_file) san[len++] = 'a' + (m.from % 8);
            else if (!same_rank) san[len++] = '8' - (m.from / 8);
            else {
                san[len++] = 'a' + (m.from % 8);
                san[len++] = '8' - (m.from / 8);
            }
        }

        if (is_cap) san[len++] = 'x';
        san[len++] = 'a' + (m.to % 8);
        san[len++] = '8' - (m.to / 8);
    }
    san[len] = '\0';

post_process:;
    BoardState next;
    make_move(&next, b, m);
    if (is_in_check(&next, next.turn)) {
        Move next_legal[256];
        int next_legal_count = 0;
        generate_legal_moves(&next, next_legal, &next_legal_count);
        if (next_legal_count == 0) strcat(san, "#");
        else strcat(san, "+");
    }
}

void move_to_string(Move m, char *buf) {
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

/* --- UCI Engine Interface --- */
void send_to_engine(const char *cmd) {
    if (engine_pid != -1) {
        write(to_engine[1], cmd, strlen(cmd));
    }
}

void start_engine(const char *path) {
    pipe(to_engine);
    pipe(from_engine);
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[1]);
        close(from_engine[0]);

        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDERR_FILENO);

        execlp(path, path, (char *)NULL);
        perror("Engine launch failed");
        exit(1);
    }
    close(to_engine[0]);
    close(from_engine[1]);
    fcntl(from_engine[0], F_SETFL, O_NONBLOCK);
}

void sync_engine_position() {
    if (engine_pid == -1) return;
    char cmd[8192] = "position startpos moves";
    for (int i = 0; i < history_len; i++) {
        char m_str[8];
        move_to_string(move_history[i].m, m_str);
        strcat(cmd, " ");
        strcat(cmd, m_str);
    }
    strcat(cmd, "\n");
    send_to_engine(cmd);
}

void trigger_engine_search() {
    if (engine_pid == -1) return;
    char cmd[128];
    if (current_tc_mode == TC_DEPTH) {
        sprintf(cmd, "go depth %d\n", tc_depth_value);
    } else if (current_tc_mode == TC_NODES) {
        sprintf(cmd, "go nodes %d\n", tc_nodes_value);
    } else {
        sprintf(cmd, "go movetime %d\n", tc_movetime_value);
    }
    send_to_engine(cmd);
}

void setup_engine() {
    printf("\n\033[1;36m┌──────────────────────────────────────────────┐\n");
    printf("│             CHESS ENGINE SETUP               │\n");
    printf("└──────────────────────────────────────────────┘\033[0m\n");
    printf(" Enter the terminal command/path to launch your engine.\n");
    printf(" (e.g. 'stockfish' or '/opt/homebrew/bin/stockfish')\n\n");
    printf(" Leave completely EMPTY to play local 2-player pass-and-play.\n\n");
    printf(" Engine Path/Command: ");
    fflush(stdout);

    char path[256];
    if (fgets(path, sizeof(path), stdin)) {
        path[strcspn(path, "\n")] = 0;
        if (strlen(path) > 0) {
            start_engine(path);
            engine_enabled = 1;
        } else {
            engine_enabled = 0;
        }
    }

    if (engine_enabled) {
        send_to_engine("uci\n");
        send_to_engine("isready\n");
        usleep(300000);

        printf("\n Choose your color:\n [1] Play as White (starts first)\n [2] Play as Black\n Your Choice [1-2, default 1]: ");
        fflush(stdout);
        char side[16];
        if (fgets(side, sizeof(side), stdin)) {
            if (side[0] == '2') human_color = BLACK;
            else human_color = WHITE;
        }
    }
}

/* --- Keyboard Input Handler --- */
int read_key() {
    char c;
    int nread = read(STDIN_FILENO, &c, 1);
    if (nread <= 0) return 0;
    if (c == '\033') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) == 0) return '\033';
        if (read(STDIN_FILENO, &seq[1], 1) == 0) return '\033';
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 'u'; // Up Arrow
                case 'B': return 'd'; // Down Arrow
                case 'C': return 'r'; // Right Arrow
                case 'D': return 'l'; // Left Arrow
            }
        }
        return '\033';
    }
    return c;
}

/* --- GUI Presentation Engine --- */
void draw_board(int cursor_idx, int selected_idx, Move last_move, int has_last_move) {
    // Return terminal cursor to home pos to redraw in-place smoothly
    printf("\033[H\033[?25l");

    printf("\n   \033[1;33mPORTABLE TERMINAL CHESS GUI\033[0m\n");
    printf("  ┌────────────────────────────────────────────────┐\n");

    for (int r = 0; r < 8; r++) {
        for (int l = 0; l < 3; l++) {
            if (l == 1) printf(" %d│", 8 - r);
            else printf("  │");

            for (int c = 0; c < 8; c++) {
                int sq = r * 8 + c;
                int is_light = (r + c) % 2 == 0;

                // Base Square Color Configuration
                const char *bg = is_light ? "\033[48;5;223m" : "\033[48;5;130m";

                // Highlighting previous move trajectory
                if (has_last_move && (sq == last_move.from || sq == last_move.to)) {
                    bg = is_light ? "\033[48;5;216m" : "\033[48;5;173m";
                }

                // Highlighting destination squares for current selection
                int is_target = 0;
                if (selected_idx != -1) {
                    for (int i = 0; i < current_legal_count; i++) {
                        if (current_legal_moves[i].from == selected_idx && current_legal_moves[i].to == sq) {
                            is_target = 1;
                            break;
                        }
                    }
                }
                if (is_target) {
                    bg = is_light ? "\033[48;5;151m" : "\033[48;5;108m"; // Mint green indicators
                }

                // Selected square highlighting
                if (sq == selected_idx) {
                    bg = "\033[48;5;75m"; // Light ocean blue
                }

                // Active navigating cursor highlighting
                if (sq == cursor_idx) {
                    bg = "\033[48;5;45m"; // Glowing cyan
                }

                printf("%s", bg);
                Square p = state.board[sq];

                if (l == 1) {
                    const char *fg = (p.color == WHITE) ? "\033[38;5;231;1m" : "\033[38;5;16;1m";
                    if (p.piece != EMPTY) {
                        printf("  %s%s\033[0m%s ", fg, piece_symbols[p.color][p.piece], bg);
                    } else if (is_target) {
                        printf("  \033[38;5;242m●\033[0m%s ", bg); // Dot indicators for empty squares
                    } else {
                        printf("    ");
                    }
                } else {
                    printf("    ");
                }
            }
            printf("\033[0m│");
            if (l == 1) printf(" %d", 8 - r);
            printf("\n");
        }
    }
    printf("  └────────────────────────────────────────────────┘\n");
    printf("       a   b   c   d   e   f   g   h\n\n");
}

void print_pgn() {
    printf(" \033[1;33mPGN Game Log:\033[0m\n  ");
    int move_num = 1;
    for (int i = 0; i < history_len; i++) {
        if (i % 2 == 0) {
            printf("%d. %s", move_num, move_history[i].san);
            move_num++;
        } else {
            printf(" %s   ", move_history[i].san);
            if ((i / 2 + 1) % 5 == 0) printf("\n  ");
        }
    }
    printf("\033[K\n\n"); // Clear trailing characters on current line
}

void config_time_control() {
    disable_raw_mode();
    printf("\033[?25h\n\033[1;36m┌──────────────────────────────────────────────┐\n");
    printf("│           TIME CONTROL OPTIONS               │\n");
    printf("└──────────────────────────────────────────────┘\033[0m\n");
    printf(" [1] Search Depth (moves are fast, fixed depth)\n");
    printf(" [2] Node Limit (limits calculations per move)\n");
    printf(" [3] Move Time (fixed milliseconds per move)\n\n");
    printf(" Select Option [1-3]: ");
    fflush(stdout);

    char choice[16];
    if (fgets(choice, sizeof(choice), stdin)) {
        int opt = atoi(choice);
        printf(" Enter new limit value: ");
        fflush(stdout);
        char val[32];
        if (fgets(val, sizeof(val), stdin)) {
            int num = atoi(val);
            if (num > 0) {
                if (opt == 1) { current_tc_mode = TC_DEPTH; tc_depth_value = num; }
                else if (opt == 2) { current_tc_mode = TC_NODES; tc_nodes_value = num; }
                else if (opt == 3) { current_tc_mode = TC_MOVETIME; tc_movetime_value = num; }
            }
        }
    }
    enable_raw_mode();
    printf("\033[2J"); // Clear standard console trace artifacts
}

int get_promotion_choice() {
    disable_raw_mode();
    printf("\033[?25h\n\033[1;35m Pawn Promotion Triggered!\033[0m Select piece type:\n");
    printf(" [q] Queen  [r] Rook  [b] Bishop  [n] Knight\n Enter piece key: ");
    fflush(stdout);
    char in[16];
    int chosen = QUEEN;
    if (fgets(in, sizeof(in), stdin)) {
        char c = in[0];
        if (c == 'r' || c == 'R') chosen = ROOK;
        else if (c == 'b' || c == 'B') chosen = BISHOP;
        else if (c == 'n' || c == 'N') chosen = KNIGHT;
    }
    enable_raw_mode();
    printf("\033[2J");
    return chosen;
}

void execute_undo() {
    int rollbacks = (engine_enabled) ? 2 : 1;
    if (history_len >= rollbacks) {
        history_len -= rollbacks;
        state = state_history[history_len];
        sync_engine_position();
    }
}

/* --- Master Controller Loop --- */
int main() {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    init_board(&state);
    setup_engine();
    enable_raw_mode();

    generate_legal_moves(&state, current_legal_moves, &current_legal_count);

    if (engine_enabled && state.turn != human_color) {
        is_engine_thinking = 1;
        sync_engine_position();
        trigger_engine_search();
    }

    printf("\033[2J"); // Initial canvas clear

    int cursor_idx = 60; // Start navigation on e1
    int selected_idx = -1;
    Move last_move = {0, 0, 0};
    int has_last_move = 0;

    while (1) {
        // Evaluate Terminal game over flags
        if (current_legal_count == 0) {
            if (is_in_check(&state, state.turn)) {
                game_over_status = (state.turn == WHITE) ? 2 : 1;
            } else {
                game_over_status = 3;
            }
        } else {
            game_over_status = 0;
        }

        draw_board(cursor_idx, selected_idx, last_move, has_last_move);

        // Sidebar Presentation Details
        printf("  Side to Move: \033[1m%s\033[0m\n", state.turn == WHITE ? "\033[1;37mWhite\033[0m" : "\033[1;30mBlack\033[0m");
        if (engine_enabled) {
            printf("  Engine State: %s\n", is_engine_thinking ? "\033[1;31mThinking...\033[0m" : "\033[1;32mIdle\033[0m");
            printf("  Active Constraints: ");
            if (current_tc_mode == TC_DEPTH) printf("Depth %d Ply\n", tc_depth_value);
            else if (current_tc_mode == TC_NODES) printf("Nodes Max %d\n", tc_nodes_value);
            else printf("Time limit %d ms\n", tc_movetime_value);
        } else {
            printf("  Engine State: \033[1;30mPassive (Pass-and-Play Mode)\033[0m\n");
        }
        printf("\033[K\n");

        if (game_over_status != 0) {
            printf("  \033[1;31m*** MATCH TERMINATED ***\033[0m\n  ");
            if (game_over_status == 1) printf("\033[1;32mWhite wins by Checkmate!\033[0m\n");
            else if (game_over_status == 2) printf("\033[1;32mBlack wins by Checkmate!\033[0m\n");
            else printf("\033[1;33mStalemate Draw reached!\033[0m\n");
            printf("\n");
        }

        print_pgn();

        printf("  [Arrows] Navigate Cursor  [Space/Enter] Interact  [U] Undo  [T] Search Configuration  [Q] Exit Game\n");
        printf("\033[J"); // Instantly wipe trailing old graphics blocks below game container
        fflush(stdout);

        // Wait for asynchronous standard input multiplexer
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        int max_fd = STDIN_FILENO;
        if (engine_pid != -1) {
            FD_SET(from_engine[0], &readfds);
            if (from_engine[0] > max_fd) max_fd = from_engine[0];
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 15000; // Fast non-blocking 15ms interface poll

        int res = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (res > 0) {
            // Read incoming Engine calculations from pipe
            if (engine_pid != -1 && FD_ISSET(from_engine[0], &readfds)) {
                char ch;
                while (read(from_engine[0], &ch, 1) > 0) {
                    if (ch == '\n' || ch == '\r') {
                        if (engine_buf_len > 0) {
                            engine_buf[engine_buf_len] = '\0';
                            
                            // Check for UCI bestmove signal
                            if (strncmp(engine_buf, "bestmove ", 9) == 0) {
                                char m_str[16];
                                sscanf(engine_buf, "bestmove %s", m_str);
                                
                                int f_col = m_str[0] - 'a', f_row = '8' - m_str[1];
                                int t_col = m_str[2] - 'a', t_row = '8' - m_str[3];
                                Move m;
                                m.from = f_row * 8 + f_col;
                                m.to = t_row * 8 + t_col;
                                m.promotion = 0;
                                if (m_str[4] != '\0' && m_str[4] != ' ' && m_str[4] != '\n' && m_str[4] != '\r') {
                                    if (m_str[4] == 'q') m.promotion = QUEEN;
                                    else if (m_str[4] == 'r') m.promotion = ROOK;
                                    else if (m_str[4] == 'b') m.promotion = BISHOP;
                                    else if (m_str[4] == 'n') m.promotion = KNIGHT;
                                }

                                char san[16];
                                get_san(&state, m, san);

                                state_history[history_len] = state;
                                move_history[history_len].m = m;
                                strcpy(move_history[history_len].san, san);
                                history_len++;

                                BoardState next;
                                make_move(&next, &state, m);
                                state = next;

                                last_move = m;
                                has_last_move = 1;
                                is_engine_thinking = 0;

                                generate_legal_moves(&state, current_legal_moves, &current_legal_count);
                            }
                            engine_buf_len = 0;
                        }
                    } else if (engine_buf_len < sizeof(engine_buf) - 2) {
                        engine_buf[engine_buf_len++] = ch;
                    }
                }
            }

            // Standard Keyboard Controls Parser
            if (FD_ISSET(STDIN_FILENO, &readfds)) {
                int key = read_key();
                if (key == 'q' || key == 'Q') {
                    break;
                }
                else if (key == 'u') { // Move Up
                    if (cursor_idx >= 8) cursor_idx -= 8;
                }
                else if (key == 'd') { // Move Down
                    if (cursor_idx < 56) cursor_idx += 8;
                }
                else if (key == 'l') { // Move Left
                    if (cursor_idx % 8 > 0) cursor_idx -= 1;
                }
                else if (key == 'r') { // Move Right
                    if (cursor_idx % 8 < 7) cursor_idx += 1;
                }
                else if (key == 'u' || key == 'U') {
                    if (!is_engine_thinking) {
                        execute_undo();
                        if (history_len > 0) {
                            last_move = move_history[history_len - 1].m;
                            has_last_move = 1;
                        } else {
                            has_last_move = 0;
                        }
                        selected_idx = -1;
                        generate_legal_moves(&state, current_legal_moves, &current_legal_count);
                    }
                }
                else if (key == 't' || key == 'T') {
                    if (!is_engine_thinking) {
                        config_time_control();
                        generate_legal_moves(&state, current_legal_moves, &current_legal_count);
                    }
                }
                else if (key == ' ' || key == '\n' || key == '\r') {
                    if (game_over_status == 0 && (!engine_enabled || state.turn == human_color)) {
                        if (selected_idx == -1) {
                            if (state.board[cursor_idx].color == state.turn) {
                                selected_idx = cursor_idx;
                            }
                        } else {
                            // Check move legalese validation
                            int valid = 0;
                            Move attempted;
                            for (int i = 0; i < current_legal_count; i++) {
                                if (current_legal_moves[i].from == selected_idx && current_legal_moves[i].to == cursor_idx) {
                                    valid = 1;
                                    attempted = current_legal_moves[i];
                                    break;
                                }
                            }

                            if (valid) {
                                if (state.board[attempted.from].piece == PAWN && (cursor_idx / 8 == 0 || cursor_idx / 8 == 7)) {
                                    attempted.promotion = get_promotion_choice();
                                }

                                char san[16];
                                get_san(&state, attempted, san);

                                state_history[history_len] = state;
                                move_history[history_len].m = attempted;
                                strcpy(move_history[history_len].san, san);
                                history_len++;

                                BoardState next;
                                make_move(&next, &state, attempted);
                                state = next;

                                last_move = attempted;
                                has_last_move = 1;
                                selected_idx = -1;

                                generate_legal_moves(&state, current_legal_moves, &current_legal_count);

                                // Fire Engine turn triggers
                                if (engine_enabled && state.turn != human_color) {
                                    is_engine_thinking = 1;
                                    sync_engine_position();
                                    trigger_engine_search();
                                }
                            } else {
                                if (state.board[cursor_idx].color == state.turn) {
                                    selected_idx = cursor_idx; // Swap selection
                                } else {
                                    selected_idx = -1; // Deselect
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    disable_raw_mode();
    if (engine_pid != -1) {
        kill(engine_pid, SIGTERM);
    }
    return 0;
}
