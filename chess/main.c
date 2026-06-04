#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdint.h>

#define WHITE 0
#define BLACK 1

// Piece definitions
enum { EMPTY = 0, WP, WN, WB, WR, WQ, WK, BP, BN, BB, BR, BQ, BK };

// Game modes
#define MODE_PVP 0
#define MODE_PVE 1

// Time Control options
#define TC_DEPTH 0
#define TC_NODES 1
#define TC_TIME  2

// Representation of a board move
typedef struct {
    int from;
    int to;
    uint8_t promote;
} Move;

// History tracking for UNDO and PGN
typedef struct {
    uint8_t board[64];
    uint8_t side;
    uint8_t castle;
    int ep;
    int halfmove;
    int fullmove;
} BoardState;

typedef struct {
    BoardState state;
    Move move;
    char san[16];
} HistoryEntry;

// Board representation
typedef struct {
    uint8_t board[64];
    uint8_t side;
    uint8_t castle; // bits: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep;         // EP target square (0-63, -1 if none)
    int halfmove;
    int fullmove;
} Board;

typedef struct {
    Move moves[256];
    int count;
} MoveList;

// Global State
Board board;
MoveList current_legal_moves;
HistoryEntry history[2048];
int history_count = 0;

int cursor_x = 4, cursor_y = 6;
int selected_x = -1, selected_y = -1;

int game_mode = MODE_PVP;
int engine_side = BLACK;
char engine_path[256] = "stockfish";
int time_control_type = TC_DEPTH;
int tc_depth = 10;
int tc_nodes = 100000;
int tc_time_ms = 1000;

// Subprocess control for UCI Engines
int engine_in[2] = {-1, -1};
int engine_out[2] = {-1, -1};
pid_t engine_pid = -1;
int engine_searching = 0;
char engine_read_buf[4096];
int engine_read_pos = 0;

struct termios orig_termios;

// ANSI colors
#define COLOR_LIGHT_SQ 223
#define COLOR_DARK_SQ  95
#define COLOR_CURSOR   205
#define COLOR_SELECTED 33
#define COLOR_LEGAL    120
#define COLOR_LAST_MV  186
#define COLOR_CHECK    196

// Clean terminal helpers
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h\n"); // Show cursor
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

// Subprocess execution & Piping
void stop_engine() {
    if (engine_pid > 0) {
        kill(engine_pid, SIGTERM);
        waitpid(engine_pid, NULL, 0);
        engine_pid = -1;
    }
}

int start_engine(const char *path) {
    stop_engine();
    if (pipe(engine_in) < 0 || pipe(engine_out) < 0) return 0;

    engine_pid = fork();
    if (engine_pid < 0) return 0;

    if (engine_pid == 0) {
        dup2(engine_in[0], STDIN_FILENO);
        dup2(engine_out[1], STDOUT_FILENO);
        close(engine_in[0]); close(engine_in[1]);
        close(engine_out[0]); close(engine_out[1]);

        char *args[] = {(char *)path, NULL};
        execvp(path, args);
        exit(1);
    }

    close(engine_in[0]);
    close(engine_out[1]);

    int flags = fcntl(engine_out[0], F_GETFL, 0);
    fcntl(engine_out[0], F_SETFL, flags | O_NONBLOCK);

    // Initialize UCI Protocol
    write(engine_in[1], "uci\nisready\n", 12);
    return 1;
}

void write_engine(const char *cmd) {
    if (engine_pid <= 0) return;
    write(engine_in[1], cmd, strlen(cmd));
}

int get_engine_line_buffered(char *line, int max_len) {
    char ch;
    while (read(engine_out[0], &ch, 1) > 0) {
        if (ch == '\n') {
            engine_read_buf[engine_read_pos] = '\0';
            strncpy(line, engine_read_buf, max_len);
            engine_read_pos = 0;
            return 1;
        } else if (ch != '\r') {
            if (engine_read_pos < (int)sizeof(engine_read_buf) - 1) {
                engine_read_buf[engine_read_pos++] = ch;
            }
        }
    }
    return 0;
}

