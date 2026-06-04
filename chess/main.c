#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>

#define ABS(x) ((x) < 0 ? -(x) : (x))
#define COLOR(p) ((p) > 0 ? 1 : ((p) < 0 ? -1 : 0))

// Chess board definitions
enum { EMPTY = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };

typedef struct {
    int from;
    int to;
    int promotion;
    int is_castling;
    int is_en_passant;
    int prev_castling_rights;
    int prev_ep_square;
    int captured_piece;
} Move;

typedef struct {
    int board[64];
    int turn;             // 1 = White, -1 = Black
    int castling_rights;  // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep_square;        // -1 if none, 0-63 otherwise
    int halfmove_clock;
    int fullmove_number;
    Move history[1024];
    int history_count;
} GameState;

typedef enum { LIMIT_TIME, LIMIT_DEPTH, LIMIT_NODES } LimitType;

// Global settings
LimitType current_limit_type = LIMIT_TIME;
int limit_value = 2000; // default 2000ms, 10 depth, or 50000 nodes

char engine_name[128] = "Searching...";
int engine_read_fd = -1;
int engine_write_fd = -1;
pid_t engine_pid = -1;
int engine_thinking = 0;

char engine_accumulation_buf[8192];
int engine_accumulation_len = 0;

struct termios orig_termios;

// Forward Declarations
void generate_moves(GameState *state, Move *moves, int *count);
int is_in_check(GameState *state, int color);
void make_move(GameState *state, Move m);
void undo_move(GameState *state);
void disable_raw_mode();

// Clean Exit
void handle_exit(int sig) {
    if (engine_pid > 0) {
        kill(engine_pid, SIGTERM);
    }
    disable_raw_mode();
    printf("\033[?25h\033[2J\033[H"); // Show cursor, clear screen
    exit(0);
}

// Terminal Raw Mode Control
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // Show cursor
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

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
                case 'A': return 'w'; // Up
                case 'B': return 's'; // Down
                case 'C': return 'd'; // Right
                case 'D': return 'a'; // Left
            }
        }
        return '\033';
    }
    return c;
}

// Chess Rules & Mechanics
void init_game(GameState *state) {
    int initial_board[64] = {
        -ROOK, -KNIGHT, -BISHOP, -QUEEN, -KING, -BISHOP, -KNIGHT, -ROOK,
        -PAWN, -PAWN,   -PAWN,   -PAWN,  -PAWN, -PAWN,   -PAWN,   -PAWN,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        PAWN,  PAWN,   PAWN,   PAWN,   PAWN,  PAWN,   PAWN,   PAWN,
        ROOK,  KNIGHT, BISHOP, QUEEN,  KING,  BISHOP, KNIGHT, ROOK
    };
    memcpy(state->board, initial_board, sizeof(initial_board));
    state->turn = 1;
    state->castling_rights = 15;
    state->ep_square = -1;
    state->halfmove_clock = 0;
    state->fullmove_number = 1;
    state->history_count = 0;
}

int get_piece(GameState *state, int r, int c) {
    if (r < 0 || r > 7 || c < 0 || c > 7) return 99; // Out of bounds indicator
    return state->board[r * 8 + c];
}

int is_square_attacked(GameState *state, int r, int c, int attacker_color) {
    // Pawn attacks
    int pawn_dir = (attacker_color == 1) ? 1 : -1;
    if (get_piece(state, r + pawn_dir, c - 1) == attacker_color * PAWN) return 1;
    if (get_piece(state, r + pawn_dir, c + 1) == attacker_color * PAWN) return 1;

    // Knight attacks
    int kn_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int kn_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        if (get_piece(state, r + kn_r[i], c + kn_c[i]) == attacker_color * KNIGHT) return 1;
    }

    // King attacks
    int k_r[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int k_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        if (get_piece(state, r + k_r[i], c + k_c[i]) == attacker_color * KING) return 1;
    }

    // Rook / Queen sliding attacks
    int r_dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += r_dirs[i][0]; nc += r_dirs[i][1];
            int p = get_piece(state, nr, nc);
            if (p == 99) break;
            if (p != EMPTY) {
                if (p == attacker_color * ROOK || p == attacker_color * QUEEN) return 1;
                break;
            }
        }
    }

    // Bishop / Queen sliding attacks
    int b_dirs[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += b_dirs[i][0]; nc += b_dirs[i][1];
            int p = get_piece(state, nr, nc);
            if (p == 99) break;
            if (p != EMPTY) {
                if (p == attacker_color * BISHOP || p == attacker_color * QUEEN) return 1;
                break;
            }
        }
    }
    return 0;
}

