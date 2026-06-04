#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/select.h>
#include <signal.h>
#include <time.h>

/* Board 0x88 Representation */
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

#define GET_COLOR(p) ((p) & COLOR_MASK)
#define GET_TYPE(p) ((p) & TYPE_MASK)

#define CASTLE_W_OO  1
#define CASTLE_W_OOO 2
#define CASTLE_B_OO  4
#define CASTLE_B_OOO 8

/* Key Mapping Constants */
#define KEY_UP 1000
#define KEY_DOWN 1001
#define KEY_LEFT 1002
#define KEY_RIGHT 1003
#define KEY_ENTER 10
#define KEY_SPACE 32

/* ANSI Styling Sequences (256-color palette) */
#define BG_LIGHT     "\033[48;5;223m" // Cream Sand
#define BG_DARK      "\033[48;5;130m" // Wood Brown
#define BG_SELECTED  "\033[48;5;125m" // Soft Red
#define BG_LEGAL     "\033[48;5;108m" // Sage Green
#define BG_CURSOR    "\033[48;5;33m"  // Dodger Blue
#define FG_WHITE     "\033[38;5;231m\033[1m" // Bold Bright White
#define FG_BLACK     "\033[38;5;232m"        // Solid Dark Black

typedef struct {
    int from;
    int to;
    int promo;
} Move;

typedef struct {
    unsigned char board[128];
    int side;
    int castle;
    int ep; // En passant square
    int halfmove;
    int fullmove;
} BoardState;

typedef struct {
    char engine_path[256];
    int max_depth;
    int max_nodes;
    int max_time_ms;
    int mode; // 0: Play White vs Eng, 1: Play Black vs Eng, 2: PvP Local, 3: Eng vs Eng
} Settings;

/* Move generator vectors */
int knight_dirs[] = { -33, -31, -18, -14, 14, 18, 31, 33 };
int bishop_dirs[] = { -17, -15, 15, 17 };
int rook_dirs[] = { -16, -1, 1, 16 };
int queen_dirs[] = { -17, -16, -15, -1, 1, 15, 16, 17 };

/* Global Variables */
BoardState current_state;
BoardState history[1024];
Move move_history[1024];
int history_count = 0;

int cursor_file = 0;
int cursor_rank = 0;
int selected_sq = -1;
int legal_destinations[128];

Settings settings;
int engine_thinking = 0;
int engine_in_pipe[2];
int engine_out_pipe[2];
pid_t engine_pid = -1;

struct termios orig_termios;

/* UCI Paths on macOS */
const char *stockfish_paths[] = {
    "/opt/homebrew/bin/stockfish",
    "/usr/local/bin/stockfish",
    "stockfish"
};

/* Terminal Manipulation & Utility */
void disable_raw_mode() {
    printf("\033[?25h\033[0m"); // Restore cursor and styling
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
    fflush(stdout);
}

int read_key() {
    char c;
    int nread = read(STDIN_FILENO, &c, 1);
    if (nread <= 0) return 0;
    if (c == '\033') {
        char seq[3];
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        int n1 = read(STDIN_FILENO, &seq[0], 1);
        int n2 = read(STDIN_FILENO, &seq[1], 1);
        fcntl(STDIN_FILENO, F_SETFL, flags); // Restore blocking

        if (n1 > 0 && n2 > 0) {
            if (seq[0] == '[') {
                switch (seq[1]) {
                    case 'A': return KEY_UP;
                    case 'B': return KEY_DOWN;
                    case 'C': return KEY_RIGHT;
                    case 'D': return KEY_LEFT;
                }
            }
        }
        return '\033';
    }
    return c;
}

/* Engine Subprocess Pipes Execution */
const char *find_engine() {
    for (int i = 0; i < 3; i++) {
        if (access(stockfish_paths[i], X_OK) == 0) {
            return stockfish_paths[i];
        }
    }
    return "stockfish";
}

void start_engine(const char *path) {
    if (pipe(engine_in_pipe) < 0 || pipe(engine_out_pipe) < 0) return;
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(engine_in_pipe[0], STDIN_FILENO);
        dup2(engine_out_pipe[1], STDOUT_FILENO);
        dup2(engine_out_pipe[1], STDERR_FILENO);

        close(engine_in_pipe[0]); close(engine_in_pipe[1]);
        close(engine_out_pipe[0]); close(engine_out_pipe[1]);

        execl(path, path, (char *)NULL);
        exit(1);
    }
    close(engine_in_pipe[0]);
    close(engine_out_pipe[1]);

    int flags = fcntl(engine_out_pipe[0], F_GETFL, 0);
    fcntl(engine_out_pipe[0], F_SETFL, flags | O_NONBLOCK);
}

