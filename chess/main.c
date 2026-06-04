#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>

/* Chess Piece Representations */
#define EMPTY 0
#define PAWN 1
#define KNIGHT 2
#define BISHOP 3
#define ROOK 4
#define QUEEN 5
#define KING 6

/* White positive, Black negative */
#define WP 1
#define WN 2
#define WB 3
#define WR 4
#define WQ 5
#define WK 6
#define BP -1
#define BN -2
#define BB -3
#define BR -4
#define BQ -5
#define BK -6

#define MAX_HISTORY 1024

/* Chess Board State Representation */
typedef struct {
    int board[64]; // Row 0 is 8th rank, Row 7 is 1st rank. File 0 is 'a', File 7 is 'h'.
    int side;      // 1 for White, -1 for Black
    int castle;    // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep;        // En Passant target square index (-1 if none)
    int halfmove;
    int fullmove;
} Board;

typedef struct {
    int from;
    int to;
    int promote; // Piece type to promote to, or EMPTY
} Move;

typedef struct {
    Move moves[256];
    int count;
} MoveList;

typedef enum { TC_DEPTH, TC_NODES, TC_TIME } TCType;

/* Game global states */
Board board_history[MAX_HISTORY];
Move move_history[MAX_HISTORY];
char san_history[MAX_HISTORY][16];
int history_count = 0;

Board current_board;
Move last_move = {-1, -1, EMPTY};
int selected_sq = -1;
int cursor_row = 6; // Starts on e2 (row 6, col 4)
int cursor_col = 4;

/* Engine process globals */
int engine_in[2];  // GUI writes, Engine reads
int engine_out[2]; // Engine writes, GUI reads
pid_t engine_pid = -1;
int engine_connected = 0;
int play_vs_engine = 1;
int engine_thinking = 0;
char engine_name[64] = "UCI Engine";

/* Search settings */
TCType tc_type = TC_DEPTH;
int tc_value = 10; // Default search depth

struct termios orig_termios;

/* Terminal utilities */
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h\033[0m"); // Restore cursor and reset colors
    fflush(stdout);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON); // Disable echo & canonical mode
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
    fflush(stdout);
}

void clean_exit(int sig) {
    if (engine_pid > 0) {
        kill(engine_pid, SIGTERM);
    }
    exit(0);
}

/* Base Chess engine board setup */
void init_board(Board *b) {
    memset(b, 0, sizeof(Board));
    b->board[0] = BR; b->board[1] = BN; b->board[2] = BB; b->board[3] = BQ;
    b->board[4] = BK; b->board[5] = BB; b->board[6] = BN; b->board[7] = BR;
    for (int i = 8; i < 16; i++) b->board[i] = BP;
    for (int i = 48; i < 56; i++) b->board[i] = WP;
    b->board[56] = WR; b->board[57] = WN; b->board[58] = WB; b->board[59] = WQ;
    b->board[60] = WK; b->board[61] = WB; b->board[62] = WN; b->board[63] = WR;
    b->side = 1;
    b->castle = 15;
    b->ep = -1;
    b->halfmove = 0;
    b->fullmove = 1;
}

/* Engine Process Controls */
void send_to_engine(const char *cmd) {
    if (engine_pid > 0) {
        write(engine_in[1], cmd, strlen(cmd));
    }
}

void start_engine(const char *path) {
    if (pipe(engine_in) < 0 || pipe(engine_out) < 0) return;
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(engine_in[0], STDIN_FILENO);
        dup2(engine_out[1], STDOUT_FILENO);
        close(engine_in[1]);
        close(engine_out[0]);
        char *args[] = {(char *)path, NULL};
        execvp(args[0], args);
        exit(1); // Exit if engine fails to boot
    } else if (engine_pid > 0) {
        close(engine_in[0]);
        close(engine_out[1]);
        // Set engine stdout read to non-blocking
        fcntl(engine_out[0], F_SETFL, O_NONBLOCK);
        engine_connected = 1;
    }
}

