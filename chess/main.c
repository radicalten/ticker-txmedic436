#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdbool.h>
#include <ctype.h>

// --- Key Mappings ---
enum Keys {
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_ENTER,
    KEY_SPACE
};

// --- Game Structures ---
typedef struct {
    int from;
    int to;
    int promotion;
} Move;

typedef struct {
    Move moves[256];
    int count;
} MoveList;

typedef struct {
    char board[64]; // Piece values: White (1..6), Black (-1..-6), 0 empty
    int turn;       // 1 = White, -1 = Black
    int castling;   // Bitmask: 1:WK, 2:WQ, 4:BK, 8:BQ
    int ep_square;  // En passant target square (-1 if none)
    int halfmove;
    int fullmove;
} BoardState;

// --- Global Variables ---
BoardState game_state;
BoardState history[1024];
char history_moves_san[1024][16];
int history_count = 0;

int cursor_sq = 12; // Start cursor at e2 (12)
int selected_sq = -1;
int last_move_from = -1;
int last_move_to = -1;

int engine_color = 0; // 0: Manual PvP, -1: Engine is Black, 1: Engine is White
int time_control_type = 1; // 0: movetime, 1: depth, 2: nodes
int time_control_value = 10; // Default: Depth 10

// Engine communication
int engine_in = -1;
int engine_out = -1;
pid_t engine_pid = -1;
char engine_buffer[8192];
int engine_buffer_len = 0;

const char *stockfish_paths[] = {
    "/opt/homebrew/bin/stockfish",
    "/usr/local/bin/stockfish",
    "/usr/bin/stockfish",
    "stockfish", // Search system PATH
    NULL
};

struct termios orig_termios;

// --- Core Helper Declarations ---
void get_legal_moves(const BoardState *state, MoveList *list);
bool is_in_check(const BoardState *state, int color);
void make_move(BoardState *state, Move move);
void render_board();

// --- Terminal Control ---
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // Show cursor
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

int read_key() {
    char c;
    int nread = read(STDIN_FILENO, &c, 1);
    if (nread <= 0) return 0;
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
    if (c == '\n' || c == '\r') return KEY_ENTER;
    if (c == ' ') return KEY_SPACE;
    return c;
}

// --- FEN Handler ---
void load_fen(BoardState *state, const char *fen) {
    memset(state->board, 0, 64);
    int rank = 7, file = 0;
    const char *p = fen;
    while (*p && *p != ' ') {
        if (*p == '/') {
            rank--;
            file = 0;
        } else if (isdigit(*p)) {
            file += (*p - '0');
        } else {
            int piece = 0;
            switch (*p) {
                case 'P': piece = 1; break;
                case 'N': piece = 2; break;
                case 'B': piece = 3; break;
                case 'R': piece = 4; break;
                case 'Q': piece = 5; break;
                case 'K': piece = 6; break;
                case 'p': piece = -1; break;
                case 'n': piece = -2; break;
                case 'b': piece = -3; break;
                case 'r': piece = -4; break;
                case 'q': piece = -5; break;
                case 'k': piece = -6; break;
            }
            state->board[rank * 8 + file] = piece;
            file++;
        }
        p++;
    }
    if (*p) p++;
    if (*p) { state->turn = (*p == 'w') ? 1 : -1; p++; }
    if (*p) p++;
    state->castling = 0;
    while (*p && *p != ' ') {
        if (*p == 'K') state->castling |= 1;
        else if (*p == 'Q') state->castling |= 2;
        else if (*p == 'k') state->castling |= 4;
        else if (*p == 'q') state->castling |= 8;
        p++;
    }
    if (*p) p++;
    state->ep_square = -1;
    if (*p && *p != ' ' && *p != '-') {
        int f = p[0] - 'a';
        int r = p[1] - '1';
        state->ep_square = r * 8 + f;
        p += 2;
    } else if (*p == '-') {
        p++;
    }
    state->halfmove = 0;
    state->fullmove = 1;
    if (*p) p++;
    if (*p && isdigit(*p)) {
        state->halfmove = atoi(p);
        while (*p && isdigit(*p)) p++;
    }
    if (*p) p++;
    if (*p && isdigit(*p)) {
        state->fullmove = atoi(p);
    }
}