void send_to_engine(const char *cmd) {
    if (engine_pid > 0) {
        write(engine_in_pipe[1], cmd, strlen(cmd));
    }
}

char engine_buffer[8192];
int engine_buf_len = 0;

int read_engine_line(char *out_line, int max_len) {
    if (engine_pid <= 0) return 0;
    char temp[512];
    int n = read(engine_out_pipe[0], temp, sizeof(temp) - 1);
    if (n > 0) {
        temp[n] = '\0';
        if (engine_buf_len + n < (int)sizeof(engine_buffer)) {
            memcpy(engine_buffer + engine_buf_len, temp, n);
            engine_buf_len += n;
            engine_buffer[engine_buf_len] = '\0';
        }
    }

    for (int i = 0; i < engine_buf_len; i++) {
        if (engine_buffer[i] == '\n' || engine_buffer[i] == '\r') {
            int len = i;
            if (len >= max_len) len = max_len - 1;
            memcpy(out_line, engine_buffer, len);
            out_line[len] = '\0';

            int skip = (engine_buffer[i] == '\r' && i + 1 < engine_buf_len && engine_buffer[i+1] == '\n') ? 2 : 1;
            memmove(engine_buffer, engine_buffer + i + skip, engine_buf_len - (i + skip));
            engine_buf_len -= (i + skip);
            engine_buffer[engine_buf_len] = '\0';
            return 1;
        }
    }
    return 0;
}

void init_uci_connection() {
    send_to_engine("uci\n");
    char line[512];
    int timeout = 100;
    while (timeout-- > 0) {
        if (read_engine_line(line, sizeof(line)) && strstr(line, "uciok")) break;
        usleep(10000);
    }
    send_to_engine("isready\n");
    timeout = 100;
    while (timeout-- > 0) {
        if (read_engine_line(line, sizeof(line)) && strstr(line, "readyok")) break;
        usleep(10000);
    }
}

/* Internal Chess Logic (0x88 Ruleset Generator) */
void init_board(BoardState *state) {
    memset(state, 0, sizeof(BoardState));
    state->side = WHITE;
    state->castle = CASTLE_W_OO | CASTLE_W_OOO | CASTLE_B_OO | CASTLE_B_OOO;
    state->ep = -1;
    state->halfmove = 0;
    state->fullmove = 1;

    int pieces[8] = { ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK };
    for (int i = 0; i < 8; i++) {
        state->board[0x00 + i] = WHITE | pieces[i];
        state->board[0x10 + i] = WHITE | PAWN;
        state->board[0x60 + i] = BLACK | PAWN;
        state->board[0x70 + i] = BLACK | pieces[i];
    }
}

int is_attacked(const BoardState *state, int sq, int attacker_color) {
    // Pawn Attacks
    if (attacker_color == WHITE) {
        int p1 = sq - 17, p2 = sq - 15;
        if (!(p1 & 0x88) && state->board[p1] == (WHITE | PAWN)) return 1;
        if (!(p2 & 0x88) && state->board[p2] == (WHITE | PAWN)) return 1;
    } else {
        int p1 = sq + 17, p2 = sq + 15;
        if (!(p1 & 0x88) && state->board[p1] == (BLACK | PAWN)) return 1;
        if (!(p2 & 0x88) && state->board[p2] == (BLACK | PAWN)) return 1;
    }

    // Knight Attacks
    for (int i = 0; i < 8; i++) {
        int t = sq + knight_dirs[i];
        if (!(t & 0x88) && state->board[t] == (attacker_color | KNIGHT)) return 1;
    }

    // King Attacks (1-step)
    for (int i = 0; i < 8; i++) {
        int t = sq + queen_dirs[i];
        if (!(t & 0x88) && state->board[t] == (attacker_color | KING)) return 1;
    }

    // Bishop/Queen Diagonals
    for (int i = 0; i < 4; i++) {
        int t = sq;
        while (1) {
            t += bishop_dirs[i];
            if (t & 0x88) break;
            int p = state->board[t];
            if (p != EMPTY) {
                if (GET_COLOR(p) == attacker_color && (GET_TYPE(p) == BISHOP || GET_TYPE(p) == QUEEN)) return 1;
                break;
            }
        }
    }

    // Rook/Queen Orthogonals
    for (int i = 0; i < 4; i++) {
        int t = sq;
        while (1) {
            t += rook_dirs[i];
            if (t & 0x88) break;
            int p = state->board[t];
            if (p != EMPTY) {
                if (GET_COLOR(p) == attacker_color && (GET_TYPE(p) == ROOK || GET_TYPE(p) == QUEEN)) return 1;
                break;
            }
        }
    }
    return 0;
}