// FEN generator for sharing positions with the Engine
void board_to_fen(const Board *b, char *fen) {
    int idx = 0;
    for (int r = 0; r < 8; r++) {
        int empty = 0;
        for (int c = 0; c < 8; c++) {
            uint8_t p = b->board[r * 8 + c];
            if (p == EMPTY) {
                empty++;
            } else {
                if (empty > 0) {
                    idx += sprintf(fen + idx, "%d", empty);
                    empty = 0;
                }
                char pc = 0;
                switch (p) {
                    case WP: pc = 'P'; break; case WN: pc = 'N'; break; case WB: pc = 'B'; break;
                    case WR: pc = 'R'; break; case WQ: pc = 'Q'; break; case WK: pc = 'K'; break;
                    case BP: pc = 'p'; break; case BN: pc = 'n'; break; case BB: pc = 'b'; break;
                    case BR: pc = 'r'; break; case BQ: pc = 'q'; break; case BK: pc = 'k'; break;
                }
                fen[idx++] = pc;
            }
        }
        if (empty > 0) idx += sprintf(fen + idx, "%d", empty);
        if (r < 7) fen[idx++] = '/';
    }

    idx += sprintf(fen + idx, " %c", b->side == WHITE ? 'w' : 'b');

    idx += sprintf(fen + idx, " ");
    int has_castle = 0;
    if (b->castle & 1) { fen[idx++] = 'K'; has_castle = 1; }
    if (b->castle & 2) { fen[idx++] = 'Q'; has_castle = 1; }
    if (b->castle & 4) { fen[idx++] = 'k'; has_castle = 1; }
    if (b->castle & 8) { fen[idx++] = 'q'; has_castle = 1; }
    if (!has_castle) fen[idx++] = '-';

    if (b->ep != -1) {
        idx += sprintf(fen + idx, " %c%c", 'a' + (b->ep % 8), '1' + (7 - (b->ep / 8)));
    } else {
        idx += sprintf(fen + idx, " -");
    }

    idx += sprintf(fen + idx, " %d %d", b->halfmove, b->fullmove);
    fen[idx] = '\0';
}

// Rules and Move Generator Engines
int get_piece_color(uint8_t p) {
    if (p >= WP && p <= WK) return WHITE;
    if (p >= BP && p <= BK) return BLACK;
    return -1;
}

int is_square_attacked(const Board *b, int sq, int attacker_side) {
    int r = sq / 8, c = sq % 8;
    // Knight
    int kn_dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int kn_dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn_dr[i], nc = c + kn_dc[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            uint8_t p = b->board[nr * 8 + nc];
            if (attacker_side == WHITE && p == WN) return 1;
            if (attacker_side == BLACK && p == BN) return 1;
        }
    }
    // King
    int k_dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int k_dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + k_dr[i], nc = c + k_dc[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            uint8_t p = b->board[nr * 8 + nc];
            if (attacker_side == WHITE && p == WK) return 1;
            if (attacker_side == BLACK && p == BK) return 1;
        }
    }
    // Pawns
    if (attacker_side == WHITE) {
        int nr = r + 1;
        for (int dc = -1; dc <= 1; dc += 2) {
            int nc = c + dc;
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8 && b->board[nr * 8 + nc] == WP) return 1;
        }
    } else {
        int nr = r - 1;
        for (int dc = -1; dc <= 1; dc += 2) {
            int nc = c + dc;
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8 && b->board[nr * 8 + nc] == BP) return 1;
        }
    }
    // Sliders
    int dirs[8][2] = {{-1,0}, {1,0}, {0,-1}, {0,1}, {-1,-1}, {-1,1}, {1,-1}, {1,1}};
    for (int d = 0; d < 8; d++) {
        int dr = dirs[d][0], dc = dirs[d][1];
        int nr = r, nc = c;
        while (1) {
            nr += dr; nc += dc;
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            uint8_t p = b->board[nr * 8 + nc];
            if (p != EMPTY) {
                int col = get_piece_color(p);
                if (col == attacker_side) {
                    if (d < 4) {
                        if (p == WR || p == WQ || p == BR || p == BQ) return 1;
                    } else {
                        if (p == WB || p == WQ || p == BB || p == BQ) return 1;
                    }
                }
                break;
            }
        }
    }
    return 0;
}

int is_in_check(const Board *b, int side) {
    int king_piece = (side == WHITE) ? WK : BK;
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (b->board[i] == king_piece) { king_sq = i; break; }
    }
    if (king_sq == -1) return 0;
    return is_square_attacked(b, king_sq, side ^ 1);
}

void add_move(MoveList *list, int from, int to, uint8_t promote) {
    list->moves[list->count++] = (Move){from, to, promote};
}

