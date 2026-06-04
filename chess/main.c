#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/select.h>
#include <signal.h>
#include <stdint.h>

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

// ANSI Colors for GUI
#define BG_LIGHT  "\033[48;5;223m" // Cream
#define BG_DARK   "\033[48;5;130m" // Brown
#define BG_SELECT "\033[48;5;226m" // Yellow
#define BG_LEGAL  "\033[48;5;121m" // Light Green
#define BG_LAST   "\033[48;5;153m" // Light Blue
#define BG_CHECK  "\033[48;5;196m" // Red

#define FG_WHITE  "\033[38;5;15m\033[1m" // Bold White
#define FG_BLACK  "\033[38;5;232m"       // Dark Black

typedef enum { TC_DEPTH, TC_NODES, TC_TIME } TCType;

typedef struct {
    TCType type;
    int depth;
    int nodes;
    int time_ms;
    char engine_path[512];
    int player_color; // WHITE, BLACK, or 0 (PVP)
} Config;

typedef struct {
    uint8_t board[64];
    int turn;          // WHITE or BLACK
    int castling;     // Bits: 1: WK, 2: WQ, 4: BK, 8: BQ
    int en_passant;   // Square index or -1
    int halfmove;
    int fullmove;
} BoardState;

typedef struct {
    int from;
    int to;
    int promotion;
} Move;

// Globals for engine process
int to_engine[2];
int from_engine[2];
pid_t engine_pid = -1;
int engine_online = 0;
struct termios orig_termios;

// Forward Declarations
void disable_raw_mode(void);
int is_square_attacked(BoardState *state, int sq, int attacker_color);
int get_legal_moves(BoardState *state, int from, int *tos);

// Termios Management
void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h\033[0m\n"); // Show cursor, reset styling
}

// UCI Engine I/O
void start_engine(const char *path) {
    pipe(to_engine);
    pipe(from_engine);
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[1]);
        close(from_engine[0]);
        execlp(path, path, NULL);
        exit(1);
    }
    close(to_engine[0]);
    close(from_engine[1]);
    int flags = fcntl(from_engine[0], F_GETFL, 0);
    fcntl(from_engine[0], F_SETFL, flags | O_NONBLOCK);
}

void stop_engine(void) {
    if (engine_pid > 0) {
        write(to_engine[1], "quit\n", 5);
        close(to_engine[1]);
        close(from_engine[0]);
        kill(engine_pid, SIGTERM);
        engine_pid = -1;
    }
    engine_online = 0;
}

void send_engine(const char *cmd) {
    if (engine_pid > 0) {
        write(to_engine[1], cmd, strlen(cmd));
        write(to_engine[1], "\n", 1);
    }
}

int read_engine(char *buf, int max_len) {
    static char internal_buf[4096];
    static int internal_len = 0;
    int n = read(from_engine[0], internal_buf + internal_len, sizeof(internal_buf) - internal_len - 1);
    if (n > 0) {
        internal_len += n;
        internal_buf[internal_len] = '\0';
    }
    char *eol = strchr(internal_buf, '\n');
    if (eol) {
        int len = eol - internal_buf;
        if (len >= max_len) len = max_len - 1;
        strncpy(buf, internal_buf, len);
        buf[len] = '\0';
        if (len > 0 && buf[len - 1] == '\r') buf[len - 1] = '\0';
        memmove(internal_buf, eol + 1, internal_len - len - 1);
        internal_len -= (len + 1);
        return 1;
    }
    return 0;
}

int restart_engine(const char *path) {
    stop_engine();
    start_engine(path);
    send_engine("uci");
    char buf[1024];
    int got_uciok = 0;
    for (int i = 0; i < 500; i++) {
        if (read_engine(buf, sizeof(buf)) && strcmp(buf, "uciok") == 0) {
            got_uciok = 1;
            break;
        }
        usleep(1000);
    }
    if (!got_uciok) return 0;
    send_engine("isready");
    int got_readyok = 0;
    for (int i = 0; i < 500; i++) {
        if (read_engine(buf, sizeof(buf)) && strcmp(buf, "readyok") == 0) {
            got_readyok = 1;
            break;
        }
        usleep(1000);
    }
    engine_online = got_readyok;
    return engine_online;
}