void generate_fen(const BoardState *state, char *fen) {
    int empty = 0;
    char *p = fen;
    for (int r = 7; r >= 0; r--) {
        for (int f = 0; f < 8; f++) {
            int piece = state->board[r * 8 + f];
            if (piece == 0) {
                empty++;
            } else {
                if (empty > 0) {
                    p += sprintf(p, "%d", empty);
                    empty = 0;
                }
                char c = " pnbrqk"[abs(piece)];
                if (piece > 0) c = toupper(c);
                *p++ = c;
            }
        }
        if (empty > 0) {
            p += sprintf(p, "%d", empty);
            empty = 0;
        }
        if (r > 0) *p++ = '/';
    }
    *p++ = ' ';
    *p++ = (state->turn == 1) ? 'w' : 'b';
    *p++ = ' ';
    if (state->castling == 0) {
        *p++ = '-';
    } else {
        if (state->castling & 1) *p++ = 'K';
        if (state->castling & 2) *p++ = 'Q';
        if (state->castling & 4) *p++ = 'k';
        if (state->castling & 8) *p++ = 'q';
    }
    *p++ = ' ';
    if (state->ep_square == -1) {
        *p++ = '-';
    } else {
        *p++ = 'a' + (state->ep_square % 8);
        *p++ = '1' + (state->ep_square / 8);
    }
    sprintf(p, " %d %d", state->halfmove, state->fullmove);
}

// --- Engine Communication ---
bool get_engine_line(char *line, int max_len, int timeout_ms) {
    for (int i = 0; i < engine_buffer_len; i++) {
        if (engine_buffer[i] == '\n') {
            memcpy(line, engine_buffer, i);
            line[i] = '\0';
            if (i > 0 && line[i - 1] == '\r') line[i - 1] = '\0';
            memmove(engine_buffer, engine_buffer + i + 1, engine_buffer_len - i - 1);
            engine_buffer_len -= (i + 1);
            return true;
        }
    }
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(engine_out, &fds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int ret = select(engine_out + 1, &fds, NULL, NULL, &tv);
    if (ret > 0 && FD_ISSET(engine_out, &fds)) {
        int bytes = read(engine_out, engine_buffer + engine_buffer_len, sizeof(engine_buffer) - engine_buffer_len - 1);
        if (bytes > 0) {
            engine_buffer_len += bytes;
            engine_buffer[engine_buffer_len] = '\0';
            return get_engine_line(line, max_len, 0);
        }
    }
    return false;
}

void send_to_engine(const char *cmd) {
    if (engine_in != -1) {
        write(engine_in, cmd, strlen(cmd));
    }
}

bool init_engine() {
    const char *chosen_path = NULL;
    for (int i = 0; stockfish_paths[i] != NULL; i++) {
        if (access(stockfish_paths[i], X_OK) == 0 || i == 3) {
            chosen_path = stockfish_paths[i];
            if (i != 3) break;
        }
    }
    if (!chosen_path) return false;

    int p_to_c[2], c_to_p[2];
    if (pipe(p_to_c) < 0 || pipe(c_to_p) < 0) return false;

    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(p_to_c[0], STDIN_FILENO);
        dup2(c_to_p[1], STDOUT_FILENO);
        close(p_to_c[0]); close(p_to_c[1]);
        close(c_to_p[0]); close(c_to_p[1]);
        execlp(chosen_path, chosen_path, NULL);
        exit(1);
    }

    close(p_to_c[0]);
    close(c_to_p[1]);
    engine_in = p_to_c[1];
    engine_out = c_to_p[0];

    send_to_engine("uci\n");
    char line[1024];
    while (get_engine_line(line, sizeof(line), 2000)) {
        if (strcmp(line, "uciok") == 0) break;
    }
    send_to_engine("isready\n");
    while (get_engine_line(line, sizeof(line), 2000)) {
        if (strcmp(line, "readyok") == 0) break;
    }
    return true;
}

void cleanup() {
    disable_raw_mode();
    if (engine_pid != -1) {
        kill(engine_pid, SIGTERM);
        waitpid(engine_pid, NULL, 0);
    }
}

