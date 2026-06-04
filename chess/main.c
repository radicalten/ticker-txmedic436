#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#define COLOR_MASK 24
#define WHITE 8
#define BLACK 16
#define COLOR(p) ((p) & 24)
#define TYPE(p) ((p) & 7)

enum { EMPTY, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };

// Terminal styling escape sequences
#define BG_LIGHT   "\033[48;5;180m" // Wood Light
#define BG_DARK    "\033[48;5;94m"  // Wood Dark
#define BG_SEL     "\033[48;5;220m" // Gold Selected
#define BG_LEGAL   "\033[48;5;114m" // Pastel Green Targets
#define BG_LAST    "\033[48;5;73m"  // Pastel Cyan Last Move
#define BG_CUR     "\033[48;5;196m" // Vivid Red Cursor
#define FG_WHITE   "\033[38;5;255m\033[1m"
#define FG_BLACK   "\033[38;5;232m\033[1m"
#define RESET      "\033[0m"

#define KEY_UP    1000
#define KEY_DOWN  1001
#define KEY_LEFT  1002
#define KEY_RIGHT 1003
#define KEY_ENTER 10
#define KEY_SPACE ' '

typedef struct {
    uint8_t board[64];
    int side;      // WHITE or BLACK
    int castle;    // Bitmask: WK=1, WQ=2, BK=4, BQ=8
    int ep;        // En Passant target square
    int halfmove;
    int fullmove;
} State;

typedef struct {
    int from;
    int to;
    int promo;     // Piece type if promotion
    int flags;
} Move;

typedef enum { TC_DEPTH, TC_NODES, TC_TIME } TCMode;

typedef struct {
    TCMode mode;
    int depth;
    int nodes;
    int time_ms;
} TimeControl;

// History Tracking
#define MAX_HISTORY 2048
State history[MAX_HISTORY];
Move move_history[MAX_HISTORY];
char san_history[MAX_HISTORY][16];
int history_count = 0;

// Engine Process Handles
int engine_in[2];
int engine_out[2];
pid_t engine_pid = -1;
struct termios orig_termios;

// Forward Declarations
int get_legal_moves(State *s, Move *legal_moves);
int is_attacked(State *s, int sq, int attacker_color);
void make_move(State *s, Move m);

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void cleanup_engine() {
    disable_raw_mode();
    if (engine_pid > 0) {
        write(engine_in[1], "quit\n", 5);
        kill(engine_pid, SIGKILL);
        waitpid(engine_pid, NULL, 0);
    }
}

int start_engine(const char *path) {
    if (pipe(engine_in) < 0 || pipe(engine_out) < 0) return 0;
    engine_pid = fork();
    if (engine_pid < 0) return 0;

    if (engine_pid == 0) { // Child Process
        dup2(engine_in[0], STDIN_FILENO);
        dup2(engine_out[1], STDOUT_FILENO);
        close(engine_in[0]); close(engine_in[1]);
        close(engine_out[0]); close(engine_out[1]);
        execlp(path, path, NULL);
        exit(1);
    }
    // Parent Process
    close(engine_in[0]);
    close(engine_out[1]);
    int flags = fcntl(engine_out[0], F_GETFL, 0);
    fcntl(engine_out[0], F_SETFL, flags | O_NONBLOCK);
    return 1;
}

void send_to_engine(const char *cmd) {
    write(engine_in[1], cmd, strlen(cmd));
}

int read_line_from_engine(char *line_buf, int max_len) {
    int idx = 0;
    char c;
    struct pollfd pfd = { engine_out[0], POLLIN, 0 };

    while (idx < max_len - 1) {
        int ret = poll(&pfd, 1, 50);
        if (ret <= 0) return (idx > 0) ? idx : -1;
        int r = read(engine_out[0], &c, 1);
        if (r <= 0) return -1;
        if (c == '\n') {
            line_buf[idx] = '\0';
            return idx;
        }
        if (c != '\r') line_buf[idx++] = c;
    }
    line_buf[idx] = '\0';
    return idx;
}