void handshake_engine() {
    send_to_engine("uci\n");
    char buf[4096];
    int bytes;
    fd_set fds;
    struct timeval tv;
    
    // Read response looking for identification and uciok
    int handshake_complete = 0;
    while (!handshake_complete) {
        FD_ZERO(&fds);
        FD_SET(engine_out[0], &fds);
        tv.tv_sec = 2; tv.tv_usec = 0;
        int active = select(engine_out[0] + 1, &fds, NULL, NULL, &tv);
        if (active <= 0) break; // Timeout
        
        bytes = read(engine_out[0], buf, sizeof(buf) - 1);
        if (bytes <= 0) break;
        buf[bytes] = '\0';
        
        char *id_name = strstr(buf, "id name ");
        if (id_name) {
            char *end = strchr(id_name + 8, '\n');
            if (end) {
                int len = end - (id_name + 8);
                if (len > 63) len = 63;
                strncpy(engine_name, id_name + 8, len);
                engine_name[len] = '\0';
            }
        }
        if (strstr(buf, "uciok")) {
            handshake_complete = 1;
        }
    }
    send_to_engine("isready\n");
    while (1) {
        FD_ZERO(&fds);
        FD_SET(engine_out[0], &fds);
        tv.tv_sec = 2; tv.tv_usec = 0;
        int active = select(engine_out[0] + 1, &fds, NULL, NULL, &tv);
        if (active <= 0) break;
        bytes = read(engine_out[0], buf, sizeof(buf) - 1);
        if (bytes <= 0) break;
        buf[bytes] = '\0';
        if (strstr(buf, "readyok")) break;
    }
}

/* Chess Move Validation Engine */
int is_square_attacked(const Board *b, int sq, int attacker) {
    int r = sq / 8, c = sq % 8;
    // Knight
    int kn[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn[i][0], nc = c + kn[i][1];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (b->board[nr * 8 + nc] == attacker * KNIGHT) return 1;
        }
    }
    // King
    int kg[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + kg[i][0], nc = c + kg[i][1];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (b->board[nr * 8 + nc] == attacker * KING) return 1;
        }
    }
    // Pawn
    int p_row = r + (attacker == 1 ? 1 : -1);
    if (p_row >= 0 && p_row < 8) {
        if (c > 0 && b->board[p_row * 8 + c - 1] == attacker * PAWN) return 1;
        if (c < 7 && b->board[p_row * 8 + c + 1] == attacker * PAWN) return 1;
    }
    // Rook/Queen Orthogonals
    int orth[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += orth[i][0]; nc += orth[i][1];
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            int p = b->board[nr * 8 + nc];
            if (p != EMPTY) {
                if (p == attacker * ROOK || p == attacker * QUEEN) return 1;
                break;
            }
        }
    }
    // Bishop/Queen Diagonals
    int diag[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += diag[i][0]; nc += diag[i][1];
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            int p = b->board[nr * 8 + nc];
            if (p != EMPTY) {
                if (p == attacker * BISHOP || p == attacker * QUEEN) return 1;
                break;
            }
        }
    }
    return 0;
}

int find_king(const Board *b, int side) {
    for (int i = 0; i < 64; i++) {
        if (b->board[i] == side * KING) return i;
    }
    return -1;
}

int is_in_check(const Board *b, int side) {
    int king_sq = find_king(b, side);
    if (king_sq == -1) return 0;
    return is_square_attacked(b, king_sq, -side);
}

void add_move(MoveList *list, int from, int to, int promote) {
    list->moves[list->count].from = from;
    list->moves[list->count].to = to;
    list->moves[list->count].promote = promote;
    list->count++;
}

void generate_pawn_moves(const Board *b, int sq, MoveList *list) {
    int r = sq / 8, c = sq % 8;
    int side = (b->board[sq] > 0) ? 1 : -1;
    int dir = (side == 1) ? -1 : 1;
    int start_row = (side == 1) ? 6 : 1;
    int promo_row = (side == 1) ? 0 : 7;

    int next_r = r + dir;
    if (next_r >= 0 && next_r < 8) {
        if (b->board[next_r * 8 + c] == EMPTY) {
            if (next_r == promo_row) {
                add_move(list, sq, next_r * 8 + c, side * QUEEN);
                add_move(list, sq, next_r * 8 + c, side * ROOK);
                add_move(list, sq, next_r * 8 + c, side * BISHOP);
                add_move(list, sq, next_r * 8 + c, side * KNIGHT);
            } else {
                add_move(list, sq, next_r * 8 + c, EMPTY);
            }
            if (r == start_row) {
                int double_r = r + 2 * dir;
                if (b->board[double_r * 8 + c] == EMPTY) {
                    add_move(list, sq, double_r * 8 + c, EMPTY);
                }
            }
        }
    }
    int cap_cols[2] = {c - 1, c + 1};
    for (int i = 0; i < 2; i++) {
        int nc = cap_cols[i];
        if (nc >= 0 && nc < 8) {
            int dest = next_r * 8 + nc;
            int dest_p = b->board[dest];
            if (dest_p != EMPTY && (dest_p > 0) != (side > 0)) {
                if (next_r == promo_row) {
                    add_move(list, sq, dest, side * QUEEN);
                    add_move(list, sq, dest, side * ROOK);
                    add_move(list, sq, dest, side * BISHOP);
                    add_move(list, sq, dest, side * KNIGHT);
                } else {
                    add_move(list, sq, dest, EMPTY);
                }
            }
            if (dest == b->ep) {
                add_move(list, sq, dest, EMPTY);
            }
        }
    }
}