int is_in_check(GameState *state, int color) {
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (state->board[r * 8 + c] == color * KING) {
                return is_square_attacked(state, r, c, -color);
            }
        }
    }
    return 0;
}

void add_move(GameState *state, Move *moves, int *count, int from, int to, int promo, int castling, int ep) {
    GameState temp = *state;
    temp.board[to] = temp.board[from];
    temp.board[from] = EMPTY;
    if (ep) {
        temp.board[to + (state->turn == 1 ? 8 : -8)] = EMPTY;
    }
    if (castling) {
        if (to == 62) { temp.board[61] = temp.board[63]; temp.board[63] = EMPTY; }
        else if (to == 58) { temp.board[59] = temp.board[56]; temp.board[56] = EMPTY; }
        else if (to == 6) { temp.board[5] = temp.board[7]; temp.board[7] = EMPTY; }
        else if (to == 2) { temp.board[3] = temp.board[0]; temp.board[0] = EMPTY; }
    }
    if (promo) {
        temp.board[to] = state->turn * promo;
    }

    if (!is_in_check(&temp, state->turn)) {
        moves[*count].from = from;
        moves[*count].to = to;
        moves[*count].promotion = promo;
        moves[*count].is_castling = castling;
        moves[*count].is_en_passant = ep;
        moves[*count].prev_castling_rights = state->castling_rights;
        moves[*count].prev_ep_square = state->ep_square;
        moves[*count].captured_piece = state->board[to];
        if (ep) moves[*count].captured_piece = state->turn * -PAWN;
        (*count)++;
    }
}

