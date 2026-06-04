#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/types.h>
#include <signal.h>

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
#define PIECE_MASK 7

#define CASTLE_WK 1
#define CASTLE_WQ 2
#define CASTLE_BK 4
#define CASTLE_BQ 8

#define MAX_HISTORY 2048

typedef struct {
    uint8_t squares[64];
    uint8_t side;       // WHITE or BLACK
    uint8_t castle;     // Castling rights flags
    int8_t ep;          // En passant square (-1 if none)
    int halfmove;
    int fullmove;
} Board;

typedef struct {
    uint8_t from;
    uint8_t to;
    uint8_t promo;      // EMPTY, KNIGHT, BISHOP, ROOK, QUEEN
} Move;

typedef struct {
    Board board;
    char move_str[8];   // UCI notation (e.g. "e2e4")
    char pgn_str[16];   // PGN notation (e.g. "Nf3+")
} HistoryEntry;

typedef enum { TC_DEPTH, TC_NODES, TC_TIME } TCType;

typedef struct {
    TCType type;
    int value;
} TimeControl;

typedef enum { MODE_HUMAN_VS_HUMAN, MODE_HUMAN_VS_ENGINE } GameMode;

// Global UI State
HistoryEntry history[MAX_HISTORY];
int history_count = 0;
struct termios orig_termios;
int engine_in[2];
int engine_out[2];
pid_t engine_pid = -1;
int engine_active = 0;

// Unicode Chess Pieces
const char* piece_symbols[] = {
    "   ", " ♟ ", " ♞ ", " ♝ ", " ♜ ", " ♛ ", " ♚ "
};

// Raw Terminal Input Controls
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h\033[0m\n"); // Restore cursor & styles
    fflush(stdout);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 0;  // Non-blocking read
    raw.c_cc[VTIME] = 1; // 100ms timeout
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide terminal cursor
    fflush(stdout);
}

// Chess Rules & Move Generator Code
void init_board(Board *b) {
    memset(b->squares, EMPTY, 64);
    b->squares[0] = BLACK | ROOK; b->squares[7] = BLACK | ROOK;
    b->squares[56] = WHITE | ROOK; b->squares[63] = WHITE | ROOK;
    b->squares[1] = BLACK | KNIGHT; b->squares[6] = BLACK | KNIGHT;
    b->squares[57] = WHITE | KNIGHT; b->squares[62] = WHITE | KNIGHT;
    b->squares[2] = BLACK | BISHOP; b->squares[5] = BLACK | BISHOP;
    b->squares[58] = WHITE | BISHOP; b->squares[61] = WHITE | BISHOP;
    b->squares[3] = BLACK | QUEEN; b->squares[59] = WHITE | QUEEN;
    b->squares[4] = BLACK | KING; b->squares[60] = WHITE | KING;
    for (int i = 8; i < 16; i++) b->squares[i] = BLACK | PAWN;
    for (int i = 48; i < 56; i++) b->squares[i] = WHITE | PAWN;

    b->side = WHITE;
    b->castle = CASTLE_WK | CASTLE_WQ | CASTLE_BK | CASTLE_BQ;
    b->ep = -1;
    b->halfmove = 0;
    b->fullmove = 1;
}