// Chess Board Initialization
void init_board(BoardState *state) {
    memset(state->board, EMPTY, 64);
    for (int i = 8; i < 16; i++) state->board[i] = BLACK | PAWN;
    for (int i = 48; i < 56; i++) state->board[i] = WHITE | PAWN;

    state->board[0] = state->board[7] = BLACK | ROOK;
    state->board[56] = state->board[63] = WHITE | ROOK;
    state->board[1] = state->board[6] = BLACK | KNIGHT;
    state->board[57] = state->board[62] = WHITE | KNIGHT;
    state->board[2] = state->board[5] = BLACK | BISHOP;
    state->board[58] = state->board[61] = WHITE | BISHOP;
    state->board[3] = BLACK | QUEEN;
    state->board[59] = WHITE | QUEEN;
    state->board[4] = BLACK | KING;
    state->board[60] = WHITE | KING;

    state->turn = WHITE;
    state->castling = 15;
    state->en_passant = -1;
    state->halfmove = 0;
    state->fullmove = 1;
}

const char *get_piece_char(int piece) {
    switch (piece & TYPE_MASK) {
        case PAWN:   return "♟";
        case KNIGHT: return "♞";
        case BISHOP: return "♝";
        case ROOK:   return "♜";
        case QUEEN:  return "♛";
        case KING:   return "♚";
    }
    return " ";
}

// Move Rules & Logic
int find_king(BoardState *state, int color) {
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == (color | KING)) return i;
    }
    return -1;
}

int is_in_check(BoardState *state, int color) {
    int king_sq = find_king(state, color);
    if (king_sq == -1) return 0;
    return is_square_attacked(state, king_sq, color == WHITE ? BLACK : WHITE);
}

int is_square_attacked(BoardState *state, int sq, int attacker_color) {
    int r = sq / 8, c = sq % 8;

    // Knight Attacks
    int kn_offsets[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn_offsets[i][0], nc = c + kn_offsets[i][1];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int p = state->board[nr * 8 + nc];
            if ((p & COLOR_MASK) == attacker_color && (p & TYPE_MASK) == KNIGHT) return 1;
        }
    }

    // Sliding Attacks (Rook, Bishop, Queen)
    int dirs[8][2] = {{-1,0},{1,0},{0,-1},{0,1},{-1,-1},{-1,1},{1,-1},{1,1}};
    for (int i = 0; i < 8; i++) {
        int dr = dirs[i][0], dc = dirs[i][1], nr = r + dr, nc = c + dc;
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int p = state->board[nr * 8 + nc];
            if (p != EMPTY) {
                if ((p & COLOR_MASK) == attacker_color) {
                    int type = p & TYPE_MASK;
                    if (i < 4 && (type == ROOK || type == QUEEN)) return 1;
                    if (i >= 4 && (type == BISHOP || type == QUEEN)) return 1;
                }
                break;
            }
            nr += dr; nc += dc;
        }
    }

    // King Attacks
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = r + dr, nc = c + dc;
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int p = state->board[nr * 8 + nc];
                if ((p & COLOR_MASK) == attacker_color && (p & TYPE_MASK) == KING) return 1;
            }
        }
    }

    // Pawn Attacks
    if (attacker_color == WHITE) {
        int pr = r + 1;
        if (pr < 8) {
            if (c > 0 && state->board[pr * 8 + c - 1] == (WHITE | PAWN)) return 1;
            if (c < 7 && state->board[pr * 8 + c + 1] == (WHITE | PAWN)) return 1;
        }
    } else {
        int pr = r - 1;
        if (pr >= 0) {
            if (c > 0 && state->board[pr * 8 + c - 1] == (BLACK | PAWN)) return 1;
            if (c < 7 && state->board[pr * 8 + c + 1] == (BLACK | PAWN)) return 1;
        }
    }
    return 0;
}