void generate_moves(GameState *state, Move *moves, int *count) {
    *count = 0;
    int turn = state->turn;
    for (int from = 0; from < 64; from++) {
        int p = state->board[from];
        if (p == EMPTY || COLOR(p) != turn) continue;

        int r = from / 8;
        int c = from % 8;
        int type = ABS(p);

        if (type == PAWN) {
            int dir = (turn == 1) ? -1 : 1;
            int start_row = (turn == 1) ? 6 : 1;
            int promo_row = (turn == 1) ? 0 : 7;

            // Forward steps
            int next_r = r + dir;
            if (next_r >= 0 && next_r <= 7) {
                if (get_piece(state, next_r, c) == EMPTY) {
                    if (next_r == promo_row) {
                        add_move(state, moves, count, from, next_r * 8 + c, QUEEN, 0, 0);
                        add_move(state, moves, count, from, next_r * 8 + c, ROOK, 0, 0);
                        add_move(state, moves, count, from, next_r * 8 + c, BISHOP, 0, 0);
                        add_move(state, moves, count, from, next_r * 8 + c, KNIGHT, 0, 0);
                    } else {
                        add_move(state, moves, count, from, next_r * 8 + c, 0, 0, 0);
                    }
                    if (r == start_row && get_piece(state, r + 2 * dir, c) == EMPTY) {
                        add_move(state, moves, count, from, (r + 2 * dir) * 8 + c, 0, 0, 0);
                    }
                }
                // Captures
                int cols[] = {c - 1, c + 1};
                for (int i = 0; i < 2; i++) {
                    int nc = cols[i];
                    if (nc >= 0 && nc <= 7) {
                        int np = get_piece(state, next_r, nc);
                        if (np != EMPTY && np != 99 && COLOR(np) == -turn) {
                            if (next_r == promo_row) {
                                add_move(state, moves, count, from, next_r * 8 + nc, QUEEN, 0, 0);
                            } else {
                                add_move(state, moves, count, from, next_r * 8 + nc, 0, 0, 0);
                            }
                        }
                        if (next_r * 8 + nc == state->ep_square) {
                            add_move(state, moves, count, from, next_r * 8 + nc, 0, 0, 1);
                        }
                    }
                }
            }
        } else if (type == KNIGHT) {
            int kn_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
            int kn_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
            for (int i = 0; i < 8; i++) {
                int nr = r + kn_r[i], nc = c + kn_c[i];
                int np = get_piece(state, nr, nc);
                if (np != 99 && (np == EMPTY || COLOR(np) == -turn)) {
                    add_move(state, moves, count, from, nr * 8 + nc, 0, 0, 0);
                }
            }
        } else if (type == BISHOP || type == QUEEN) {
            int b_dirs[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
            for (int i = 0; i < 4; i++) {
                int nr = r, nc = c;
                while (1) {
                    nr += b_dirs[i][0]; nc += b_dirs[i][1];
                    int np = get_piece(state, nr, nc);
                    if (np == 99) break;
                    if (np == EMPTY) {
                        add_move(state, moves, count, from, nr * 8 + nc, 0, 0, 0);
                    } else {
                        if (COLOR(np) == -turn) add_move(state, moves, count, from, nr * 8 + nc, 0, 0, 0);
                        break;
                    }
                }
            }
        }
        if (type == ROOK || type == QUEEN) {
            int r_dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (int i = 0; i < 4; i++) {
                int nr = r, nc = c;
                while (1) {
                    nr += r_dirs[i][0]; nc += r_dirs[i][1];
                    int np = get_piece(state, nr, nc);
                    if (np == 99) break;
                    if (np == EMPTY) {
                        add_move(state, moves, count, from, nr * 8 + nc, 0, 0, 0);
                    } else {
                        if (COLOR(np) == -turn) add_move(state, moves, count, from, nr * 8 + nc, 0, 0, 0);
                        break;
                    }
                }
            }
        } else if (type == KING) {
            int k_r[] = {-1, -1, -1, 0, 0, 1, 1, 1};
            int k_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
            for (int i = 0; i < 8; i++) {
                int nr = r + k_r[i], nc = c + k_c[i];
                int np = get_piece(state, nr, nc);
                if (np != 99 && (np == EMPTY || COLOR(np) == -turn)) {
                    add_move(state, moves, count, from, nr * 8 + nc, 0, 0, 0);
                }
            }
            // Castling Rights Check
            if (turn == 1) {
                if ((state->castling_rights & 1) && !is_in_check(state, 1)) {
                    if (get_piece(state, 7, 5) == EMPTY && get_piece(state, 7, 6) == EMPTY) {
                        if (!is_square_attacked(state, 7, 5, -1) && !is_square_attacked(state, 7, 6, -1)) {
                            add_move(state, moves, count, 60, 62, 0, 1, 0);
                        }
                    }
                }
                if ((state->castling_rights & 2) && !is_in_check(state, 1)) {
                    if (get_piece(state, 7, 3) == EMPTY && get_piece(state, 7, 2) == EMPTY && get_piece(state, 7, 1) == EMPTY) {
                        if (!is_square_attacked(state, 7, 3, -1) && !is_square_attacked(state, 7, 2, -1)) {
                            add_move(state, moves, count, 60, 58, 0, 1, 0);
                        }
                    }
                }
            } else {
                if ((state->castling_rights & 4) && !is_in_check(state, -1)) {
                    if (get_piece(state, 0, 5) == EMPTY && get_piece(state, 0, 6) == EMPTY) {
                        if (!is_square_attacked(state, 0, 5, 1) && !is_square_attacked(state, 0, 6, 1)) {
                            add_move(state, moves, count, 4, 6, 0, 1, 0);
                        }
                    }
                }
                if ((state->castling_rights & 8) && !is_in_check(state, -1)) {
                    if (get_piece(state, 0, 3) == EMPTY && get_piece(state, 0, 2) == EMPTY && get_piece(state, 0, 1) == EMPTY) {
                        if (!is_square_attacked(state, 0, 3, 1) && !is_square_attacked(state, 0, 2, 1)) {
                            add_move(state, moves, count, 4, 2, 0, 1, 0);
                        }
                    }
                }
            }
        }
    }
}

void make_move(GameState *state, Move m) {
    int turn = state->turn;
    int p = state->board[m.from];

    state->history[state->history_count++] = m;
    state->board[m.to] = p;
    state->board[m.from] = EMPTY;

    if (m.promotion) {
        state->board[m.to] = turn * m.promotion;
    }
    if (m.is_en_passant) {
        state->board[m.to + (turn == 1 ? 8 : -8)] = EMPTY;
    }
    if (m.is_castling) {
        if (m.to == 62) { state->board[61] = state->board[63]; state->board[63] = EMPTY; }
        else if (m.to == 58) { state->board[59] = state->board[56]; state->board[56] = EMPTY; }
        else if (m.to == 6) { state->board[5] = state->board[7]; state->board[7] = EMPTY; }
        else if (m.to == 2) { state->board[3] = state->board[0]; state->board[0] = EMPTY; }
    }

    // Castling Rights Update
    if (m.from == 60) state->castling_rights &= ~3;
    if (m.from == 4) state->castling_rights &= ~12;
    if (m.from == 56 || m.to == 56) state->castling_rights &= ~2;
    if (m.from == 63 || m.to == 63) state->castling_rights &= ~1;
    if (m.from == 0 || m.to == 0) state->castling_rights &= ~8;
    if (m.from == 7 || m.to == 7) state->castling_rights &= ~4;

    state->ep_square = -1;
    if (ABS(p) == PAWN && ABS(m.to - m.from) == 16) {
        state->ep_square = m.from + (turn == 1 ? -8 : 8);
    }

    if (turn == -1) state->fullmove_number++;
    state->turn = -turn;
}

void undo_move(GameState *state) {
    if (state->history_count == 0) return;
    Move m = state->history[--state->history_count];
    int prev_turn = -state->turn;

    state->board[m.from] = state->board[m.to];
    state->board[m.to] = m.captured_piece;

    if (m.promotion) {
        state->board[m.from] = prev_turn * PAWN;
    }
    if (m.is_en_passant) {
        state->board[m.to + (prev_turn == 1 ? 8 : -8)] = prev_turn * PAWN;
        state->board[m.to] = EMPTY;
    }
    if (m.is_castling) {
        if (m.to == 62) { state->board[63] = state->board[61]; state->board[61] = EMPTY; }
        else if (m.to == 58) { state->board[56] = state->board[59]; state->board[59] = EMPTY; }
        else if (m.to == 6) { state->board[7] = state->board[5]; state->board[5] = EMPTY; }
        else if (m.to == 2) { state->board[0] = state->board[3]; state->board[3] = EMPTY; }
    }

    state->castling_rights = m.prev_castling_rights;
    state->ep_square = m.prev_ep_square;
    state->turn = prev_turn;
    if (prev_turn == -1) state->fullmove_number--;
}

// UCI Engine I/O Implementation
int start_engine(const char *path) {
    int to_engine[2], from_engine[2];
    if (pipe(to_engine) < 0 || pipe(from_engine) < 0) return 0;

    engine_pid = fork();
    if (engine_pid < 0) return 0;

    if (engine_pid == 0) {
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[0]); close(to_engine[1]);
        close(from_engine[0]); close(from_engine[1]);
        char *argv[] = {(char*)path, NULL};
        execvp(path, argv);
        exit(1);
    }

    close(to_engine[0]); close(from_engine[1]);
    engine_read_fd = from_engine[0];
    engine_write_fd = to_engine[1];

    int flags = fcntl(engine_read_fd, F_GETFL, 0);
    fcntl(engine_read_fd, F_SETFL, flags | O_NONBLOCK);
    return 1;
}