int is_attacked(const Board *b, int sq, int attacker_color) {
    int r = sq / 8, c = sq % 8;
    // Knight attacks
    int kn_dx[] = {-2, -1, 1, 2, 2, 1, -1, -2};
    int kn_dy[] = {1, 2, 2, 1, -1, -2, -2, -1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn_dy[i], nc = c + kn_dx[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int p = b->squares[nr * 8 + nc];
            if ((p & COLOR_MASK) == attacker_color && (p & PIECE_MASK) == KNIGHT) return 1;
        }
    }
    // King attacks
    int k_dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    int k_dy[] = {1, 1, 1, 0, 0, -1, -1, -1};
    for (int i = 0; i < 8; i++) {
        int nr = r + k_dy[i], nc = c + k_dx[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int p = b->squares[nr * 8 + nc];
            if ((p & COLOR_MASK) == attacker_color && (p & PIECE_MASK) == KING) return 1;
        }
    }
    // Pawn attacks
    int p_row = (attacker_color == WHITE) ? r + 1 : r - 1;
    if (p_row >= 0 && p_row < 8) {
        if (c - 1 >= 0) {
            int p = b->squares[p_row * 8 + c - 1];
            if ((p & COLOR_MASK) == attacker_color && (p & PIECE_MASK) == PAWN) return 1;
        }
        if (c + 1 < 8) {
            int p = b->squares[p_row * 8 + c + 1];
            if ((p & COLOR_MASK) == attacker_color && (p & PIECE_MASK) == PAWN) return 1;
        }
    }
    // Sliding Bishop / Queen
    int diag_dx[] = {-1, 1, -1, 1};
    int diag_dy[] = {-1, -1, 1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += diag_dy[i]; nc += diag_dx[i];
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            int p = b->squares[nr * 8 + nc];
            if (p != EMPTY) {
                if ((p & COLOR_MASK) == attacker_color && ((p & PIECE_MASK) == BISHOP || (p & PIECE_MASK) == QUEEN)) return 1;
                break;
            }
        }
    }
    // Sliding Rook / Queen
    int orth_dx[] = {0, 0, -1, 1};
    int orth_dy[] = {-1, 1, 0, 0};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += orth_dy[i]; nc += orth_dx[i];
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            int p = b->squares[nr * 8 + nc];
            if (p != EMPTY) {
                if ((p & COLOR_MASK) == attacker_color && ((p & PIECE_MASK) == ROOK || (p & PIECE_MASK) == QUEEN)) return 1;
                break;
            }
        }
    }
    return 0;
}

int is_in_check(const Board *b, int side) {
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (b->squares[i] == (side | KING)) {
            king_sq = i;
            break;
        }
    }
    if (king_sq == -1) return 0;
    return is_attacked(b, king_sq, side ^ COLOR_MASK);
}

void make_move(const Board *prev, Board *next, Move m) {
    *next = *prev;
    int p = next->squares[m.from];
    int color = p & COLOR_MASK;
    int type = p & PIECE_MASK;

    next->squares[m.to] = next->squares[m.from];
    next->squares[m.from] = EMPTY;

    // En Passant capture
    if (type == PAWN && m.to == prev->ep) {
        int ep_pawn_sq = (color == WHITE) ? m.to + 8 : m.to - 8;
        next->squares[ep_pawn_sq] = EMPTY;
    }

    next->ep = -1;

    // Set potential En Passant target
    if (type == PAWN) {
        if (color == WHITE && m.from >= 48 && m.from <= 55 && m.to == m.from - 16) {
            next->ep = m.from - 8;
        } else if (color == BLACK && m.from >= 8 && m.from <= 15 && m.to == m.from + 16) {
            next->ep = m.from + 8;
        }
    }

    // Castling execution
    if (type == KING) {
        if (color == WHITE) {
            if (m.from == 60 && m.to == 62) {
                next->squares[61] = WHITE | ROOK; next->squares[63] = EMPTY;
            } else if (m.from == 60 && m.to == 58) {
                next->squares[59] = WHITE | ROOK; next->squares[56] = EMPTY;
            }
            next->castle &= ~3;
        } else {
            if (m.from == 4 && m.to == 6) {
                next->squares[5] = BLACK | ROOK; next->squares[7] = EMPTY;
            } else if (m.from == 4 && m.to == 2) {
                next->squares[3] = BLACK | ROOK; next->squares[0] = EMPTY;
            }
            next->castle &= ~12;
        }
    }

    // Revoke castling rights if pieces leave start positions or get captured
    if (m.from == 56) next->castle &= ~2;
    if (m.from == 63) next->castle &= ~1;
    if (m.from == 0)  next->castle &= ~8;
    if (m.from == 7)  next->castle &= ~4;

    if (m.to == 56) next->castle &= ~2;
    if (m.to == 63) next->castle &= ~1;
    if (m.to == 0)  next->castle &= ~8;
    if (m.to == 7)  next->castle &= ~4;

    // Promotion
    if (type == PAWN && m.promo != EMPTY) {
        next->squares[m.to] = color | m.promo;
    }

    if (type == PAWN || prev->squares[m.to] != EMPTY) {
        next->halfmove = 0;
    } else {
        next->halfmove++;
    }

    if (next->side == BLACK) next->fullmove++;
    next->side ^= COLOR_MASK;
}