int generate_pseudo_moves(BoardState *state, int from, int *tos) {
    int count = 0, piece = state->board[from];
    if (piece == EMPTY) return 0;
    int type = piece & TYPE_MASK, color = piece & COLOR_MASK;
    if (color != state->turn) return 0;
    int r = from / 8, c = from % 8;

    if (type == PAWN) {
        int dir = (color == WHITE) ? -1 : 1;
        int start_row = (color == WHITE) ? 6 : 1;
        int nr = r + dir;
        if (nr >= 0 && nr < 8) {
            if (state->board[nr * 8 + c] == EMPTY) {
                tos[count++] = nr * 8 + c;
                if (r == start_row && state->board[(r + 2 * dir) * 8 + c] == EMPTY) {
                    tos[count++] = (r + 2 * dir) * 8 + c;
                }
            }
            int cap_cols[2] = {c - 1, c + 1};
            for (int i = 0; i < 2; i++) {
                int nc = cap_cols[i];
                if (nc >= 0 && nc < 8) {
                    int target = nr * 8 + nc;
                    if (state->board[target] != EMPTY && (state->board[target] & COLOR_MASK) != color) {
                        tos[count++] = target;
                    } else if (target == state->en_passant) {
                        tos[count++] = target;
                    }
                }
            }
        }
    } else if (type == KNIGHT) {
        int offsets[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
        for (int i = 0; i < 8; i++) {
            int nr = r + offsets[i][0], nc = c + offsets[i][1];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int target = nr * 8 + nc;
                if (state->board[target] == EMPTY || (state->board[target] & COLOR_MASK) != color) tos[count++] = target;
            }
        }
    } else if (type == KING) {
        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                if (dr == 0 && dc == 0) continue;
                int nr = r + dr, nc = c + dc;
                if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                    int target = nr * 8 + nc;
                    if (state->board[target] == EMPTY || (state->board[target] & COLOR_MASK) != color) tos[count++] = target;
                }
            }
        }
        // Castling logic
        if (color == WHITE) {
            if ((state->castling & 1) && state->board[61] == EMPTY && state->board[62] == EMPTY) {
                if (!is_square_attacked(state, 60, BLACK) && !is_square_attacked(state, 61, BLACK) && !is_square_attacked(state, 62, BLACK)) tos[count++] = 62;
            }
            if ((state->castling & 2) && state->board[59] == EMPTY && state->board[58] == EMPTY && state->board[57] == EMPTY) {
                if (!is_square_attacked(state, 60, BLACK) && !is_square_attacked(state, 59, BLACK) && !is_square_attacked(state, 58, BLACK)) tos[count++] = 58;
            }
        } else {
            if ((state->castling & 4) && state->board[5] == EMPTY && state->board[6] == EMPTY) {
                if (!is_square_attacked(state, 4, WHITE) && !is_square_attacked(state, 5, WHITE) && !is_square_attacked(state, 6, WHITE)) tos[count++] = 6;
            }
            if ((state->castling & 8) && state->board[3] == EMPTY && state->board[2] == EMPTY && state->board[1] == EMPTY) {
                if (!is_square_attacked(state, 4, WHITE) && !is_square_attacked(state, 3, WHITE) && !is_square_attacked(state, 2, WHITE)) tos[count++] = 2;
            }
        }
    } else {
        int dirs[8][2] = {{-1,0},{1,0},{0,-1},{0,1},{-1,-1},{-1,1},{1,-1},{1,1}};
        int start = (type == BISHOP) ? 4 : 0;
        int end = (type == ROOK) ? 4 : 8;
        for (int d = start; d < end; d++) {
            int nr = r + dirs[d][0], nc = c + dirs[d][1];
            while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int target = nr * 8 + nc;
                if (state->board[target] == EMPTY) {
                    tos[count++] = target;
                } else {
                    if ((state->board[target] & COLOR_MASK) != color) tos[count++] = target;
                    break;
                }
                nr += dirs[d][0]; nc += dirs[d][1];
            }
        }
    }
    return count;
}