int check_engine_output(int fd, char *line_out, int max_len) {
    char tmp[256];
    int n = read(fd, tmp, sizeof(tmp) - 1);
    if (n > 0) {
        tmp[n] = '\0';
        if (engine_accumulation_len + n < (int)sizeof(engine_accumulation_buf)) {
            memcpy(engine_accumulation_buf + engine_accumulation_len, tmp, n);
            engine_accumulation_len += n;
            engine_accumulation_buf[engine_accumulation_len] = '\0';
        }
    }

    for (int i = 0; i < engine_accumulation_len; i++) {
        if (engine_accumulation_buf[i] == '\n') {
            int len = i;
            if (len >= max_len) len = max_len - 1;
            memcpy(line_out, engine_accumulation_buf, len);
            line_out[len] = '\0';
            if (len > 0 && line_out[len - 1] == '\r') line_out[len - 1] = '\0';

            int remaining = engine_accumulation_len - (i + 1);
            memmove(engine_accumulation_buf, engine_accumulation_buf + i + 1, remaining);
            engine_accumulation_len = remaining;
            engine_accumulation_buf[engine_accumulation_len] = '\0';
            return 1;
        }
    }
    return 0;
}

void move_to_uci(Move m, char *buf) {
    int f_col = m.from % 8, f_row = 8 - (m.from / 8);
    int t_col = m.to % 8, t_row = 8 - (m.to / 8);
    if (m.promotion) {
        char p_char = 'q';
        if (m.promotion == ROOK) p_char = 'r';
        else if (m.promotion == BISHOP) p_char = 'b';
        else if (m.promotion == KNIGHT) p_char = 'n';
        sprintf(buf, "%c%d%c%d%c", 'a' + f_col, f_row, 'a' + t_col, t_row, p_char);
    } else {
        sprintf(buf, "%c%d%c%d", 'a' + f_col, f_row, 'a' + t_col, t_row);
    }
}

