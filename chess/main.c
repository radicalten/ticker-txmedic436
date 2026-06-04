#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <fcntl.h>
#include <signal.h>
#include <locale.h>

#define MAX_HISTORY 1024

// --- Chess Engine Structs ---
typedef struct {
    int from;
    int to;
    int promo; // 0=none, 2=N, 3=B, 4=R, 5=Q
} Move;

typedef struct {
    int board[64];      // 0 = empty, 1 = P, 2 = N, 3 = B, 4 = R, 5 = Q, 6 = K. positive = White, negative = Black
    int turn;          // 1 = White, -1 = Black
    int castling[4];   // W_OO, W_OOO, B_OO, B_OOO
    int ep_square;     // en passant target square (-1 if none)
    int halfmove;
    int fullmove;
    int last_from;
    int last_to;
} GameState;

// --- Global Variables ---
GameState current_state;
GameState history[MAX_HISTORY];
Move played_moves[MAX_HISTORY];
char san_history[MAX_HISTORY][16];
int history_count = 0;

int selected_square = -1;
int cursor_r = 6;
int cursor_c = 4;
int running = 1;

// Engine properties
int engine_to_gui[2];
int gui_to_engine[2];
pid_t engine_pid = -1;
char engine_path[1024] = "stockfish";
int engine_connected = 0;
int engine_thinking = 0;

// Search controls
int engine_mode = 0; // 0 = Depth, 1 = Nodes, 2 = Move Time
int engine_depth = 8;
int engine_nodes = 50000;
int engine_time = 1500; // ms

// Terminal properties
struct termios orig_termios;

// --- Function Declarations ---
void init_board(GameState *state);
int get_legal_moves(const GameState *state, Move *legal);
int is_attacked(const GameState *state, int target_sq, int attacker_color);
int make_move(const GameState *prev, GameState *next, Move mv);
void get_san(const GameState *prev, Move mv, char *san);
void format_uci_move(Move mv, char *buf);
Move parse_uci_move(const char *str, const GameState *state);

int start_engine();
void stop_engine();
void send_to_engine(const char *cmd);
int poll_engine(char *best_move_out);
void trigger_engine_move();

void disableRawMode();
void enableRawMode();
int wait_for_input(int engine_fd, int timeout_ms);
void draw_game(const GameState *state);
void draw_sidebar_line(const GameState *state, int line);
int get_square_bg(int r, int c, int selected, int check_sq, int last_f, int last_t, int is_legal);
const char *get_piece_unicode(int pc);
void handle_input();
void handle_selection();

// --- Main Program ---
int main() {
    setlocale(LC_ALL, ""); // Enable unicode output in terminal
    
    init_board(&current_state);
    
    // Header welcome screen
    printf("\033[2J\033[H");
    printf("\033[1;36m====================================================\033[0m\n");
    printf("\033[1;36m           CHESS ENGINE TERMINAL GUI (C)            \033[0m\n");
    printf("\033[1;36m====================================================\033[0m\n\n");
    printf("Connecting to UCI Engine...\n");
    printf("Attempting to spawn Stockfish binary from default path...\n\n");
    
    if (!start_engine()) {
        printf("\033[1;31mCould not locate or start default 'stockfish' engine.\033[0m\n");
        printf("Enter custom path to UCI engine, or press [Enter] for local 2-Player mode: ");
        char custom_path[1024];
        if (fgets(custom_path, sizeof(custom_path), stdin)) {
            // strip newline
            custom_path[strcspn(custom_path, "\n")] = 0;
            if (strlen(custom_path) > 0) {
                strcpy(engine_path, custom_path);
                if (!start_engine()) {
                    printf("\033[1;31mFailed to start custom engine. Starting 2-Player offline mode...\033[0m\n");
                    sleep(2);
                }
            } else {
                printf("Entering local offline game mode...\n");
                sleep(1);
            }
        }
    } else {
        printf("\033[1;32mEngine connected successfully!\033[0m\n");
        sleep(1);
    }
    
    enableRawMode();
    printf("\033[?25l"); // Hide cursor
    printf("\033[2J");   // Clear screen once
    
    draw_game(&current_state);
    
    char engine_move_str[32];
    
    while (running) {
        int engine_fd = engine_connected ? engine_to_gui[0] : -1;
        int active_event = wait_for_input(engine_fd, 50); // wait for keyboard or engine pipe (50ms timeout)
        
        if (active_event == 1) {
            handle_input();
            draw_game(&current_state);
        } else if (active_event == 2) {
            if (poll_engine(engine_move_str)) {
                Move mv = parse_uci_move(engine_move_str, &current_state);
                if (mv.from != -1) {
                    GameState next;
                    if (make_move(&current_state, &next, mv)) {
                        history[history_count] = current_state;
                        played_moves[history_count] = mv;
                        get_san(&current_state, mv, san_history[history_count]);
                        history_count++;
                        
                        current_state = next;
                        engine_thinking = 0;
                        selected_square = -1;
                        draw_game(&current_state);
                    }
                }
            }
        }
    }
    
    printf("\033[?25h"); // Restore terminal cursor
    disableRawMode();
    stop_engine();
    printf("\nExited gracefully. Thanks for playing!\n");
    return 0;
}

