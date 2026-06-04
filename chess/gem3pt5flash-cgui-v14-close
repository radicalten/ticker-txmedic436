#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdarg.h>

/* Chess rules and state representations */
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

typedef struct {
    int from;
    int to;
    int promo;
} Move;

typedef struct {
    int board[64];
    int turn;       // WHITE or BLACK
    int castling;   // Bitmask: 1:WK, 2:WQ, 4:BK, 8:BQ
    int ep;         // En passant target square index (-1 if none)
    int halfmove;
    int fullmove;
    char move_str[8];
    char pgn[16];
} GameState;

/* Global state history for Undos */
GameState current_state;
GameState history[2048];
int history_count = 0;

/* Engine communication globals */
int engine_in = -1;
int engine_out = -1;
pid_t engine_pid = -1;
int engine_status = 0; // 0: Disconnected, 1: Connecting, 2: Ready
int is_engine_thinking = 0;

/* User-defined engine parameters */
int engine_depth = 10;
int engine_nodes = 0;
int engine_time_ms = 1000;
char engine_path[256] = "stockfish";
int engine_side = BLACK; // Color engine plays. 0 to disable engine play.

char engine_buffer[8192];
int engine_buf_len = 0;

/* Raw terminal helper functions */
struct termios orig_termios;

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // Show cursor
    fflush(stdout);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
    fflush(stdout);
}

/* Print safe, coordinates-based line to HUD layout area */
void print_hud(int row, const char *fmt, ...) {
    printf("\033[%d;32H\033[K", row); // Row, Column 32, clear to end of line
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void get_user_string(const char *prompt, char *buffer, int max_len) {
    disable_raw_mode();
    printf("\033[24;1H\033[K%s", prompt);
    fflush(stdout);
    if (fgets(buffer, max_len, stdin)) {
        buffer[strcspn(buffer, "\n")] = 0;
    }
    enable_raw_mode();
    printf("\033[24;1H\033[K");
    fflush(stdout);
}

/* Chess Board Rule Validation Engine */
void init_board(GameState *state) {
    memset(state, 0, sizeof(GameState));
    int back_row[] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
    for (int i = 0; i < 8; i++) {
        state->board[i] = back_row[i] | BLACK;
        state->board[8 + i] = PAWN | BLACK;
        state->board[48 + i] = PAWN | WHITE;
        state->board[56 + i] = back_row[i] | WHITE;
    }
    state->turn = WHITE;
    state->castling = 15;
    state->ep = -1;
    state->halfmove = 0;
    state->fullmove = 1;
}

int is_square_attacked(int sq, int attacker_color, int *board) {
    int r = sq / 8;
    int c = sq % 8;

    // Knight attacks
    int knight_offsets[8][2] = {{-2,-1}, {-2,1}, {-1,-2}, {-1,2}, {1,-2}, {1,2}, {2,-1}, {2,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + knight_offsets[i][0];
        int nc = c + knight_offsets[i][1];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int p = board[nr * 8 + nc];
            if ((p & COLOR_MASK) == attacker_color && (p & TYPE_MASK) == KNIGHT) return 1;
        }
    }

    // Rook / Queen sliding
    int rook_dirs[4][2] = {{-1,0}, {1,0}, {0,-1}, {0,1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += rook_dirs[i][0];
            nc += rook_dirs[i][1];
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            int p = board[nr * 8 + nc];
            if (p != EMPTY) {
                if ((p & COLOR_MASK) == attacker_color && ((p & TYPE_MASK) == ROOK || (p & TYPE_MASK) == QUEEN)) return 1;
                break;
            }
        }
    }

    // Bishop / Queen sliding
    int bishop_dirs[4][2] = {{-1,-1}, {-1,1}, {1,-1}, {1,1}};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += bishop_dirs[i][0];
            nc += bishop_dirs[i][1];
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            int p = board[nr * 8 + nc];
            if (p != EMPTY) {
                if ((p & COLOR_MASK) == attacker_color && ((p & TYPE_MASK) == BISHOP || (p & TYPE_MASK) == QUEEN)) return 1;
                break;
            }
        }
    }

    // King attacks
    int king_dirs[8][2] = {{-1,-1}, {-1,0}, {-1,1}, {0,-1}, {0,1}, {1,-1}, {1,0}, {1,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + king_dirs[i][0];
        int nc = c + king_dirs[i][1];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int p = board[nr * 8 + nc];
            if ((p & COLOR_MASK) == attacker_color && (p & TYPE_MASK) == KING) return 1;
        }
    }

    // Pawn attacks
    int p_dir = (attacker_color == WHITE) ? 1 : -1;
    int pr = r + p_dir;
    if (pr >= 0 && pr < 8) {
        for (int dc = -1; dc <= 1; dc += 2) {
            int pc = c + dc;
            if (pc >= 0 && pc < 8) {
                int p = board[pr * 8 + pc];
                if ((p & COLOR_MASK) == attacker_color && (p & TYPE_MASK) == PAWN) return 1;
            }
        }
    }
    return 0;
}