void init_uci() {
    send_to_engine("uci\n");
    char line[1024];
    while (1) {
        if (read_line_from_engine(line, sizeof(line)) >= 0) {
            if (strstr(line, "uciok")) break;
        }
    }
    send_to_engine("isready\n");
    while (1) {
        if (read_line_from_engine(line, sizeof(line)) >= 0) {
            if (strstr(line, "readyok")) break;
        }
    }
}

void init_board(State *s) {
    memset(s->board, 0, sizeof(s->board));
    int setup[8] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
    for (int i = 0; i < 8; i++) {
        s->board[0 * 8 + i] = setup[i] | BLACK;
        s->board[1 * 8 + i] = PAWN | BLACK;
        s->board[6 * 8 + i] = PAWN | WHITE;
        s->board[7 * 8 + i] = setup[i] | WHITE;
    }
    s->side = WHITE;
    s->castle = 15;
    s->ep = -1;
    s->halfmove = 0;
    s->fullmove = 1;
}

int in_bounds(int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 8; }

int generate_pseudo_moves(State *s, Move *moves) {
    int count = 0;
    int side = s->side;
    int opp = (side == WHITE) ? BLACK : WHITE;

    for (int i = 0; i < 64; i++) {
        int p = s->board[i];
        if (p == EMPTY || COLOR(p) != side) continue;

        int r = i / 8;
        int c = i % 8;
        int type = TYPE(p);

        if (type == PAWN) {
            int dir = (side == WHITE) ? -1 : 1;
            int start_row = (side == WHITE) ? 6 : 1;
            int promo_row = (side == WHITE) ? 0 : 7;

            // Forward single
            int next_r = r + dir;
            if (in_bounds(next_r, c) && s->board[next_r * 8 + c] == EMPTY) {
                if (next_r == promo_row) {
                    moves[count++] = (Move){i, next_r * 8 + c, KNIGHT, 0};
                    moves[count++] = (Move){i, next_r * 8 + c, BISHOP, 0};
                    moves[count++] = (Move){i, next_r * 8 + c, ROOK, 0};
                    moves[count++] = (Move){i, next_r * 8 + c, QUEEN, 0};
                } else {
                    moves[count++] = (Move){i, next_r * 8 + c, 0, 0};
                    // Double push
                    int double_r = r + 2 * dir;
                    if (r == start_row && s->board[double_r * 8 + c] == EMPTY) {
                        moves[count++] = (Move){i, double_r * 8 + c, 0, 0};
                    }
                }
            }
            // Captures
            for (int dc = -1; dc <= 1; dc += 2) {
                int next_c = c + dc;
                if (in_bounds(next_r, next_c)) {
                    int target_sq = next_r * 8 + next_c;
                    int dest_p = s->board[target_sq];
                    if (dest_p != EMPTY && COLOR(dest_p) == opp) {
                        if (next_r == promo_row) {
                            moves[count++] = (Move){i, target_sq, KNIGHT, 0};
                            moves[count++] = (Move){i, target_sq, BISHOP, 0};
                            moves[count++] = (Move){i, target_sq, ROOK, 0};
                            moves[count++] = (Move){i, target_sq, QUEEN, 0};
                        } else {
                            moves[count++] = (Move){i, target_sq, 0, 0};
                        }
                    }
                    if (target_sq == s->ep) {
                        moves[count++] = (Move){i, target_sq, 0, 0};
                    }
                }
            }
        } else if (type == KNIGHT) {
            int off[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
            for (int j = 0; j < 8; j++) {
                int nr = r + off[j][0], nc = c + off[j][1];
                if (in_bounds(nr, nc)) {
                    int sq = nr * 8 + nc;
                    if (s->board[sq] == EMPTY || COLOR(s->board[sq]) == opp) moves[count++] = (Move){i, sq, 0, 0};
                }
            }
        } else if (type == BISHOP || type == QUEEN) {
            int dirs[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
            for (int d = 0; d < 4; d++) {
                int nr = r, nc = c;
                while (1) {
                    nr += dirs[d][0]; nc += dirs[d][1];
                    if (!in_bounds(nr, nc)) break;
                    int sq = nr * 8 + nc;
                    if (s->board[sq] == EMPTY) {
                        moves[count++] = (Move){i, sq, 0, 0};
                    } else {
                        if (COLOR(s->board[sq]) == opp) moves[count++] = (Move){i, sq, 0, 0};
                        break;
                    }
                }
            }
        }
        if (type == ROOK || type == QUEEN) {
            int dirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
            for (int d = 0; d < 4; d++) {
                int nr = r, nc = c;
                while (1) {
                    nr += dirs[d][0]; nc += dirs[d][1];
                    if (!in_bounds(nr, nc)) break;
                    int sq = nr * 8 + nc;
                    if (s->board[sq] == EMPTY) {
                        moves[count++] = (Move){i, sq, 0, 0};
                    } else {
                        if (COLOR(s->board[sq]) == opp) moves[count++] = (Move){i, sq, 0, 0};
                        break;
                    }
                }
            }
        } else if (type == KING) {
            int dirs[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
            for (int d = 0; d < 8; d++) {
                int nr = r + dirs[d][0], nc = c + dirs[d][1];
                if (in_bounds(nr, nc)) {
                    int sq = nr * 8 + nc;
                    if (s->board[sq] == EMPTY || COLOR(s->board[sq]) == opp) moves[count++] = (Move){i, sq, 0, 0};
                }
            }
            // Castling
            if (side == WHITE && i == 60) {
                if ((s->castle & 1) && s->board[61] == EMPTY && s->board[62] == EMPTY) {
                    if (!is_attacked(s, 60, BLACK) && !is_attacked(s, 61, BLACK) && !is_attacked(s, 62, BLACK)) moves[count++] = (Move){60, 62, 0, 0};
                }
                if ((s->castle & 2) && s->board[59] == EMPTY && s->board[58] == EMPTY && s->board[57] == EMPTY) {
                    if (!is_attacked(s, 60, BLACK) && !is_attacked(s, 59, BLACK) && !is_attacked(s, 58, BLACK)) moves[count++] = (Move){60, 58, 0, 0};
                }
            } else if (side == BLACK && i == 4) {
                if ((s->castle & 4) && s->board[5] == EMPTY && s->board[6] == EMPTY) {
                    if (!is_attacked(s, 4, WHITE) && !is_attacked(s, 5, WHITE) && !is_attacked(s, 6, WHITE)) moves[count++] = (Move){4, 6, 0, 0};
                }
                if ((s->castle & 8) && s->board[3] == EMPTY && s->board[2] == EMPTY && s->board[1] == EMPTY) {
                    if (!is_attacked(s, 4, WHITE) && !is_attacked(s, 3, WHITE) && !is_attacked(s, 2, WHITE)) moves[count++] = (Move){4, 2, 0, 0};
                }
            }
        }
    }
    return count;
}

int is_attacked(State *s, int sq, int attacker_color) {
    int r = sq / 8, c = sq % 8;
    // Knight
    int kn[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn[i][0], nc = c + kn[i][1];
        if (in_bounds(nr, nc) && COLOR(s->board[nr*8 + nc]) == attacker_color && TYPE(s->board[nr*8 + nc]) == KNIGHT) return 1;
    }
    // King
    int kg[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + kg[i][0], nc = c + kg[i][1];
        if (in_bounds(nr, nc) && COLOR(s->board[nr*8 + nc]) == attacker_color && TYPE(s->board[nr*8 + nc]) == KING) return 1;
    }
    // Pawn
    int p_dir = (attacker_color == WHITE) ? 1 : -1;
    int pr = r + p_dir;
    for (int dc = -1; dc <= 1; dc += 2) {
        int pc = c + dc;
        if (in_bounds(pr, pc) && COLOR(s->board[pr*8 + pc]) == attacker_color && TYPE(s->board[pr*8 + pc]) == PAWN) return 1;
    }
    // Sliding (Diagonal)
    int diag[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += diag[i][0]; nc += diag[i][1];
            if (!in_bounds(nr, nc)) break;
            int p = s->board[nr*8 + nc];
            if (p != EMPTY) {
                if (COLOR(p) == attacker_color && (TYPE(p) == BISHOP || TYPE(p) == QUEEN)) return 1;
                break;
            }
        }
    }
    // Sliding (Orthogonal)
    int orth[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += orth[i][0]; nc += orth[i][1];
            if (!in_bounds(nr, nc)) break;
            int p = s->board[nr*8 + nc];
            if (p != EMPTY) {
                if (COLOR(p) == attacker_color && (TYPE(p) == ROOK || TYPE(p) == QUEEN)) return 1;
                break;
            }
        }
    }
    return 0;
}

int find_king(State *s, int side) {
    for (int i = 0; i < 64; i++) {
        if (COLOR(s->board[i]) == side && TYPE(s->board[i]) == KING) return i;
    }
    return -1;
}

int get_legal_moves(State *s, Move *legal_moves) {
    Move pseudo[256];
    int num_pseudo = generate_pseudo_moves(s, pseudo);
    int num_legal = 0;
    for (int i = 0; i < num_pseudo; i++) {
        State next = *s;
        make_move(&next, pseudo[i]);
        int king_sq = find_king(&next, s->side);
        if (king_sq != -1 && !is_attacked(&next, king_sq, (s->side == WHITE) ? BLACK : WHITE)) {
            legal_moves[num_legal++] = pseudo[i];
        }
    }
    return num_legal;
}

void make_move(State *s, Move m) {
    int p = s->board[m.from];
    int type = TYPE(p);
    int side = COLOR(p);
    int opp = (side == WHITE) ? BLACK : WHITE;

    if (type == KING) {
        if (side == WHITE) {
            if (m.from == 60 && m.to == 62) { s->board[61] = ROOK|WHITE; s->board[63] = EMPTY; }
            else if (m.from == 60 && m.to == 58) { s->board[59] = ROOK|WHITE; s->board[56] = EMPTY; }
            s->castle &= ~3;
        } else {
            if (m.from == 4 && m.to == 6) { s->board[5] = ROOK|BLACK; s->board[7] = EMPTY; }
            else if (m.from == 4 && m.to == 2) { s->board[3] = ROOK|BLACK; s->board[0] = EMPTY; }
            s->castle &= ~12;
        }
    }
    if (type == ROOK) {
        if (m.from == 56) s->castle &= ~2;
        if (m.from == 63) s->castle &= ~1;
        if (m.from == 0) s->castle &= ~8;
        if (m.from == 7) s->castle &= ~4;
    }
    // Captured rook corner safety checks
    if (m.to == 56) s->castle &= ~2;
    if (m.to == 63) s->castle &= ~1;
    if (m.to == 0) s->castle &= ~8;
    if (m.to == 7) s->castle &= ~4;

    if (type == PAWN && m.to == s->ep) {
        s->board[(m.from / 8) * 8 + (m.to % 8)] = EMPTY;
    }

    s->board[m.to] = p;
    s->board[m.from] = EMPTY;

    if (m.promo) s->board[m.to] = m.promo | side;

    if (type == PAWN && abs(m.from - m.to) == 16) {
        s->ep = (m.from + m.to) / 2;
    } else {
        s->ep = -1;
    }

    if (side == BLACK) s->fullmove++;
    s->side = opp;
}

void to_san(State *prev_s, Move m, char *buf) {
    int type = TYPE(prev_s->board[m.from]);
    int side = COLOR(prev_s->board[m.from]);
    int is_cap = (prev_s->board[m.to] != EMPTY) || (type == PAWN && m.to == prev_s->ep);

    if (type == KING) {
        if (m.from == 60 && m.to == 62 && side == WHITE) { strcpy(buf, "O-O"); return; }
        if (m.from == 60 && m.to == 58 && side == WHITE) { strcpy(buf, "O-O-O"); return; }
        if (m.from == 4 && m.to == 6 && side == BLACK) { strcpy(buf, "O-O"); return; }
        if (m.from == 4 && m.to == 2 && side == BLACK) { strcpy(buf, "O-O-O"); return; }
    }

    char *ptr = buf;
    if (type == PAWN) {
        if (is_cap) {
            *ptr++ = (m.from % 8) + 'a';
            *ptr++ = 'x';
        }
        *ptr++ = (m.to % 8) + 'a';
        *ptr++ = '8' - (m.to / 8);
        if (m.promo) {
            *ptr++ = '=';
            if (m.promo == QUEEN) *ptr++ = 'Q';
            else if (m.promo == ROOK) *ptr++ = 'R';
            else if (m.promo == BISHOP) *ptr++ = 'B';
            else if (m.promo == KNIGHT) *ptr++ = 'N';
        }
    } else {
        char pcs[] = " PNBRQK";
        *ptr++ = pcs[type];
        
        Move leg[256];
        int num_leg = get_legal_moves(prev_s, leg);
        int duplicate = 0, same_file = 0, same_rank = 0;
        for (int i = 0; i < num_leg; i++) {
            if (leg[i].from != m.from && leg[i].to == m.to && TYPE(prev_s->board[leg[i].from]) == type) {
                duplicate = 1;
                if (leg[i].from % 8 == m.from % 8) same_file = 1;
                if (leg[i].from / 8 == m.from / 8) same_rank = 1;
            }
        }
        if (duplicate) {
            if (!same_file) *ptr++ = (m.from % 8) + 'a';
            else if (!same_rank) *ptr++ = '8' - (m.from / 8);
            else {
                *ptr++ = (m.from % 8) + 'a';
                *ptr++ = '8' - (m.from / 8);
            }
        }
        if (is_cap) *ptr++ = 'x';
        *ptr++ = (m.to % 8) + 'a';
        *ptr++ = '8' - (m.to / 8);
    }

    State next = *prev_s;
    make_move(&next, m);
    int opp = (side == WHITE) ? BLACK : WHITE;
    int king_sq = find_king(&next, opp);
    if (king_sq != -1 && is_attacked(&next, king_sq, side)) {
        Move opp_moves[256];
        if (get_legal_moves(&next, opp_moves) == 0) *ptr++ = '#';
        else *ptr++ = '+';
    }
    *ptr = '\0';
}

void move_to_uci(Move m, char *buf) {
    int ff = m.from % 8, fr = 8 - (m.from / 8);
    int tf = m.to % 8, tr = 8 - (m.to / 8);
    if (m.promo) {
        char p = 'q';
        if (m.promo == KNIGHT) p = 'n';
        else if (m.promo == BISHOP) p = 'b';
        else if (m.promo == ROOK) p = 'r';
        sprintf(buf, "%c%d%c%d%c", ff + 'a', fr, tf + 'a', tr, p);
    } else {
        sprintf(buf, "%c%d%c%d", ff + 'a', fr, tf + 'a', tr);
    }
}

int parse_uci_move(State *s, const char *str, Move *m) {
    if (strlen(str) < 4) return 0;
    int ff = str[0] - 'a', fr = 8 - (str[1] - '0');
    int tf = str[2] - 'a', tr = 8 - (str[3] - '0');
    if (!in_bounds(fr, ff) || !in_bounds(tr, tf)) return 0;

    m->from = fr * 8 + ff;
    m->to = tr * 8 + tf;
    m->promo = 0;
    if (strlen(str) == 5) {
        char p = str[4];
        if (p == 'n') m->promo = KNIGHT;
        else if (p == 'b') m->promo = BISHOP;
        else if (p == 'r') m->promo = ROOK;
        else if (p == 'q') m->promo = QUEEN;
    }
    return 1;
}

void send_position_command() {
    char cmd[16384] = "position startpos moves";
    for (int i = 0; i < history_count; i++) {
        char uci_buf[8];
        move_to_uci(move_history[i], uci_buf);
        strcat(cmd, " ");
        strcat(cmd, uci_buf);
    }
    strcat(cmd, "\n");
    send_to_engine(cmd);
}

int get_engine_move(char *move_out) {
    char line[2048];
    while (1) {
        int len = read_line_from_engine(line, sizeof(line));
        if (len < 0) continue;
        if (strncmp(line, "bestmove", 8) == 0) {
            sscanf(line, "bestmove %s", move_out);
            return 1;
        }
    }
}

int read_key() {
    char c;
    int n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return 0;
    if (c == '\033') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return '\033';
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return '\033';
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
            }
        }
        return '\033';
    }
    return c;
}