// --- Chess Rule Engine Implementation ---
void init_board(GameState *state) {
    int back_row[] = {4, 2, 3, 5, 6, 3, 2, 4}; // R, N, B, Q, K, B, N, R
    for (int c = 0; c < 8; c++) {
        state->board[c] = -back_row[c];      // Black pieces
        state->board[8 + c] = -1;             // Black pawns
        for (int r = 2; r < 6; r++) {
            state->board[r * 8 + c] = 0;
        }
        state->board[48 + c] = 1;            // White pawns
        state->board[56 + c] = back_row[c];  // White pieces
    }
    state->turn = 1;
    state->castling[0] = state->castling[1] = 1; // White OO, OOO
    state->castling[2] = state->castling[3] = 1; // Black OO, OOO
    state->ep_square = -1;
    state->halfmove = 0;
    state->fullmove = 1;
    state->last_from = -1;
    state->last_to = -1;
}

int is_attacked(const GameState *state, int target_sq, int attacker_color) {
    int sq_r = target_sq / 8, sq_c = target_sq % 8;
    
    // Knight Attacks
    int knight_offs[] = {-17, -15, -10, -6, 6, 10, 15, 17};
    for (int i = 0; i < 8; i++) {
        int to = target_sq + knight_offs[i];
        if (to >= 0 && to < 64 && abs(to % 8 - sq_c) <= 2) {
            if (state->board[to] == attacker_color * 2) return 1;
        }
    }
    
    // Pawn Attacks
    int p_dir = (attacker_color == 1) ? 1 : -1; 
    int p_offsets[2] = {p_dir * 8 - 1, p_dir * 8 + 1};
    for (int i = 0; i < 2; i++) {
        int att_sq = target_sq + p_offsets[i];
        if (att_sq >= 0 && att_sq < 64 && abs(att_sq % 8 - sq_c) == 1) {
            if (state->board[att_sq] == attacker_color * 1) return 1;
        }
    }
    
    // Orthogonal sliding (Rook / Queen)
    int r_dirs[] = {-8, 8, -1, 1};
    for (int i = 0; i < 4; i++) {
        int curr = target_sq;
        while (1) {
            int prev_c = curr % 8;
            curr += r_dirs[i];
            if (curr < 0 || curr >= 64) break;
            if (abs(r_dirs[i]) == 1 && abs(curr % 8 - prev_c) > 1) break;
            int pc = state->board[curr];
            if (pc != 0) {
                if (pc == attacker_color * 4 || pc == attacker_color * 5) return 1;
                break;
            }
        }
    }
    
    // Diagonal sliding (Bishop / Queen)
    int b_dirs[] = {-9, -7, 7, 9};
    for (int i = 0; i < 4; i++) {
        int curr = target_sq;
        while (1) {
            int prev_c = curr % 8;
            curr += b_dirs[i];
            if (curr < 0 || curr >= 64) break;
            if (abs(curr % 8 - prev_c) != 1) break;
            int pc = state->board[curr];
            if (pc != 0) {
                if (pc == attacker_color * 3 || pc == attacker_color * 5) return 1;
                break;
            }
        }
    }
    
    // King Attacks
    int k_dirs[] = {-9, -8, -7, -1, 1, 7, 8, 9};
    for (int i = 0; i < 8; i++) {
        int to = target_sq + k_dirs[i];
        if (to >= 0 && to < 64 && abs(to % 8 - sq_c) <= 1) {
            if (state->board[to] == attacker_color * 6) return 1;
        }
    }
    return 0;
}