int parse_uci_move(GameState *state, const char *str, Move *out_move) {
    Move legal_moves[256];
    int count;
    generate_moves(state, legal_moves, &count);
    for (int i = 0; i < count; i++) {
        char uci_buf[16];
        move_to_uci(legal_moves[i], uci_buf);
        if (strcmp(str, uci_buf) == 0) {
            *out_move = legal_moves[i];
            return 1;
        }
    }
    return 0;
}

void init_uci_communication() {
    write(engine_write_fd, "uci\n", 4);
    time_t start = time(NULL);
    char line[512];
    while (time(NULL) - start < 3) {
        if (check_engine_output(engine_read_fd, line, sizeof(line))) {
            if (strncmp(line, "id name ", 8) == 0) {
                strncpy(engine_name, line + 8, sizeof(engine_name) - 1);
                engine_name[sizeof(engine_name) - 1] = '\0';
            }
            if (strcmp(line, "uciok") == 0) break;
        }
        usleep(10000);
    }
    write(engine_write_fd, "isready\n", 8);
    start = time(NULL);
    while (time(NULL) - start < 3) {
        if (check_engine_output(engine_read_fd, line, sizeof(line))) {
            if (strcmp(line, "readyok") == 0) return;
        }
        usleep(10000);
    }
}

void trigger_engine_calculation(GameState *state) {
    char cmd[4096] = "position startpos moves";
    GameState temp;
    init_game(&temp);

    for (int i = 0; i < state->history_count; i++) {
        char uci_move[16];
        move_to_uci(state->history[i], uci_move);
        strcat(cmd, " ");
        strcat(cmd, uci_move);
    }
    strcat(cmd, "\n");
    write(engine_write_fd, cmd, strlen(cmd));

    char go_cmd[128];
    if (current_limit_type == LIMIT_TIME) {
        sprintf(go_cmd, "go movetime %d\n", limit_value);
    } else if (current_limit_type == LIMIT_DEPTH) {
        sprintf(go_cmd, "go depth %d\n", limit_value);
    } else {
        sprintf(go_cmd, "go nodes %d\n", limit_value);
    }
    write(engine_write_fd, go_cmd, strlen(go_cmd));
    engine_thinking = 1;
}

// PGN Generation
void get_san(GameState *state_before, Move m, char *buf) {
    if (m.is_castling) {
        if (m.to == 62 || m.to == 6) strcpy(buf, "O-O");
        else strcpy(buf, "O-O-O");
        return;
    }
    int piece = ABS(state_before->board[m.from]);
    int is_cap = (state_before->board[m.to] != EMPTY) || m.is_en_passant;
    int t_col = m.to % 8, t_row = 8 - (m.to / 8);
    int f_col = m.from % 8;

    int idx = 0;
    if (piece == PAWN) {
        if (is_cap) {
            buf[idx++] = 'a' + f_col;
            buf[idx++] = 'x';
        }
        buf[idx++] = 'a' + t_col;
        buf[idx++] = '1' + (t_row - 1);
        if (m.promotion) {
            buf[idx++] = '=';
            buf[idx++] = 'Q';
        }
    } else {
        char p_char = 'K';
        if (piece == KNIGHT) p_char = 'N';
        else if (piece == BISHOP) p_char = 'B';
        else if (piece == ROOK) p_char = 'R';
        else if (piece == QUEEN) p_char = 'Q';
        buf[idx++] = p_char;
        if (is_cap) buf[idx++] = 'x';
        buf[idx++] = 'a' + t_col;
        buf[idx++] = '1' + (t_row - 1);
    }
    buf[idx] = '\0';
}