int in_check(const BoardState *state, int side) {
    int king_sq = -1;
    for (int sq = 0; sq < 128; sq++) {
        if (sq & 0x88) { sq += 7; continue; }
        if (state->board[sq] == (side | KING)) {
            king_sq = sq;
            break;
        }
    }
    if (king_sq == -1) return 0;
    return is_attacked(state, king_sq, side == WHITE ? BLACK : WHITE);
}

int generate_moves(const BoardState *state, Move *moves) {
    int count = 0;
    int us = state->side;
    int them = us == WHITE ? BLACK : WHITE;

    for (int sq = 0; sq < 128; sq++) {
        if (sq & 0x88) { sq += 7; continue; }
        int p = state->board[sq];
        if (p == EMPTY || GET_COLOR(p) != us) continue;
        int type = GET_TYPE(p);

        if (type == PAWN) {
            int dir = us == WHITE ? 16 : -16;
            int next = sq + dir;
            if (!(next & 0x88) && state->board[next] == EMPTY) {
                int rank = next >> 4;
                if (rank == 0 || rank == 7) {
                    moves[count++] = (Move){ sq, next, QUEEN };
                    moves[count++] = (Move){ sq, next, ROOK };
                    moves[count++] = (Move){ sq, next, BISHOP };
                    moves[count++] = (Move){ sq, next, KNIGHT };
                } else {
                    moves[count++] = (Move){ sq, next, 0 };
                    int start_rank = us == WHITE ? 1 : 6;
                    if ((sq >> 4) == start_rank && state->board[sq + 2 * dir] == EMPTY) {
                        moves[count++] = (Move){ sq, sq + 2 * dir, 0 };
                    }
                }
            }
            int caps[2] = { dir - 1, dir + 1 };
            for (int i = 0; i < 2; i++) {
                int cap_sq = sq + caps[i];
                if (cap_sq & 0x88) continue;
                if (state->board[cap_sq] != EMPTY && GET_COLOR(state->board[cap_sq]) == them) {
                    int rank = cap_sq >> 4;
                    if (rank == 0 || rank == 7) {
                        moves[count++] = (Move){ sq, cap_sq, QUEEN };
                        moves[count++] = (Move){ sq, cap_sq, ROOK };
                        moves[count++] = (Move){ sq, cap_sq, BISHOP };
                        moves[count++] = (Move){ sq, cap_sq, KNIGHT };
                    } else {
                        moves[count++] = (Move){ sq, cap_sq, 0 };
                    }
                }
                if (cap_sq == state->ep) {
                    moves[count++] = (Move){ sq, cap_sq, 0 };
                }
            }
        } else if (type == KNIGHT) {
            for (int i = 0; i < 8; i++) {
                int next = sq + knight_dirs[i];
                if (next & 0x88) continue;
                if (state->board[next] == EMPTY || GET_COLOR(state->board[next]) == them) {
                    moves[count++] = (Move){ sq, next, 0 };
                }
            }
        } else if (type == BISHOP || type == ROOK || type == QUEEN) {
            int num_dirs = type == QUEEN ? 8 : 4;
            int *dirs = type == BISHOP ? bishop_dirs : (type == ROOK ? rook_dirs : queen_dirs);
            for (int i = 0; i < num_dirs; i++) {
                int next = sq;
                while (1) {
                    next += dirs[i];
                    if (next & 0x88) break;
                    if (state->board[next] == EMPTY) {
                        moves[count++] = (Move){ sq, next, 0 };
                    } else {
                        if (GET_COLOR(state->board[next]) == them) {
                            moves[count++] = (Move){ sq, next, 0 };
                        }
                        break;
                    }
                }
            }
        } else if (type == KING) {
            for (int i = 0; i < 8; i++) {
                int next = sq + queen_dirs[i];
                if (next & 0x88) continue;
                if (state->board[next] == EMPTY || GET_COLOR(state->board[next]) == them) {
                    moves[count++] = (Move){ sq, next, 0 };
                }
            }
            if (us == WHITE) {
                if ((state->castle & CASTLE_W_OO) && state->board[0x05] == EMPTY && state->board[0x06] == EMPTY &&
                    !is_attacked(state, 0x04, BLACK) && !is_attacked(state, 0x05, BLACK) && !is_attacked(state, 0x06, BLACK)) {
                    moves[count++] = (Move){ 0x04, 0x06, 0 };
                }
                if ((state->castle & CASTLE_W_OOO) && state->board[0x03] == EMPTY && state->board[0x02] == EMPTY && state->board[0x01] == EMPTY &&
                    !is_attacked(state, 0x04, BLACK) && !is_attacked(state, 0x03, BLACK) && !is_attacked(state, 0x02, BLACK)) {
                    moves[count++] = (Move){ 0x04, 0x02, 0 };
                }
            } else {
                if ((state->castle & CASTLE_B_OO) && state->board[0x75] == EMPTY && state->board[0x76] == EMPTY &&
                    !is_attacked(state, 0x74, WHITE) && !is_attacked(state, 0x75, WHITE) && !is_attacked(state, 0x76, WHITE)) {
                    moves[count++] = (Move){ 0x74, 0x76, 0 };
                }
                if ((state->castle & CASTLE_B_OOO) && state->board[0x73] == EMPTY && state->board[0x72] == EMPTY && state->board[0x71] == EMPTY &&
                    !is_attacked(state, 0x74, WHITE) && !is_attacked(state, 0x73, WHITE) && !is_attacked(state, 0x72, WHITE)) {
                    moves[count++] = (Move){ 0x74, 0x72, 0 };
                }
            }
        }
    }
    return count;
}