int is_move_legal(GameState *state, int from, int to, int promo) {
    int board[64];
    memcpy(board, state->board, sizeof(board));

    int us = state->board[from] & COLOR_MASK;
    int op = (us == WHITE) ? BLACK : WHITE;

    int piece = board[from];
    board[from] = EMPTY;
    board[to] = promo ? (promo | us) : piece;

    if ((piece & TYPE_MASK) == PAWN && to == state->ep) {
        int ep_capture_sq = (us == WHITE) ? (to + 8) : (to - 8);
        board[ep_capture_sq] = EMPTY;
    }

    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if ((board[i] & TYPE_MASK) == KING && (board[i] & COLOR_MASK) == us) {
            king_sq = i;
            break;
        }
    }
    if (king_sq == -1) return 0;
    return !is_square_attacked(king_sq, op, board);
}

int get_legal_moves(int from, Move *moves, GameState *state) {
    int count = 0;
    int piece = state->board[from];
    if (piece == EMPTY) return 0;
    int color = piece & COLOR_MASK;
    if (color != state->turn) return 0;
    int type = piece & TYPE_MASK;

    int r = from / 8;
    int c = from % 8;

    if (type == PAWN) {
        int dir = (color == WHITE) ? -1 : 1;
        int start_row = (color == WHITE) ? 6 : 1;
        int promo_row = (color == WHITE) ? 0 : 7;

        int target = from + dir * 8;
        if (target >= 0 && target < 64 && state->board[target] == EMPTY) {
            if (target / 8 == promo_row) {
                int promos[] = {QUEEN, ROOK, BISHOP, KNIGHT};
                for (int i = 0; i < 4; i++) {
                    if (is_move_legal(state, from, target, promos[i])) {
                        moves[count++] = (Move){from, target, promos[i]};
                    }
                }
            } else {
                if (is_move_legal(state, from, target, 0)) {
                    moves[count++] = (Move){from, target, 0};
                }
            }

            int target2 = from + dir * 16;
            if (r == start_row && state->board[target2] == EMPTY) {
                if (is_move_legal(state, from, target2, 0)) {
                    moves[count++] = (Move){from, target2, 0};
                }
            }
        }

        int cols[2] = {c - 1, c + 1};
        for (int i = 0; i < 2; i++) {
            int nc = cols[i];
            if (nc >= 0 && nc < 8) {
                int target_cap = (r + dir) * 8 + nc;
                int cap_piece = state->board[target_cap];
                if (cap_piece != EMPTY && (cap_piece & COLOR_MASK) != color) {
                    if (target_cap / 8 == promo_row) {
                        int promos[] = {QUEEN, ROOK, BISHOP, KNIGHT};
                        for (int p = 0; p < 4; p++) {
                            if (is_move_legal(state, from, target_cap, promos[p])) {
                                moves[count++] = (Move){from, target_cap, promos[p]};
                            }
                        }
                    } else {
                        if (is_move_legal(state, from, target_cap, 0)) {
                            moves[count++] = (Move){from, target_cap, 0};
                        }
                    }
                }
                if (target_cap == state->ep) {
                    if (is_move_legal(state, from, target_cap, 0)) {
                        moves[count++] = (Move){from, target_cap, 0};
                    }
                }
            }
        }
    }
    else if (type == KNIGHT) {
        int knight_offsets[8][2] = {{-2,-1}, {-2,1}, {-1,-2}, {-1,2}, {1,-2}, {1,2}, {2,-1}, {2,1}};
        for (int i = 0; i < 8; i++) {
            int nr = r + knight_offsets[i][0];
            int nc = c + knight_offsets[i][1];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int target = nr * 8 + nc;
                int p = state->board[target];
                if (p == EMPTY || (p & COLOR_MASK) != color) {
                    if (is_move_legal(state, from, target, 0)) {
                        moves[count++] = (Move){from, target, 0};
                    }
                }
            }
        }
    }
    else if (type == KING) {
        int king_dirs[8][2] = {{-1,-1}, {-1,0}, {-1,1}, {0,-1}, {0,1}, {1,-1}, {1,0}, {1,1}};
        for (int i = 0; i < 8; i++) {
            int nr = r + king_dirs[i][0];
            int nc = c + king_dirs[i][1];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int target = nr * 8 + nc;
                int p = state->board[target];
                if (p == EMPTY || (p & COLOR_MASK) != color) {
                    if (is_move_legal(state, from, target, 0)) {
                        moves[count++] = (Move){from, target, 0};
                    }
                }
            }
        }

        int op = (color == WHITE) ? BLACK : WHITE;
        if (!is_square_attacked(from, op, state->board)) {
            if (color == WHITE) {
                if ((state->castling & 1) && state->board[61] == EMPTY && state->board[62] == EMPTY) {
                    if (!is_square_attacked(61, op, state->board) && !is_square_attacked(62, op, state->board)) {
                        moves[count++] = (Move){60, 62, 0};
                    }
                }
                if ((state->castling & 2) && state->board[59] == EMPTY && state->board[58] == EMPTY && state->board[57] == EMPTY) {
                    if (!is_square_attacked(59, op, state->board) && !is_square_attacked(58, op, state->board)) {
                        moves[count++] = (Move){60, 58, 0};
                    }
                }
            } else {
                if ((state->castling & 4) && state->board[5] == EMPTY && state->board[6] == EMPTY) {
                    if (!is_square_attacked(5, op, state->board) && !is_square_attacked(6, op, state->board)) {
                        moves[count++] = (Move){4, 6, 0};
                    }
                }
                if ((state->castling & 8) && state->board[3] == EMPTY && state->board[2] == EMPTY && state->board[1] == EMPTY) {
                    if (!is_square_attacked(3, op, state->board) && !is_square_attacked(2, op, state->board)) {
                        moves[count++] = (Move){4, 2, 0};
                    }
                }
            }
        }
    }
    else {
        int dirs[8][2];
        int d_count = 0;
        if (type == BISHOP || type == QUEEN) {
            int b_dirs[4][2] = {{-1,-1}, {-1,1}, {1,-1}, {1,1}};
            for (int i = 0; i < 4; i++) { dirs[d_count][0] = b_dirs[i][0]; dirs[d_count][1] = b_dirs[i][1]; d_count++; }
        }
        if (type == ROOK || type == QUEEN) {
            int r_dirs[4][2] = {{-1,0}, {1,0}, {0,-1}, {0,1}};
            for (int i = 0; i < 4; i++) { dirs[d_count][0] = r_dirs[i][0]; dirs[d_count][1] = r_dirs[i][1]; d_count++; }
        }

        for (int i = 0; i < d_count; i++) {
            int nr = r, nc = c;
            while (1) {
                nr += dirs[i][0];
                nc += dirs[i][1];
                if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
                int target = nr * 8 + nc;
                int p = state->board[target];
                if (p == EMPTY) {
                    if (is_move_legal(state, from, target, 0)) {
                        moves[count++] = (Move){from, target, 0};
                    }
                } else {
                    if ((p & COLOR_MASK) != color) {
                        if (is_move_legal(state, from, target, 0)) {
                            moves[count++] = (Move){from, target, 0};
                        }
                    }
                    break;
                }
            }
        }
    }
    return count;
}