void generate_pseudo_moves(const Board *b, MoveList *list) {
    list->count = 0;
    int side = b->side;
    for (int from = 0; from < 64; from++) {
        uint8_t p = b->board[from];
        if (p == EMPTY || get_piece_color(p) != side) continue;

        int r = from / 8, c = from % 8;
        if (p == WP) {
            int to = from - 8;
            if (to >= 0 && b->board[to] == EMPTY) {
                if (to / 8 == 0) {
                    add_move(list, from, to, WQ); add_move(list, from, to, WR);
                    add_move(list, from, to, WB); add_move(list, from, to, WN);
                } else {
                    add_move(list, from, to, 0);
                }
                if (r == 6 && b->board[from - 16] == EMPTY) add_move(list, from, from - 16, 0);
            }
            int caps[] = {-9, -7};
            for (int i = 0; i < 2; i++) {
                int to = from + caps[i];
                if (to >= 0 && to < 64 && abs((to % 8) - c) == 1) {
                    if (b->board[to] != EMPTY && get_piece_color(b->board[to]) == BLACK) {
                        if (to / 8 == 0) {
                            add_move(list, from, to, WQ); add_move(list, from, to, WR);
                            add_move(list, from, to, WB); add_move(list, from, to, WN);
                        } else {
                            add_move(list, from, to, 0);
                        }
                    } else if (to == b->ep) {
                        add_move(list, from, to, 0);
                    }
                }
            }
        } else if (p == BP) {
            int to = from + 8;
            if (to < 64 && b->board[to] == EMPTY) {
                if (to / 8 == 7) {
                    add_move(list, from, to, BQ); add_move(list, from, to, BR);
                    add_move(list, from, to, BB); add_move(list, from, to, BN);
                } else {
                    add_move(list, from, to, 0);
                }
                if (r == 1 && b->board[from + 16] == EMPTY) add_move(list, from, from + 16, 0);
            }
            int caps[] = {7, 9};
            for (int i = 0; i < 2; i++) {
                int to = from + caps[i];
                if (to >= 0 && to < 64 && abs((to % 8) - c) == 1) {
                    if (b->board[to] != EMPTY && get_piece_color(b->board[to]) == WHITE) {
                        if (to / 8 == 7) {
                            add_move(list, from, to, BQ); add_move(list, from, to, BR);
                            add_move(list, from, to, BB); add_move(list, from, to, BN);
                        } else {
                            add_move(list, from, to, 0);
                        }
                    } else if (to == b->ep) {
                        add_move(list, from, to, 0);
                    }
                }
            }
        } else if (p == WN || p == BN) {
            int dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
            int dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
            for (int i = 0; i < 8; i++) {
                int nr = r + dr[i], nc = c + dc[i];
                if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                    int to = nr * 8 + nc;
                    if (b->board[to] == EMPTY || get_piece_color(b->board[to]) != side) add_move(list, from, to, 0);
                }
            }
        } else if (p == WK || p == BK) {
            int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
            int dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
            for (int i = 0; i < 8; i++) {
                int nr = r + dr[i], nc = c + dc[i];
                if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                    int to = nr * 8 + nc;
                    if (b->board[to] == EMPTY || get_piece_color(b->board[to]) != side) add_move(list, from, to, 0);
                }
            }
            if (side == WHITE) {
                if ((b->castle & 1) && b->board[61] == EMPTY && b->board[62] == EMPTY) add_move(list, 60, 62, 0);
                if ((b->castle & 2) && b->board[59] == EMPTY && b->board[58] == EMPTY && b->board[57] == EMPTY) add_move(list, 60, 58, 0);
            } else {
                if ((b->castle & 4) && b->board[5] == EMPTY && b->board[6] == EMPTY) add_move(list, 4, 6, 0);
                if ((b->castle & 8) && b->board[3] == EMPTY && b->board[2] == EMPTY && b->board[1] == EMPTY) add_move(list, 4, 2, 0);
            }
        } else { // Sliders (R, B, Q)
            int is_rook = (p == WR || p == BR || p == WQ || p == BQ);
            int is_bishop = (p == WB || p == BB || p == WQ || p == BQ);
            int d_count = 0;
            int dirs[8][2];
            if (is_rook) {
                dirs[d_count][0] = -1; dirs[d_count][1] = 0; d_count++;
                dirs[d_count][0] = 1;  dirs[d_count][1] = 0; d_count++;
                dirs[d_count][0] = 0;  dirs[d_count][1] = -1; d_count++;
                dirs[d_count][0] = 0;  dirs[d_count][1] = 1; d_count++;
            }
            if (is_bishop) {
                dirs[d_count][0] = -1; dirs[d_count][1] = -1; d_count++;
                dirs[d_count][0] = -1; dirs[d_count][1] = 1; d_count++;
                dirs[d_count][0] = 1;  dirs[d_count][1] = -1; d_count++;
                dirs[d_count][0] = 1;  dirs[d_count][1] = 1; d_count++;
            }
            for (int d = 0; d < d_count; d++) {
                int dr = dirs[d][0], dc = dirs[d][1];
                int nr = r, nc = c;
                while (1) {
                    nr += dr; nc += dc;
                    if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
                    int to = nr * 8 + nc;
                    if (b->board[to] == EMPTY) {
                        add_move(list, from, to, 0);
                    } else {
                        if (get_piece_color(b->board[to]) != side) add_move(list, from, to, 0);
                        break;
                    }
                }
            }
        }
    }
}