void make_move(const BoardState *src, BoardState *dst, Move m) {
    *dst = *src;
    int p = dst->board[m.from];
    int type = GET_TYPE(p);
    int color = GET_COLOR(p);

    if (type == PAWN && m.to == dst->ep) {
        dst->board[color == WHITE ? m.to - 16 : m.to + 16] = EMPTY;
    }

    dst->ep = -1;
    if (type == PAWN && abs(m.to - m.from) == 32) {
        dst->ep = (m.from + m.to) / 2;
    }

    if (type == KING) {
        if (m.to - m.from == 2) {
            if (color == WHITE) { dst->board[0x05] = dst->board[0x07]; dst->board[0x07] = EMPTY; }
            else { dst->board[0x75] = dst->board[0x77]; dst->board[0x77] = EMPTY; }
        } else if (m.to - m.from == -2) {
            if (color == WHITE) { dst->board[0x03] = dst->board[0x00]; dst->board[0x00] = EMPTY; }
            else { dst->board[0x73] = dst->board[0x70]; dst->board[0x70] = EMPTY; }
        }
    }

    if (type == KING) {
        dst->castle &= (color == WHITE) ? ~(CASTLE_W_OO | CASTLE_W_OOO) : ~(CASTLE_B_OO | CASTLE_B_OOO);
    }
    if (m.from == 0x00 || m.to == 0x00) dst->castle &= ~CASTLE_W_OOO;
    if (m.from == 0x07 || m.to == 0x07) dst->castle &= ~CASTLE_W_OO;
    if (m.from == 0x70 || m.to == 0x70) dst->castle &= ~CASTLE_B_OOO;
    if (m.from == 0x77 || m.to == 0x77) dst->castle &= ~CASTLE_B_OO;

    dst->board[m.from] = EMPTY;
    dst->board[m.to] = m.promo ? (color | m.promo) : p;

    dst->side = (dst->side == WHITE) ? BLACK : WHITE;
    if (type == PAWN || dst->board[m.to] != EMPTY) dst->halfmove = 0;
    else dst->halfmove++;
    if (dst->side == WHITE) dst->fullmove++;
}

int get_legal_moves(const BoardState *state, Move *legal_moves) {
    Move pseudo[256];
    int count = generate_moves(state, pseudo);
    int legal_count = 0;
    for (int i = 0; i < count; i++) {
        BoardState temp;
        make_move(state, &temp, pseudo[i]);
        if (!in_check(&temp, state->side)) {
            legal_moves[legal_count++] = pseudo[i];
        }
    }
    return legal_count;
}

/* Move Translations (UCI <-> Coordinates <-> PGN Standard) */
void move_to_uci(Move m, char *buf) {
    sprintf(buf, "%c%d%c%d", 'a' + (m.from & 7), (m.from >> 4) + 1, 'a' + (m.to & 7), (m.to >> 4) + 1);
    if (m.promo) {
        char p = 'q';
        if (m.promo == ROOK) p = 'r';
        else if (m.promo == BISHOP) p = 'b';
        else if (m.promo == KNIGHT) p = 'n';
        sprintf(buf + 4, "%c", p);
    }
}