int get_promotion_piece() {
    disable_raw_mode();
    printf("\nPromote pawn to: (q)ueen, (r)ook, (b)ishop, (n)ight: ");
    char c = ' ';
    while (1) {
        if (scanf(" %c", &c) <= 0) continue;
        if (c == 'q' || c == 'Q') { enable_raw_mode(); return QUEEN; }
        if (c == 'r' || c == 'R') { enable_raw_mode(); return ROOK; }
        if (c == 'b' || c == 'B') { enable_raw_mode(); return BISHOP; }
        if (c == 'n' || c == 'N') { enable_raw_mode(); return KNIGHT; }
        printf("Invalid choice. Try again: ");
    }
}

void draw_board(State *s, int cursor, int selected, int last_from, int last_to, TimeControl *tc) {
    printf("\033[H\033[2J");
    printf("\033[1;36m=== TERMINAL CHESS GUI ===\033[0m\n\n");

    int valid_targets[64] = {0};
    if (selected != -1) {
        Move leg[256];
        int num_leg = get_legal_moves(s, leg);
        for (int i = 0; i < num_leg; i++) {
            if (leg[i].from == selected) valid_targets[leg[i].to] = 1;
        }
    }

    printf("    a  b  c  d  e  f  g  h\n");
    for (int r = 0; r < 8; r++) {
        printf(" %d ", 8 - r);
        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            int p = s->board[sq];
            const char *bg = ((r + c) % 2 == 0) ? BG_LIGHT : BG_DARK;

            if (sq == last_from || sq == last_to) bg = BG_LAST;
            if (valid_targets[sq]) bg = BG_LEGAL;
            if (sq == selected) bg = BG_SEL;
            if (sq == cursor) bg = BG_CUR;

            printf("%s", bg);
            if (p == EMPTY) {
                printf("   ");
            } else {
                const char *fg = (COLOR(p) == WHITE) ? FG_WHITE : FG_BLACK;
                const char *syms[] = { " ", "♟", "♞", "♝", "♜", "♛", "♚" };
                printf(" %s%s%s ", fg, syms[TYPE(p)], bg);
            }
            printf("%s", RESET);
        }
        printf(" %d\n", 8 - r);
    }
    printf("    a  b  c  d  e  f  g  h\n\n");

    // Display Active Game Parameters
    printf("Turn: \033[1m%s\033[0m", (s->side == WHITE) ? "White" : "Black");
    printf("   |   Mode: ");
    if (tc->mode == TC_DEPTH) printf("\033[1;33mDEPTH (%d plies)\033[0m\n", tc->depth);
    else if (tc->mode == TC_NODES) printf("\033[1;33mNODES (%d)\033[0m\n", tc->nodes);
    else printf("\033[1;33mTIME (%d ms)\033[0m\n", tc->time_ms);

    // Render Move History (Standard PGN view)
    printf("Recent Moves: ");
    int start_hist = (history_count > 5) ? history_count - 5 : 0;
    for (int i = start_hist; i < history_count; i++) {
        if (i % 2 == 0) {
            printf("%d. %s ", (i / 2) + 1, san_history[i]);
        } else {
            printf("%s  ", san_history[i]);
        }
    }
    printf("\n\n");

    printf("\033[2mControls: Arrows=Move Cursor | Space/Enter=Select/Drop\n");
    printf("          U=Undo (Takeback)  | C=Time Controls       | Q=Exit\033[0m\n");
}