void generate_piece_moves(const Board *b, int sq, MoveList *list) {
    int p = b->board[sq];
    int side = (p > 0) ? 1 : -1;
    int type = abs(p);
    int r = sq / 8, c = sq % 8;

    if (type == KNIGHT) {
        int kn[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
        for (int i = 0; i < 8; i++) {
            int nr = r + kn[i][0], nc = c + kn[i][1];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int dest_p = b->board[nr * 8 + nc];
                if (dest_p == EMPTY || (dest_p > 0) != (side > 0)) {
                    add_move(list, sq, nr * 8 + nc, EMPTY);
                }
            }
        }
    } else if (type == KING) {
        int kg[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
        for (int i = 0; i < 8; i++) {
            int nr = r + kg[i][0], nc = c + kg[i][1];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int dest_p = b->board[nr * 8 + nc];
                if (dest_p == EMPTY || (dest_p > 0) != (side > 0)) {
                    add_move(list, sq, nr * 8 + nc, EMPTY);
                }
            }
        }
        // Castling
        if (side == 1) {
            if ((b->castle & 1) && b->board[61] == EMPTY && b->board[62] == EMPTY) {
                if (!is_square_attacked(b, 60, -1) && !is_square_attacked(b, 61, -1) && !is_square_attacked(b, 62, -1))
                    add_move(list, 60, 62, EMPTY);
            }
            if ((b->castle & 2) && b->board[59] == EMPTY && b->board[58] == EMPTY && b->board[57] == EMPTY) {
                if (!is_square_attacked(b, 60, -1) && !is_square_attacked(b, 59, -1) && !is_square_attacked(b, 58, -1))
                    add_move(list, 60, 58, EMPTY);
            }
        } else {
            if ((b->castle & 4) && b->board[5] == EMPTY && b->board[6] == EMPTY) {
                if (!is_square_attacked(b, 4, 1) && !is_square_attacked(b, 5, 1) && !is_square_attacked(b, 6, 1))
                    add_move(list, 4, 6, EMPTY);
            }
            if ((b->castle & 8) && b->board[3] == EMPTY && b->board[2] == EMPTY && b->board[1] == EMPTY) {
                if (!is_square_attacked(b, 4, 1) && !is_square_attacked(b, 3, 1) && !is_square_attacked(b, 2, 1))
                    add_move(list, 4, 2, EMPTY);
            }
        }
    } else { // Bishop, Rook, Queen
        int dirs[8][2];
        int n_dirs = 0;
        if (type == BISHOP || type == QUEEN) {
            dirs[n_dirs][0] = -1; dirs[n_dirs][1] = -1; n_dirs++;
            dirs[n_dirs][0] = -1; dirs[n_dirs][1] = 1; n_dirs++;
            dirs[n_dirs][0] = 1;  dirs[n_dirs][1] = -1; n_dirs++;
            dirs[n_dirs][0] = 1;  dirs[n_dirs][1] = 1; n_dirs++;
        }
        if (type == ROOK || type == QUEEN) {
            dirs[n_dirs][0] = -1; dirs[n_dirs][1] = 0; n_dirs++;
            dirs[n_dirs][0] = 1;  dirs[n_dirs][1] = 0; n_dirs++;
            dirs[n_dirs][0] = 0;  dirs[n_dirs][1] = -1; n_dirs++;
            dirs[n_dirs][0] = 0;  dirs[n_dirs][1] = 1; n_dirs++;
        }
        for (int d = 0; d < n_dirs; d++) {
            int nr = r, nc = c;
            while (1) {
                nr += dirs[d][0]; nc += dirs[d][1];
                if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
                int dest = nr * 8 + nc;
                int dest_p = b->board[dest];
                if (dest_p == EMPTY) {
                    add_move(list, sq, dest, EMPTY);
                } else {
                    if ((dest_p > 0) != (side > 0)) {
                        add_move(list, sq, dest, EMPTY);
                    }
                    break;
                }
            }
        }
    }
}