void make_move(GameState *state, Move move) {
    int from = move.from;
    int to = move.to;
    int piece = state->board[from];
    int type = piece & TYPE_MASK;
    int color = piece & COLOR_MASK;

    state->ep = -1;

    char files[] = "abcdefgh";
    char ranks[] = "87654321";
    char promo_char = '\0';
    if (move.promo) {
        if ((move.promo & TYPE_MASK) == QUEEN) promo_char = 'q';
        else if ((move.promo & TYPE_MASK) == ROOK) promo_char = 'r';
        else if ((move.promo & TYPE_MASK) == BISHOP) promo_char = 'b';
        else if ((move.promo & TYPE_MASK) == KNIGHT) promo_char = 'n';
    }

    if (promo_char) {
        sprintf(state->move_str, "%c%c%c%c%c", files[from % 8], ranks[from / 8], files[to % 8], ranks[to / 8], promo_char);
    } else {
        sprintf(state->move_str, "%c%c%c%c", files[from % 8], ranks[from / 8], files[to % 8], ranks[to / 8]);
    }

    char pgn[16] = "";
    if (type == KING && abs(from % 8 - to % 8) == 2) {
        if (to % 8 == 6) strcpy(pgn, "O-O");
        else strcpy(pgn, "O-O-O");
    } else {
        int idx = 0;
        if (type != PAWN) {
            char p_chars[] = "  NBRQK";
            pgn[idx++] = p_chars[type];
        } else {
            if (from % 8 != to % 8) {
                pgn[idx++] = files[from % 8];
            }
        }
        if (state->board[to] != EMPTY || (type == PAWN && to == state->ep)) {
            pgn[idx++] = 'x';
        }
        pgn[idx++] = files[to % 8];
        pgn[idx++] = ranks[to / 8];
        if (move.promo) {
            pgn[idx++] = '=';
            char p_chars[] = "  NBRQK";
            pgn[idx++] = p_chars[move.promo & TYPE_MASK];
        }
        pgn[idx] = '\0';
    }
    strcpy(state->pgn, pgn);

    state->board[from] = EMPTY;
    state->board[to] = move.promo ? (move.promo | color) : piece;

    if (type == KING) {
        if (from == 60 && to == 62) { state->board[61] = state->board[63]; state->board[63] = EMPTY; }
        else if (from == 60 && to == 58) { state->board[59] = state->board[56]; state->board[56] = EMPTY; }
        else if (from == 4 && to == 6) { state->board[5] = state->board[7]; state->board[7] = EMPTY; }
        else if (from == 4 && to == 2) { state->board[3] = state->board[0]; state->board[0] = EMPTY; }
    }

    if (type == PAWN && to == state->ep) {
        int cap_sq = (color == WHITE) ? (to + 8) : (to - 8);
        state->board[cap_sq] = EMPTY;
    }

    if (type == PAWN && abs(from - to) == 16) {
        state->ep = (from + to) / 2;
    }

    if (from == 60) state->castling &= ~3;
    if (from == 4)  state->castling &= ~12;
    if (from == 63 || to == 63) state->castling &= ~1;
    if (from == 56 || to == 56) state->castling &= ~2;
    if (from == 7  || to == 7)  state->castling &= ~4;
    if (from == 0  || to == 0)  state->castling &= ~8;

    if (color == BLACK) {
        state->fullmove++;
    }
    state->turn = (color == WHITE) ? BLACK : WHITE;
}