void make_move(const BoardState *src, BoardState *dst, Move move) {
    *dst = *src;
    int piece = dst->board[move.from];
    int ptype = piece & TYPE_MASK, pcolor = piece & COLOR_MASK;

    if (ptype == PAWN && move.to == dst->en_passant) {
        dst->board[(pcolor == WHITE) ? move.to + 8 : move.to - 8] = EMPTY;
    }

    dst->en_passant = -1;
    if (ptype == PAWN) {
        if (pcolor == WHITE && move.from - move.to == 16) dst->en_passant = move.from - 8;
        else if (pcolor == BLACK && move.to - move.from == 16) dst->en_passant = move.from + 8;
    }

    if (ptype == KING) {
        if (move.from == 60 && move.to == 62 && pcolor == WHITE) { dst->board[61] = dst->board[63]; dst->board[63] = EMPTY; }
        else if (move.from == 60 && move.to == 58 && pcolor == WHITE) { dst->board[59] = dst->board[56]; dst->board[56] = EMPTY; }
        else if (move.from == 4 && move.to == 6 && pcolor == BLACK) { dst->board[5] = dst->board[7]; dst->board[7] = EMPTY; }
        else if (move.from == 4 && move.to == 2 && pcolor == BLACK) { dst->board[3] = dst->board[0]; dst->board[0] = EMPTY; }
        dst->castling &= (pcolor == WHITE) ? ~3 : ~12;
    }

    if (move.from == 56 || move.to == 56) dst->castling &= ~2;
    if (move.from == 63 || move.to == 63) dst->castling &= ~1;
    if (move.from == 0 || move.to == 0) dst->castling &= ~8;
    if (move.from == 7 || move.to == 7) dst->castling &= ~4;

    dst->board[move.to] = move.promotion ? (pcolor | move.promotion) : piece;
    dst->board[move.from] = EMPTY;
    dst->turn = (dst->turn == WHITE) ? BLACK : WHITE;
}

int get_legal_moves(BoardState *state, int from, int *tos) {
    int pseudo[64], p_count = generate_pseudo_moves(state, from, pseudo), l_count = 0;
    for (int i = 0; i < p_count; i++) {
        BoardState next;
        Move m = {from, pseudo[i], 0};
        make_move(state, &next, m);
        if (!is_in_check(&next, state->turn)) tos[l_count++] = pseudo[i];
    }
    return l_count;
}

int has_legal_moves(BoardState *state) {
    for (int from = 0; from < 64; from++) {
        if ((state->board[from] & COLOR_MASK) == state->turn) {
            int tos[64];
            if (get_legal_moves(state, from, tos) > 0) return 1;
        }
    }
    return 0;
}