void format_pgn(GameState *state, char lines[8][40]) {
    for (int i = 0; i < 8; i++) lines[i][0] = '\0';
    GameState temp;
    init_game(&temp);

    char pgn_buf[1024] = "";
    for (int i = 0; i < state->history_count; i++) {
        char san[16];
        get_san(&temp, state->history[i], san);
        char move_str[32];
        if (temp.turn == 1) {
            sprintf(move_str, "%d. %s ", temp.fullmove_number, san);
        } else {
            sprintf(move_str, "%s ", san);
        }
        strcat(pgn_buf, move_str);
        make_move(&temp, state->history[i]);
    }

    int pgn_idx = 0, total_len = strlen(pgn_buf);
    for (int i = 0; i < 8; i++) {
        if (pgn_idx >= total_len) break;
        int slice = 38;
        if (pgn_idx + slice > total_len) slice = total_len - pgn_idx;
        else {
            while (slice > 0 && pgn_buf[pgn_idx + slice] != ' ') slice--;
            if (slice == 0) slice = 38;
        }
        strncpy(lines[i], pgn_buf + pgn_idx, slice);
        lines[i][slice] = '\0';
        pgn_idx += slice;
        while (pgn_buf[pgn_idx] == ' ') pgn_idx++;
    }
}

// UI Terminal Rendering Style Definitions
const char* get_piece_symbol(int piece) {
    switch (ABS(piece)) {
        case PAWN:   return "♟";
        case KNIGHT: return "♞";
        case BISHOP: return "♝";
        case ROOK:   return "♜";
        case QUEEN:  return "♛";
        case KING:   return "♚";
        default:     return " ";
    }
}

const char* get_piece_fg(int piece) {
    if (piece > 0) return "\033[38;5;255m\033[1m"; // Bold white
    if (piece < 0) return "\033[38;5;234m\033[1m"; // Bold black
    return "";
}

void get_square_color(GameState *state, int r, int c, int cur_r, int cur_c, int sel_idx, Move *legal_moves, int legal_count, char *color_esc) {
    int idx = r * 8 + c;
    if (r == cur_r && c == cur_c) {
        strcpy(color_esc, "\033[48;5;201m"); // Pink Cursor
        return;
    }
    if (sel_idx == idx) {
        strcpy(color_esc, "\033[48;5;214m"); // Orange Select Accent
        return;
    }
    if (sel_idx != -1) {
        for (int i = 0; i < legal_count; i++) {
            if (legal_moves[i].from == sel_idx && legal_moves[i].to == idx) {
                strcpy(color_esc, "\033[48;5;117m"); // Sky Blue Legal target
                return;
            }
        }
    }
    if (state->history_count > 0) {
        Move last_m = state->history[state->history_count - 1];
        if (last_m.from == idx || last_m.to == idx) {
            strcpy(color_esc, "\033[48;5;186m"); // Lichess-style Gold Last Move
            return;
        }
    }
    if ((r + c) % 2 == 0) {
        strcpy(color_esc, "\033[48;5;230m"); // Light Cream Sq
    } else {
        strcpy(color_esc, "\033[48;5;101m"); // Dark Olive Sq
    }
}