void generate_legal_moves(const Board *b, MoveList *list) {
    MoveList pseudo;
    pseudo.count = 0;
    list->count = 0;
    for (int sq = 0; sq < 64; sq++) {
        int piece = b->board[sq];
        if (piece != EMPTY && (piece > 0) == (b->side > 0)) {
            if (abs(piece) == PAWN) {
                generate_pawn_moves(b, sq, &pseudo);
            } else {
                generate_piece_moves(b, sq, &pseudo);
            }
        }
    }
    // Filter illegal moves (leaves king in check)
    for (int i = 0; i < pseudo.count; i++) {
        Board temp;
        // Mock make_move logic
        temp = *b;
        int p = temp.board[pseudo.moves[i].from];
        temp.board[pseudo.moves[i].from] = EMPTY;
        temp.board[pseudo.moves[i].to] = (pseudo.moves[i].promote != EMPTY) ? pseudo.moves[i].promote : p;
        if (abs(p) == PAWN && pseudo.moves[i].to == b->ep) {
            temp.board[pseudo.moves[i].to + (b->side == 1 ? 8 : -8)] = EMPTY;
        }
        if (!is_in_check(&temp, b->side)) {
            list->moves[list->count++] = pseudo.moves[i];
        }
    }
}

void make_move(const Board *src, Board *dst, Move m) {
    *dst = *src;
    int piece = dst->board[m.from];
    int type = abs(piece);
    int side = dst->side;

    dst->board[m.from] = EMPTY;
    dst->board[m.to] = (m.promote != EMPTY) ? m.promote : piece;

    // Handle Castling moves
    if (type == KING) {
        if (m.from == 60 && m.to == 62 && side == 1) {
            dst->board[61] = dst->board[63]; dst->board[63] = EMPTY;
        } else if (m.from == 60 && m.to == 58 && side == 1) {
            dst->board[59] = dst->board[56]; dst->board[56] = EMPTY;
        } else if (m.from == 4 && m.to == 6 && side == -1) {
            dst->board[5] = dst->board[7]; dst->board[7] = EMPTY;
        } else if (m.from == 4 && m.to == 2 && side == -1) {
            dst->board[3] = dst->board[0]; dst->board[0] = EMPTY;
        }
        if (side == 1) dst->castle &= ~3; else dst->castle &= ~12;
    }

    // Castling updates if Rooks move/captured
    if (m.from == 56 || m.to == 56) dst->castle &= ~2;
    if (m.from == 63 || m.to == 63) dst->castle &= ~1;
    if (m.from == 0  || m.to == 0)  dst->castle &= ~8;
    if (m.from == 7  || m.to == 7)  dst->castle &= ~4;

    // Handle En Passant capture
    if (type == PAWN && m.to == src->ep) {
        dst->board[m.to + (side == 1 ? 8 : -8)] = EMPTY;
    }

    // Double pawn push sets EP targets
    dst->ep = -1;
    if (type == PAWN && abs(m.to - m.from) == 16) {
        dst->ep = m.from + (side == 1 ? -8 : 8);
    }

    dst->side = -side;
    if (side == -1) dst->fullmove++;
    if (type == PAWN || src->board[m.to] != EMPTY) dst->halfmove = 0; else dst->halfmove++;
}

/* Dynamic SAN Formatting (PGN Moves generator) */
void move_to_san(const Board *b, Move m, char *san) {
    int piece = b->board[m.from];
    int type = abs(piece);
    int side = (piece > 0) ? 1 : -1;

    if (type == KING) {
        if (m.from == 60 && m.to == 62 && side == 1) { strcpy(san, "O-O"); goto add_checks; }
        if (m.from == 60 && m.to == 58 && side == 1) { strcpy(san, "O-O-O"); goto add_checks; }
        if (m.from == 4 && m.to == 6 && side == -1) { strcpy(san, "O-O"); goto add_checks; }
        if (m.from == 4 && m.to == 2 && side == -1) { strcpy(san, "O-O-O"); goto add_checks; }
    }

    int len = 0;
    if (type != PAWN) {
        char p_chars[] = {'?', '?', 'N', 'B', 'R', 'Q', 'K'};
        san[len++] = p_chars[type];

        // Search for piece ambiguity
        MoveList list;
        generate_legal_moves(b, &list);
        int file_needed = 0, rank_needed = 0;
        for (int i = 0; i < list.count; i++) {
            Move tm = list.moves[i];
            if (tm.from != m.from && tm.to == m.to && abs(b->board[tm.from]) == type) {
                if (tm.from % 8 != m.from % 8) file_needed = 1; else rank_needed = 1;
            }
        }
        if (file_needed) san[len++] = 'a' + (m.from % 8);
        if (rank_needed) san[len++] = '1' + (7 - (m.from / 8));
    } else {
        if (b->board[m.to] != EMPTY || m.to == b->ep) {
            san[len++] = 'a' + (m.from % 8);
        }
    }

    if (b->board[m.to] != EMPTY || (type == PAWN && m.to == b->ep)) {
        san[len++] = 'x';
    }

    san[len++] = 'a' + (m.to % 8);
    san[len++] = '1' + (7 - (m.to / 8));

    if (m.promote != EMPTY) {
        san[len++] = '=';
        char p_chars[] = {'?', '?', 'N', 'B', 'R', 'Q', 'K'};
        san[len++] = p_chars[abs(m.promote)];
    }
    san[len] = '\0';

add_checks:;
    Board temp;
    make_move(b, &temp, m);
    if (is_in_check(&temp, temp.side)) {
        MoveList next_moves;
        generate_legal_moves(&temp, &next_moves);
        if (next_moves.count == 0) strcat(san, "#"); else strcat(san, "+");
    }
}