void make_move(const Board *src, Board *dst, Move m) {
    *dst = *src;
    uint8_t p = dst->board[m.from];
    dst->board[m.from] = EMPTY;

    if ((p == WP || p == BP) && m.to == dst->ep) {
        if (p == WP) dst->board[m.to + 8] = EMPTY;
        else dst->board[m.to - 8] = EMPTY;
    }

    dst->ep = -1;
    if (p == WP && m.from - m.to == 16) dst->ep = m.from - 8;
    else if (p == BP && m.to - m.from == 16) dst->ep = m.from + 8;

    if (p == WK && m.from == 60) {
        if (m.to == 62) { dst->board[61] = WR; dst->board[63] = EMPTY; }
        else if (m.to == 58) { dst->board[59] = WR; dst->board[56] = EMPTY; }
    } else if (p == BK && m.from == 4) {
        if (m.to == 6) { dst->board[5] = BR; dst->board[7] = EMPTY; }
        else if (m.to == 2) { dst->board[3] = BR; dst->board[0] = EMPTY; }
    }

    if (m.from == 60) dst->castle &= ~3;
    if (m.from == 4)  dst->castle &= ~12;
    if (m.from == 56 || m.to == 56) dst->castle &= ~2;
    if (m.from == 63 || m.to == 63) dst->castle &= ~1;
    if (m.from == 0 || m.to == 0)   dst->castle &= ~8;
    if (m.from == 7 || m.to == 7)   dst->castle &= ~4;

    if (m.promote != 0) dst->board[m.to] = m.promote;
    else dst->board[m.to] = p;

    dst->side ^= 1;
    if (p == WP || p == BP || src->board[m.to] != EMPTY) {
        dst->halfmove = 0;
    } else {
        dst->halfmove++;
    }
    if (dst->side == WHITE) dst->fullmove++;
}

void generate_legal_moves(const Board *b, MoveList *list) {
    MoveList pseudo;
    generate_pseudo_moves(b, &pseudo);
    list->count = 0;
    for (int i = 0; i < pseudo.count; i++) {
        Move m = pseudo.moves[i];
        uint8_t p = b->board[m.from];
        if (p == WK && m.from == 60) {
            if (m.to == 62 && (is_in_check(b, WHITE) || is_square_attacked(b, 61, BLACK) || is_square_attacked(b, 62, BLACK))) continue;
            if (m.to == 58 && (is_in_check(b, WHITE) || is_square_attacked(b, 59, BLACK) || is_square_attacked(b, 58, BLACK))) continue;
        } else if (p == BK && m.from == 4) {
            if (m.to == 6 && (is_in_check(b, BLACK) || is_square_attacked(b, 5, WHITE) || is_square_attacked(b, 6, WHITE))) continue;
            if (m.to == 2 && (is_in_check(b, BLACK) || is_square_attacked(b, 3, WHITE) || is_square_attacked(b, 2, WHITE))) continue;
        }
        Board next;
        make_move(b, &next, m);
        if (!is_in_check(&next, b->side)) {
            list->moves[list->count++] = m;
        }
    }
}

// Generates Standard Algebraic Notation (SAN) for moves
void get_move_san(const Board *b, Move m, char *san) {
    uint8_t p = b->board[m.from];
    int to_col = m.to % 8, to_row = m.to / 8;
    int from_col = m.from % 8, from_row = m.from / 8;

    if (p == WK && m.from == 60) {
        if (m.to == 62) { strcpy(san, "O-O"); return; }
        if (m.to == 58) { strcpy(san, "O-O-O"); return; }
    }
    if (p == BK && m.from == 4) {
        if (m.to == 6) { strcpy(san, "O-O"); return; }
        if (m.to == 2) { strcpy(san, "O-O-O"); return; }
    }

    char piece_char = 0;
    if (p == WN || p == BN) piece_char = 'N';
    else if (p == WB || p == BB) piece_char = 'B';
    else if (p == WR || p == BR) piece_char = 'R';
    else if (p == WQ || p == BQ) piece_char = 'Q';
    else if (p == WK || p == BK) piece_char = 'K';

    int capture = (b->board[m.to] != EMPTY) || ((p == WP || p == BP) && m.to == b->ep);
    int idx = 0;

    if (piece_char) {
        san[idx++] = piece_char;
        MoveList ml;
        generate_legal_moves(b, &ml);
        int dup_file = 0, dup_rank = 0, dup = 0;
        for (int i = 0; i < ml.count; i++) {
            Move tm = ml.moves[i];
            if (tm.from != m.from && tm.to == m.to && b->board[tm.from] == p) {
                dup = 1;
                if (tm.from % 8 == from_col) dup_rank = 1;
                else dup_file = 1;
            }
        }
        if (dup) {
            if (dup_file || !dup_rank) san[idx++] = 'a' + from_col;
            if (dup_rank) san[idx++] = '1' + (7 - from_row);
        }
    } else if (capture) {
        san[idx++] = 'a' + from_col;
    }

    if (capture) san[idx++] = 'x';
    san[idx++] = 'a' + to_col;
    san[idx++] = '1' + (7 - to_row);

    if (m.promote) {
        san[idx++] = '=';
        if (m.promote == WQ || m.promote == BQ) san[idx++] = 'Q';
        else if (m.promote == WR || m.promote == BR) san[idx++] = 'R';
        else if (m.promote == WB || m.promote == BB) san[idx++] = 'B';
        else if (m.promote == WN || m.promote == BN) san[idx++] = 'N';
    }

    Board next;
    make_move(b, &next, m);
    if (is_in_check(&next, next.side)) {
        MoveList next_ml;
        generate_legal_moves(&next, &next_ml);
        san[idx++] = (next_ml.count == 0) ? '#' : '+';
    }
    san[idx] = '\0';
}