int has_legal_moves(GameState *state) {
    for (int i = 0; i < 64; i++) {
        if (state->board[i] != EMPTY && (state->board[i] & COLOR_MASK) == state->turn) {
            Move dummy[128];
            if (get_legal_moves(i, dummy, state) > 0) return 1;
        }
    }
    return 0;
}

int parse_uci_move(const char *str, Move *move, GameState *state) {
    if (strlen(str) < 4) return 0;
    int f_col = str[0] - 'a';
    int f_row = '8' - str[1];
    int t_col = str[2] - 'a';
    int t_row = '8' - str[3];

    if (f_col < 0 || f_col > 7 || f_row < 0 || f_row > 7 ||
        t_col < 0 || t_col > 7 || t_row < 0 || t_row > 7) return 0;

    move->from = f_row * 8 + f_col;
    move->to = t_row * 8 + t_col;
    move->promo = 0;

    if (strlen(str) == 5) {
        char p = str[4];
        if (p == 'q') move->promo = QUEEN;
        else if (p == 'r') move->promo = ROOK;
        else if (p == 'b') move->promo = BISHOP;
        else if (p == 'n') move->promo = KNIGHT;
    }
    return 1;
}

/* Pipe Subprocessing with Chess Engine */
void write_engine(const char *cmd) {
    if (engine_in != -1) {
        write(engine_in, cmd, strlen(cmd));
        write(engine_in, "\n", 1);
    }
}

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
        execlp(path, path, (char *)NULL);
        exit(1);
    } else {
        close(to_engine[0]);
        close(from_engine[1]);
        engine_in = to_engine[1];
        engine_out = from_engine[0];

        int flags = fcntl(engine_out, F_GETFL, 0);
        fcntl(engine_out, F_SETFL, flags | O_NONBLOCK);
        return 1;
    }
}