int generate_pseudo_moves(const GameState *state, Move *moves) {
    int count = 0;
    int side = state->turn;
    
    for (int sq = 0; sq < 64; sq++) {
        int pc = state->board[sq];
        if (pc == 0 || (pc > 0 && side < 0) || (pc < 0 && side > 0)) continue;
        
        int type = abs(pc);
        int sq_r = sq / 8, sq_c = sq % 8;
        
        if (type == 1) { // Pawn
            int dir = (side == 1) ? -8 : 8;
            int start_row = (side == 1) ? 6 : 1;
            int promo_row = (side == 1) ? 0 : 7;
            
            int to1 = sq + dir;
            if (to1 >= 0 && to1 < 64 && state->board[to1] == 0) {
                if (to1 / 8 == promo_row) {
                    moves[count++] = (Move){sq, to1, 5};
                    moves[count++] = (Move){sq, to1, 4};
                    moves[count++] = (Move){sq, to1, 3};
                    moves[count++] = (Move){sq, to1, 2};
                } else {
                    moves[count++] = (Move){sq, to1, 0};
                }
                int to2 = sq + 2 * dir;
                if (sq_r == start_row && state->board[to2] == 0) {
                    moves[count++] = (Move){sq, to2, 0};
                }
            }
            int cap_offs[] = {dir - 1, dir + 1};
            for (int i = 0; i < 2; i++) {
                int to = sq + cap_offs[i];
                if (to >= 0 && to < 64 && abs(to % 8 - sq_c) == 1) {
                    int tp = state->board[to];
                    if (tp != 0 && ((tp < 0 && side > 0) || (tp > 0 && side < 0))) {
                        if (to / 8 == promo_row) {
                            moves[count++] = (Move){sq, to, 5};
                            moves[count++] = (Move){sq, to, 4};
                            moves[count++] = (Move){sq, to, 3};
                            moves[count++] = (Move){sq, to, 2};
                        } else {
                            moves[count++] = (Move){sq, to, 0};
                        }
                    } else if (to == state->ep_square) {
                        moves[count++] = (Move){sq, to, 0};
                    }
                }
            }
        }
        else if (type == 2) { // Knight
            int knight_offs[] = {-17, -15, -10, -6, 6, 10, 15, 17};
            for (int i = 0; i < 8; i++) {
                int to = sq + knight_offs[i];
                if (to >= 0 && to < 64 && abs(to % 8 - sq_c) <= 2) {
                    int tp = state->board[to];
                    if (tp == 0 || (tp < 0 && side > 0) || (tp > 0 && side < 0)) {
                        moves[count++] = (Move){sq, to, 0};
                    }
                }
            }
        }
        else if (type == 3 || type == 5) { // Bishop / Queen diagonal
            int b_dirs[] = {-9, -7, 7, 9};
            for (int d = 0; d < 4; d++) {
                int curr = sq;
                while (1) {
                    int prev_c = curr % 8;
                    curr += b_dirs[d];
                    if (curr < 0 || curr >= 64) break;
                    if (abs(curr % 8 - prev_c) != 1) break;
                    int tp = state->board[curr];
                    if (tp == 0) {
                        moves[count++] = (Move){sq, curr, 0};
                    } else {
                        if ((tp < 0 && side > 0) || (tp > 0 && side < 0)) {
                            moves[count++] = (Move){sq, curr, 0};
                        }
                        break;
                    }
                }
            }
        }
        if (type == 4 || type == 5) { // Rook / Queen orthogonal
            int r_dirs[] = {-8, 8, -1, 1};
            for (int d = 0; d < 4; d++) {
                int curr = sq;
                while (1) {
                    int prev_c = curr % 8;
                    curr += r_dirs[d];
                    if (curr < 0 || curr >= 64) break;
                    if (abs(r_dirs[d]) == 1 && abs(curr % 8 - prev_c) > 1) break;
                    int tp = state->board[curr];
                    if (tp == 0) {
                        moves[count++] = (Move){sq, curr, 0};
                    } else {
                        if ((tp < 0 && side > 0) || (tp > 0 && side < 0)) {
                            moves[count++] = (Move){sq, curr, 0};
                        }
                        break;
                    }
                }
            }
        }
        else if (type == 6) { // King
            int k_dirs[] = {-9, -8, -7, -1, 1, 7, 8, 9};
            for (int i = 0; i < 8; i++) {
                int to = sq + k_dirs[i];
                if (to >= 0 && to < 64 && abs(to % 8 - sq_c) <= 1) {
                    int tp = state->board[to];
                    if (tp == 0 || (tp < 0 && side > 0) || (tp > 0 && side < 0)) {
                        moves[count++] = (Move){sq, to, 0};
                    }
                }
            }
            // Castling pseudo checks (further detailed safety verified inside make_move)
            if (side == 1) {
                if (state->castling[0] && state->board[61] == 0 && state->board[62] == 0)
                    moves[count++] = (Move){60, 62, 0};
                if (state->castling[1] && state->board[59] == 0 && state->board[58] == 0 && state->board[57] == 0)
                    moves[count++] = (Move){60, 58, 0};
            } else {
                if (state->castling[2] && state->board[5] == 0 && state->board[6] == 0)
                    moves[count++] = (Move){4, 6, 0};
                if (state->castling[3] && state->board[3] == 0 && state->board[2] == 0 && state->board[1] == 0)
                    moves[count++] = (Move){4, 2, 0};
            }
        }
    }
    return count;
}