int parse_uci_move(const char *str, const BoardState *state, Move *out_move) {
    if (strlen(str) < 4) return 0;
    int f_file = str[0] - 'a', f_rank = str[1] - '1';
    int t_file = str[2] - 'a', t_rank = str[3] - '1';
    int from = f_rank * 16 + f_file, to = t_rank * 16 + t_file;

    int promo = 0;
    if (strlen(str) >= 5) {
        if (str[4] == 'q') promo = QUEEN;
        else if (str[4] == 'r') promo = ROOK;
        else if (str[4] == 'b') promo = BISHOP;
        else if (str[4] == 'n') promo = KNIGHT;
    }

    Move legal[256];
    int count = get_legal_moves(state, legal);
    for (int i = 0; i < count; i++) {
        if (legal[i].from == from && legal[i].to == to) {
            if (!promo || legal[i].promo == promo) {
                *out_move = legal[i];
                return 1;
            }
        }
    }
    return 0;
}

void get_san(const BoardState *before, Move m, char *buf) {
    BoardState after;
    make_move(before, &after, m);
    int type = GET_TYPE(before->board[m.from]);

    if (type == KING && abs(m.to - m.from) == 2) {
        strcpy(buf, m.to > m.from ? "O-O" : "O-O-O");
    } else {
        int p = 0;
        if (type != PAWN) {
            char code[] = { ' ', ' ', 'N', 'B', 'R', 'Q', 'K' };
            buf[p++] = code[type];

            // Resolve piece ambiguity
            Move legal[256];
            int count = get_legal_moves(before, legal);
            int dup_file = 0, dup_rank = 0, matches = 0;
            for (int i = 0; i < count; i++) {
                if (legal[i].from != m.from && legal[i].to == m.to && GET_TYPE(before->board[legal[i].from]) == type) {
                    matches++;
                    if ((legal[i].from & 7) == (m.from & 7)) dup_file = 1;
                    if ((legal[i].from >> 4) == (m.from >> 4)) dup_rank = 1;
                }
            }
            if (matches > 0) {
                if (!dup_file) buf[p++] = 'a' + (m.from & 7);
                else if (!dup_rank) buf[p++] = '1' + (m.from >> 4);
                else {
                    buf[p++] = 'a' + (m.from & 7);
                    buf[p++] = '1' + (m.from >> 4);
                }
            }
        } else if (before->board[m.to] != EMPTY || m.to == before->ep) {
            buf[p++] = 'a' + (m.from & 7);
        }

        if (before->board[m.to] != EMPTY || (type == PAWN && m.to == before->ep)) buf[p++] = 'x';
        buf[p++] = 'a' + (m.to & 7);
        buf[p++] = '1' + (m.to >> 4);

        if (m.promo) {
            buf[p++] = '=';
            char code[] = { ' ', ' ', 'N', 'B', 'R', 'Q', 'K' };
            buf[p++] = code[m.promo];
        }
        buf[p] = '\0';
    }

    if (in_check(&after, after.side)) {
        Move opp_legal[256];
        strcat(buf, get_legal_moves(&after, opp_legal) == 0 ? "#" : "+");
    }
}

/* GUI TUI Render Architecture */
void get_sq_colors(int f, int r, char *out) {
    int sq = r * 16 + f;
    const char *bg = ((f + r) % 2 != 0) ? BG_LIGHT : BG_DARK;

    if (sq == selected_sq) bg = BG_SELECTED;
    else if (legal_destinations[sq]) bg = BG_LEGAL;

    if (f == cursor_file && r == cursor_rank) bg = BG_CURSOR;
    strcpy(out, bg);
}

const char* get_piece_glyph(int p) {
    int type = GET_TYPE(p);
    if (type == EMPTY) return " ";
    const char* glyphs[] = { " ", "♟", "♞", "♝", "♜", "♛", "♚" };
    return glyphs[type];
}