// Notation Generators
void get_san(BoardState *prev_state, Move move, char *san) {
    int piece = prev_state->board[move.from];
    int type = piece & TYPE_MASK, color = piece & COLOR_MASK;

    if (type == KING) {
        if (move.from == 60 && move.to == 62 && color == WHITE) { strcpy(san, "O-O"); return; }
        if (move.from == 60 && move.to == 58 && color == WHITE) { strcpy(san, "O-O-O"); return; }
        if (move.from == 4 && move.to == 6 && color == BLACK) { strcpy(san, "O-O"); return; }
        if (move.from == 4 && move.to == 2 && color == BLACK) { strcpy(san, "O-O-O"); return; }
    }

    char p_char = '\0';
    if (type == KNIGHT) p_char = 'N';
    else if (type == BISHOP) p_char = 'B';
    else if (type == ROOK) p_char = 'R';
    else if (type == QUEEN) p_char = 'Q';
    else if (type == KING) p_char = 'K';

    char f_file = 'a' + (move.from % 8), f_rank = '8' - (move.from / 8);
    char t_file = 'a' + (move.to % 8), t_rank = '8' - (move.to / 8);
    int is_cap = (prev_state->board[move.to] != EMPTY) || (type == PAWN && move.to == prev_state->en_passant);

    int idx = 0;
    if (type == PAWN) {
        if (is_cap) { san[idx++] = f_file; san[idx++] = 'x'; }
    } else {
        san[idx++] = p_char;
        int amb_file = 0, amb_rank = 0;
        for (int i = 0; i < 64; i++) {
            if (i != move.from && prev_state->board[i] == piece) {
                int tos[64], cnt = get_legal_moves(prev_state, i, tos);
                for (int j = 0; j < cnt; j++) {
                    if (tos[j] == move.to) {
                        if (i % 8 == move.from % 8) amb_rank = 1;
                        else amb_file = 1;
                    }
                }
            }
        }
        if (amb_file) san[idx++] = f_file;
        if (amb_rank) san[idx++] = f_rank;
        if (is_cap) san[idx++] = 'x';
    }
    san[idx++] = t_file; san[idx++] = t_rank;

    if (move.promotion) {
        san[idx++] = '=';
        if (move.promotion == QUEEN) san[idx++] = 'Q';
        else if (move.promotion == ROOK) san[idx++] = 'R';
        else if (move.promotion == BISHOP) san[idx++] = 'B';
        else if (move.promotion == KNIGHT) san[idx++] = 'N';
    }

    BoardState next;
    make_move(prev_state, &next, move);
    if (is_in_check(&next, next.turn)) {
        san[idx++] = has_legal_moves(&next) ? '+' : '#';
    }
    san[idx] = '\0';
}

Move parse_move(BoardState *state, const char *str) {
    Move m = {-1, -1, 0};
    if (strlen(str) < 4) return m;
    int f_col = str[0] - 'a', f_row = '8' - str[1];
    int t_col = str[2] - 'a', t_row = '8' - str[3];
    if (f_col >= 0 && f_col < 8 && f_row >= 0 && f_row < 8 && t_col >= 0 && t_col < 8 && t_row >= 0 && t_row < 8) {
        m.from = f_row * 8 + f_col;
        m.to = t_row * 8 + t_col;
    }
    if (strlen(str) == 5) {
        char p = str[4];
        if (p == 'q') m.promotion = QUEEN;
        else if (p == 'r') m.promotion = ROOK;
        else if (p == 'b') m.promotion = BISHOP;
        else if (p == 'n') m.promotion = KNIGHT;
    }
    return m;
}

void move_to_str(Move m, char *str) {
    sprintf(str, "%c%c%c%c", 'a' + (m.from % 8), '8' - (m.from / 8), 'a' + (m.to % 8), '8' - (m.to / 8));
    if (m.promotion) {
        char p = ' ';
        if (m.promotion == QUEEN) p = 'q';
        else if (m.promotion == ROOK) p = 'r';
        else if (m.promotion == BISHOP) p = 'b';
        else if (m.promotion == KNIGHT) p = 'n';
        sprintf(str + 4, "%c", p);
    }
}