void trigger_engine_move() {
    if (engine_pid <= 0) return;
    char fen[256];
    board_to_fen(&board, fen);

    char pos_cmd[512];
    snprintf(pos_cmd, sizeof(pos_cmd), "position fen %s\n", fen);
    write_engine(pos_cmd);

    char go_cmd[128];
    if (time_control_type == TC_DEPTH) {
        snprintf(go_cmd, sizeof(go_cmd), "go depth %d\n", tc_depth);
    } else if (time_control_type == TC_NODES) {
        snprintf(go_cmd, sizeof(go_cmd), "go nodes %d\n", tc_nodes);
    } else {
        snprintf(go_cmd, sizeof(go_cmd), "go movetime %d\n", tc_time_ms);
    }
    write_engine(go_cmd);
    engine_searching = 1;
}

// State initialization
void init_board(Board *b) {
    memset(b, 0, sizeof(Board));
    uint8_t row0[] = {BR, BN, BB, BQ, BK, BB, BN, BR};
    uint8_t row7[] = {WR, WN, WB, WQ, WK, WB, WN, WR};
    for (int i = 0; i < 8; i++) {
        b->board[i] = row0[i];
        b->board[8 + i] = BP;
        b->board[48 + i] = WP;
        b->board[56 + i] = row7[i];
    }
    b->side = WHITE;
    b->castle = 15;
    b->ep = -1;
    b->halfmove = 0;
    b->fullmove = 1;
}

// Match square targets with legal selections
int is_legal_dest(int r, int c) {
    if (selected_x == -1) return 0;
    int from = selected_y * 8 + selected_x;
    int to = r * 8 + c;
    for (int i = 0; i < current_legal_moves.count; i++) {
        if (current_legal_moves.moves[i].from == from && current_legal_moves.moves[i].to == to) return 1;
    }
    return 0;
}

int is_king_in_check_sq(int r, int c) {
    int idx = r * 8 + c;
    int side = board.side;
    int king_piece = (side == WHITE) ? WK : BK;
    return (board.board[idx] == king_piece && is_in_check(&board, side));
}

// Modern Terminal Unicode Chess Set
const char* piece_glyphs[] = {
    " ", "♟", "♞", "♝", "♜", "♛", "♚", "♟", "♞", "♝", "♜", "♛", "♚"
};