int make_move(const GameState *prev, GameState *next, Move mv) {
    *next = *prev;
    int side = prev->turn;
    int pc = prev->board[mv.from];
    int type = abs(pc);
    
    // Castling restrictions & safety
    if (type == 6 && abs(mv.from - mv.to) == 2) {
        if (is_attacked(prev, mv.from, -side)) return 0; // cannot castle out of check
        int step = (mv.to > mv.from) ? 1 : -1;
        if (is_attacked(prev, mv.from + step, -side)) return 0; // intermediate squares
        if (is_attacked(prev, mv.to, -side)) return 0; // target square
    }
    
    // Apply movement
    next->board[mv.from] = 0;
    if (mv.promo != 0) {
        next->board[mv.to] = side * mv.promo;
    } else {
        next->board[mv.to] = pc;
    }
    
    // Execute Rook movement for Castles
    if (type == 6) {
        if (mv.from == 60 && mv.to == 62) { next->board[61] = 4; next->board[63] = 0; }
        else if (mv.from == 60 && mv.to == 58) { next->board[59] = 4; next->board[56] = 0; }
        else if (mv.from == 4 && mv.to == 6) { next->board[5] = -4; next->board[7] = 0; }
        else if (mv.from == 4 && mv.to == 2) { next->board[3] = -4; next->board[0] = 0; }
    }
    
    // Execute En Passant capture
    if (type == 1 && mv.to == prev->ep_square) {
        int ep_victim = mv.to + ((side == 1) ? 8 : -8);
        next->board[ep_victim] = 0;
    }
    
    // Track En Passant opportunity
    if (type == 1 && abs(mv.from - mv.to) == 16) {
        next->ep_square = mv.from + ((side == 1) ? -8 : 8);
    } else {
        next->ep_square = -1;
    }
    
    // Revoke Castling rights
    if (mv.from == 60) { next->castling[0] = 0; next->castling[1] = 0; }
    if (mv.from == 4)  { next->castling[2] = 0; next->castling[3] = 0; }
    if (mv.from == 56 || mv.to == 56) next->castling[1] = 0;
    if (mv.from == 63 || mv.to == 63) next->castling[0] = 0;
    if (mv.from == 0  || mv.to == 0)  next->castling[3] = 0;
    if (mv.from == 7  || mv.to == 7)  next->castling[2] = 0;
    
    // Validate King Safety
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (next->board[i] == side * 6) { king_sq = i; break; }
    }
    if (king_sq != -1 && is_attacked(next, king_sq, -side)) {
        return 0; // King remains in check (illegal move)
    }
    
    next->turn = -side;
    next->last_from = mv.from;
    next->last_to = mv.to;
    if (side == -1) next->fullmove++;
    if (type == 1 || prev->board[mv.to] != 0) {
        next->halfmove = 0;
    } else {
        next->halfmove++;
    }
    return 1;
}

int get_legal_moves(const GameState *state, Move *legal) {
    Move pseudo[256];
    int p_count = generate_pseudo_moves(state, pseudo);
    int l_count = 0;
    for (int i = 0; i < p_count; i++) {
        GameState dummy;
        if (make_move(state, &dummy, pseudo[i])) {
            legal[l_count++] = pseudo[i];
        }
    }
    return l_count;
}