// User Interface and Drawing
void draw_board(BoardState *state, int cursor_idx, int selected_idx, int *legal_moves, int legal_count, Move last_move, Config *config, int history_count, char PGN[2048][20]) {
    printf("\033[H\033[2J"); // Refresh Terminal In-Place
    printf("\n  \033[1;36m=== C-CHESS TERMINAL GUI ===\033[0m\n\n");

    int w_check = is_in_check(state, WHITE), b_check = is_in_check(state, BLACK);

    for (int r = 0; r < 8; r++) {
        printf("  \033[1;30m%d\033[0m ", 8 - r);
        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c, piece = state->board[sq];
            const char *bg = ((r + c) % 2 == 0) ? BG_LIGHT : BG_DARK;

            if ((piece == (WHITE | KING) && w_check) || (piece == (BLACK | KING) && b_check)) bg = BG_CHECK;
            else if (sq == selected_idx) bg = BG_SELECT;
            else {
                for (int i = 0; i < legal_count; i++) {
                    if (legal_moves[i] == sq) { bg = BG_LEGAL; break; }
                }
                if (bg != BG_LEGAL && (sq == last_move.from || sq == last_move.to)) bg = BG_LAST;
            }

            printf("%s", bg);
            printf(sq == cursor_idx ? "\033[1;31m>" : " ");

            if (piece != EMPTY) {
                printf("%s%s", ((piece & COLOR_MASK) == WHITE) ? FG_WHITE : FG_BLACK, get_piece_char(piece));
            } else {
                int is_leg = 0;
                for (int i = 0; i < legal_count; i++) { if (legal_moves[i] == sq) { is_leg = 1; break; } }
                printf(is_leg ? "\033[38;5;34m•" : " ");
            }
            printf("%s%s\033[0m", bg, sq == cursor_idx ? "\033[1;31m<" : " ");
        }
        printf("\n");
    }
    printf("     \033[1;30ma  b  c  d  e  f  g  h\033[0m\n\n");

    printf("  \033[1mTurn\033[0m: %s | \033[1mEngine\033[0m: %s\n",
           state->turn == WHITE ? "\033[1;37mWHITE" : "\033[1;30mBLACK",
           !engine_online ? "\033[1;31mOFFLINE (PVP Mode)" : "\033[1;32mONLINE");

    printf("  \033[1mTime Control\033[0m: ");
    if (config->type == TC_DEPTH) printf("Depth %d\n", config->depth);
    else if (config->type == TC_NODES) printf("Nodes %d\n", config->nodes);
    else printf("Time %d ms\n", config->time_ms);

    printf("\n  \033[1mPGN History\033[0m: ");
    for (int i = 0; i < history_count; i++) {
        if (i % 2 == 0) printf("%d. %s ", (i / 2) + 1, PGN[i]);
        else printf("%s  ", PGN[i]);
        if (i > 0 && i % 8 == 7) printf("\n               ");
    }
    printf("\n\n  [Arrows/WASD]: Move Cursor  | [Space/Enter]: Select/Place\n");
    printf("  [U]: Undo Step               | [C]: Settings  | [Q]: Quit\n\n");
}

void configure_game(Config *config) {
    disable_raw_mode();
    printf("\033[H\033[2J");
    printf("\033[1;33m=== CONFIGURATION MENU ===\033[0m\n\n");
    printf("1. Set Engine Executable Path\n   (Current: %s)\n\n", config->engine_path);
    printf("2. Set Limit Controls\n   (Current: %s)\n\n", config->type == TC_DEPTH ? "Depth" : (config->type == TC_NODES ? "Nodes" : "Time Limit"));
    printf("3. Select Player Color\n   (Current: %s)\n\n", config->player_color == WHITE ? "White" : (config->player_color == BLACK ? "Black" : "PVP Mode"));
    printf("4. Return to game\n\nOption (1-4): ");
    fflush(stdout);

    char line[512];
    if (fgets(line, sizeof(line), stdin)) {
        int opt = atoi(line);
        if (opt == 1) {
            printf("Enter absolute path: ");
            fflush(stdout);
            if (fgets(line, sizeof(line), stdin)) {
                line[strcspn(line, "\n")] = 0;
                if (strlen(line) > 0) strcpy(config->engine_path, line);
            }
        } else if (opt == 2) {
            printf("Select Limiter (1: Depth, 2: Nodes, 3: Time limit MS): ");
            fflush(stdout);
            if (fgets(line, sizeof(line), stdin)) {
                int choice = atoi(line);
                if (choice == 1) {
                    config->type = TC_DEPTH;
                    printf("Depth levels (1-99): "); fflush(stdout);
                    if (fgets(line, sizeof(line), stdin)) config->depth = atoi(line);
                } else if (choice == 2) {
                    config->type = TC_NODES;
                    printf("Node count limit: "); fflush(stdout);
                    if (fgets(line, sizeof(line), stdin)) config->nodes = atoi(line);
                } else if (choice == 3) {
                    config->type = TC_TIME;
                    printf("Calculation duration limit (ms): "); fflush(stdout);
                    if (fgets(line, sizeof(line), stdin)) config->time_ms = atoi(line);
                }
            }
        } else if (opt == 3) {
            printf("Select color profile (1: White, 2: Black, 3: Player Vs Player): ");
            fflush(stdout);
            if (fgets(line, sizeof(line), stdin)) {
                int col = atoi(line);
                if (col == 1) config->player_color = WHITE;
                else if (col == 2) config->player_color = BLACK;
                else config->player_color = 0;
            }
        }
    }
    enable_raw_mode();
}