void draw_interface() {
    // In-place rewrite: Move terminal cursor back to top-left
    printf("\033[H");

    printf("\033[1;36m┌────────────────────────────────────────────────────────┐\033[0m\n");
    printf("\033[1;36m│                 TERMINAL CHESS C-GUI                   │\033[0m\n");
    printf("\033[1;36m└────────────────────────────────────────────────────────┘\033[0m\n\n");

    printf("     A  B  C  D  E  F  G  H        \033[1;33m[STATUS & CONTROLS]\033[0m\n");
    for (int r = 0; r < 8; r++) {
        printf("  %d  ", 8 - r);
        for (int c = 0; c < 8; c++) {
            int idx = r * 8 + c;
            int is_light = (r + c) % 2 == 0;
            int bg = is_light ? COLOR_LIGHT_SQ : COLOR_DARK_SQ;

            if (r == cursor_y && c == cursor_x) {
                bg = COLOR_CURSOR;
            } else if (selected_y == r && selected_x == c) {
                bg = COLOR_SELECTED;
            } else if (is_legal_dest(r, c)) {
                bg = COLOR_LEGAL;
            } else if (is_king_in_check_sq(r, c)) {
                bg = COLOR_CHECK;
            } else if (history_count > 0 &&
                       (idx == history[history_count - 1].move.from ||
                        idx == history[history_count - 1].move.to)) {
                bg = COLOR_LAST_MV;
            }

            uint8_t p = board.board[idx];
            int p_color = get_piece_color(p);

            printf("\033[48;5;%dm", bg);
            if (p != EMPTY) {
                if (p_color == WHITE) {
                    printf("\033[38;5;231m\033[1m %s \033[48;5;%dm", piece_glyphs[p], bg);
                } else {
                    printf("\033[38;5;16m\033[1m %s \033[48;5;%dm", piece_glyphs[p], bg);
                }
            } else {
                printf("   ");
            }
            printf("\033[0m");
        }
        printf("  %d    ", 8 - r);

        // Sidebar printing details
        switch (r) {
            case 0:
                printf("Turn: %s", board.side == WHITE ? "\033[1;37mWHITE\033[0m" : "\033[1;35mBLACK\033[0m");
                break;
            case 1:
                printf("Mode: %s", game_mode == MODE_PVP ? "\033[1;32mPlayer vs Player\033[0m" : "\033[1;35mPlayer vs Engine\033[0m");
                break;
            case 2:
                if (game_mode == MODE_PVE) {
                    printf("Engine Side: %s", engine_side == WHITE ? "WHITE" : "BLACK");
                }
                break;
            case 3:
                printf("Engine Connection: %s", engine_pid > 0 ? "\033[1;32mOK\033[0m" : "\033[1;31mOFF\033[0m");
                break;
            case 4:
                printf("Engine Executable: \033[1;33m%s\033[0m", engine_path);
                break;
            case 5:
                if (time_control_type == TC_DEPTH) {
                    printf("Constraint: DEPTH (%d)", tc_depth);
                } else if (time_control_type == TC_NODES) {
                    printf("Constraint: NODES (%d)", tc_nodes);
                } else {
                    printf("Constraint: TIME (%d ms)", tc_time_ms);
                }
                break;
            case 6:
                printf("Controller: \033[32m[Arrows/WASD]\033[0m Navigate | \033[32m[Space/Enter]\033[0m Select");
                break;
            case 7:
                printf("Hotkey:     \033[32m[U]\033[0m Undo | \033[32m[R]\033[0m Reset | \033[32m[T]\033[0m Constraint | \033[32m[M]\033[0m Mode");
                break;
        }
        printf("\n");
    }
    printf("     A  B  C  D  E  F  G  H\n\n");

    // Dynamic clean PGN formatter
    printf("\033[1;33m[PGN Moves]:\033[0m ");
    int start_pgn = (history_count > 14) ? history_count - 14 : 0;
    if (start_pgn > 0) printf("... ");
    for (int i = start_pgn; i < history_count; i++) {
        if (i % 2 == 0) {
            printf("%d. ", (i / 2) + 1);
        }
        printf("%s ", history[i].san);
    }
    // Clean up trailing PGN line endings
    printf("\033[K\n\n");

    // Checkmate/Check overlays
    char status[128] = {0};
    if (current_legal_moves.count == 0) {
        if (is_in_check(&board, board.side)) {
            snprintf(status, sizeof(status), "\033[1;31mCHECKMATE! %s wins!\033[0m", board.side == WHITE ? "BLACK" : "WHITE");
        } else {
            snprintf(status, sizeof(status), "\033[1;33mSTALEMATE! Game is drawn.\033[0m");
        }
    } else if (board.halfmove >= 100) {
        snprintf(status, sizeof(status), "\033[1;33mDRAW! 50-move rule reached.\033[0m");
    } else if (is_in_check(&board, board.side)) {
        snprintf(status, sizeof(status), "\033[1;31m*** CHECK! ***\033[0m");
    }
    printf("%s\033[K\n", status);
    fflush(stdout);
}

int read_key() {
    char c;
    ssize_t nread = read(STDIN_FILENO, &c, 1);
    if (nread <= 0) return 0;

    if (c == '\033') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\033';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\033';

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 'u'; // UP
                case 'B': return 'd'; // DOWN
                case 'C': return 'r'; // RIGHT
                case 'D': return 'l'; // LEFT
            }
        }
        return '\033';
    }
    if (c == 'w' || c == 'W') return 'u';
    if (c == 's' || c == 'S') return 'd';
    if (c == 'a' || c == 'A') return 'l';
    if (c == 'd' || c == 'D') return 'r';
    return c;
}

uint8_t prompt_promotion() {
    printf("\r\033[1;33mPROMOTION! Type [Q]ueen, [R]ook, [B]ishop, [K]night: \033[0m");
    fflush(stdout);
    while (1) {
        int key = read_key();
        if (key == 'q' || key == 'Q') return (board.side == WHITE) ? WQ : BQ;
        if (key == 'r' || key == 'R') return (board.side == WHITE) ? WR : BR;
        if (key == 'b' || key == 'B') return (board.side == WHITE) ? WB : BB;
        if (key == 'n' || key == 'N') return (board.side == WHITE) ? WN : BN;
    }
}