int main() {
    State current_state;
    init_board(&current_state);

    char engine_path[256] = "stockfish";
    int player_color = WHITE;

    printf("\033[1;36m=== TERMINAL CHESS SETUP ===\033[0m\n");
    printf("Enter engine command/path [default: stockfish]: ");
    char path_buf[256];
    if (fgets(path_buf, sizeof(path_buf), stdin)) {
        path_buf[strcspn(path_buf, "\n")] = '\0';
        if (strlen(path_buf) > 0) strcpy(engine_path, path_buf);
    }

    printf("Choose your color (w/b) [default: w]: ");
    char color_buf[16];
    if (fgets(color_buf, sizeof(color_buf), stdin)) {
        if (color_buf[0] == 'b' || color_buf[0] == 'B') player_color = BLACK;
    }

    if (!start_engine(engine_path)) {
        fprintf(stderr, "Error: Failed to launch engine '%s'\n", engine_path);
        return 1;
    }
    atexit(cleanup_engine);
    init_uci();

    enable_raw_mode();

    TimeControl tc = { TC_DEPTH, 10, 100000, 2000 };
    int cursor = 60; // default starts on e1 (White King)
    int selected = -1;
    int last_from = -1, last_to = -1;

    while (1) {
        // Evaluate Checkmate / Stalemate conditions
        Move leg[256];
        int num_leg = get_legal_moves(&current_state, leg);
        if (num_leg == 0) {
            draw_board(&current_state, cursor, selected, last_from, last_to, &tc);
            int king_sq = find_king(&current_state, current_state.side);
            int opp = (current_state.side == WHITE) ? BLACK : WHITE;
            if (is_attacked(&current_state, king_sq, opp)) {
                printf("\n\033[1;31m*** CHECKMATE! %s wins. ***\033[0m\n", (current_state.side == WHITE) ? "BLACK" : "WHITE");
            } else {
                printf("\n\033[1;33m*** STALEMATE! Game is drawn. ***\033[0m\n");
            }
            disable_raw_mode();
            return 0;
        }

        if (current_state.side != player_color) {
            // Engine calculation turn
            draw_board(&current_state, cursor, selected, last_from, last_to, &tc);
            printf("\n\033[5mEngine is thinking...\033[0m\n");
            send_position_command();

            char go_cmd[128];
            if (tc.mode == TC_DEPTH) sprintf(go_cmd, "go depth %d\n", tc.depth);
            else if (tc.mode == TC_NODES) sprintf(go_cmd, "go nodes %d\n", tc.nodes);
            else sprintf(go_cmd, "go movetime %d\n", tc.time_ms);
            send_to_engine(go_cmd);

            char best_move[16];
            if (get_engine_move(best_move)) {
                Move m;
                if (parse_uci_move(&current_state, best_move, &m)) {
                    // Match legal array to preserve specific move details
                    for (int i = 0; i < num_leg; i++) {
                        if (leg[i].from == m.from && leg[i].to == m.to) {
                            m = leg[i];
                            break;
                        }
                    }
                    to_san(&current_state, m, san_history[history_count]);
                    history[history_count] = current_state;
                    move_history[history_count] = m;
                    history_count++;

                    make_move(&current_state, m);
                    last_from = m.from;
                    last_to = m.to;
                }
            }
            continue;
        }

        // Draw Player UI
        draw_board(&current_state, cursor, selected, last_from, last_to, &tc);

        int key = read_key();
        if (key == KEY_UP) {
            int r = cursor / 8, c = cursor % 8;
            r = (r - 1 + 8) % 8; cursor = r * 8 + c;
        } else if (key == KEY_DOWN) {
            int r = cursor / 8, c = cursor % 8;
            r = (r + 1) % 8; cursor = r * 8 + c;
        } else if (key == KEY_LEFT) {
            int r = cursor / 8, c = cursor % 8;
            c = (c - 1 + 8) % 8; cursor = r * 8 + c;
        } else if (key == KEY_RIGHT) {
            int r = cursor / 8, c = cursor % 8;
            c = (c + 1) % 8; cursor = r * 8 + c;
        } else if (key == KEY_ENTER || key == KEY_SPACE) {
            if (selected == -1) {
                int piece = current_state.board[cursor];
                if (piece != EMPTY && COLOR(piece) == current_state.side) {
                    selected = cursor;
                }
            } else {
                if (cursor == selected) {
                    selected = -1;
                } else {
                    int found_idx = -1;
                    for (int i = 0; i < num_leg; i++) {
                        if (leg[i].from == selected && leg[i].to == cursor) {
                            found_idx = i;
                            break;
                        }
                    }
                    if (found_idx != -1) {
                        Move played = leg[found_idx];
                        int moving_piece = current_state.board[selected];
                        if (TYPE(moving_piece) == PAWN && (cursor / 8 == 0 || cursor / 8 == 7)) {
                            played.promo = get_promotion_piece();
                        }

                        to_san(&current_state, played, san_history[history_count]);
                        history[history_count] = current_state;
                        move_history[history_count] = played;
                        history_count++;

                        make_move(&current_state, played);
                        last_from = played.from;
                        last_to = played.to;
                        selected = -1;
                    } else {
                        // Attempt to directly pivot selections
                        int p = current_state.board[cursor];
                        if (p != EMPTY && COLOR(p) == current_state.side) selected = cursor;
                        else selected = -1;
                    }
                }
            }
        } else if (key == 'u' || key == 'U') {
            // Pop two states from history stack to revert back to human player's turn
            if (history_count >= 2) {
                history_count -= 2;
                current_state = history[history_count];
                if (history_count > 0) {
                    last_from = move_history[history_count - 1].from;
                    last_to = move_history[history_count - 1].to;
                } else {
                    last_from = last_to = -1;
                }
                selected = -1;
            }
        } else if (key == 'c' || key == 'C') {
            disable_raw_mode();
            printf("\n\033[1;36m=== MODIFY TIME CONTROLS ===\033[0m\n");
            printf("1. Set Engine Depth limit (plies)\n");
            printf("2. Set Engine Node limit\n");
            printf("3. Set Milliseconds per Move\n");
            printf("Enter Selection (1-3): ");
            int sel = 0;
            if (scanf("%d", &sel) == 1) {
                if (sel == 1) {
                    tc.mode = TC_DEPTH;
                    printf("Enter new ply limit (1-20): ");
                    scanf("%d", &tc.depth);
                } else if (sel == 2) {
                    tc.mode = TC_NODES;
                    printf("Enter node limit (e.g. 100000): ");
                    scanf("%d", &tc.nodes);
                } else if (sel == 3) {
                    tc.mode = TC_TIME;
                    printf("Enter target move time in ms: ");
                    scanf("%d", &tc.time_ms);
                }
            }
            enable_raw_mode();
            selected = -1;
        } else if (key == 'q' || key == 'Q') {
            cleanup_engine();
            printf("\033[H\033[2JQuit Game.\n");
            return 0;
        }
    }
    return 0;
}