void get_san(const GameState *prev, Move mv, char *san) {
    int pc = abs(prev->board[mv.from]);
    int is_cap = (prev->board[mv.to] != 0 || (pc == 1 && mv.to == prev->ep_square));
    char p_char = '\0';
    if (pc == 2) p_char = 'N';
    else if (pc == 3) p_char = 'B';
    else if (pc == 4) p_char = 'R';
    else if (pc == 5) p_char = 'Q';
    else if (pc == 6) p_char = 'K';
    
    if (pc == 6 && abs(mv.from - mv.to) == 2) {
        if (mv.to > mv.from) strcpy(san, "O-O");
        else strcpy(san, "O-O-O");
        return;
    }
    
    int idx = 0;
    if (p_char != '\0') {
        san[idx++] = p_char;
    } else if (is_cap) {
        san[idx++] = 'a' + (mv.from % 8);
    }
    
    if (is_cap) san[idx++] = 'x';
    san[idx++] = 'a' + (mv.to % 8);
    san[idx++] = '8' - (mv.to / 8);
    
    if (mv.promo == 5) { san[idx++] = '='; san[idx++] = 'Q'; }
    else if (mv.promo == 4) { san[idx++] = '='; san[idx++] = 'R'; }
    else if (mv.promo == 3) { san[idx++] = '='; san[idx++] = 'B'; }
    else if (mv.promo == 2) { san[idx++] = '='; san[idx++] = 'N'; }
    san[idx] = '\0';
    
    GameState post;
    if (make_move(prev, &post, mv)) {
        int enemy_king = -1;
        for (int i = 0; i < 64; i++) {
            if (post.board[i] == -prev->turn * 6) { enemy_king = i; break; }
        }
        if (enemy_king != -1 && is_attacked(&post, enemy_king, prev->turn)) {
            Move temp[256];
            if (get_legal_moves(&post, temp) == 0) strcat(san, "#");
            else strcat(san, "+");
        }
    }
}

void format_uci_move(Move mv, char *buf) {
    sprintf(buf, "%c%d%c%d", 'a' + (mv.from % 8), 8 - (mv.from / 8), 'a' + (mv.to % 8), 8 - (mv.to / 8));
    if (mv.promo == 5) strcat(buf, "q");
    else if (mv.promo == 4) strcat(buf, "r");
    else if (mv.promo == 3) strcat(buf, "b");
    else if (mv.promo == 2) strcat(buf, "n");
}

Move parse_uci_move(const char *str, const GameState *state) {
    Move mv = {-1, -1, 0};
    if (strlen(str) < 4) return mv;
    int f_col = str[0] - 'a', f_row = '8' - str[1];
    int t_col = str[2] - 'a', t_row = '8' - str[3];
    if (f_col >= 0 && f_col < 8 && f_row >= 0 && f_row < 8 &&
        t_col >= 0 && t_col < 8 && t_row >= 0 && t_row < 8) {
        mv.from = f_row * 8 + f_col;
        mv.to = t_row * 8 + t_col;
        if (str[4] == 'q') mv.promo = 5;
        else if (str[4] == 'r') mv.promo = 4;
        else if (str[4] == 'b') mv.promo = 3;
        else if (str[4] == 'n') mv.promo = 2;
    }
    return mv;
}

// --- Engine I/O Pipeline ---
int start_engine() {
    if (pipe(engine_to_gui) < 0 || pipe(gui_to_engine) < 0) return 0;
    engine_pid = fork();
    if (engine_pid < 0) return 0;
    if (engine_pid == 0) { // Child Process
        dup2(gui_to_engine[0], STDIN_FILENO);
        dup2(engine_to_gui[1], STDOUT_FILENO);
        close(gui_to_engine[1]);
        close(engine_to_gui[0]);
        char *args[] = {engine_path, NULL};
        execvp(engine_path, args);
        exit(127); // Failure execution code
    }
    close(gui_to_engine[0]);
    close(engine_to_gui[1]);
    
    // Set child output pipe read-end to non-blocking
    int flags = fcntl(engine_to_gui[0], F_GETFL, 0);
    fcntl(engine_to_gui[0], F_SETFL, flags | O_NONBLOCK);
    
    engine_connected = 1;
    send_to_engine("uci\n");
    send_to_engine("isready\n");
    return 1;
}