int generate_moves(const Board *b, Move *moves) {
    int count = 0;
    int side = b->side;
    int opp_side = side ^ COLOR_MASK;

    for (int from = 0; from < 64; from++) {
        int p = b->squares[from];
        if ((p & COLOR_MASK) != side) continue;
        int type = p & PIECE_MASK;
        int r = from / 8, c = from % 8;

        if (type == PAWN) {
            int dir = (side == WHITE) ? -1 : 1;
            int next_row = r + dir;
            if (next_row >= 0 && next_row < 8) {
                int to = next_row * 8 + c;
                if (b->squares[to] == EMPTY) {
                    if (next_row == 0 || next_row == 7) {
                        moves[count++] = (Move){from, to, QUEEN};
                        moves[count++] = (Move){from, to, ROOK};
                        moves[count++] = (Move){from, to, BISHOP};
                        moves[count++] = (Move){from, to, KNIGHT};
                    } else {
                        moves[count++] = (Move){from, to, EMPTY};
                    }
                    int start_row = (side == WHITE) ? 6 : 1;
                    if (r == start_row) {
                        int to2 = (r + 2 * dir) * 8 + c;
                        if (b->squares[to2] == EMPTY) {
                            moves[count++] = (Move){from, to2, EMPTY};
                        }
                    }
                }
            }
            int dcols[] = {-1, 1};
            for (int i = 0; i < 2; i++) {
                int nc = c + dcols[i];
                if (nc >= 0 && nc < 8 && next_row >= 0 && next_row < 8) {
                    int to = next_row * 8 + nc;
                    int target = b->squares[to];
                    if (target != EMPTY && (target & COLOR_MASK) == opp_side) {
                        if (next_row == 0 || next_row == 7) {
                            moves[count++] = (Move){from, to, QUEEN};
                            moves[count++] = (Move){from, to, ROOK};
                            moves[count++] = (Move){from, to, BISHOP};
                            moves[count++] = (Move){from, to, KNIGHT};
                        } else {
                            moves[count++] = (Move){from, to, EMPTY};
                        }
                    } else if (to == b->ep) {
                        moves[count++] = (Move){from, to, EMPTY};
                    }
                }
            }
        } else if (type == KNIGHT) {
            int kn_dx[] = {-2, -1, 1, 2, 2, 1, -1, -2};
            int kn_dy[] = {1, 2, 2, 1, -1, -2, -2, -1};
            for (int i = 0; i < 8; i++) {
                int nr = r + kn_dy[i], nc = c + kn_dx[i];
                if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                    int to = nr * 8 + nc;
                    int target = b->squares[to];
                    if (target == EMPTY || (target & COLOR_MASK) == opp_side) {
                        moves[count++] = (Move){from, to, EMPTY};
                    }
                }
            }
        } else if (type == KING) {
            int k_dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
            int k_dy[] = {1, 1, 1, 0, 0, -1, -1, -1};
            for (int i = 0; i < 8; i++) {
                int nr = r + k_dy[i], nc = c + k_dx[i];
                if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                    int to = nr * 8 + nc;
                    int target = b->squares[to];
                    if (target == EMPTY || (target & COLOR_MASK) == opp_side) {
                        moves[count++] = (Move){from, to, EMPTY};
                    }
                }
            }
            // Castling checks
            if (side == WHITE) {
                if ((b->castle & CASTLE_WK) && b->squares[61] == EMPTY && b->squares[62] == EMPTY) {
                    if (!is_attacked(b, 60, BLACK) && !is_attacked(b, 61, BLACK) && !is_attacked(b, 62, BLACK)) {
                        moves[count++] = (Move){60, 62, EMPTY};
                    }
                }
                if ((b->castle & CASTLE_WQ) && b->squares[59] == EMPTY && b->squares[58] == EMPTY && b->squares[57] == EMPTY) {
                    if (!is_attacked(b, 60, BLACK) && !is_attacked(b, 59, BLACK) && !is_attacked(b, 58, BLACK)) {
                        moves[count++] = (Move){60, 58, EMPTY};
                    }
                }
            } else {
                if ((b->castle & CASTLE_BK) && b->squares[5] == EMPTY && b->squares[6] == EMPTY) {
                    if (!is_attacked(b, 4, WHITE) && !is_attacked(b, 5, WHITE) && !is_attacked(b, 6, WHITE)) {
                        moves[count++] = (Move){4, 6, EMPTY};
                    }
                }
                if ((b->castle & CASTLE_BQ) && b->squares[3] == EMPTY && b->squares[2] == EMPTY && b->squares[1] == EMPTY) {
                    if (!is_attacked(b, 4, WHITE) && !is_attacked(b, 3, WHITE) && !is_attacked(b, 2, WHITE)) {
                        moves[count++] = (Move){4, 2, EMPTY};
                    }
                }
            }
        } else {
            // Sliding Pieces: Queen, Rook, Bishop
            int dirs[8][2];
            int d_count = 0;
            if (type == BISHOP || type == QUEEN) {
                int diag[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
                for (int i = 0; i < 4; i++) { dirs[d_count][0] = diag[i][0]; dirs[d_count][1] = diag[i][1]; d_count++; }
            }
            if (type == ROOK || type == QUEEN) {
                int orth[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (int i = 0; i < 4; i++) { dirs[d_count][0] = orth[i][0]; dirs[d_count][1] = orth[i][1]; d_count++; }
            }
            for (int i = 0; i < d_count; i++) {
                int nr = r, nc = c;
                while (1) {
                    nr += dirs[i][0]; nc += dirs[i][1];
                    if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
                    int to = nr * 8 + nc;
                    int target = b->squares[to];
                    if (target == EMPTY) {
                        moves[count++] = (Move){from, to, EMPTY};
                    } else {
                        if ((target & COLOR_MASK) == opp_side) {
                            moves[count++] = (Move){from, to, EMPTY};
                        }
                        break;
                    }
                }
            }
        }
    }

    // Filter out moves that leave own King in check
    int legal_count = 0;
    for (int i = 0; i < count; i++) {
        Board temp;
        make_move(b, &temp, moves[i]);
        if (!is_in_check(&temp, side)) {
            moves[legal_count++] = moves[i];
        }
    }
    return legal_count;
}