void get_sidebar_line(int line, const BoardState *state, char *buf, int max) {
    buf[0] = '\0';
    switch (line) {
        case 0:  snprintf(buf, max, "  CHESS TERMINAL GUI (UCI CLIENT)"); break;
        case 1:  snprintf(buf, max, "  ==========================================="); break;
        case 2: {
            int checked = in_check(state, state->side);
            Move legal[256];
            int count = get_legal_moves(state, legal);
            const char *side = state->side == WHITE ? "White" : "Black";
            if (count == 0) {
                if (checked) snprintf(buf, max, "  STATUS: [ MATE! %s is Defeated ]", side);
                else snprintf(buf, max, "  STATUS: [ STALEMATE! Game is Draw ]");
            } else if (checked) {
                snprintf(buf, max, "  STATUS: [ CHECK! %s King under Attack ]", side);
            } else {
                snprintf(buf, max, "  STATUS: [ %s's Turn ]", side);
            }
            break;
        }
        case 3: {
            const char *modes[] = { "Human (White) vs Engine", "Human (Black) vs Engine", "Local PvP", "Engine vs Engine" };
            snprintf(buf, max, "  MODE:   [ %s ] ([m] Cycle)", modes[settings.mode]);
            break;
        }
        case 4:  snprintf(buf, max, "  ENGINE: [ %s ] (%s)", settings.engine_path, engine_pid > 0 ? "OK" : "NO_ENG"); break;
        case 5:  snprintf(buf, max, "  "); break;
        case 6:  snprintf(buf, max, "  ENGINE CONSTRAINTS:"); break;
        case 7:  snprintf(buf, max, "  -------------------------------------------"); break;
        case 8:  snprintf(buf, max, "  [d] Max Depth: %s", settings.max_depth == 0 ? "Unlimited" : (sprintf(buf+128, "%d", settings.max_depth), buf+128)); break;
        case 9:  snprintf(buf, max, "  [n] Max Nodes: %s", settings.max_nodes == 0 ? "Unlimited" : (sprintf(buf+128, "%d", settings.max_nodes), buf+128)); break;
        case 10: snprintf(buf, max, "  [t] Max Time:  %s", settings.max_time_ms == 0 ? "Unlimited" : (sprintf(buf+128, "%d ms", settings.max_time_ms), buf+128)); break;
        case 11: snprintf(buf, max, "  "); break;
        case 12: snprintf(buf, max, "  PGN HISTORY:"); break;
        case 13: snprintf(buf, max, "  -------------------------------------------"); break;
        case 14:
        case 15:
        case 16: {
            int row = line - 14;
            int total_moves = (history_count + 1) / 2;
            int moves_per_row = 4;
            int start_idx = (total_moves > 12) ? ((total_moves - 12) / moves_per_row) * moves_per_row + 1 : 1;
            start_idx += row * moves_per_row;

            strcpy(buf, "  ");
            for (int i = 0; i < moves_per_row; i++) {
                int turn = start_idx + i;
                int w_idx = (turn - 1) * 2;
                int b_idx = w_idx + 1;
                if (w_idx < history_count) {
                    char temp[32], san_w[16];
                    get_san(&history[w_idx], move_history[w_idx], san_w);
                    if (b_idx < history_count) {
                        char san_b[16];
                        get_san(&history[b_idx], move_history[b_idx], san_b);
                        snprintf(temp, sizeof(temp), "%d.%s %s  ", turn, san_w, san_b);
                    } else {
                        snprintf(temp, sizeof(temp), "%d.%s ... ", turn, san_w);
                    }
                    strcat(buf, temp);
                }
            }
            break;
        }
        case 17: snprintf(buf, max, "  "); break;
        case 18: snprintf(buf, max, "  KEYS: [Arrows] Navigate   [Space/Enter] Interact"); break;
        case 19: snprintf(buf, max, "  [u] Undo Move   [r] Reset Game   [q] Terminate"); break;
        default: break;
    }
}

void draw_screen(const BoardState *state) {
    printf("\033[H"); // Home cursor (smooth double buffer emulation)
    char sidebar[256];

    get_sidebar_line(0, state, sidebar, sizeof(sidebar));
    printf("     A   B   C   D   E   F   G   H%s\n", sidebar);

    int s_idx = 1;
    for (int r = 7; r >= 0; r--) {
        // Line 1 of Rank (Pieces display)
        printf(" %d ", r + 1);
        for (int f = 0; f < 8; f++) {
            char bg[64];
            get_sq_colors(f, r, bg);
            int p = state->board[r * 16 + f];
            const char *glyph = get_piece_glyph(p);
            const char *fg = GET_COLOR(p) == WHITE ? FG_WHITE : FG_BLACK;
            if (p != EMPTY) printf("%s  %s%s \033[0m", bg, fg, glyph);
            else printf("%s    \033[0m", bg);
        }
        printf(" %d", r + 1);
        get_sidebar_line(s_idx++, state, sidebar, sizeof(sidebar));
        printf("%s\n", sidebar);

        // Line 2 of Rank (Padding squares)
        printf("   ");
        for (int f = 0; f < 8; f++) {
            char bg[64];
            get_sq_colors(f, r, bg);
            printf("%s    \033[0m", bg);
        }
        printf("   ");
        get_sidebar_line(s_idx++, state, sidebar, sizeof(sidebar));
        printf("%s\n", sidebar);
    }

    get_sidebar_line(s_idx++, state, sidebar, sizeof(sidebar));
    printf("     A   B   C   D   E   F   G   H%s\n", sidebar);

    while (s_idx <= 19) {
        get_sidebar_line(s_idx++, state, sidebar, sizeof(sidebar));
        printf("                                     %s\n", sidebar);
    }
    fflush(stdout);
}