void stop_engine() {
    if (engine_connected && engine_pid > 0) {
        send_to_engine("quit\n");
        kill(engine_pid, SIGTERM);
        close(gui_to_engine[1]);
        close(engine_to_gui[0]);
        engine_connected = 0;
        engine_pid = -1;
    }
}

void send_to_engine(const char *cmd) {
    if (engine_connected) write(gui_to_engine[1], cmd, strlen(cmd));
}

char engine_read_buf[8192];
int engine_read_len = 0;

int poll_engine(char *best_move_out) {
    if (!engine_connected) return 0;
    char temp[1024];
    ssize_t n = read(engine_to_gui[0], temp, sizeof(temp) - 1);
    if (n > 0) {
        temp[n] = '\0';
        if (engine_read_len + n < (int)sizeof(engine_read_buf) - 1) {
            memcpy(engine_read_buf + engine_read_len, temp, n);
            engine_read_len += n;
            engine_read_buf[engine_read_len] = '\0';
        } else {
            engine_read_len = 0; // Overflow safety
        }
        
        char *line_start = engine_read_buf;
        char *newline;
        int found_bestmove = 0;
        while ((newline = strchr(line_start, '\n')) != NULL) {
            *newline = '\0';
            if (strncmp(line_start, "bestmove ", 9) == 0) {
                sscanf(line_start, "bestmove %s", best_move_out);
                found_bestmove = 1;
            }
            line_start = newline + 1;
        }
        int consumed = line_start - engine_read_buf;
        if (consumed > 0) {
            memmove(engine_read_buf, line_start, engine_read_len - consumed);
            engine_read_len -= consumed;
            engine_read_buf[engine_read_len] = '\0';
        }
        return found_bestmove;
    }
    return 0;
}

void trigger_engine_move() {
    if (!engine_connected) return;
    char cmd[8192] = "position startpos moves";
    for (int i = 0; i < history_count; i++) {
        char mv_str[16];
        format_uci_move(played_moves[i], mv_str);
        strcat(cmd, " ");
        strcat(cmd, mv_str);
    }
    strcat(cmd, "\n");
    send_to_engine(cmd);
    
    char search_cmd[128];
    if (engine_mode == 0) {
        sprintf(search_cmd, "go depth %d\n", engine_depth);
    } else if (engine_mode == 1) {
        sprintf(search_cmd, "go nodes %d\n", engine_nodes);
    } else {
        sprintf(search_cmd, "go movetime %d\n", engine_time);
    }
    send_to_engine(search_cmd);
    engine_thinking = 1;
}

// --- Terminal Handlers ---
void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int wait_for_input(int engine_fd, int timeout_ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    int max_fd = STDIN_FILENO;
    if (engine_fd >= 0) {
        FD_SET(engine_fd, &fds);
        if (engine_fd > max_fd) max_fd = engine_fd;
    }
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    int result = select(max_fd + 1, &fds, NULL, NULL, &tv);
    if (result <= 0) return 0;
    if (FD_ISSET(STDIN_FILENO, &fds)) return 1;
    if (engine_fd >= 0 && FD_ISSET(engine_fd, &fds)) return 2;
    return 0;
}

// --- Visual Rendering ---
int get_square_bg(int r, int c, int selected, int check_sq, int last_f, int last_t, int is_legal) {
    int sq = r * 8 + c;
    if (r == cursor_r && c == cursor_c) return 202;       // Flashing Orange active selector
    if (sq == selected) return 220;                     // Solid Yellow selection
    if (sq == check_sq) return 196;                     // Bright Red Check alert
    if (is_legal) return 114;                           // Soft Sage-green target indicators
    if (sq == last_f || sq == last_t) return 153;       // Soft Blue history indicators
    return ((r + c) % 2 == 0) ? 254 : 243;              // Alternating light beige and charcoal
}

const char *get_piece_unicode(int pc) {
    int type = abs(pc);
    switch (type) {
        case 1: return "♟";
        case 2: return "♞";
        case 3: return "♝";
        case 4: return "♜";
        case 5: return "♛";
        case 6: return "♚";
        default: return " ";
    }
}