void handle_sigint(int sig) {
    exit(0);
}

// --- Chess Move Rules & Checking ---
bool is_square_attacked(const BoardState *state, int sq, int attacker_color) {
    int sq_rank = sq / 8, sq_file = sq % 8;
    
    // Knight attacks
    int knight_offsets[] = {-17, -15, -10, -6, 6, 10, 15, 17};
    for (int i = 0; i < 8; i++) {
        int to = sq + knight_offsets[i];
        if (to >= 0 && to < 64) {
            int r = to / 8, f = to % 8;
            if (abs(r - sq_rank) <= 2 && abs(f - sq_file) <= 2) {
                if (state->board[to] == attacker_color * 2) return true;
            }
        }
    }
    // Bishop / Queen sliding attacks
    int bishop_dirs[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
    for (int i = 0; i < 4; i++) {
        int r = sq_rank, f = sq_file;
        while (1) {
            r += bishop_dirs[i][0]; f += bishop_dirs[i][1];
            if (r < 0 || r > 7 || f < 0 || f > 7) break;
            int target = r * 8 + f;
            int p = state->board[target];
            if (p != 0) {
                if (p == attacker_color * 3 || p == attacker_color * 5) return true;
                break;
            }
        }
    }
    // Rook / Queen sliding attacks
    int rook_dirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    for (int i = 0; i < 4; i++) {
        int r = sq_rank, f = sq_file;
        while (1) {
            r += rook_dirs[i][0]; f += rook_dirs[i][1];
            if (r < 0 || r > 7 || f < 0 || f > 7) break;
            int target = r * 8 + f;
            int p = state->board[target];
            if (p != 0) {
                if (p == attacker_color * 4 || p == attacker_color * 5) return true;
                break;
            }
        }
    }
    // Pawn attacks
    int pawn_dir = -attacker_color;
    int pawn_rank = sq_rank + pawn_dir;
    if (pawn_rank >= 0 && pawn_rank < 8) {
        if (sq_file > 0 && state->board[pawn_rank * 8 + sq_file - 1] == attacker_color * 1) return true;
        if (sq_file < 7 && state->board[pawn_rank * 8 + sq_file + 1] == attacker_color * 1) return true;
    }
    // King attacks
    int king_offsets[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
    for (int i = 0; i < 8; i++) {
        int r = sq_rank + king_offsets[i][0];
        int f = sq_file + king_offsets[i][1];
        if (r >= 0 && r < 8 && f >= 0 && f < 8) {
            if (state->board[r * 8 + f] == attacker_color * 6) return true;
        }
    }
    return false;
}

bool is_in_check(const BoardState *state, int color) {
    int king_val = color * 6;
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == king_val) {
            king_sq = i;
            break;
        }
    }
    if (king_sq == -1) return false;
    return is_square_attacked(state, king_sq, -color);
}

void add_move(MoveList *list, int from, int to, int promo) {
    list->moves[list->count++] = (Move){from, to, promo};
}