/* User Interactive Controls */
void cycle_depth() {
    int arr[] = { 0, 5, 8, 10, 12, 15, 20 };
    int n = sizeof(arr) / sizeof(arr[0]), idx = 0;
    for (int i = 0; i < n; i++) if (settings.max_depth == arr[i]) { idx = (i + 1) % n; break; }
    settings.max_depth = arr[idx];
}

void cycle_nodes() {
    int arr[] = { 0, 10000, 50000, 100000, 500000, 1000000 };
    int n = sizeof(arr) / sizeof(arr[0]), idx = 0;
    for (int i = 0; i < n; i++) if (settings.max_nodes == arr[i]) { idx = (i + 1) % n; break; }
    settings.max_nodes = arr[idx];
}

void cycle_time() {
    int arr[] = { 0, 100, 250, 500, 1000, 2000, 5000 };
    int n = sizeof(arr) / sizeof(arr[0]), idx = 0;
    for (int i = 0; i < n; i++) if (settings.max_time_ms == arr[i]) { idx = (i + 1) % n; break; }
    settings.max_time_ms = arr[idx];
}

void cycle_mode() {
    settings.mode = (settings.mode + 1) % 4;
    selected_sq = -1;
    memset(legal_destinations, 0, sizeof(legal_destinations));
}

void execute_undo() {
    int steps = (settings.mode == 2) ? 1 : 2;
    if (history_count >= steps) {
        history_count -= steps;
        current_state = history[history_count];
    } else if (history_count > 0) {
        history_count--;
        current_state = history[history_count];
    }
    selected_sq = -1;
    memset(legal_destinations, 0, sizeof(legal_destinations));
}

int is_engine_turn() {
    if (settings.mode == 0 && current_state.side == BLACK) return 1;
    if (settings.mode == 1 && current_state.side == WHITE) return 1;
    if (settings.mode == 3) return 1;
    return 0;
}