void change_time_controls_ui() {
    disable_raw_mode();
    printf("\n\033[1;36m=== CONSTRAINT SETTINGS ===\033[0m\n");
    printf("1. Search Depth limit (current: %d)\n", tc_depth);
    printf("2. Search Node limit (current: %d)\n", tc_nodes);
    printf("3. Search Time limit (current: %d ms)\n", tc_time_ms);
    printf("Select option (1-3): ");
    fflush(stdout);

    char opt[16];
    if (fgets(opt, sizeof(opt), stdin)) {
        int selection = atoi(opt);
        if (selection == 1) {
            printf("Enter target depth (1-50): ");
            fflush(stdout);
            if (fgets(opt, sizeof(opt), stdin)) {
                int val = atoi(opt);
                if (val >= 1 && val <= 50) { tc_depth = val; time_control_type = TC_DEPTH; }
            }
        } else if (selection == 2) {
            printf("Enter target node limit (e.g. 100000): ");
            fflush(stdout);
            if (fgets(opt, sizeof(opt), stdin)) {
                int val = atoi(opt);
                if (val > 0) { tc_nodes = val; time_control_type = TC_NODES; }
            }
        } else if (selection == 3) {
            printf("Enter target search time (ms): ");
            fflush(stdout);
            if (fgets(opt, sizeof(opt), stdin)) {
                int val = atoi(opt);
                if (val > 0) { tc_time_ms = val; time_control_type = TC_TIME; }
            }
        }
    }
    enable_raw_mode();
    printf("\033[2J"); // Complete screen clear
}

void change_engine_path_ui() {
    disable_raw_mode();
    printf("\n\033[1;36m=== ENGINE SELECTION ===\033[0m\n");
    printf("Enter target UCI binary name or full file system path:\n> ");
    fflush(stdout);

    char path[256];
    if (fgets(path, sizeof(path), stdin)) {
        path[strcspn(path, "\n")] = 0;
        if (strlen(path) > 0) {
            strncpy(engine_path, path, sizeof(engine_path) - 1);
            start_engine(engine_path);
        }
    }
    enable_raw_mode();
    printf("\033[2J");
}

void handle_key(int key) {
    if (key == 0) return;

    if (key == 'u') {
        if (cursor_y > 0) cursor_y--;
    } else if (key == 'd') {
        if (cursor_y < 7) cursor_y++;
    } else if (key == 'l') {
        if (cursor_x > 0) cursor_x--;
    } else if (key == 'r') {
        if (cursor_x < 7) cursor_x++;
    }
    else if (key == ' ' || key == 13 || key == 10) {
        int idx = cursor_y * 8 + cursor_x;
        if (selected_x == -1) {
            if (board.board[idx] != EMPTY && get_piece_color(board.board[idx]) == board.side) {
                selected_x = cursor_x;
                selected_y = cursor_y;
            }
        } else {
            int from = selected_y * 8 + selected_x;
            int to = cursor_y * 8 + cursor_x;

            if (from == to) {
                selected_x = -1; selected_y = -1;
                return;
            }
            if (board.board[to] != EMPTY && get_piece_color(board.board[to]) == board.side) {
                selected_x = cursor_x; selected_y = cursor_y;
                return;
            }

            int found_idx = -1;
            for (int i = 0; i < current_legal_moves.count; i++) {
                if (current_legal_moves.moves[i].from == from && current_legal_moves.moves[i].to == to) {
                    found_idx = i;
                    break;
                }
            }

            if (found_idx != -1) {
                Move m = current_legal_moves.moves[found_idx];
                uint8_t p = board.board[m.from];
                if ((p == WP && m.to / 8 == 0) || (p == BP && m.to / 8 == 7)) {
                    m.promote = prompt_promotion();
                }

                char san[16];
                get_move_san(&board, m, san);

                // Commit history entry
                history[history_count].state = (BoardState){
                    .side = board.side, .castle = board.castle, .ep = board.ep,
                    .halfmove = board.halfmove, .fullmove = board.fullmove
                };
                memcpy(history[history_count].state.board, board.board, 64);
                history[history_count].move = m;
                strcpy(history[history_count].san, san);
                history_count++;

                Board next;
                make_move(&board, &next, m);
                board = next;

                selected_x = -1; selected_y = -1;
                generate_legal_moves(&board, &current_legal_moves);
            } else {
                selected_x = -1; selected_y = -1;
            }
        }
    }
    // Takeback (Undo) Request
    else if (key == 'u' || key == 'U') {
        if (history_count > 0) {
            int undos_to_execute = (game_mode == MODE_PVE) ? 2 : 1;
            for (int i = 0; i < undos_to_execute; i++) {
                if (history_count > 0) {
                    history_count--;
                    memcpy(board.board, history[history_count].state.board, 64);
                    board.side = history[history_count].state.side;
                    board.castle = history[history_count].state.castle;
                    board.ep = history[history_count].state.ep;
                    board.halfmove = history[history_count].state.halfmove;
                    board.fullmove = history[history_count].state.fullmove;
                }
            }
            selected_x = -1; selected_y = -1;
            generate_legal_moves(&board, &current_legal_moves);
            engine_searching = 0;
        }
    }
    // Soft Reset
    else if (key == 'r' || key == 'R') {
        init_board(&board);
        history_count = 0;
        selected_x = -1; selected_y = -1;
        generate_legal_moves(&board, &current_legal_moves);
        engine_searching = 0;
    }
    // Change Mode
    else if (key == 'm' || key == 'M') {
        game_mode = (game_mode == MODE_PVP) ? MODE_PVE : MODE_PVP;
        selected_x = -1; selected_y = -1;
        engine_searching = 0;
    }
    // Time Controls
    else if (key == 't' || key == 'T') {
        change_time_controls_ui();
    }
    // Change Engine Binary Path
    else if (key == 'e' || key == 'E') {
        change_engine_path_ui();
    }
}