void get_legal_moves(const BoardState *state, MoveList *list) {
    MoveList pseudo;
    pseudo.count = 0;
    list->count = 0;

    for (int sq = 0; sq < 64; sq++) {
        int piece = state->board[sq];
        if (piece * state->turn > 0) {
            int p_type = abs(piece);
            int rank = sq / 8, file = sq % 8;
            if (p_type == 1) { // Pawn
                int dir = state->turn;
                int to = sq + dir * 8;
                if (to >= 0 && to < 64 && state->board[to] == 0) {
                    int r = to / 8;
                    if (r == 7 || r == 0) {
                        add_move(&pseudo, sq, to, state->turn * 5);
                        add_move(&pseudo, sq, to, state->turn * 4);
                        add_move(&pseudo, sq, to, state->turn * 3);
                        add_move(&pseudo, sq, to, state->turn * 2);
                    } else {
                        add_move(&pseudo, sq, to, 0);
                        int start_rank = (state->turn == 1) ? 1 : 6;
                        if (rank == start_rank && state->board[sq + dir * 16] == 0) {
                            add_move(&pseudo, sq, sq + dir * 16, 0);
                        }
                    }
                }
                int captures[2] = {sq + dir * 8 - 1, sq + dir * 8 + 1};
                int files[2] = {file - 1, file + 1};
                for (int c = 0; c < 2; c++) {
                    int to_c = captures[c];
                    int file_c = files[c];
                    if (to_c >= 0 && to_c < 64 && file_c >= 0 && file_c < 8 && abs(file_c - file) == 1) {
                        if (state->board[to_c] * state->turn < 0 || to_c == state->ep_square) {
                            int r = to_c / 8;
                            if (r == 7 || r == 0) {
                                add_move(&pseudo, sq, to_c, state->turn * 5);
                                add_move(&pseudo, sq, to_c, state->turn * 4);
                                add_move(&pseudo, sq, to_c, state->turn * 3);
                                add_move(&pseudo, sq, to_c, state->turn * 2);
                            } else {
                                add_move(&pseudo, sq, to_c, 0);
                            }
                        }
                    }
                }
            } else if (p_type == 2) { // Knight
                int knight_offsets[] = {-17, -15, -10, -6, 6, 10, 15, 17};
                for (int i = 0; i < 8; i++) {
                    int to = sq + knight_offsets[i];
                    if (to >= 0 && to < 64) {
                        if (abs((to / 8) - rank) <= 2 && abs((to % 8) - file) <= 2) {
                            if (state->board[to] * state->turn <= 0) {
                                add_move(&pseudo, sq, to, 0);
                            }
                        }
                    }
                }
            } else if (p_type == 3 || p_type == 5) { // Bishop / Queen
                int dirs[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
                for (int d = 0; d < 4; d++) {
                    int r = rank, f = file;
                    while (1) {
                        r += dirs[d][0]; f += dirs[d][1];
                        if (r < 0 || r > 7 || f < 0 || f > 7) break;
                        int target = r * 8 + f;
                        if (state->board[target] == 0) {
                            add_move(&pseudo, sq, target, 0);
                        } else {
                            if (state->board[target] * state->turn < 0) {
                                add_move(&pseudo, sq, target, 0);
                            }
                            break;
                        }
                    }
                }
            }
            if (p_type == 4 || p_type == 5) { // Rook / Queen
                int dirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
                for (int d = 0; d < 4; d++) {
                    int r = rank, f = file;
                    while (1) {
                        r += dirs[d][0]; f += dirs[d][1];
                        if (r < 0 || r > 7 || f < 0 || f > 7) break;
                        int target = r * 8 + f;
                        if (state->board[target] == 0) {
                            add_move(&pseudo, sq, target, 0);
                        } else {
                            if (state->board[target] * state->turn < 0) {
                                add_move(&pseudo, sq, target, 0);
                            }
                            break;
                        }
                    }
                }
            }
            if (p_type == 6) { // King
                int offsets[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
                for (int i = 0; i < 8; i++) {
                    int r = rank + offsets[i][0];
                    int f = file + offsets[i][1];
                    if (r >= 0 && r < 8 && f >= 0 && f < 8) {
                        int target = r * 8 + f;
                        if (state->board[target] * state->turn <= 0) {
                            add_move(&pseudo, sq, target, 0);
                        }
                    }
                }
                // Castling
                if (state->turn == 1) {
                    if ((state->castling & 1) && state->board[5] == 0 && state->board[6] == 0) {
                        if (!is_square_attacked(state, 4, -1) && !is_square_attacked(state, 5, -1) && !is_square_attacked(state, 6, -1)) {
                            add_move(&pseudo, 4, 6, 0);
                        }
                    }
                    if ((state->castling & 2) && state->board[1] == 0 && state->board[2] == 0 && state->board[3] == 0) {
                        if (!is_square_attacked(state, 4, -1) && !is_square_attacked(state, 3, -1) && !is_square_attacked(state, 2, -1)) {
                            add_move(&pseudo, 4, 2, 0);
                        }
                    }
                } else {
                    if ((state->castling & 4) && state->board[61] == 0 && state->board[62] == 0) {
                        if (!is_square_attacked(state, 60, 1) && !is_square_attacked(state, 61, 1) && !is_square_attacked(state, 62, 1)) {
                            add_move(&pseudo, 60, 62, 0);
                        }
                    }
                    if ((state->castling & 8) && state->board[57] == 0 && state->board[58] == 0 && state->board[59] == 0) {
                        if (!is_square_attacked(state, 60, 1) && !is_square_attacked(state, 59, 1) && !is_square_attacked(state, 58, 1)) {
                            add_move(&pseudo, 60, 58, 0);
                        }
                    }
                }
            }
        }
    }

    for (int i = 0; i < pseudo.count; i++) {
        BoardState temp = *state;
        make_move(&temp, pseudo.moves[i]);
        if (!is_in_check(&temp, state->turn)) {
            list->moves[list->count++] = pseudo.moves[i];
        }
    }
}

void make_move(BoardState *state, Move move) {
    int piece = state->board[move.from];
    int target = state->board[move.to];
    int next_ep_square = -1;

    // Remove castling flags if kings/rooks move or are taken
    if (move.from == 4) state->castling &= ~3;
    if (move.from == 60) state->castling &= ~12;
    if (move.from == 0 || move.to == 0) state->castling &= ~2;
    if (move.from == 7 || move.to == 7) state->castling &= ~1;
    if (move.from == 56 || move.to == 56) state->castling &= ~8;
    if (move.from == 63 || move.to == 63) state->castling &= ~4;

    // Castling adjustments
    if (abs(piece) == 6 && abs(move.to - move.from) == 2) {
        if (move.to == 6) { state->board[5] = state->board[7]; state->board[7] = 0; }
        else if (move.to == 2) { state->board[3] = state->board[0]; state->board[0] = 0; }
        else if (move.to == 62) { state->board[61] = state->board[63]; state->board[63] = 0; }
        else if (move.to == 58) { state->board[59] = state->board[56]; state->board[56] = 0; }
    }

    // En Passant capture execution
    if (abs(piece) == 1 && move.to == state->ep_square) {
        state->board[move.to - state->turn * 8] = 0;
    }

    // Double push creates EP square
    if (abs(piece) == 1 && abs(move.to - move.from) == 16) {
        next_ep_square = move.from + state->turn * 8;
    }

    state->board[move.to] = move.promotion ? move.promotion : piece;
    state->board[move.from] = 0;

    if (abs(piece) == 1 || target != 0) state->halfmove = 0;
    else state->halfmove++;

    if (state->turn == -1) state->fullmove++;
    state->ep_square = next_ep_square;
    state->turn = -state->turn;
}

// --- SAN PGN Move Formatter ---
void to_san(const BoardState *prev_state, Move move, char *san) {
    int piece = prev_state->board[move.from];
    int p_type = abs(piece);
    int to_target = prev_state->board[move.to];

    if (p_type == 6 && (move.to - move.from == 2)) {
        strcpy(san, "O-O");
    } else if (p_type == 6 && (move.to - move.from == -2)) {
        strcpy(san, "O-O-O");
    } else {
        char p_char = " PNBRQK"[p_type];
        char f_from = 'a' + (move.from % 8);
        char f_to = 'a' + (move.to % 8);
        char r_to = '1' + (move.to / 8);
        bool capture = (to_target != 0) || (p_type == 1 && move.to == prev_state->ep_square);

        int idx = 0;
        if (p_type == 1) {
            if (capture) {
                san[idx++] = f_from;
                san[idx++] = 'x';
            }
            san[idx++] = f_to;
            san[idx++] = r_to;
            if (move.promotion) {
                san[idx++] = '=';
                san[idx++] = "  NBRQ"[abs(move.promotion)];
            }
        } else {
            san[idx++] = p_char;
            if (capture) san[idx++] = 'x';
            san[idx++] = f_to;
            san[idx++] = r_to;
        }
        san[idx] = '\0';
    }

    BoardState temp = *prev_state;
    make_move(&temp, move);
    bool check = is_in_check(&temp, temp.turn);
    MoveList legal;
    get_legal_moves(&temp, &legal);
    if (legal.count == 0) {
        if (check) strcat(san, "#");
    } else if (check) {
        strcat(san, "+");
    }
}

// --- Graphical Rendering Pipeline ---
void render_board() {
    printf("\033[H"); // Move cursor to home
    printf("\033[1;36m  CHESS TERMINAL GUI (Stockfish Engine)\033[0m\n");
    printf("  -----------------------------------------------------\n");

    bool is_king_in_check = is_in_check(&game_state, game_state.turn);
    bool is_legal_target[64];
    memset(is_legal_target, 0, 64);

    if (selected_sq != -1) {
        MoveList legal;
        get_legal_moves(&game_state, &legal);
        for (int i = 0; i < legal.count; i++) {
            if (legal.moves[i].from == selected_sq) {
                is_legal_target[legal.moves[i].to] = true;
            }
        }
    }

    for (int r = 7; r >= 0; r--) {
        printf("  %d │", r + 1);
        for (int f = 0; f < 8; f++) {
            int sq = r * 8 + f;
            int piece = game_state.board[sq];
            bool is_dark = ((r + f) % 2 == 0);

            // Palette Background Mapping
            int bg_color = is_dark ? 94 : 252; // Wooden Brown vs Light Grey
            if (sq == cursor_sq) {
                bg_color = 51; // Vibrant Cyan
            } else if (sq == selected_sq) {
                bg_color = 220; // Beautiful Gold
            } else if (is_legal_target[sq]) {
                bg_color = 120; // Soft Green (Legal target hint)
            } else if (is_king_in_check && piece == game_state.turn * 6) {
                bg_color = 196; // Crimson Red (Check warning)
            } else if (sq == last_move_from || sq == last_move_to) {
                bg_color = 117; // Cool Blue (Recent move historical marker)
            }

            // Print square and piece
            printf("\033[48;5;%dm", bg_color);
            if (piece > 0) {
                printf("\033[38;5;231m\033[1m %s ", " ♟♞♝♜♛♚"[piece]); // High Contrast Pure White
            } else if (piece < 0) {
                printf("\033[38;5;16m\033[1m %s ", " ♟♞♝♜♛♚"[-piece]);  // Solid Dark Velvet Black
            } else {
                printf("   ");
            }
            printf("\033[0m");
        }
        printf("│  ");

        // Side Menu Details Panel
        switch (r) {
            case 7: printf("Turn: %s", (game_state.turn == 1) ? "\033[1;37mWhite\033[0m" : "\033[1;30mBlack\033[0m"); break;
            case 6: printf("Last: %s", (history_count > 0) ? history_moves_san[history_count - 1] : "None"); break;
            case 5: printf("Mode: %s", (engine_color == 1) ? "Stockfish is White" : (engine_color == -1) ? "Stockfish is Black" : "Manual (PvP) [Toggle: M]"); break;
            case 4: printf("TC Options: [T] to Toggle Engine Config"); break;
            case 3: printf("Engine Config: %s Limit = %d", (time_control_type == 0) ? "Movetime" : (time_control_type == 1) ? "Depth" : "Nodes", time_control_value); break;
            case 2: printf("Stockfish connection: %s", (engine_pid != -1) ? "\033[1;32mOnline\033[0m" : "\033[1;31mOffline\033[0m"); break;
            case 1: printf("Controls: Arrows/WASD to Move | Space/Enter to Select"); break;
            case 0: printf("Takeback: [U] (Undo) | Trigger Engine: [E] | Quit: [Q]"); break;
        }
        printf("\n");
    }
    printf("     └────────────────────────┘\n");
    printf("       a  b  c  d  e  f  g  h\n\n");

    // Dynamic wrapped PGN History Drawer
    printf("\033[1m  PGN History List:\033[0m\n  ");
    int col_width = 0;
    for (int i = 0; i < history_count; i += 2) {
        char move_str[64];
        if (i + 1 < history_count) {
            sprintf(move_str, "%d. %s %s   ", (i / 2) + 1, history_moves_san[i], history_moves_san[i + 1]);
        } else {
            sprintf(move_str, "%d. %s   ", (i / 2) + 1, history_moves_san[i]);
        }
        if (col_width + strlen(move_str) > 75) {
            printf("\n  ");
            col_width = 0;
        }
        printf("%s", move_str);
        col_width += strlen(move_str);
    }
    printf("\n");
    fflush(stdout);
}

// --- Player Promotion Choice Prompt ---
int get_promotion_choice(int color) {
    disable_raw_mode();
    printf("\n\033[1;33m  Pawn Promotion! Choose: [Q]ueen, [R]ook, [B]ishop, [K]night: \033[0m");
    fflush(stdout);
    int choice = color * 5;
    while (1) {
        int c = getchar();
        if (c == 'q' || c == 'Q') { choice = color * 5; break; }
        if (c == 'r' || c == 'R') { choice = color * 4; break; }
        if (c == 'b' || c == 'B') { choice = color * 3; break; }
        if (c == 'k' || c == 'K' || c == 'n' || c == 'N') { choice = color * 2; break; }
    }
    enable_raw_mode();
    printf("\033[2J"); // Re-clear screen safely
    return choice;
}

// --- Parse Engine Response ---
Move parse_engine_move(const BoardState *state, const char *str) {
    Move move = {-1, -1, 0};
    if (strlen(str) < 4) return move;
    int f1 = str[0] - 'a';
    int r1 = str[1] - '1';
    int f2 = str[2] - 'a';
    int r2 = str[3] - '1';
    if (f1 < 0 || f1 > 7 || r1 < 0 || r1 > 7 || f2 < 0 || f2 > 7 || r2 < 0 || r2 > 7) {
        return move;
    }
    move.from = r1 * 8 + f1;
    move.to = r2 * 8 + f2;
    if (str[4] != '\0' && str[4] != ' ' && str[4] != '\n' && str[4] != '\r') {
        int promo = 0;
        switch (str[4]) {
            case 'q': promo = 5; break;
            case 'r': promo = 4; break;
            case 'b': promo = 3; break;
            case 'n': promo = 2; break;
        }
        move.promotion = state->turn * promo;
    }
    return move;
}

// --- Triggers Stockfish Engine Execution ---
void play_engine_move() {
    if (engine_pid == -1) return;
    
    // Check if game is already over
    MoveList legal;
    get_legal_moves(&game_state, &legal);
    if (legal.count == 0) return;

    printf("  \033[1;33mStockfish is thinking...\033[0m");
    fflush(stdout);

    char fen[256];
    generate_fen(&game_state, fen);
    char cmd[512];
    sprintf(cmd, "position fen %s\n", fen);
    send_to_engine(cmd);

    if (time_control_type == 0) {
        sprintf(cmd, "go movetime %d\n", time_control_value);
    } else if (time_control_type == 1) {
        sprintf(cmd, "go depth %d\n", time_control_value);
    } else {
        sprintf(cmd, "go nodes %d\n", time_control_value);
    }
    send_to_engine(cmd);

    char line[1024];
    Move engine_move = {-1, -1, 0};
    while (get_engine_line(line, sizeof(line), 20000)) {
        if (strncmp(line, "bestmove", 8) == 0) {
            char move_str[64];
            if (sscanf(line, "bestmove %s", move_str) == 1) {
                engine_move = parse_engine_move(&game_state, move_str);
            }
            break;
        }
    }

    if (engine_move.from != -1) {
        char san[16];
        to_san(&game_state, engine_move, san);
        history[history_count] = game_state;
        strcpy(history_moves_san[history_count], san);
        history_count++;

        make_move(&game_state, engine_move);
        last_move_from = engine_move.from;
        last_move_to = engine_move.to;
    }
}

// --- Main Program Entry ---
int main() {
    signal(SIGINT, handle_sigint);
    atexit(cleanup);

    load_fen(&game_state, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    init_engine();

    enable_raw_mode();
    printf("\033[2J"); // Initial screen clearing

    while (1) {
        render_board();

        // Check for Game Over States
        MoveList legal;
        get_legal_moves(&game_state, &legal);
        if (legal.count == 0) {
            if (is_in_check(&game_state, game_state.turn)) {
                printf("\n  \033[1;31mCHECKMATE! %s Wins!\033[0m\n", (game_state.turn == 1) ? "Black" : "White");
            } else {
                printf("\n  \033[1;33mSTALEMATE! Game is drawn.\033[0m\n");
            }
            disable_raw_mode();
            exit(0);
        }

        // Automatic Engine Action Handshake
        if (engine_color != 0 && game_state.turn == engine_color) {
            play_engine_move();
            continue;
        }

        int key = read_key();
        if (key == 0) {
            usleep(10000);
            continue;
        }

        if (key == 'q' || key == 'Q') {
            break;
        }

        // Arrow and WASD Navigation
        if (key == KEY_UP || key == 'w' || key == 'W') {
            if (cursor_sq / 8 < 7) cursor_sq += 8;
        } else if (key == KEY_DOWN || key == 's' || key == 'S') {
            if (cursor_sq / 8 > 0) cursor_sq -= 8;
        } else if (key == KEY_RIGHT || key == 'd' || key == 'D') {
            if (cursor_sq % 8 < 7) cursor_sq += 1;
        } else if (key == KEY_LEFT || key == 'a' || key == 'A') {
            if (cursor_sq % 8 > 0) cursor_sq -= 1;
        }

        // Selection Action Event Handlers
        else if (key == KEY_ENTER || key == KEY_SPACE) {
            if (selected_sq == -1) {
                // Select starting piece
                if (game_state.board[cursor_sq] * game_state.turn > 0) {
                    selected_sq = cursor_sq;
                }
            } else {
                if (cursor_sq == selected_sq) {
                    selected_sq = -1; // Deselect
                } else {
                    // Search if chosen coordinates map to a valid move
                    int found_idx = -1;
                    for (int i = 0; i < legal.count; i++) {
                        if (legal.moves[i].from == selected_sq && legal.moves[i].to == cursor_sq) {
                            found_idx = i;
                            break;
                        }
                    }

                    if (found_idx != -1) {
                        Move final_move = legal.moves[found_idx];
                        if (abs(game_state.board[selected_sq]) == 1 && (cursor_sq / 8 == 7 || cursor_sq / 8 == 0)) {
                            final_move.promotion = get_promotion_choice(game_state.turn);
                        }

                        char san[16];
                        to_san(&game_state, final_move, san);

                        // Update undo history logs
                        history[history_count] = game_state;
                        strcpy(history_moves_san[history_count], san);
                        history_count++;

                        make_move(&game_state, final_move);
                        last_move_from = final_move.from;
                        last_move_to = final_move.to;
                        selected_sq = -1;
                    } else if (game_state.board[cursor_sq] * game_state.turn > 0) {
                        selected_sq = cursor_sq; // Change selected piece
                    } else {
                        selected_sq = -1; // Invalid target click, reset selection
                    }
                }
            }
        }

        // Interactive Menu Actions
        else if (key == 'u' || key == 'U') {
            // Undo logic (Pushes states backwards)
            if (history_count > 0) {
                history_count--;
                game_state = history[history_count];
                // In engine play, we undo two moves (yours and engine's)
                if (engine_color != 0 && history_count > 0) {
                    history_count--;
                    game_state = history[history_count];
                }
                selected_sq = -1;
                last_move_from = -1;
                last_move_to = -1;
                printf("\033[2J");
            }
        } else if (key == 'e' || key == 'E') {
            // Force Stockfish to play the next move immediately
            play_engine_move();
        } else if (key == 'm' || key == 'M') {
            // Toggle through Engine Modes
            if (engine_color == 0) engine_color = -1; // Engine is Black
            else if (engine_color == -1) engine_color = 1; // Engine is White
            else engine_color = 0; // PvP Manual mode
        } else if (key == 't' || key == 'T') {
            // Swap Search Time Control Limits
            time_control_type = (time_control_type + 1) % 3;
            disable_raw_mode();
            printf("\n\033[1;33m  Enter target values for %s: \033[0m",
                   (time_control_type == 0) ? "Movetime (ms)" : (time_control_type == 1) ? "Depth" : "Nodes");
            int val;
            if (scanf("%d", &val) == 1 && val > 0) {
                time_control_value = val;
            }
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF); // Clear trailing carriage returns
            enable_raw_mode();
            printf("\033[2J");
        }
    }

    return 0;
}