/* Coordinate Converter for Engine Communications */
void format_move(Move m, char *str) {
    int f_col = m.from % 8; int f_row = 7 - (m.from / 8);
    int t_col = m.to % 8;   int t_row = 7 - (m.to / 8);
    if (m.promote != EMPTY) {
        char p = 'q';
        if (abs(m.promote) == ROOK) p = 'r';
        else if (abs(m.promote) == BISHOP) p = 'b';
        else if (abs(m.promote) == KNIGHT) p = 'n';
        sprintf(str, "%c%d%c%d%c", 'a' + f_col, f_row + 1, 'a' + t_col, t_row + 1, p);
    } else {
        sprintf(str, "%c%d%c%d", 'a' + f_col, f_row + 1, 'a' + t_col, t_row + 1);
    }
}

int parse_move(const Board *b, const char *str, Move *m) {
    if (strlen(str) < 4) return 0;
    int f_col = str[0] - 'a'; int f_row = 7 - (str[1] - '1');
    int t_col = str[2] - 'a'; int t_row = 7 - (str[3] - '1');
    if (f_col < 0 || f_col > 7 || f_row < 0 || f_row > 7 || t_col < 0 || t_col > 7 || t_row < 0 || t_row > 7) return 0;
    
    m->from = f_row * 8 + f_col;
    m->to = t_row * 8 + t_col;
    m->promote = EMPTY;
    if (strlen(str) == 5) {
        char p = str[4];
        if (p == 'q') m->promote = b->side * QUEEN;
        else if (p == 'r') m->promote = b->side * ROOK;
        else if (p == 'b') m->promote = b->side * BISHOP;
        else if (p == 'n') m->promote = b->side * KNIGHT;
    }
    
    MoveList list;
    generate_legal_moves(b, &list);
    for (int i = 0; i < list.count; i++) {
        if (list.moves[i].from == m->from && list.moves[i].to == m->to) {
            if (m->promote == EMPTY || abs(list.moves[i].promote) == abs(m->promote)) {
                *m = list.moves[i];
                return 1;
            }
        }
    }
    return 0;
}

void trigger_engine_search(const Board *b) {
    char cmd[4096] = "position startpos";
    if (history_count > 0) {
        strcat(cmd, " moves");
        for (int i = 0; i < history_count; i++) {
            char m_str[16];
            format_move(move_history[i], m_str);
            strcat(cmd, " ");
            strcat(cmd, m_str);
        }
    }
    strcat(cmd, "\n");
    send_to_engine(cmd);

    char go_cmd[128];
    if (tc_type == TC_DEPTH) {
        sprintf(go_cmd, "go depth %d\n", tc_value);
    } else if (tc_type == TC_NODES) {
        sprintf(go_cmd, "go nodes %d\n", tc_value);
    } else {
        sprintf(go_cmd, "go movetime %d\n", tc_value);
    }
    send_to_engine(go_cmd);
    engine_thinking = 1;
}

/* Graphics Rendering Engine */
const char *get_piece_symbol(int piece) {
    if (piece == EMPTY) return "  ";
    int type = abs(piece);
    if (piece > 0) {
        switch (type) {
            case PAWN:   return "♙ ";
            case KNIGHT: return "♘ ";
            case BISHOP: return "♗ ";
            case ROOK:   return "♖ ";
            case QUEEN:  return "♕ ";
            case KING:   return "♔ ";
        }
    } else {
        switch (type) {
            case PAWN:   return "♟ ";
            case KNIGHT: return "♞ ";
            case BISHOP: return "♝ ";
            case ROOK:   return "♜ ";
            case QUEEN:  return "♛ ";
            case KING:   return "♚ ";
        }
    }
    return "  ";
}