void render_all(GameState *state, int cursor_r, int cursor_c, int selected_idx) {
    Move legal_moves[256];
    int legal_count = 0;
    if (selected_idx != -1) generate_moves(state, legal_moves, &legal_count);

    char pgn_lines[8][40];
    format_pgn(state, pgn_lines);

    Move all_moves[256];
    int total_legal_count;
    generate_moves(state, all_moves, &total_legal_count);
    int checkmate = (total_legal_count == 0 && is_in_check(state, state->turn));
    int stalemate = (total_legal_count == 0 && !is_in_check(state, state->turn));

    printf("\033[H"); // Home cursor position
    printf("\033[1;36m========================= CHESS TERMINAL GUI =========================\033[0m\033[K\n\n");

    for (int row_idx = -1; row_idx <= 16; row_idx++) {
        // --- CHESSBOARD RENDER ---
        if (row_idx == -1 || row_idx == 16) {
            printf("    a     b     c     d     e     f     g     h  ");
        } else {
            int r = row_idx / 2;
            int sr = row_idx % 2;
            if (sr == 0) printf(" %d ", 8 - r);
            else printf("   ");

            for (int c = 0; c < 8; c++) {
                char bg[64];
                get_square_color(state, r, c, cursor_r, cursor_c, selected_idx, legal_moves, legal_count, bg);
                int p = state->board[r * 8 + c];
                printf("%s", bg);
                if (sr == 0) printf("      ");
                else {
                    if (p != EMPTY) printf("  %s%s  ", get_piece_fg(p), get_piece_symbol(p));
                    else printf("      ");
                }
            }
            printf("\033[0m");
            if (sr == 0) printf(" %d", 8 - r);
            else printf("  ");
        }

        // --- DASHBOARD PANEL RENDER ---
        printf("   │ ");
        switch (row_idx + 1) {
            case 0:  printf("\033[1;33mGAME STATUS:\033[0m"); break;
            case 1:
                if (checkmate) printf("Active Turn  : \033[1;31mCHECKMATE! %s Wins\033[0m", (state->turn == 1) ? "Black" : "White");
                else if (stalemate) printf("Active Turn  : \033[1;33mSTALEMATE (Draw)\033[0m");
                else printf("Active Turn  : %s", (state->turn == 1) ? "\033[1;32mWhite (You)\033[0m" : "\033[1;35mBlack (Engine)\033[0m");
                break;
            case 2:  printf("Check Status : %s", is_in_check(state, state->turn) ? "\033[1;31mCHECK!\033[0m" : "Safe"); break;
            case 3:  printf("Move Number  : %d", state->fullmove_number); break;
            case 4:  printf("---------------------------------------"); break;
            case 5:  printf("\033[1;33mENGINE CONFIGURATION:\033[0m"); break;
            case 6:  printf("Name         : %s", engine_name); break;
            case 7:  printf("State        : %s", engine_thinking ? "\033[1;31mThinking...\033[0m" : "Idle"); break;
            case 8:  printf("Limit Type   : %s", (current_limit_type == LIMIT_TIME) ? "Move Time Out" : (current_limit_type == LIMIT_DEPTH) ? "Search Depth" : "Node Limit"); break;
            case 9:  printf("Value Target : %d %s", limit_value, (current_limit_type == LIMIT_TIME) ? "ms" : (current_limit_type == LIMIT_DEPTH) ? "plies" : "nodes"); break;
            case 10: printf("---------------------------------------"); break;
            case 11: printf("\033[1;33mPGN HISTORY:\033[0m"); break;
            case 12: printf(" %s", pgn_lines[0]); break;
            case 13: printf(" %s", pgn_lines[1]); break;
            case 14: printf(" %s", pgn_lines[2]); break;
            case 15: printf(" %s", pgn_lines[3]); break;
            case 16: printf(" %s", pgn_lines[4]); break;
            case 17: printf(" %s", pgn_lines[5]); break;
            default: break;
        }
        printf("\033[K\n");
    }

    printf("\n\033[1;36m============================= CONTROLS =============================\033[0m\033[K\n");
    printf(" [W/A/S/D] or [Arrows] Move Cursor  │  [Space/Enter] Select / Commit Move\n");
    printf(" [U]       Undo Moves (Dual-Turn)   │  [T]           Toggle TC Constraint\n");
    printf(" [+] / [-] Increment/Decrement TC   │  [Q] or [Esc]  Kill Engine & Exit\n");
    printf("\033[1;36m====================================================================\033[0m\033[K\n");
}