void stop_engine() {
    if (engine_pid > 0) {
        write_engine("quit");
        kill(engine_pid, SIGTERM);
        waitpid(engine_pid, NULL, 0);
        engine_pid = -1;
        engine_in = -1;
        engine_out = -1;
    }
}

int check_engine_response(char *bestmove_str) {
    if (engine_out == -1) return 0;
    char tmp[1024];
    int n = read(engine_out, tmp, sizeof(tmp) - 1);
    if (n <= 0) return 0;

    tmp[n] = '\0';
    if (engine_buf_len + n < (int)sizeof(engine_buffer) - 1) {
        memcpy(engine_buffer + engine_buf_len, tmp, n);
        engine_buf_len += n;
        engine_buffer[engine_buf_len] = '\0';
    } else {
        engine_buf_len = 0;
    }

    char *line_start = engine_buffer;
    char *newline;
    int parsed = 0;
    while ((newline = strchr(line_start, '\n')) != NULL) {
        *newline = '\0';
        if (strncmp(line_start, "info ", 5) == 0) {
            char info_trim[60] = "";
            strncpy(info_trim, line_start + 5, 45);
            info_trim[45] = '\0';
            print_hud(18, "Engine evaluation: %s...", info_trim);
        }
        if (strncmp(line_start, "bestmove ", 9) == 0) {
            sscanf(line_start + 9, "%s", bestmove_str);
            parsed = 1;
        }
        if (strcmp(line_start, "uciok") == 0) {
            write_engine("isready");
        }
        if (strcmp(line_start, "readyok") == 0) {
            engine_status = 2;
        }
        line_start = newline + 1;
    }

    int consumed = line_start - engine_buffer;
    if (consumed > 0) {
        memmove(engine_buffer, line_start, engine_buf_len - consumed);
        engine_buf_len -= consumed;
        engine_buffer[engine_buf_len] = '\0';
    }
    return parsed;
}

void trigger_engine_move() {
    char pos_cmd[8192] = "position startpos moves";
    for (int i = 1; i <= history_count; i++) {
        strcat(pos_cmd, " ");
        strcat(pos_cmd, history[i].move_str);
    }
    write_engine(pos_cmd);

    char go_cmd[128];
    if (engine_nodes > 0) {
        sprintf(go_cmd, "go nodes %d", engine_nodes);
    } else if (engine_depth > 0) {
        sprintf(go_cmd, "go depth %d", engine_depth);
    } else {
        sprintf(go_cmd, "go movetime %d", engine_time_ms);
    }
    write_engine(go_cmd);
}

/* GUI Keyboard input and execution loop */
int read_key() {
    char c;
    struct timeval tv = {0, 50000}; // 50ms Select timeout
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    if (engine_out != -1) FD_SET(engine_out, &fds);

    int max_fd = (engine_out > STDIN_FILENO) ? engine_out : STDIN_FILENO;
    int activity = select(max_fd + 1, &fds, NULL, NULL, &tv);
    if (activity < 0) return 0;

    if (engine_out != -1 && FD_ISSET(engine_out, &fds)) {
        return -2; // Engine response pending flag
    }

    if (FD_ISSET(STDIN_FILENO, &fds)) {
        int n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) return 0;
        if (c == '\033') {
            char seq[3];
            struct timeval sub_tv = {0, 10000};
            fd_set sub_fds;
            FD_ZERO(&sub_fds);
            FD_SET(STDIN_FILENO, &sub_fds);
            if (select(STDIN_FILENO + 1, &sub_fds, NULL, NULL, &sub_tv) > 0) {
                if (read(STDIN_FILENO, &seq[0], 1) <= 0) return '\033';
                if (read(STDIN_FILENO, &seq[1], 1) <= 0) return '\033';
                if (seq[0] == '[') {
                    switch (seq[1]) {
                        case 'A': return 'w'; // Up
                        case 'B': return 's'; // Down
                        case 'C': return 'd'; // Right
                        case 'D': return 'a'; // Left
                    }
                }
            }
            return '\033';
        }
        return c;
    }
    return 0;
}