void print_square(int row, int col, int piece, int is_cursor, int is_selected, int is_last_move, int is_check) {
    const char *bg;
    if (is_check) {
        bg = "\033[48;5;196m"; // Deep Red for Check
    } else if (is_cursor) {
        bg = "\033[48;5;39m";  // Sky Blue Cursor
    } else if (is_selected) {
        bg = "\033[48;5;220m"; // Golden yellow selected
    } else if (is_last_move) {
        bg = "\033[48;5;111m"; // Pastel blue last move
    } else {
        if ((row + col) % 2 == 0) {
            bg = "\033[48;5;253m"; // Light square Cream
        } else {
            bg = "\033[48;5;108m"; // Dark square Sage Green
        }
    }
    const char *fg = (piece > 0) ? "\033[38;5;231;1m" : "\033[38;5;232;1m"; // Crisp Contrast pieces
    printf("%s%s %s\033[0m", bg, fg, get_piece_symbol(piece));
}

void get_pgn_line(int line_no, char *out, int max_len) {
    int total_pairs = (history_count + 1) / 2;
    int max_visible_pairs = 3 * 2; // 3 lines, 2 pairs per line
    int start_pair = 0;
    if (total_pairs > max_visible_pairs) {
        start_pair = total_pairs - max_visible_pairs;
    }
    int pair_idx = start_pair + line_no * 2;
    out[0] = '\0';
    for (int i = 0; i < 2; i++) {
        int p = pair_idx + i;
        if (p < total_pairs) {
            char temp[64];
            int m1 = p * 2; int m2 = p * 2 + 1;
            if (m2 < history_count) {
                snprintf(temp, sizeof(temp), "%d. %-5s %-5s  ", p + 1, san_history[m1], san_history[m2]);
            } else {
                snprintf(temp, sizeof(temp), "%d. %-5s        ", p + 1, san_history[m1]);
            }
            strncat(out, temp, max_len - strlen(out) - 1);
        }
    }
}

void render_game() {
    printf("\033[H"); // Push cursor to home
    printf("  \033[1;36m┌────────────────────────┐\033[0m   \033[1;35mCHESS ENGINE TERMINAL GUI\033[0m\n");

    for (int r = 0; r < 8; r++) {
        printf("%d \033[1;36m│\033[0m", 8 - r);
        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            int p = current_board.board[sq];
            int is_cur = (r == cursor_row && c == cursor_col);
            int is_sel = (selected_sq == sq);
            int is_last = (last_move.from != -1 && (last_move.from == sq || last_move.to == sq));
            int is_chk = (abs(p) == KING && (p > 0) == (current_board.side > 0) && is_in_check(&current_board, current_board.side));
            print_square(r, c, p, is_cur, is_sel, is_last, is_chk);
        }
        printf("\033[1;36m│\033[0m %d   ", 8 - r);

        // Sidebar rendering
        char pgn_line[128];
        switch (r) {
            case 0:
                if (engine_connected) {
                    printf("\033[1;32mEngine:\033[0m %s (Connected)", engine_name);
                } else {
                    printf("\033[1;31mEngine:\033[0m Offline (2-Player Local)");
                }
                break;
            case 1:
                printf("\033[1;34mGame Mode:\033[0m %s", play_vs_engine ? "Player vs Computer" : "Local Player vs Player");
                break;
            case 2: {
                MoveList ml;
                generate_legal_moves(&current_board, &ml);
                if (ml.count == 0) {
                    if (is_in_check(&current_board, current_board.side)) {
                        printf("\033[1;31mSTATUS: Checkmate! %s wins!\033[0m", current_board.side == 1 ? "BLACK" : "WHITE");
                    } else {
                        printf("\033[1;33mSTATUS: Stalemate Draw!\033[0m");
                    }
                } else {
                    printf("\033[1;34mTurn:\033[0m %s %s", (current_board.side == 1) ? "\033[1;37mWHITE\033[0m" : "\033[1;33mBLACK\033[0m", engine_thinking ? "(\033[5;31mThinking...\033[0m)" : "");
                }
                break;
            }
            case 3:
                if (tc_type == TC_DEPTH) {
                    printf("\033[1;34mConstraints:\033[0m Depth: %d ply", tc_value);
                } else if (tc_type == TC_NODES) {
                    printf("\033[1;34mConstraints:\033[0m Nodes: %d", tc_value);
                } else {
                    printf("\033[1;34mConstraints:\033[0m Time: %d ms", tc_value);
                }
                break;
            case 4:
                printf("\033[1;33mPGN Moves:\033[0m");
                break;
            case 5:
                get_pgn_line(0, pgn_line, sizeof(pgn_line));
                printf("  %s", pgn_line);
                break;
            case 6:
                get_pgn_line(1, pgn_line, sizeof(pgn_line));
                printf("  %s", pgn_line);
                break;
            case 7:
                get_pgn_line(2, pgn_line, sizeof(pgn_line));
                printf("  %s", pgn_line);
                break;
        }
        printf("\033[K\n");
    }
    printf("  \033[1;36m└────────────────────────┘\033[0m   \033[1;33mControls:\033[0m\n");
    printf("     a  b  c  d  e  f  g  h        [Arrows/WASD] Move Cursor\n");
    printf("                                   [Space/Enter] Select/Move Piece\n");
    printf("                                   [U] Undo Last   [C] Change Constraints\n");
    printf("                                   [M] Toggle Mode [Q] Quit Game\n");
    printf("\033[K\n");
    fflush(stdout);
}