void draw_game(const GameState *state) {
    printf("\033[H"); // Cursor to 1,1
    
    int check_sq = -1;
    int current_king = -1;
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == state->turn * 6) { current_king = i; break; }
    }
    if (current_king != -1 && is_attacked(state, current_king, -state->turn)) {
        check_sq = current_king;
    }
    
    Move legal[256];
    int legal_count = get_legal_moves(state, legal);
    int active_targets[64] = {0};
    if (selected_square != -1) {
        for (int i = 0; i < legal_count; i++) {
            if (legal[i].from == selected_square) active_targets[legal[i].to] = 1;
        }
    }
    
    for (int r = 0; r < 8; r++) {
        for (int sub_y = 0; sub_y < 3; sub_y++) {
            if (sub_y == 1) printf(" %d ", 8 - r);
            else printf("   ");
            
            for (int c = 0; c < 8; c++) {
                int sq = r * 8 + c;
                int bg = get_square_bg(r, c, selected_square, check_sq, state->last_from, state->last_to, active_targets[sq]);
                int pc = state->board[sq];
                
                printf("\033[48;5;%dm", bg);
                if (sub_y == 1) {
                    if (pc != 0) {
                        if (pc > 0) printf("\033[38;5;231;1m  %s   ", get_piece_unicode(pc)); // White pieces
                        else printf("\033[38;5;16;1m  %s   ", get_piece_unicode(pc));  // Black pieces
                    } else if (active_targets[sq]) {
                        printf("\033[38;5;28;1m  •   "); // Dot indicator for empty legal moves
                    } else {
                        printf("      ");
                    }
                } else {
                    printf("      ");
                }
            }
            printf("\033[0m");
            draw_sidebar_line(state, r * 3 + sub_y);
            printf("\n");
        }
    }
    printf("     ");
    for (int c = 0; c < 8; c++) printf("  %c   ", 'A' + c);
    printf("\033[K\n");
}

void draw_sidebar_line(const GameState *state, int line) {
    printf("   │ ");
    switch (line) {
        case 0:  printf("\033[1;36m=== CHESS ENGINE TERMINAL GUI ===\033[0m"); break;
        case 1:  printf("---------------------------------"); break;
        case 2:
            if (state->turn == 1) printf("Active Turn: \033[1;33mWhite\033[0m");
            else printf("Active Turn: \033[1;35mBlack\033[0m");
            break;
        case 3: {
            Move temp[256];
            int l_count = get_legal_moves(state, temp);
            int king_sq = -1;
            for (int i = 0; i < 64; i++) {
                if (state->board[i] == state->turn * 6) { king_sq = i; break; }
            }
            int in_chk = (king_sq != -1 && is_attacked(state, king_sq, -state->turn));
            if (l_count == 0) {
                if (in_chk) printf("Status     : \033[1;31mCHECKMATE!\033[0m");
                else printf("Status     : \033[1;33mSTALEMATE!\033[0m");
            } else if (in_chk) {
                printf("Status     : \033[1;31mCHECK\033[0m");
            } else {
                printf("Status     : Normal Play");
            }
            break;
        }
        case 5:  printf("\033[1;33mENGINE STATUS:\033[0m"); break;
        case 6:
            if (engine_connected) printf("  Binary   : \033[1;32m%s\033[0m", engine_path);
            else printf("  Binary   : \033[1;31mDisconnected\033[0m");
            break;
        case 7:
            if (engine_thinking) printf("  State    : \033[1;5;33mSearching...\033[0m");
            else printf("  State    : Idle");
            break;
        case 9:  printf("\033[1;33mSEARCH PARAMETERS (Change with [t]/[[]/[]]):\033[0m"); break;
        case 10: printf("  [t] Mode : \033[1;36m%s\033[0m", engine_mode == 0 ? "Depth" : (engine_mode == 1 ? "Nodes" : "Move Time")); break;
        case 11:
            if (engine_mode == 0) printf("  [[]/[]]  : \033[1;32m%d ply\033[0m", engine_depth);
            else if (engine_mode == 1) printf("  [[]/[]]  : \033[1;32m%d nodes\033[0m", engine_nodes);
            else printf("  [[]/[]]  : \033[1;32m%.1fs\033[0m", engine_time / 1000.0);
            break;
        case 13: printf("\033[1;33mRECENT MOVES (PGN):\033[0m"); break;
        case 14: case 15: case 16: case 17: case 18: case 19: case 20: case 21: {
            int move_pair_line = line - 14;
            int total_full = (history_count + 1) / 2;
            int offset = total_full - 8;
            if (offset < 0) offset = 0;
            int curr_full = offset + move_pair_line;
            int w_idx = curr_full * 2;
            int b_idx = curr_full * 2 + 1;
            if (w_idx < history_count) {
                printf("  %d. %-8s", curr_full + 1, san_history[w_idx]);
                if (b_idx < history_count) printf(" %-8s", san_history[b_idx]);
            }
            break;
        }
        case 22: printf("\033[1;33mCONTROLS:\033[0m"); break;
        case 23: printf("  WASD/Arrows: Move | Space/Enter: Click"); break;
        case 24: printf("  [u]: Undo | [e]: Play Engine | [q]: Quit"); break;
    }
    printf("\033[K");
}