// Notation parsers
void move_to_uci(Move m, char *buf) {
    int f1 = m.from % 8, r1 = 8 - (m.from / 8);
    int f2 = m.to % 8, r2 = 8 - (m.to / 8);
    buf[0] = 'a' + f1; buf[1] = '0' + r1;
    buf[2] = 'a' + f2; buf[3] = '0' + r2;
    if (m.promo != EMPTY) {
        char p_char = 'q';
        if (m.promo == KNIGHT) p_char = 'n';
        if (m.promo == BISHOP) p_char = 'b';
        if (m.promo == ROOK) p_char = 'r';
        buf[4] = p_char; buf[5] = '\0';
    } else {
        buf[4] = '\0';
    }
}

int parse_uci_move(const Board *b, const char *str, Move *m) {
    if (strlen(str) < 4) return 0;
    int f1 = str[0] - 'a'; int r1 = 8 - (str[1] - '0');
    int f2 = str[2] - 'a'; int r2 = 8 - (str[3] - '0');
    if (f1 < 0 || f1 >= 8 || r1 < 0 || r1 >= 8 || f2 < 0 || f2 >= 8 || r2 < 0 || r2 >= 8) return 0;
    m->from = r1 * 8 + f1;
    m->to = r2 * 8 + f2;
    m->promo = EMPTY;
    if (strlen(str) >= 5) {
        char p = str[4];
        if (p == 'q') m->promo = QUEEN;
        else if (p == 'r') m->promo = ROOK;
        else if (p == 'b') m->promo = BISHOP;
        else if (p == 'n') m->promo = KNIGHT;
    }
    Move legal[256];
    int count = generate_moves(b, legal);
    for (int i = 0; i < count; i++) {
        if (legal[i].from == m->from && legal[i].to == m->to) {
            if (m->promo != EMPTY && legal[i].promo != m->promo) continue;
            if (legal[i].promo != EMPTY && m->promo == EMPTY) m->promo = QUEEN;
            return 1;
        }
    }
    return 0;
}