int main() {
    char engine_path[256] = "stockfish";
    printf("====================================================\n");
    printf("             CHESS TERMINAL GUI LAUNCHER           \n");
    printf("====================================================\n");
    printf("Default paths checked on macOS:\n");
    printf(" 1. /opt/homebrew/bin/stockfish\n");
    printf(" 2. /usr/local/bin/stockfish\n");
    printf(" 3. 'stockfish' (system PATH)\n\n");
    printf("Enter custom path OR press [Enter] to use default PATH: ");
    fflush(stdout);

    char input[256];
    if (fgets(input, sizeof(input), stdin)) {
        input[strcspn(input, "\n")] = 0;
        if (strlen(input) > 0) {
            strcpy(engine_path, input);
        } else {
            if (access("/opt/homebrew/bin/stockfish", X_OK) == 0) {
                strcpy(engine_path, "/opt/homebrew/bin/stockfish");
            } else if (access("/usr/local/bin/stockfish", X_OK) == 0) {
                strcpy(engine_path, "/usr/local/bin/stockfish");
            }
        }
    }

    if (!start_engine(engine_path)) {
        fprintf(stderr, "Critical Error: Engine process could not start up using route '%s'\n", engine_path);
        return 1;
    }

    signal(SIGINT, handle_exit);
    signal(SIGTERM, handle_exit);

    init_uci_communication();

    GameState state;
    init_game(&state);

    int cursor_r = 6, cursor_c = 4;
    int selected_idx = -1;

    enable_raw_mode();
    printf("\033[2J\033[H"); // Complete terminal clear

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[1].fd = engine_read_fd;
    fds[1].events = POLLIN;

    render_all(&state, cursor_r, cursor_c, selected_idx);

    while (1) {
        int ret = poll(fds, 2, 50); // Dynamic event-driven lock (50ms timeout)
        if (ret < 0) break;

        // 1. Process Engine Outputs asynchronously
        if (fds[1].revents & POLLIN) {
            char engine_line[512];
            while (check_engine_output(engine_read_fd, engine_line, sizeof(engine_line))) {
                if (strncmp(engine_line, "bestmove ", 9) == 0) {
                    char move_str[16];
                    sscanf(engine_line + 9, "%s", move_str);
                    Move m;
                    if (parse_uci_move(&state, move_str, &m)) {
                        make_move(&state, m);
                    }
                    engine_thinking = 0;
                    render_all(&state, cursor_r, cursor_c, selected_idx);
                }
            }
        }

        // 2. Process User Key Inputs
        if (fds[0].revents & POLLIN) {
            int key = read_key();
            if (key == 'q' || key == 'Q' || key == '\033') {
                break;
            }

            if (!engine_thinking) {
                if (key == 'w' || key == 'W') { if (cursor_r > 0) cursor_r--; }
                if (key == 's' || key == 'S') { if (cursor_r < 7) cursor_r++; }
                if (key == 'a' || key == 'A') { if (cursor_c > 0) cursor_c--; }
                if (key == 'd' || key == 'D') { if (cursor_c < 7) cursor_c++; }

                int cursor_idx = cursor_r * 8 + cursor_c;

                if (key == ' ' || key == '\n') {
                    if (selected_idx == -1) {
                        if (state.board[cursor_idx] > 0) {
                            selected_idx = cursor_idx;
                        }
                    } else {
                        if (cursor_idx == selected_idx) {
                            selected_idx = -1;
                        } else if (state.board[cursor_idx] > 0) {
                            selected_idx = cursor_idx; // Quick-switch target piece
                        } else {
                            Move legal_moves[256];
                            int count;
                            generate_moves(&state, legal_moves, &count);
                            int found = -1;
                            for (int i = 0; i < count; i++) {
                                if (legal_moves[i].from == selected_idx && legal_moves[i].to == cursor_idx) {
                                    found = i;
                                    break;
                                }
                            }
                            if (found != -1) {
                                make_move(&state, legal_moves[found]);
                                selected_idx = -1;
                                render_all(&state, cursor_r, cursor_c, selected_idx);
                                if (state.turn == -1) {
                                    trigger_engine_calculation(&state);
                                }
                            } else {
                                printf("\a"); // Play console bell on illegal move
                            }
                        }
                    }
                }
            }

            // Game Control Commands
            if (key == 'u' || key == 'U') {
                if (engine_thinking) {
                    write(engine_write_fd, "stop\n", 5);
                    engine_thinking = 0;
                }
                if (state.history_count >= 2) {
                    undo_move(&state);
                    undo_move(&state);
                } else if (state.history_count == 1) {
                    undo_move(&state);
                }
                selected_idx = -1;
            }

            if (key == 't' || key == 'T') {
                if (current_limit_type == LIMIT_TIME) {
                    current_limit_type = LIMIT_DEPTH;
                    limit_value = 10; // Plies
                } else if (current_limit_type == LIMIT_DEPTH) {
                    current_limit_type = LIMIT_NODES;
                    limit_value = 50000; // Nodes
                } else {
                    current_limit_type = LIMIT_TIME;
                    limit_value = 2000; // Milliseconds
                }
            }

            if (key == '+' || key == '=') {
                if (current_limit_type == LIMIT_TIME) limit_value += 500;
                else if (current_limit_type == LIMIT_DEPTH) limit_value += 1;
                else limit_value += 10000;
            }

            if (key == '-' || key == '_') {
                if (current_limit_type == LIMIT_TIME) { if (limit_value > 500) limit_value -= 500; }
                else if (current_limit_type == LIMIT_DEPTH) { if (limit_value > 1) limit_value -= 1; }
                else { if (limit_value > 10000) limit_value -= 10000; }
            }

            render_all(&state, cursor_r, cursor_c, selected_idx);
        }
    }

    handle_exit(0);
    return 0;
}