int get_promotion_piece(void) {
    disable_raw_mode();
    int piece = QUEEN;
    while (1) {
        printf("\nPromote Pawn! Choose Piece (q: Queen, r: Rook, b: Bishop, n: Knight): ");
        fflush(stdout);
        char line[256];
        if (fgets(line, sizeof(line), stdin)) {
            char choice = line[0];
            if (choice == 'q' || choice == 'Q') { piece = QUEEN; break; }
            if (choice == 'r' || choice == 'R') { piece = ROOK; break; }
            if (choice == 'b' || choice == 'B') { piece = BISHOP; break; }
            if (choice == 'n' || choice == 'N') { piece = KNIGHT; break; }
        }
    }
    enable_raw_mode();
    return piece;
}

int get_key(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == '\033') {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                if (seq[0] == '[') {
                    switch (seq[1]) {
                        case 'A': return 'w';
                        case 'B': return 's';
                        case 'C': return 'd';
                        case 'D': return 'a';
                    }
                }
            }
            return '\033';
        }
        return c;
    }
    return 0;
}

int get_engine_move(char *bestmove_str) {
    char buf[1024];
    while (1) {
        if (read_engine(buf, sizeof(buf))) {
            if (strncmp(buf, "bestmove", 8) == 0) {
                sscanf(buf, "bestmove %s", bestmove_str);
                return 1;
            }
        }
        usleep(1000);
    }
    return 0;
}