int main() {
    const char *path = find_engine();
    strcpy(settings.engine_path, path);
    settings.max_depth = 10;
    settings.max_nodes = 0;
    settings.max_time_ms = 1000;
    settings.mode = 0;

    start_engine(settings.engine_path);
    init_uci_connection();
    init_board(&current_state);

    enable_raw_mode();
    printf("\033[2J"); // Initial screen flush

    while (1) {
        draw_screen(&current_state);

        // Run UCI Calculation on engine turns
        if (is_engine_turn() && !engine_thinking) {
            Move legal[256];
            if (get_legal_moves(&current_state, legal) > 0) {
                engine_thinking = 1;
                char pos_cmd[8192] = "position startpos moves";
                for (int i = 0; i < history_count; i++) {
                    char step[10];
                    move_to_uci(move_history[i], step);
                    strcat(pos_cmd, " ");
                    strcat(pos_cmd, step);
                }
                strcat(pos_cmd, "\n");
                send_to_engine(pos_cmd);

                char go_cmd[256] = "go";
                if (settings.max_time_ms > 0) sprintf(go_cmd + strlen(go_cmd), " movetime %d", settings.max_time_ms);
                if (settings.max_depth > 0) sprintf(go_cmd + strlen(go_cmd), " depth %d", settings.max_depth);
                if (settings.max_nodes > 0) sprintf(go_cmd + strlen(go_cmd), " nodes %d", settings.max_nodes);
                strcat(go_cmd, "\n");
                send_to_engine(go_cmd);
            }
        }

        // Standard Engine Output Poller
        if (engine_thinking) {
            char line[512];
            while (read_engine_line(line, sizeof(line))) {
                if (strncmp(line, "bestmove", 8) == 0) {
                    char mv[16];
                    sscanf(line, "bestmove %s", mv);
                    Move m;
                    if (parse_uci_move(mv, &current_state, &m)) {
                        history[history_count] = current_state;
                        move_history[history_count] = m;
                        history_count++;

                        BoardState next;
                        make_move(&current_state, &next, m);
                        current_state = next;
                    }
                    engine_thinking = 0;
                    break;
                }
            }
        }

        // Non-blocking terminal Keyboard Input
        int key = read_key();
        if (key != 0) {
            if (key == 'q' || key == 'Q') break;
            if (key == 'r' || key == 'R') {
                init_board(&current_state);
                history_count = 0;
                selected_sq = -1;
                memset(legal_destinations, 0, sizeof(legal_destinations));
                engine_thinking = 0;
            } else if (key == 'u' || key == 'U') {
                execute_undo();
                engine_thinking = 0;
            } else if (key == 'd' || key == 'D') {
                cycle_depth();
            } else if (key == 'n' || key == 'N') {
                cycle_nodes();
            } else if (key == 't' || key == 'T') {
                cycle_time();
            } else if (key == 'm' || key == 'M') {
                cycle_mode();
            } else if (!is_engine_turn()) {
                if (key == KEY_UP && cursor_rank < 7) cursor_rank++;
                else if (key == KEY_DOWN && cursor_rank > 0) cursor_rank--;
                else if (key == KEY_LEFT && cursor_file > 0) cursor_file--;
                else if (key == KEY_RIGHT && cursor_file < 7) cursor_file++;
                else if (key == KEY_ENTER || key == KEY_SPACE) {
                    int sq = cursor_rank * 16 + cursor_file;
                    if (selected_sq == -1) {
                        int piece = current_state.board[sq];
                        if (piece != EMPTY && GET_COLOR(piece) == current_state.side) {
                            selected_sq = sq;
                            memset(legal_destinations, 0, sizeof(legal_destinations));
                            Move legal[256];
                            int count = get_legal_moves(&current_state, legal);
                            for (int i = 0; i < count; i++) {
                                if (legal[i].from == sq) legal_destinations[legal[i].to] = 1;
                            }
                        }
                    } else {
                        if (sq == selected_sq) {
                            selected_sq = -1;
                            memset(legal_destinations, 0, sizeof(legal_destinations));
                        } else if (legal_destinations[sq]) {
                            Move legal[256];
                            int count = get_legal_moves(&current_state, legal);
                            Move matched;
                            int valid = 0;
                            for (int i = 0; i < count; i++) {
                                if (legal[i].from == selected_sq && legal[i].to == sq) {
                                    matched = legal[i];
                                    valid = 1;
                                    break;
                                }
                            }
                            if (valid) {
                                // Pawn Promotion Interface Trigger
                                if (GET_TYPE(current_state.board[selected_sq]) == PAWN && (cursor_rank == 0 || cursor_rank == 7)) {
                                    disable_raw_mode();
                                    printf("\033[H\033[2J\n\n   PAWN PROMOTION! Choose piece:\n");
                                    printf("   [Q] Queen   [R] Rook   [B] Bishop   [N] Knight\n\n");
                                    fflush(stdout);
                                    int p_choice = QUEEN;
                                    while (1) {
                                        char ch = getchar();
                                        if (ch == 'q' || ch == 'Q') { p_choice = QUEEN; break; }
                                        if (ch == 'r' || ch == 'R') { p_choice = ROOK; break; }
                                        if (ch == 'b' || ch == 'B') { p_choice = BISHOP; break; }
                                        if (ch == 'n' || ch == 'N') { p_choice = KNIGHT; break; }
                                    }
                                    enable_raw_mode();
                                    matched.promo = p_choice;
                                }

                                history[history_count] = current_state;
                                move_history[history_count] = matched;
                                history_count++;

                                BoardState next;
                                make_move(&current_state, &next, matched);
                                current_state = next;

                                selected_sq = -1;
                                memset(legal_destinations, 0, sizeof(legal_destinations));
                            }
                        } else {
                            // Quick Selector Switch
                            int piece = current_state.board[sq];
                            if (piece != EMPTY && GET_COLOR(piece) == current_state.side) {
                                selected_sq = sq;
                                memset(legal_destinations, 0, sizeof(legal_destinations));
                                Move legal[256];
                                int count = get_legal_moves(&current_state, legal);
                                for (int i = 0; i < count; i++) {
                                    if (legal[i].from == sq) legal_destinations[legal[i].to] = 1;
                                }
                            }
                        }
                    }
                }
            }
        }
        usleep(15000); // Guard to preserve low CPU execution metrics
    }

    disable_raw_mode();
    if (engine_pid > 0) kill(engine_pid, SIGTERM);
    return 0;
}