void draw_board(GameState *state, int cursor_r, int cursor_c, int selected_sq, Move *legal_moves, int num_legal) {
    printf("\033[H"); // Reset cursor home
    printf("    \033[1m  A  B  C  D  E  F  G  H  \033[0m\n");

    for (int r = 0; r < 8; r++) {
        printf(" \033[1m%d \033[0m", 8 - r);
        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            int is_cursor = (r == cursor_r && c == cursor_c);
            int is_selected = (sq == selected_sq);
            int is_legal = 0;
            for (int i = 0; i < num_legal; i++) {
                if (legal_moves[i].to == sq) {
                    is_legal = 1;
                    break;
                }
            }

            // High-fidelity styling block utilizing standard macOS 256 terminal colors
            if (is_cursor) {
                printf("\033[48;5;196m"); // Cursor
            } else if (is_selected) {
                printf("\033[48;5;220m"); // Selected piece
            } else if (is_legal) {
                printf("\033[48;5;114m"); // Highlighted legal options
            } else {
                if ((r + c) % 2 == 0) {
                    printf("\033[48;5;253m"); // Light squares
                } else {
                    printf("\033[48;5;242m"); // Dark squares
                }
            }

            int piece = state->board[sq];
            int p_color = piece & COLOR_MASK;
            int p_type = piece & TYPE_MASK;

            if (p_color == WHITE) {
                printf("\033[38;5;15m\033[1m");
            } else {
                printf("\033[38;5;232m");
            }

            const char *sym = "  ";
            switch (p_type) {
                case PAWN:   sym = " ♟"; break;
                case KNIGHT: sym = " ♞"; break;
                case BISHOP: sym = " ♝"; break;
                case ROOK:   sym = " ♜"; break;
                case QUEEN:  sym = " ♛"; break;
                case KING:   sym = " ♚"; break;
                default:     sym = "  "; break;
            }
            printf("%s ", sym);
            printf("\033[0m");
        }
        printf(" \033[1m%d\033[0m\n", 8 - r);
    }
    printf("    \033[1m  A  B  C  D  E  F  G  H  \033[0m\n\n");
}