// Entry Point
int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN); // Ignore crash on faulty engine pipes

    Config config;
    config.type = TC_DEPTH;
    config.depth = 10;
    config.nodes = 100000;
    config.time_ms = 1000;
    config.player_color = WHITE;
    strcpy(config.engine_path, "/opt/homebrew/bin/stockfish"); // Default brew location

    if (argc > 1) strncpy(config.engine_path, argv[1], sizeof(config.engine_path) - 1);

    BoardState state_history[2048];
    char pgn_history[2048][20];
    char uci_history[2048][10];
    int history_count = 0;

    BoardState current_state;
    init_board(&current_state);
    state_history[history_count] = current_state;

    restart_engine(config.engine_path);
    enable_raw_mode();

    int cursor_idx = 60; // Start selection hovering on e1
    int selected_idx = -1;
    int legal_moves[64];
    int legal_count = 0;
    Move last_move = {-1, -1, 0};

    while (1) {
        int checkmate = 0, stalemate = 0;
        if (!has_legal_moves(&current_state)) {
            if (is_in_check(&current_state, current_state.turn)) checkmate = 1;
            else stalemate = 1;
        }

        draw_board(&current_state, cursor_idx, selected_idx, legal_moves, legal_count, last_move, &config, history_count, pgn_history);

        if (checkmate) {
            printf("  \033[1;31m*** CHECKMATE! %s Wins. ***\033[0m\n\n", current_state.turn == WHITE ? "BLACK" : "WHITE");
        } else if (stalemate) {
            printf("  \033[1;33m*** STALEMATE! Draw Game. ***\033[0m\n\n");
        }

        int is_engine_turn = (config.player_color != 0) && (current_state.turn != config.player_color);
        if (is_engine_turn && !checkmate && !stalemate) {
            if (!engine_online) {
                printf("  [ERROR] Engine Offline! Reverting to PVP.\n");
                config.player_color = 0;
                fflush(stdout);
                sleep(2);
                continue;
            }
            printf("  \033[5mEngine is planning strategy...\033[0m\n");
            fflush(stdout);

            char position_cmd[8192] = "position startpos moves";
            for (int i = 0; i < history_count; i++) {
                strcat(position_cmd, " ");
                strcat(position_cmd, uci_history[i]);
            }
            send_engine(position_cmd);

            char go_cmd[128];
            if (config.type == TC_DEPTH) sprintf(go_cmd, "go depth %d", config.depth);
            else if (config.type == TC_NODES) sprintf(go_cmd, "go nodes %d", config.nodes);
            else sprintf(go_cmd, "go movetime %d", config.time_ms);
            send_engine(go_cmd);

            char best_move_str[10];
            if (get_engine_move(best_move_str)) {
                Move m = parse_move(&current_state, best_move_str);
                if (m.from != -1) {
                    get_san(&current_state, m, pgn_history[history_count]);
                    strcpy(uci_history[history_count], best_move_str);

                    BoardState next;
                    make_move(&current_state, &next, m);
                    current_state = next;
                    history_count++;
                    state_history[history_count] = current_state;
                    last_move = m;
                }
            }
            selected_idx = -1;
            legal_count = 0;
            continue;
        }

        int key = get_key();
        if (key == 'q' || key == 'Q') break;
        if (key == 'c' || key == 'C') {
            configure_game(&config);
            restart_engine(config.engine_path);
        } else if (key == 'u' || key == 'U') {
            int takebacks = (config.player_color == 0) ? 1 : 2;
            if (history_count >= takebacks) {
                history_count -= takebacks;
                current_state = state_history[history_count];
                selected_idx = -1;
                legal_count = 0;
                if (history_count > 0) {
                    last_move = parse_move(&state_history[history_count - 1], uci_history[history_count - 1]);
                } else {
                    last_move.from = last_move.to = -1;
                }
            }
        } else if (key == 'w') { // Up
            if (cursor_idx >= 8) cursor_idx -= 8;
        } else if (key == 's') { // Down
            if (cursor_idx < 56) cursor_idx += 8;
        } else if (key == 'a') { // Left
            if (cursor_idx % 8 > 0) cursor_idx -= 1;
        } else if (key == 'd') { // Right
            if (cursor_idx % 8 < 7) cursor_idx += 1;
        } else if (key == ' ' || key == '\r' || key == '\n') {
            if (checkmate || stalemate) continue;

            if (selected_idx == -1) {
                int piece = current_state.board[cursor_idx];
                if (piece != EMPTY && (piece & COLOR_MASK) == current_state.turn) {
                    selected_idx = cursor_idx;
                    legal_count = get_legal_moves(&current_state, selected_idx, legal_moves);
                }
            } else {
                int is_leg = 0;
                for (int i = 0; i < legal_count; i++) {
                    if (legal_moves[i] == cursor_idx) { is_leg = 1; break; }
                }

                if (is_leg) {
                    Move m = {selected_idx, cursor_idx, 0};
                    int piece = current_state.board[selected_idx];
                    if ((piece & TYPE_MASK) == PAWN && (cursor_idx / 8 == 0 || cursor_idx / 8 == 7)) {
                        m.promotion = get_promotion_piece();
                    }

                    char move_str[10];
                    move_to_str(m, move_str);
                    strcpy(uci_history[history_count], move_str);
                    get_san(&current_state, m, pgn_history[history_count]);

                    BoardState next;
                    make_move(&current_state, &next, m);
                    current_state = next;
                    history_count++;
                    state_history[history_count] = current_state;

                    last_move = m;
                    selected_idx = -1;
                    legal_count = 0;
                } else {
                    int clicked_piece = current_state.board[cursor_idx];
                    if (clicked_piece != EMPTY && (clicked_piece & COLOR_MASK) == current_state.turn) {
                        selected_idx = cursor_idx;
                        legal_count = get_legal_moves(&current_state, selected_idx, legal_moves);
                    } else {
                        selected_idx = -1;
                        legal_count = 0;
                    }
                }
            }
        }
    }

    stop_engine();
    return 0;
}