void parse_engine_response() {
    char line[2048];
    while (get_engine_line_buffered(line, sizeof(line))) {
        if (strncmp(line, "bestmove ", 9) == 0) {
            char mv_str[16];
            if (sscanf(line, "bestmove %s", mv_str) == 1) {
                int from_col = mv_str[0] - 'a';
                int from_row = 7 - (mv_str[1] - '1');
                int to_col = mv_str[2] - 'a';
                int to_row = 7 - (mv_str[3] - '1');
                int from = from_row * 8 + from_col;
                int to = to_row * 8 + to_col;

                uint8_t promo = 0;
                if (strlen(mv_str) > 4) {
                    char pc = mv_str[4];
                    if (board.side == WHITE) {
                        if (pc == 'q') promo = WQ; else if (pc == 'r') promo = WR;
                        else if (pc == 'b') promo = WB; else if (pc == 'n') promo = WN;
                    } else {
                        if (pc == 'q') promo = BQ; else if (pc == 'r') promo = BR;
                        else if (pc == 'b') promo = BB; else if (pc == 'n') promo = BN;
                    }
                }

                MoveList ml;
                generate_legal_moves(&board, &ml);
                int matched = 0;
                Move engine_move;
                for (int i = 0; i < ml.count; i++) {
                    Move m = ml.moves[i];
                    if (m.from == from && m.to == to && (promo == 0 || m.promote == promo)) {
                        engine_move = m;
                        matched = 1;
                        break;
                    }
                }

                if (matched) {
                    char san[16];
                    get_move_san(&board, engine_move, san);

                    history[history_count].state = (BoardState){
                        .side = board.side, .castle = board.castle, .ep = board.ep,
                        .halfmove = board.halfmove, .fullmove = board.fullmove
                    };
                    memcpy(history[history_count].state.board, board.board, 64);
                    history[history_count].move = engine_move;
                    strcpy(history[history_count].san, san);
                    history_count++;

                    Board next;
                    make_move(&board, &next, engine_move);
                    board = next;

                    generate_legal_moves(&board, &current_legal_moves);
                }
            }
            engine_searching = 0;
            break;
        }
    }
}

int main() {
    init_board(&board);
    generate_legal_moves(&board, &current_legal_moves);

    // Initial Engine Discovery Attempt
    start_engine(engine_path);

    enable_raw_mode();
    printf("\033[2J"); // Complete initial terminal clear

    struct pollfd fds[2];
    while (1) {
        draw_interface();

        if (game_mode == MODE_PVE && board.side == engine_side && !engine_searching && current_legal_moves.count > 0) {
            trigger_engine_move();
        }

        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;
        fds[1].fd = (engine_pid > 0) ? engine_out[0] : -1;
        fds[1].events = POLLIN;

        int ret = poll(fds, 2, 50);
        if (ret > 0) {
            if (fds[0].revents & POLLIN) {
                int key = read_key();
                if (key == 'q' || key == 'Q' || key == 3) { // Ctrl-C or 'Q' to quit
                    break;
                }
                handle_key(key);
            }
            if (fds[1].fd != -1 && (fds[1].revents & POLLIN)) {
                parse_engine_response();
            }
        } else {
            // Read background loops if active
            if (fds[1].fd != -1) {
                parse_engine_response();
            }
        }
    }

    disable_raw_mode();
    stop_engine();
    return 0;
}