int main() {
    init_board(&current_state);
    history[0] = current_state;

    // Start engine in background using pipes
    engine_status = start_engine(engine_path) ? 1 : 0;
    if (engine_status == 1) {
        write_engine("uci");
        write_engine("isready");
    }

    enable_raw_mode();
    printf("\033[2J"); // Clear Screen once

    int cursor_r = 7, cursor_c = 4;
    int selected_sq = -1;
    Move legal_moves[128];
    int num_legal_moves = 0;

    while (1) {
        int active_king_in_check = 0;
        int king_sq = -1;
        for (int i = 0; i < 64; i++) {
            if ((current_state.board[i] & TYPE_MASK) == KING && (current_state.board[i] & COLOR_MASK) == current_state.turn) {
                king_sq = i;
                break;
            }
        }
        int opponent_color = (current_state.turn == WHITE) ? BLACK : WHITE;
        if (king_sq != -1 && is_square_attacked(king_sq, opponent_color, current_state.board)) {
            active_king_in_check = 1;
        }

        int over = !has_legal_moves(&current_state);

        draw_board(&current_state, cursor_r, cursor_c, selected_sq, legal_moves, num_legal_moves);

        // Sidebar rendering
        print_hud(2, "=== TERMINAL CHESS GUI ===");
        print_hud(3, "Turn: %s", (current_state.turn == WHITE) ? "WHITE" : "BLACK");

        if (over) {
            if (active_king_in_check) {
                print_hud(4, "Status: \033[31;1mCHECKMATE! %s Wins\033[0m", (current_state.turn == WHITE) ? "BLACK" : "WHITE");
            } else {
                print_hud(4, "Status: \033[33;1mSTALEMATE (Draw)\033[0m");
            }
        } else {
            if (is_engine_thinking) {
                print_hud(4, "Status: \033[35;1mEngine Thinking...\033[0m");
            } else {
                if (engine_side != 0 && current_state.turn == engine_side) {
                    print_hud(4, "Status: \033[33;1mWaiting for Engine...\033[0m");
                } else {
                    print_hud(4, "Status: \033[32;1mYour Turn\033[0m");
                }
            }
        }

        if (active_king_in_check && !over) {
            print_hud(5, "\033[31;1m  [ KING IN CHECK ]\033[0m");
        } else {
            print_hud(5, "-----------------------------------");
        }

        print_hud(6, "Engine Status: %s", (engine_status == 0) ? "\033[31mDisconnected\033[0m" : (engine_status == 1) ? "\033[33mConnecting...\033[0m" : "\033[32mReady\033[0m");
        print_hud(7, "  (P) Path: %s", engine_path);
        print_hud(8, "  (E) Opponent: %s", (engine_side == WHITE) ? "WHITE" : (engine_side == BLACK) ? "BLACK" : "NONE (Pass-n-Play)");
        if (engine_depth > 0) print_hud(9, "  (D) Max Depth: %d", engine_depth);
        else print_hud(9, "  (D) Max Depth: Disabled");
        if (engine_nodes > 0) print_hud(10, "  (N) Max Nodes: %d", engine_nodes);
        else print_hud(10, "  (N) Max Nodes: Disabled");
        if (engine_time_ms > 0) print_hud(11, "  (T) Max Time: %d ms", engine_time_ms);
        else print_hud(11, "  (T) Max Time: Disabled");
        print_hud(12, "-----------------------------------");
        print_hud(13, "Controls:");
        print_hud(14, "  [Arrows/WASD] Move Cursor");
        print_hud(15, "  [Space/Enter] Select/Move Piece");
        print_hud(16, "  (U) Undo/Takeback Move");
        print_hud(17, "  (Q) Quit Game");
        print_hud(18, "-----------------------------------");

        // Dynamically wrap and display the standard PGN move notation log
        print_hud(19, "PGN Move List:");
        int line = 20;
        char pgn_line[80] = "";
        int char_count = 0;
        for (int i = 1; i <= history_count; i++) {
            char move_part[32];
            if (i % 2 == 1) {
                sprintf(move_part, "%d. %s ", (i + 1) / 2, history[i].pgn);
            } else {
                sprintf(move_part, "%s ", history[i].pgn);
            }

            if (char_count + (int)strlen(move_part) > 40) {
                print_hud(line++, "  %s", pgn_line);
                pgn_line[0] = '\0';
                char_count = 0;
                if (line >= 24) break;
            }
            strcat(pgn_line, move_part);
            char_count += strlen(move_part);
        }
        if (strlen(pgn_line) > 0 && line < 24) {
            print_hud(line++, "  %s", pgn_line);
        }
        while (line < 24) {
            print_hud(line++, "");
        }

        fflush(stdout);

        // Process engine's turn if ready and active
        if (!over && engine_status == 2 && current_state.turn == engine_side && !is_engine_thinking) {
            is_engine_thinking = 1;
            trigger_engine_move();
        }

        int key = read_key();

        if (key == -2) { // Read engine pipe output async
            char bestmove_str[16];
            if (check_engine_response(bestmove_str)) {
                Move engine_move;
                if (parse_uci_move(bestmove_str, &engine_move, &current_state)) {
                    GameState next_state = current_state;
                    make_move(&next_state, engine_move);
                    history_count++;
                    history[history_count] = next_state;
                    current_state = next_state;
                }
                is_engine_thinking = 0;
            }
        } else if (key > 0) {
            if (key == 'q' || key == 'Q') {
                break;
            }
            else if (key == 'u' || key == 'U') {
                if (engine_side != 0 && history_count >= 2) {
                    current_state = history[history_count - 2];
                    history_count -= 2;
                    selected_sq = -1;
                    num_legal_moves = 0;
                } else if (engine_side == 0 && history_count >= 1) {
                    current_state = history[history_count - 1];
                    history_count -= 1;
                    selected_sq = -1;
                    num_legal_moves = 0;
                }
            }
            else if (key == 'p' || key == 'P') {
                char path_buf[256];
                get_user_string("Enter engine path: ", path_buf, sizeof(path_buf));
                if (strlen(path_buf) > 0) {
                    strcpy(engine_path, path_buf);
                    stop_engine();
                    engine_status = start_engine(engine_path) ? 1 : 0;
                    if (engine_status == 1) {
                        write_engine("uci");
                        write_engine("isready");
                    }
                }
            }
            else if (key == 'd' || key == 'D') {
                char val_buf[32];
                get_user_string("Enter max depth (1-99, 0 to disable): ", val_buf, sizeof(val_buf));
                int val = atoi(val_buf);
                if (val >= 0) {
                    engine_depth = val;
                    if (val > 0) { engine_nodes = 0; engine_time_ms = 0; }
                }
            }
            else if (key == 'n' || key == 'N') {
                char val_buf[32];
                get_user_string("Enter max nodes (0 to disable): ", val_buf, sizeof(val_buf));
                int val = atoi(val_buf);
                if (val >= 0) {
                    engine_nodes = val;
                    if (val > 0) { engine_depth = 0; engine_time_ms = 0; }
                }
            }
            else if (key == 't' || key == 'T') {
                char val_buf[32];
                get_user_string("Enter max move time in ms (0 to disable): ", val_buf, sizeof(val_buf));
                int val = atoi(val_buf);
                if (val >= 0) {
                    engine_time_ms = val;
                    if (val > 0) { engine_depth = 0; engine_nodes = 0; }
                }
            }
            else if (key == 'e' || key == 'E') {
                if (engine_side == BLACK) {
                    engine_side = WHITE;
                } else if (engine_side == WHITE) {
                    engine_side = 0;
                } else {
                    engine_side = BLACK;
                }
            }
            else if (key == 'w' || key == 'W') {
                if (cursor_r > 0) cursor_r--;
            }
            else if (key == 's' || key == 'S') {
                if (cursor_r < 7) cursor_r++;
            }
            else if (key == 'a' || key == 'A') {
                if (cursor_c > 0) cursor_c--;
            }
            else if (key == 'd' || key == 'D') {
                if (cursor_c < 7) cursor_c++;
            }
            else if (key == ' ' || key == '\n' || key == '\r') {
                int target_sq = cursor_r * 8 + cursor_c;
                if (selected_sq == -1) {
                    int piece = current_state.board[target_sq];
                    if (piece != EMPTY && (piece & COLOR_MASK) == current_state.turn) {
                        if (engine_side == 0 || current_state.turn != engine_side) {
                            selected_sq = target_sq;
                            num_legal_moves = get_legal_moves(selected_sq, legal_moves, &current_state);
                        }
                    }
                } else {
                    int valid_move_idx = -1;
                    for (int i = 0; i < num_legal_moves; i++) {
                        if (legal_moves[i].to == target_sq) {
                            valid_move_idx = i;
                            break;
                        }
                    }

                    if (valid_move_idx != -1) {
                        GameState next_state = current_state;
                        make_move(&next_state, legal_moves[valid_move_idx]);
                        history_count++;
                        history[history_count] = next_state;
                        current_state = next_state;
                    }
                    selected_sq = -1;
                    num_legal_moves = 0;
                }
            }
        } else {
            char bestmove_str[16];
            if (check_engine_response(bestmove_str)) {
                Move engine_move;
                if (parse_uci_move(bestmove_str, &engine_move, &current_state)) {
                    GameState next_state = current_state;
                    make_move(&next_state, engine_move);
                    history_count++;
                    history[history_count] = next_state;
                    current_state = next_state;
                }
                is_engine_thinking = 0;
            }
        }
    }

    stop_engine();
    disable_raw_mode();
    printf("\033[2J\033[H"); // Reset screen on exit
    return 0;
}