// --- Input Processing ---
void handle_input() {
    char c;
    if (read(STDIN_FILENO, &c, 1) <= 0) return;
    
    if (c == '\033') { // Arrow Key Escape sequence parser
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
            if (seq[0] == '[') {
                switch (seq[1]) {
                    case 'A': if (cursor_r > 0) cursor_r--; break; // Up
                    case 'B': if (cursor_r < 7) cursor_r++; break; // Down
                    case 'C': if (cursor_c < 7) cursor_c++; break; // Right
                    case 'D': if (cursor_c > 0) cursor_c--; break; // Left
                }
            }
        }
    } else {
        switch (c) {
            case 'w': case 'W': if (cursor_r > 0) cursor_r--; break;
            case 's': case 'S': if (cursor_r < 7) cursor_r++; break;
            case 'a': case 'A': if (cursor_c > 0) cursor_c--; break;
            case 'd': case 'D': if (cursor_c < 7) cursor_c++; break;
            case ' ': case '\n': case '\r': handle_selection(); break;
            case 'u': case 'U':
                if (engine_thinking) break;
                if (engine_connected && history_count >= 2) {
                    current_state = history[--history_count];
                    current_state = history[--history_count];
                } else if (history_count >= 1) {
                    current_state = history[--history_count];
                }
                selected_square = -1;
                break;
            case 'e': case 'E':
                if (!engine_thinking) trigger_engine_move();
                break;
            case 't': case 'T':
                engine_mode = (engine_mode + 1) % 3;
                break;
            case '[':
                if (engine_mode == 0 && engine_depth > 1) engine_depth--;
                else if (engine_mode == 1 && engine_nodes > 10000) engine_nodes -= 10000;
                else if (engine_mode == 2 && engine_time > 500) engine_time -= 500;
                break;
            case ']':
                if (engine_mode == 0 && engine_depth < 30) engine_depth++;
                else if (engine_mode == 1 && engine_nodes < 1000000) engine_nodes += 10000;
                else if (engine_mode == 2 && engine_time < 60000) engine_time += 500;
                break;
            case 'q': case 'Q':
                running = 0;
                break;
        }
    }
}

void handle_selection() {
    int sq = cursor_r * 8 + cursor_c;
    int pc = current_state.board[sq];
    int turn = current_state.turn;
    
    if (selected_square == -1) {
        if (pc != 0 && ((pc > 0 && turn == 1) || (pc < 0 && turn == -1))) {
            selected_square = sq;
        }
    } else {
        if (sq == selected_square) {
            selected_square = -1;
        } else if (pc != 0 && ((pc > 0 && turn == 1) || (pc < 0 && turn == -1))) {
            selected_square = sq; // Change active piece selection
        } else {
            // Attempt to make move
            Move legal[256];
            int count = get_legal_moves(&current_state, legal);
            int chosen_idx = -1;
            for (int i = 0; i < count; i++) {
                if (legal[i].from == selected_square && legal[i].to == sq) {
                    chosen_idx = i;
                    break;
                }
            }
            if (chosen_idx != -1) {
                GameState next;
                if (make_move(&current_state, &next, legal[chosen_idx])) {
                    history[history_count] = current_state;
                    played_moves[history_count] = legal[chosen_idx];
                    get_san(&current_state, legal[chosen_idx], san_history[history_count]);
                    history_count++;
                    
                    current_state = next;
                    selected_square = -1;
                    
                    // Trigger engine move automatically if playing with an active engine
                    if (engine_connected && current_state.turn == -1) {
                        trigger_engine_move();
                    }
                }
            }
        }
    }
}