void make_pgn_string(const Board *prev, Move m, char *buf) {
    int p = prev->squares[m.from];
    int type = p & PIECE_MASK;
    int color = p & COLOR_MASK;
    int target = prev->squares[m.to];

    // Castle check
    if (type == KING) {
        if (m.from == 60 && m.to == 62 && color == WHITE) { strcpy(buf, "O-O"); goto check_step; }
        if (m.from == 60 && m.to == 58 && color == WHITE) { strcpy(buf, "O-O-O"); goto check_step; }
        if (m.from == 4 && m.to == 6 && color == BLACK) { strcpy(buf, "O-O"); goto check_step; }
        if (m.from == 4 && m.to == 2 && color == BLACK) { strcpy(buf, "O-O-O"); goto check_step; }
    }

    int len = 0;
    if (type == PAWN) {
        if (target != EMPTY || m.to == prev->ep) {
            buf[len++] = 'a' + (m.from % 8);
            buf[len++] = 'x';
        }
        buf[len++] = 'a' + (m.to % 8);
        buf[len++] = '1' + (7 - (m.to / 8));
        if (m.promo != EMPTY) {
            buf[len++] = '=';
            if (m.promo == QUEEN) buf[len++] = 'Q';
            else if (m.promo == ROOK) buf[len++] = 'R';
            else if (m.promo == BISHOP) buf[len++] = 'B';
            else if (m.promo == KNIGHT) buf[len++] = 'N';
        }
    } else {
        char p_char = ' ';
        if (type == KNIGHT) p_char = 'N';
        else if (type == BISHOP) p_char = 'B';
        else if (type == ROOK) p_char = 'R';
        else if (type == QUEEN) p_char = 'Q';
        else if (type == KING) p_char = 'K';
        buf[len++] = p_char;

        // Disambiguate source files/ranks
        Move legal[256];
        int count = generate_moves(prev, legal);
        int file_conflict = 0, rank_conflict = 0, conflict = 0;
        for (int i = 0; i < count; i++) {
            if (legal[i].to == m.to && legal[i].from != m.from) {
                if (prev->squares[legal[i].from] == p) {
                    conflict = 1;
                    if ((legal[i].from % 8) == (m.from % 8)) file_conflict = 1;
                    if ((legal[i].from / 8) == (m.from / 8)) rank_conflict = 1;
                }
            }
        }
        if (conflict) {
            if (!file_conflict) buf[len++] = 'a' + (m.from % 8);
            else if (!rank_conflict) buf[len++] = '1' + (7 - (m.from / 8));
            else {
                buf[len++] = 'a' + (m.from % 8);
                buf[len++] = '1' + (7 - (m.from / 8));
            }
        }

        if (target != EMPTY) buf[len++] = 'x';
        buf[len++] = 'a' + (m.to % 8);
        buf[len++] = '1' + (7 - (m.to / 8));
    }
    buf[len] = '\0';

check_step:;
    Board next;
    make_move(prev, &next, m);
    int opp_side = color ^ COLOR_MASK;
    if (is_in_check(&next, opp_side)) {
        Move next_moves[256];
        if (generate_moves(&next, next_moves) == 0) {
            strcat(buf, "#");
        } else {
            strcat(buf, "+");
        }
    }
}

// Background Subprocess UCI Pipeline Integration
int start_engine(const char *path) {
    if (pipe(engine_in) < 0 || pipe(engine_out) < 0) return 0;
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(engine_in[0], STDIN_FILENO);
        dup2(engine_out[1], STDOUT_FILENO);
        close(engine_in[0]); close(engine_in[1]);
        close(engine_out[0]); close(engine_out[1]);
        char *args[] = {(char*)path, NULL};
        execvp(path, args);
        exit(1);
    }
    close(engine_in[0]);
    close(engine_out[1]);
    fcntl(engine_out[0], F_SETFL, O_NONBLOCK);
    return 1;
}

void send_to_engine(const char *cmd) {
    if (engine_active) write(engine_in[1], cmd, strlen(cmd));
}

void send_position_to_engine() {
    char cmd[8192] = "position startpos";
    if (history_count > 0) {
        strcat(cmd, " moves");
        for (int i = 0; i < history_count; i++) {
            strcat(cmd, " ");
            strcat(cmd, history[i].move_str);
        }
    }
    strcat(cmd, "\n");
    send_to_engine(cmd);
}

void start_engine_search(TimeControl tc) {
    char cmd[128];
    if (tc.type == TC_DEPTH) sprintf(cmd, "go depth %d\n", tc.value);
    else if (tc.type == TC_NODES) sprintf(cmd, "go nodes %d\n", tc.value);
    else sprintf(cmd, "go movetime %d\n", tc.value);
    send_to_engine(cmd);
}