/* Modal dialog prompts (in-place terminal updates) */
void handle_tc_change() {
    printf("\033[12;0H\033[K\033[1;33mTC Constraint: [1] Depth  [2] Nodes  [3] Time (ms): \033[0m");
    fflush(stdout);
    char choice = 0;
    while (choice < '1' || choice > '3') {
        read(STDIN_FILENO, &choice, 1);
    }
    if (choice == '1') {
        tc_type = TC_DEPTH;
        printf("\033[12;0H\033[K\033[1;33mEnter search ply depth (e.g. 10): \033[0m");
    } else if (choice == '2') {
        tc_type = TC_NODES;
        printf("\033[12;0H\033[K\033[1;33mEnter search node count (e.g. 100000): \033[0m");
    } else {
        tc_type = TC_TIME;
        printf("\033[12;0H\033[K\033[1;33mEnter engine calculation time (ms): \033[0m");
    }
    fflush(stdout);

    // Swap back to buffered input temporarily to parse numbers
    struct termios raw;
    tcgetattr(STDIN_FILENO, &raw);
    struct termios temp = raw;
    temp.c_lflag |= (ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &temp);

    char val_buf[32];
    if (fgets(val_buf, sizeof(val_buf), stdin)) {
        int val = atoi(val_buf);
        if (val > 0) tc_value = val;
    }
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int handle_move_attempt(int from, int to, Move *out_move) {
    MoveList list;
    generate_legal_moves(&current_board, &list);
    int match_count = 0;
    Move matches[4];
    for (int i = 0; i < list.count; i++) {
        if (list.moves[i].from == from && list.moves[i].to == to) {
            matches[match_count++] = list.moves[i];
        }
    }
    if (match_count == 0) return 0; // Illegal move

    if (matches[0].promote != EMPTY) {
        printf("\033[12;0H\033[K\033[1;33mPromote pawn! Enter key: [q] Queen, [r] Rook, [b] Bishop, [n] Knight: \033[0m");
        fflush(stdout);
        int promo_type = EMPTY;
        while (promo_type == EMPTY) {
            char c;
            if (read(STDIN_FILENO, &c, 1) > 0) {
                if (c == 'q' || c == 'Q') promo_type = current_board.side * QUEEN;
                if (c == 'r' || c == 'R') promo_type = current_board.side * ROOK;
                if (c == 'b' || c == 'B') promo_type = current_board.side * BISHOP;
                if (c == 'n' || c == 'N') promo_type = current_board.side * KNIGHT;
            }
        }
        for (int i = 0; i < match_count; i++) {
            if (matches[i].promote == promo_type) {
                *out_move = matches[i];
                return 1;
            }
        }
        *out_move = matches[0];
        return 1;
    }
    *out_move = matches[0];
    return 1;
}

void check_engine_input() {
    char buf[4096];
    static char parse_buffer[8192] = {0};
    int bytes = read(engine_out[0], buf, sizeof(buf) - 1);
    if (bytes > 0) {
        buf[bytes] = '\0';
        strcat(parse_buffer, buf);
        
        char *bestmove_ptr = strstr(parse_buffer, "bestmove");
        if (bestmove_ptr) {
            char move_str[16];
            if (sscanf(bestmove_ptr, "bestmove %s", move_str) == 1) {
                Move m;
                if (parse_move(&current_board, move_str, &m)) {
                    move_to_san(&current_board, m, san_history[history_count]);
                    board_history[history_count] = current_board;
                    move_history[history_count] = m;
                    history_count++;

                    Board next;
                    make_move(&current_board, &next, m);
                    current_board = next;
                    last_move = m;
                }
                engine_thinking = 0;
            }
            // Reset state buffer once parsed
            parse_buffer[0] = '\0';
        }
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT, clean_exit);
    signal(SIGTERM, clean_exit);

    // Clean viewport
    printf("\033[2J");
    fflush(stdout);

    init_board(&current_board);

    if (argc > 1) {
        start_engine(argv[1]);
        if (engine_connected) {
            handshake_engine();
        }
    } else {
        // Look for localized binary paths before defaulting to offline Mode
        if (access("./stockfish", X_OK) == 0) {
            start_engine("./stockfish");
            if (engine_connected) handshake_engine();
        } else if (access("/opt/homebrew/bin/stockfish", X_OK) == 0) {
            start_engine("/opt/homebrew/bin/stockfish");
            if (engine_connected) handshake_engine();
        } else {
            play_vs_engine = 0; // Fallback to PvP local mode
        }
    }

    enable_raw_mode();

    while (1) {
        render_game();

        // Check if engine should move
        if (play_vs_engine && engine_connected && current_board.side == -1 && !engine_thinking) {
            trigger_engine_search(&current_board);
        }

        // Multiplexed non-blocking engine and keyboard I/O using select()
        fd_set read_fds;
        struct timeval timeout;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        int max_fd = STDIN_FILENO;
        if (engine_pid > 0 && engine_thinking) {
            FD_SET(engine_out[0], &read_fds);
            if (engine_out[0] > max_fd) max_fd = engine_out[0];
        }

        timeout.tv_sec = 0;
        timeout.tv_usec = 30000; // 30ms responsive tick rates
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (activity > 0) {
            if (engine_pid > 0 && FD_ISSET(engine_out[0], &read_fds)) {
                check_engine_input();
            }

            if (FD_ISSET(STDIN_FILENO, &read_fds)) {
                char ch;
                if (read(STDIN_FILENO, &ch, 1) > 0) {
                    if (ch == '\033') { // Escape Sequence for Arrow Keys
                        char seq[2];
                        if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                            if (seq[0] == '[') {
                                switch (seq[1]) {
                                    case 'A': if (cursor_row > 0) cursor_row--; break; // Up
                                    case 'B': if (cursor_row < 7) cursor_row++; break; // Down
                                    case 'C': if (cursor_col < 7) cursor_col++; break; // Right
                                    case 'D': if (cursor_col > 0) cursor_col--; break; // Left
                                }
                            }
                        }
                    } else {
                        switch (ch) {
                            case 'w': case 'W': if (cursor_row > 0) cursor_row--; break;
                            case 's': case 'S': if (cursor_row < 7) cursor_row++; break;
                            case 'a': case 'A': if (cursor_col > 0) cursor_col--; break;
                            case 'd': case 'D': if (cursor_col < 7) cursor_col++; break;
                            case ' ': case '\n': case '\r': { // Selector
                                int clicked_sq = cursor_row * 8 + cursor_col;
                                if (selected_sq == -1) {
                                    int p = current_board.board[clicked_sq];
                                    if (p != EMPTY && (p > 0) == (current_board.side > 0)) {
                                        selected_sq = clicked_sq;
                                    }
                                } else {
                                    if (selected_sq == clicked_sq) {
                                        selected_sq = -1; // Deselect
                                    } else {
                                        Move m;
                                        if (handle_move_attempt(selected_sq, clicked_sq, &m)) {
                                            move_to_san(&current_board, m, san_history[history_count]);
                                            board_history[history_count] = current_board;
                                            move_history[history_count] = m;
                                            history_count++;

                                            Board next;
                                            make_move(&current_board, &next, m);
                                            current_board = next;
                                            last_move = m;
                                        }
                                        selected_sq = -1;
                                    }
                                }
                                break;
                            }
                            case 'u': case 'U': { // Undo / Take back
                                if (history_count > 0) {
                                    int steps = (play_vs_engine && engine_connected) ? 2 : 1;
                                    while (steps > 0 && history_count > 0) {
                                        history_count--;
                                        current_board = board_history[history_count];
                                        steps--;
                                    }
                                    if (history_count > 0) {
                                        last_move = move_history[history_count - 1];
                                    } else {
                                        last_move = (Move){-1, -1, EMPTY};
                                    }
                                    selected_sq = -1;
                                }
                                break;
                            }
                            case 'c': case 'C': { // Time control configuration
                                handle_tc_change();
                                break;
                            }
                            case 'm': case 'M': { // Change Play Modes
                                if (engine_connected) {
                                    play_vs_engine = !play_vs_engine;
                                    selected_sq = -1;
                                }
                                break;
                            }
                            case 'q': case 'Q': {
                                clean_exit(0);
                            }
                        }
                    }
                }
            }
        }
    }
    return 0;
}