void push_history(const Board *b, Move m, const char *pgn, const char *uci) {
    if (history_count < MAX_HISTORY) {
        history[history_count].board = *b;
        strcpy(history[history_count].move_str, uci);
        strcpy(history[history_count].pgn_str, pgn);
        history_count++;
    }
}

int undo_move(Board *b, GameMode mode) {
    int pops = (mode == MODE_HUMAN_VS_ENGINE) ? 2 : 1;
    if (history_count >= pops) {
        history_count -= pops;
        if (history_count == 0) init_board(b);
        else *b = history[history_count].board;
        return 1;
    }
    return 0;
}

// Dynamic Graphical Render Logic (ANSI-Escapes)
void draw_board(const Board *b, int cursor_r, int cursor_c, int selected_sq, const Move *legal_moves, int num_legal, Move last_move, int in_check_side) {
    int check_sq = -1;
    if (in_check_side != 0) {
        for (int i = 0; i < 64; i++) {
            if (b->squares[i] == (in_check_side | KING)) {
                check_sq = i;
                break;
            }
        }
    }

    printf("\033[H"); // Push cursor home to redraw cleanly over last frame
    printf("    a  b  c  d  e  f  g  h\n");
    printf("  ┌────────────────────────┐\n");
    for (int r = 0; r < 8; r++) {
        printf("%d │", 8 - r);
        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            int p = b->squares[sq];
            int p_type = p & PIECE_MASK;
            int p_color = p & COLOR_MASK;

            // Background coloring
            int bg = ((r + c) % 2 == 0) ? 250 : 242; // Light & dark standard squares

            if (sq == selected_sq) bg = 142; // Greenish-Olive for selected piece

            int is_legal = 0;
            for (int i = 0; i < num_legal; i++) {
                if (legal_moves[i].from == selected_sq && legal_moves[i].to == sq) {
                    is_legal = 1; break;
                }
            }
            if (is_legal) {
                bg = ((r + c) % 2 == 0) ? 114 : 108; // Soft emerald for target legal moves
            }

            if (last_move.from != last_move.to && (sq == last_move.from || sq == last_move.to)) {
                bg = 110; // Blue for highlights on last made moves
            }

            if (sq == check_sq) bg = 167;        // Red alert for King in check!
            if (r == cursor_r && c == cursor_c) bg = 125; // Magenta for user keyboard cursor

            int fg = (p_color == BLACK) ? 16 : 231; // Contrasting pieces (White vs Black)

            printf("\033[38;5;%dm\033[48;5;%dm", fg, bg);
            if (p_type != EMPTY) printf("%s", piece_symbols[p_type]);
            else if (is_legal) printf(" • ");
            else printf("   ");
        }
        printf("\033[0m│ %d\n", 8 - r);
    }
    printf("  └────────────────────────┘\n");
    printf("    a  b  c  d  e  f  g  h\n\n");
}

void print_recent_pgn(int max_pairs) {
    int total_pairs = (history_count + 1) / 2;
    int start_pair = total_pairs > max_pairs ? total_pairs - max_pairs : 0;
    for (int p = start_pair; p < total_pairs; p++) {
        int idx = p * 2;
        printf("%d. %s ", p + 1, history[idx].pgn_str);
        if (idx + 1 < history_count) {
            printf("%s  ", history[idx+1].pgn_str);
        }
    }
    printf("\n");
}

int get_promotion_choice() {
    printf("\nPromote Pawn! Select Option: [q] Queen, [r] Rook, [b] Bishop, [n] Knight: ");
    fflush(stdout);
    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) > 0) {
            if (c == 'q' || c == 'Q') return QUEEN;
            if (c == 'r' || c == 'R') return ROOK;
            if (c == 'b' || c == 'B') return BISHOP;
            if (c == 'n' || c == 'N') return KNIGHT;
        }
        usleep(10000);
    }
}

int main(int argc, char **argv) {
    char engine_path[512] = "stockfish"; // Tries standard $PATH first
    if (argc > 1) strncpy(engine_path, argv[1], 511);

    engine_active = start_engine(engine_path);
    if (engine_active) {
        send_to_engine("uci\n");
        send_to_engine("isready\n");
        char init_line[1024];
        int attempts = 0;
        while (attempts++ < 300) {
            int len = read(engine_out[0], init_line, 1024);
            if (len > 0 && strstr(init_line, "readyok")) break;
            usleep(10000);
        }
    }

    enable_raw_mode();

    Board board;
    init_board(&board);

    int cursor_r = 6, cursor_c = 4; // Primary selection cursor
    int selected_sq = -1;
    Move legal_moves[256];
    int num_legal = 0;
    Move last_move = {0, 0, 0};
    int engine_thinking = 0;
    int human_color = WHITE;
    GameMode mode = engine_active ? MODE_HUMAN_VS_ENGINE : MODE_HUMAN_VS_HUMAN;
    TimeControl tc = { TC_DEPTH, 8 };

    int running = 1, need_redraw = 1;

    // Game Event Loop
    while (running) {
        if (need_redraw) {
            int in_check_side = 0;
            if (is_in_check(&board, WHITE)) in_check_side = WHITE;
            else if (is_in_check(&board, BLACK)) in_check_side = BLACK;

            printf("\033[2J"); // Wipe current canvas frame
            draw_board(&board, cursor_r, cursor_c, selected_sq, legal_moves, num_legal, last_move, in_check_side);

            printf("Active Turn:  %s %s\n", (board.side == WHITE) ? "WHITE ⚪" : "BLACK ⚫", in_check_side ? "\033[1;31m[CHECK]\033[0m" : "");
            printf("Game Mode:    %s\n", (mode == MODE_HUMAN_VS_ENGINE) ? "Human vs Engine" : "Human vs Human");
            printf("Engine:       %s\n", engine_active ? "Connected" : "Disconnected (Running local pass-and-play)");
            
            printf("Time Control: ");
            if (tc.type == TC_DEPTH) printf("[Depth Limit: %d]\n", tc.value);
            else if (tc.type == TC_NODES) printf("[Node Limit: %d]\n", tc.value);
            else printf("[Move Time Limit: %d ms]\n", tc.value);

            printf("\nPGN Move Log:\n");
            print_recent_pgn(8);

            printf("\nControls:\n");
            printf("  WASD / Arrows : Move cursor\n");
            printf("  Space / Enter : Select / Move piece\n");
            printf("  U             : Undo (take back)\n");
            if (engine_active) printf("  E             : Request immediate Engine Move\n");
            printf("  M             : Toggle Game Mode (Human/Engine)\n");
            printf("  T             : Cycle Time Control Types\n");
            printf("  + / -         : Increase / Decrease Time Control constraints\n");
            printf("  Q             : Quit Game\n");

            fflush(stdout);
            need_redraw = 0;
        }

        // Asynchronous non-blocking evaluation of engine replies
        if (engine_active && engine_thinking) {
            char ch;
            static char line[2048];
            static int line_idx = 0;
            while (read(engine_out[0], &ch, 1) > 0) {
                if (ch == '\n' || line_idx >= 2047) {
                    line[line_idx] = '\0';
                    if (strncmp(line, "bestmove ", 9) == 0) {
                        char move_str[16];
                        sscanf(line, "bestmove %s", move_str);
                        if (strcmp(move_str, "(none)") != 0) {
                            Move m;
                            if (parse_uci_move(&board, move_str, &m)) {
                                char pgn[16];
                                make_pgn_string(&board, m, pgn);
                                push_history(&board, m, pgn, move_str);
                                make_move(&board, &board, m);
                                last_move = m;
                            }
                        }
                        engine_thinking = 0; need_redraw = 1; break;
                    }
                    line_idx = 0;
                } else {
                    line[line_idx++] = ch;
                }
            }
        }

        // Automatically trigger engine turn
        if (mode == MODE_HUMAN_VS_ENGINE && board.side != human_color && !engine_thinking && engine_active) {
            engine_thinking = 1;
            send_position_to_engine();
            start_engine_search(tc);
        }

        // Handle Keyboard Events
        char c;
        int bytes = read(STDIN_FILENO, &c, 1);
        if (bytes > 0) {
            if (c == '\033') { // Process Arrow Escapes
                char seq[3];
                if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                    if (seq[0] == '[') {
                        switch (seq[1]) {
                            case 'A': if (cursor_r > 0) cursor_r--; need_redraw = 1; break;
                            case 'B': if (cursor_r < 7) cursor_r++; need_redraw = 1; break;
                            case 'C': if (cursor_c < 7) cursor_c++; need_redraw = 1; break;
                            case 'D': if (cursor_c > 0) cursor_c--; need_redraw = 1; break;
                        }
                    }
                }
            } else {
                switch (c) {
                    case 'q': case 'Q': running = 0; break;
                    case 'w': case 'W': if (cursor_r > 0) cursor_r--; need_redraw = 1; break;
                    case 's': case 'S': if (cursor_r < 7) cursor_r++; need_redraw = 1; break;
                    case 'a': case 'A': if (cursor_c > 0) cursor_c--; need_redraw = 1; break;
                    case 'd': case 'D': if (cursor_c < 7) cursor_c++; need_redraw = 1; break;
                    case 'u': case 'U':
                        if (undo_move(&board, mode)) {
                            if (history_count > 0) {
                                parse_uci_move(&history[history_count - 1].board, history[history_count - 1].move_str, &last_move);
                            } else {
                                last_move = (Move){0,0,0};
                            }
                            selected_sq = -1; num_legal = 0; need_redraw = 1;
                        }
                        break;
                    case 'e': case 'E':
                        if (engine_active && !engine_thinking) {
                            engine_thinking = 1;
                            send_position_to_engine();
                            start_engine_search(tc);
                        }
                        break;
                    case 'm': case 'M':
                        mode = (mode == MODE_HUMAN_VS_ENGINE) ? MODE_HUMAN_VS_HUMAN : MODE_HUMAN_VS_ENGINE;
                        if (mode == MODE_HUMAN_VS_ENGINE) human_color = board.side;
                        need_redraw = 1; break;
                    case 't': case 'T':
                        if (tc.type == TC_DEPTH) { tc.type = TC_NODES; tc.value = 100000; }
                        else if (tc.type == TC_NODES) { tc.type = TC_TIME; tc.value = 2000; }
                        else { tc.type = TC_DEPTH; tc.value = 8; }
                        need_redraw = 1; break;
                    case '+': case '=':
                        if (tc.type == TC_DEPTH) { if (tc.value < 30) tc.value++; }
                        else if (tc.type == TC_NODES) tc.value += 10000;
                        else tc.value += 500;
                        need_redraw = 1; break;
                    case '-': case '_':
                        if (tc.type == TC_DEPTH) { if (tc.value > 1) tc.value--; }
                        else if (tc.type == TC_NODES) { if (tc.value > 10000) tc.value -= 10000; }
                        else { if (tc.value > 500) tc.value -= 500; }
                        need_redraw = 1; break;
                    case ' ': case '\n': {
                        int sq = cursor_r * 8 + cursor_c;
                        if (selected_sq == -1) {
                            int p = board.squares[sq];
                            if (p != EMPTY && (p & COLOR_MASK) == board.side) {
                                selected_sq = sq;
                                num_legal = generate_moves(&board, legal_moves);
                                need_redraw = 1;
                            }
                        } else {
                            int found_idx = -1;
                            for (int i = 0; i < num_legal; i++) {
                                if (legal_moves[i].from == selected_sq && legal_moves[i].to == sq) {
                                    found_idx = i; break;
                                }
                            }
                            if (found_idx != -1) {
                                Move chosen = legal_moves[found_idx];
                                int piece_type = board.squares[chosen.from] & PIECE_MASK;
                                if (piece_type == PAWN && (chosen.to / 8 == 0 || chosen.to / 8 == 7)) {
                                    chosen.promo = get_promotion_choice();
                                }
                                char uci_str[16], pgn_str[16];
                                move_to_uci(chosen, uci_str);
                                make_pgn_string(&board, chosen, pgn_str);
                                push_history(&board, chosen, pgn_str, uci_str);
                                make_move(&board, &board, chosen);
                                last_move = chosen;
                                selected_sq = -1; num_legal = 0; need_redraw = 1;
                            } else {
                                int p = board.squares[sq];
                                if (p != EMPTY && (p & COLOR_MASK) == board.side) {
                                    selected_sq = sq;
                                    num_legal = generate_moves(&board, legal_moves);
                                } else {
                                    selected_sq = -1; num_legal = 0;
                                }
                                need_redraw = 1;
                            }
                        }
                        break;
                    }
                }
            }
        }
        usleep(10000); // 10ms loop throttle to keep CPU idle low
    }

    if (engine_pid > 0) kill(engine_pid, SIGTERM);
    return 0;
}
