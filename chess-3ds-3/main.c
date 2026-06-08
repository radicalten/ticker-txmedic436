#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <unistd.h>
#include <3ds.h>

#define MAX_HISTORY 2048

// --- THREAD-SAFE QUEUE SYSTEM ---
typedef struct {
    char buffer[16384];
    int head;
    int tail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} SafeQueue;

SafeQueue gui_to_engine;
SafeQueue engine_to_gui;

void queue_init(SafeQueue *q) {
    q->head = 0;
    q->tail = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
}

void queue_write(SafeQueue *q, const char *str, int len) {
    pthread_mutex_lock(&q->lock);
    for (int i = 0; i < len; i++) {
        q->buffer[q->head] = str[i];
        q->head = (q->head + 1) % sizeof(q->buffer);
    }
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

int queue_read_blocking(SafeQueue *q, char *out_buf, int max_len) {
    pthread_mutex_lock(&q->lock);
    while (q->head == q->tail) {
        pthread_cond_wait(&q->cond, &q->lock);
    }
    int idx = 0;
    while (q->head != q->tail && idx < max_len) {
        out_buf[idx++] = q->buffer[q->tail];
        q->tail = (q->tail + 1) % sizeof(q->buffer);
    }
    pthread_mutex_unlock(&q->lock);
    return idx;
}

int queue_read_nonblocking(SafeQueue *q, char *out_buf, int max_len) {
    pthread_mutex_lock(&q->lock);
    int idx = 0;
    while (q->head != q->tail && idx < max_len) {
        out_buf[idx++] = q->buffer[q->tail];
        q->tail = (q->tail + 1) % sizeof(q->buffer);
    }
    pthread_mutex_unlock(&q->lock);
    return idx;
}


// --- LINKER WRAPPING FOR INTERCEPTING CFISH STANDARD I/O ---
// These are the real, underlying 3DS system functions
extern int __real_printf(const char *format, ...);
extern int __real_putchar(int c);
extern int __real_puts(const char *s);
extern ssize_t __real_getline(char **lineptr, size_t *n, FILE *stream);

// Define a macro so our GUI functions can write directly to the LCD screen
#define gui_printf(...) __real_printf(__VA_ARGS__)

// Any printf call in Cfish's files will automatically route through here instead
int __wrap_printf(const char *format, ...) {
    char temp[2048];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(temp, sizeof(temp), format, args);
    va_end(args);
    queue_write(&engine_to_gui, temp, n);
    return n;
}

int __wrap_putchar(int c) {
    char ch = (char)c;
    queue_write(&engine_to_gui, &ch, 1);
    return c;
}

int __wrap_puts(const char *s) {
    int len = strlen(s);
    queue_write(&engine_to_gui, s, len);
    queue_write(&engine_to_gui, "\n", 1);
    return len + 1;
}

ssize_t __wrap_getline(char **lineptr, size_t *n, FILE *stream) {
    static char local_line[4096];
    int idx = 0;
    while (idx < (int)sizeof(local_line) - 1) {
        char c;
        if (queue_read_blocking(&gui_to_engine, &c, 1) <= 0) {
            break;
        }
        local_line[idx++] = c;
        if (c == '\n') break;
    }
    local_line[idx] = '\0';

    if (*lineptr == NULL || *n < (size_t)idx + 1) {
        *lineptr = realloc(*lineptr, idx + 1);
        *n = idx + 1;
    }
    strcpy(*lineptr, local_line);
    return idx;
}


// --- CHESS ENGINE & GUI STATE MANAGEMENT ---
typedef struct {
    int board[64];
    int turn;      
    int castle;    
    int ep;        
    int halfmoves; 
    int fullmoves;
} BoardState;

typedef struct {
    int from;
    int to;
    int promo; 
} GuiMove;

BoardState current_state;
BoardState history[MAX_HISTORY];
GuiMove move_history[MAX_HISTORY];
int history_count = 0;

int cursor_r = 6;  
int cursor_c = 4;  
int selected_sq = -1;

int board_orientation = 1; 
int user_side = 1;         // 1 = White, -1 = Black, 0 = Hotseat, 2 = Watch

int time_control_type = 0;   
int time_control_val = 1000; 

pthread_t engine_thread;
int engine_thinking = 0;
long long engine_nps = 0;    
int engine_score_type = -1;  
int engine_score_val = 0;    

char engine_buffer[8192];
int engine_buf_len = 0;
int engine_running = 0;

extern void psqt_init();
extern void bitboards_init();
extern void zob_init();
extern void bitbases_init();
#ifndef NNUE_PURE
extern void endgames_init();
#endif
extern void threads_init();
extern void options_init();
extern void search_clear();
extern void uci_loop(int argc, char **argv);

void init_board(BoardState *state);
int is_legal_move(const BoardState *state, GuiMove m);
int has_legal_moves(const BoardState *state);
int is_square_attacked(const BoardState *state, int sq, int attacker);
void make_move(const BoardState *src, BoardState *dst, GuiMove m);
void print_side_panel_line(int panel_row);
void send_to_engine(const char *cmd);
int find_king(const BoardState *state, int color);
int count_repetitions(const BoardState *state);
int get_promo_choice();
void draw_ui();

void* cfish_worker_thread(void *arg) {
    psqt_init();
    bitboards_init();
    zob_init();
    bitbases_init();
#ifndef NNUE_PURE
    endgames_init();
#endif
    threads_init();
    options_init();
    search_clear();

    char *argv[] = {"Cfish", NULL};
    uci_loop(1, argv);

    return NULL;
}

void start_engine() {
    queue_init(&gui_to_engine);
    queue_init(&engine_to_gui);
    
    engine_running = 1;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024); 

    pthread_create(&engine_thread, &attr, cfish_worker_thread, NULL);
    pthread_attr_destroy(&attr);

    send_to_engine("uci\nisready\n");
}

void send_to_engine(const char *cmd) {
    if (engine_running) {
        queue_write(&gui_to_engine, cmd, strlen(cmd));
    }
}

int screen_to_board_sq(int r, int c) {
    return (board_orientation == 1) ? (r * 8 + c) : ((7 - r) * 8 + (7 - c));
}

void gui_move_to_uci(GuiMove m, char *buf) {
    int f_col = m.from % 8;
    int f_row = 8 - (m.from / 8);
    int t_col = m.to % 8;
    int t_row = 8 - (m.to / 8);
    sprintf(buf, "%c%d%c%d", 'a' + f_col, f_row, 'a' + t_col, t_row);
    if (m.promo != 0) {
        char p = 'q';
        if (m.promo == 2) p = 'n';
        if (m.promo == 3) p = 'b';
        if (m.promo == 4) p = 'r';
        sprintf(buf + 4, "%c", p);
    }
}

GuiMove gui_uci_to_move(const char *str) {
    GuiMove m = {-1, -1, 0};
    if (strlen(str) < 4) return m;
    int f_col = str[0] - 'a';
    int f_row = 8 - (str[1] - '0');
    int t_col = str[2] - 'a';
    int t_row = 8 - (str[3] - '0');
    m.from = f_row * 8 + f_col;
    m.to = t_row * 8 + t_col;
    if (strlen(str) == 5) {
        char p = str[4];
        if (p == 'n') m.promo = 2;
        else if (p == 'b') m.promo = 3;
        else if (p == 'r') m.promo = 4;
        else m.promo = 5; 
    }
    return m;
}

void push_state(const BoardState *state, GuiMove m) {
    if (history_count < MAX_HISTORY - 1) {
        history[history_count] = *state;
        move_history[history_count] = m;
        history_count++;
    }
}

void trigger_engine_move() {
    engine_nps = 0; 
    engine_score_type = -1; 
    engine_score_val = 0;

    static char cmd[16384]; 
    cmd[0] = '\0';
    strcpy(cmd, "position startpos moves");
    
    int len = strlen(cmd);
    for (int i = 0; i < history_count; i++) {
        char uci_m[10];
        gui_move_to_uci(move_history[i], uci_m);
        int move_len = strlen(uci_m);
        
        if (len + 1 + move_len + 2 >= (int)sizeof(cmd)) {
            break; 
        }
        cmd[len++] = ' ';
        strcpy(cmd + len, uci_m);
        len += move_len;
    }
    cmd[len++] = '\n';
    cmd[len] = '\0';
    
    send_to_engine(cmd);

    char go_cmd[256];
    if (time_control_type == 0) {
        sprintf(go_cmd, "go movetime %d\n", time_control_val);
    } else if (time_control_type == 1) {
        sprintf(go_cmd, "go depth %d\n", time_control_val);
    } else {
        sprintf(go_cmd, "go nodes %d\n", time_control_val);
    }
    send_to_engine(go_cmd);
}

void process_engine_output(char *line) {
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[len - 1] = '\0';
        len--;
    }

    if (strncmp(line, "info", 4) == 0) {
        char *nps_ptr = strstr(line, " nps ");
        if (nps_ptr) {
            long long val;
            if (sscanf(nps_ptr, " nps %lld", &val) == 1) {
                engine_nps = val;
            }
        }

        char *score_ptr = strstr(line, " score ");
        if (score_ptr) {
            int score_val = 0;
            char score_type[16];
            if (sscanf(score_ptr, " score %15s %d", score_type, &score_val) == 2) {
                if (strcmp(score_type, "cp") == 0) {
                    engine_score_type = 0;
                    engine_score_val = score_val * current_state.turn;
                } else if (strcmp(score_type, "mate") == 0) {
                    engine_score_type = 1;
                    engine_score_val = score_val * current_state.turn;
                }
            }
        }
    }

    if (strncmp(line, "bestmove", 8) == 0) {
        char move_str[16];
        if (sscanf(line, "bestmove %15s", move_str) == 1) {
            if (strcmp(move_str, "(none)") == 0 || strcmp(move_str, "NULL") == 0) {
                engine_thinking = 0;
                return;
            }
            GuiMove m = gui_uci_to_move(move_str);
            if (is_legal_move(&current_state, m)) {
                push_state(&current_state, m);
                BoardState next;
                make_move(&current_state, &next, m);
                current_state = next;
            }
            engine_thinking = 0;
        }
    }
}

void read_from_engine() {
    char tmp[4096];
    int n;
    while ((n = queue_read_nonblocking(&engine_to_gui, tmp, sizeof(tmp))) > 0) {
        tmp[n] = '\0';
        for (int i = 0; i < n; i++) {
            if (tmp[i] == '\n') {
                if (engine_buf_len > 0) {
                    engine_buffer[engine_buf_len] = '\0';
                    process_engine_output(engine_buffer);
                    engine_buf_len = 0;
                }
            } else if (tmp[i] != '\r') {
                if (engine_buf_len < (int)sizeof(engine_buffer) - 1) {
                    engine_buffer[engine_buf_len++] = tmp[i];
                } else {
                    engine_buffer[engine_buf_len] = '\0';
                    process_engine_output(engine_buffer);
                    engine_buf_len = 0;
                    engine_buffer[engine_buf_len++] = tmp[i];
                }
            }
        }
    }
}

// --- STANDARD BOARD RULES LOGIC ---
int find_king(const BoardState *state, int color) {
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == color * 6) return i;
    }
    return -1;
}

int is_square_attacked(const BoardState *state, int sq, int attacker) {
    int r = sq / 8, c = sq % 8;

    int k_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int k_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + k_r[i], nc = c + k_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 2) return 1;
        }
    }

    int kg_r[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int kg_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kg_r[i], nc = c + kg_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 6) return 1;
        }
    }

    int p_offset = (attacker == 1) ? 1 : -1;
    for (int dc = -1; dc <= 1; dc += 2) {
        int nr = r + p_offset, nc = c + dc;
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 1) return 1;
        }
    }

    int d_r[] = {-1, -1, 1, 1};
    int d_c[] = {-1, 1, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + d_r[i], nc = c + d_c[i];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int target = state->board[nr * 8 + nc];
            if (target != 0) {
                if (target == attacker * 3 || target == attacker * 5) return 1;
                break;
            }
            nr += d_r[i]; nc += d_c[i];
        }
    }

    int s_r[] = {-1, 1, 0, 0};
    int s_c[] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + s_r[i], nc = c + s_c[i];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int target = state->board[nr * 8 + nc];
            if (target != 0) {
                if (target == attacker * 4 || target == attacker * 5) return 1;
                break;
            }
            nr += s_r[i]; nc += s_c[i];
        }
    }
    return 0;
}

int is_pseudo_legal_move(const BoardState *state, GuiMove m) {
    int p = state->board[m.from];
    int target = state->board[m.to];
    int turn = state->turn;

    if (p == 0) return 0;
    if ((turn == 1 && p < 0) || (turn == -1 && p > 0)) return 0;
    if (m.from == m.to) return 0;
    if (target != 0 && ((turn == 1 && target > 0) || (turn == -1 && target < 0))) return 0;

    int fr = m.from / 8, fc = m.from % 8;
    int tr = m.to / 8, tc = m.to % 8;
    int dr = tr - fr, dc = tc - fc;
    int abs_dr = abs(dr), abs_dc = abs(dc);

    switch (abs(p)) {
        case 1: { 
            int dir = (turn == 1) ? -1 : 1;
            if (dc == 0 && dr == dir && target == 0) return 1;
            int start_r = (turn == 1) ? 6 : 1;
            if (dc == 0 && fr == start_r && dr == 2 * dir) {
                if (state->board[(fr + dir) * 8 + fc] == 0 && target == 0) return 1;
            }
            if (abs_dc == 1 && dr == dir) {
                if (target != 0) return 1;
                if (m.to == state->ep) return 1;
            }
            return 0;
        }
        case 2: 
            return ((abs_dr == 2 && abs_dc == 1) || (abs_dr == 1 && abs_dc == 2));
        case 3: { 
            if (abs_dr != abs_dc) return 0;
            int sr = (dr > 0) ? 1 : -1, sc = (dc > 0) ? 1 : -1;
            int r = fr + sr, c = fc + sc;
            while (r != tr && c != tc) {
                if (state->board[r * 8 + c] != 0) return 0;
                r += sr; c += sc;
            }
            return 1;
        }
        case 4: { 
            if (dr != 0 && dc != 0) return 0;
            int sr = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
            int sc = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);
            int r = fr + sr, c = fc + sc;
            while (r != tr || c != tc) {
                if (state->board[r * 8 + c] != 0) return 0;
                r += sr; c += sc;
            }
            return 1;
        }
        case 5: { 
            if (abs_dr != abs_dc && dr != 0 && dc != 0) return 0;
            int sr = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
            int sc = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);
            if (abs_dr == abs_dc) {
                sr = (dr > 0) ? 1 : -1;
                sc = (dc > 0) ? 1 : -1;
            }
            int r = fr + sr, c = fc + sc;
            while (r != tr || c != tc) {
                if (state->board[r * 8 + c] != 0) return 0;
                r += sr; c += sc;
            }
            return 1;
        }
        case 6: { 
            if (abs_dr <= 1 && abs_dc <= 1) return 1;
            if (dr == 0 && abs_dc == 2) {
                if (turn == 1 && fr == 7 && fc == 4) {
                    if (m.to == 62 && (state->castle & 1)) {
                        if (state->board[61] == 0 && state->board[62] == 0) {
                            if (!is_square_attacked(state, 60, -1) &&
                                !is_square_attacked(state, 61, -1) &&
                                !is_square_attacked(state, 62, -1)) return 1;
                        }
                    }
                    if (m.to == 58 && (state->castle & 2)) {
                        if (state->board[59] == 0 && state->board[58] == 0 && state->board[57] == 0) {
                            if (!is_square_attacked(state, 60, -1) &&
                                !is_square_attacked(state, 59, -1) &&
                                !is_square_attacked(state, 58, -1)) return 1;
                        }
                    }
                }
                if (turn == -1 && fr == 0 && fc == 4) {
                    if (m.to == 6 && (state->castle & 4)) {
                        if (state->board[5] == 0 && state->board[6] == 0) {
                            if (!is_square_attacked(state, 4, 1) &&
                                !is_square_attacked(state, 5, 1) &&
                                !is_square_attacked(state, 6, 1)) return 1;
                        }
                    }
                    if (m.to == 2 && (state->castle & 8)) {
                        if (state->board[3] == 0 && state->board[2] == 0 && state->board[1] == 0) {
                            if (!is_square_attacked(state, 4, 1) &&
                                !is_square_attacked(state, 3, 1) &&
                                !is_square_attacked(state, 2, 1)) return 1;
                        }
                    }
                }
            }
            return 0;
        }
    }
    return 0;
}

int is_legal_move(const BoardState *state, GuiMove m) {
    if (!is_pseudo_legal_move(state, m)) return 0;
    BoardState next;
    make_move(state, &next, m);
    int king = find_king(&next, state->turn);
    if (king == -1) return 0;
    return !is_square_attacked(&next, king, -state->turn);
}

int has_legal_moves(const BoardState *state) {
    for (int f = 0; f < 64; f++) {
        if (state->board[f] == 0) continue;
        if ((state->turn == 1 && state->board[f] < 0) || (state->turn == -1 && state->board[f] > 0)) continue;
        for (int t = 0; t < 64; t++) {
            GuiMove m = {f, t, 0};
            if (abs(state->board[f]) == 1 && (t / 8 == 0 || t / 8 == 7)) {
                m.promo = 5; 
            }
            if (is_legal_move(state, m)) return 1;
        }
    }
    return 0;
}

int count_repetitions(const BoardState *state) {
    int count = 1; 
    for (int i = 0; i < history_count; i++) {
        if (state->turn == history[i].turn &&
            state->castle == history[i].castle &&
            state->ep == history[i].ep &&
            memcmp(state->board, history[i].board, sizeof(state->board)) == 0) {
            count++;
        }
    }
    return count;
}

void make_move(const BoardState *src, BoardState *dst, GuiMove m) {
    *dst = *src;
    int p = dst->board[m.from];
    
    int is_capture = (src->board[m.to] != 0) || (abs(p) == 1 && m.to == src->ep);

    dst->board[m.from] = 0;

    if (m.promo != 0) {
        dst->board[m.to] = dst->turn * m.promo;
    } else {
        dst->board[m.to] = p;
    }

    if (abs(p) == 1 && m.to == dst->ep) {
        int p_dir = (dst->turn == 1 ? 8 : -8);
        int mt_cap = m.to + p_dir;
        dst->board[mt_cap] = 0;
    }

    dst->ep = -1;
    if (abs(p) == 1 && abs(m.from - m.to) == 16) {
        dst->ep = m.from + (dst->turn == 1 ? -8 : 8);
    }

    if (abs(p) == 6) {
        if (m.from == 60 && m.to == 62) { dst->board[61] = dst->board[63]; dst->board[63] = 0; }
        else if (m.from == 60 && m.to == 58) { dst->board[59] = dst->board[56]; dst->board[56] = 0; }
        else if (m.from == 4 && m.to == 6) { dst->board[5] = dst->board[7]; dst->board[7] = 0; }
        else if (m.from == 4 && m.to == 2) { dst->board[3] = dst->board[0]; dst->board[0] = 0; }
    }

    if (m.from == 60) dst->castle &= ~1 & ~2;
    if (m.from == 4)  dst->castle &= ~4 & ~8;
    if (m.from == 63 || m.to == 63) dst->castle &= ~1;
    if (m.from == 56 || m.to == 56) dst->castle &= ~2;
    if (m.from == 7  || m.to == 7)  dst->castle &= ~4;
    if (m.from == 0  || m.to == 0)  dst->castle &= ~8;

    dst->turn = -dst->turn;
    
    if (abs(p) == 1 || is_capture) dst->halfmoves = 0;
    else dst->halfmoves++;
    
    if (dst->turn == 1) dst->fullmoves++;
}


// --- 3DS RENDERING LAYOUT ---
void draw_ui() {
    gui_printf("\033[H\r\n"); 

    const char *turn_str = (current_state.turn == 1) ? "\033[1;33mWhite\033[0m" : "\033[1;35mBlack\033[0m";
    int king = find_king(&current_state, current_state.turn);
    int is_ch = is_square_attacked(&current_state, king, -current_state.turn);
    int has_mov = has_legal_moves(&current_state);
    const char *w_play = (user_side == 1 || user_side == 0) ? "Hum" : "Eng";
    const char *b_play = (user_side == -1 || user_side == 0) ? "Hum" : "Eng";
    int repetitions = count_repetitions(&current_state);

    gui_printf("  ");
    if (current_state.halfmoves >= 100) {
        gui_printf("\033[1;36mDRAW (50-move rule)\033[0m");
    } else if (repetitions >= 3) {
        gui_printf("\033[1;36mDRAW (threefold repetition)\033[0m");
    } else if (!has_mov) {
        if (is_ch) gui_printf("\033[1;31mCHECKMATE!\033[0m");
        else gui_printf("\033[1;36mSTALEMATE!\033[0m");
    } else if (is_ch) {
        gui_printf("%s (\033[1;31mCHECK!\033[0m)", turn_str);
    } else {
        gui_printf("%s's Turn", turn_str);
    }
    gui_printf(" | W:%s B:%s", w_play, b_play);

    const char *types[] = {"Time-Limit", "Depth-Limit", "Node-Limit"};
    gui_printf(" | %s", types[time_control_type]);
    if (time_control_type == 0) {
        gui_printf(" (%d ms)", time_control_val);
    } else if (time_control_type == 1) {
        gui_printf(" (depth %d)", time_control_val);
    } else {
        gui_printf(" (%d nodes)", time_control_val);
    }
    gui_printf("\033[K\r\n\r\n");

    if (board_orientation == 1) {
        gui_printf("     a  b  c  d  e  f  g  h    ");
    } else {
        gui_printf("     h  g  f  e  d  c  b  a    ");
    }
    print_side_panel_line(0);
    gui_printf("\033[K\r\n");

    int king_in_check = -1;
    int w_king = find_king(&current_state, 1);
    int b_king = find_king(&current_state, -1);
    if (w_king != -1 && is_square_attacked(&current_state, w_king, -1)) {
        king_in_check = w_king;
    } else if (b_king != -1 && is_square_attacked(&current_state, b_king, 1)) {
        king_in_check = b_king;
    }

    for (int r = 0; r < 8; r++) {
        int rank_lbl = (board_orientation == 1) ? (8 - r) : (r + 1);
        gui_printf("  %d ", rank_lbl);

        for (int c = 0; c < 8; c++) {
            int sq = screen_to_board_sq(r, c);
            int p = current_state.board[sq];

            int is_light = ((sq / 8) + (sq % 8)) % 2 == 0;
            const char *bg_color;

            int is_selected = (sq == selected_sq);
            int is_cursor = (r == cursor_r && c == cursor_c);

            int is_prev_move = 0;
            if (history_count > 0) {
                GuiMove last_move = move_history[history_count - 1];
                if (sq == last_move.from || sq == last_move.to) {
                    is_prev_move = 1;
                }
            }

            int is_legal_dest = 0;
            if (selected_sq != -1) {
                GuiMove test_m = {selected_sq, sq, 0};
                if (abs(current_state.board[selected_sq]) == 1 && (sq / 8 == 0 || sq / 8 == 7)) {
                    test_m.promo = 5;
                }
                if (is_legal_move(&current_state, test_m)) {
                    is_legal_dest = 1;
                }
            }

            if (is_cursor) {
                bg_color = "\033[45m"; 
            } else if (is_selected) {
                bg_color = "\033[42m"; 
            } else if (sq == king_in_check) {
                bg_color = "\033[41m"; 
            } else if (is_prev_move) {
                bg_color = "\033[43m"; 
            } else if (is_legal_dest) {
                bg_color = "\033[46m"; 
            } else {
                bg_color = is_light ? "\033[47m" : "\033[40m"; 
            }

            const char *piece_str = " ";
            const char *fg_color = "\033[30m"; 
            if (p != 0) {
                if (p > 0) fg_color = "\033[1;37m"; 
                switch (abs(p)) {
                    case 1: piece_str = "P"; break;
                    case 2: piece_str = "N"; break;
                    case 3: piece_str = "B"; break;
                    case 4: piece_str = "R"; break;
                    case 5: piece_str = "Q"; break;
                    case 6: piece_str = "K"; break;
                }
            }

            gui_printf("%s%s %s \033[0m", bg_color, fg_color, piece_str);
        }

        gui_printf(" %d ", rank_lbl);
        print_side_panel_line(r + 1);
        gui_printf("\033[K\r\n"); 
    }

    if (board_orientation == 1) {
        gui_printf("     a  b  c  d  e  f  g  h    ");
    } else {
        gui_printf("     h  g  f  e  d  c  b  a    ");
    }
    print_side_panel_line(9);
    gui_printf("\033[K\r\n\r\n");

    gui_printf(" \033[1;30m[D-Pad] Move  | [A] Select | [B] Undo | [START] Reset\033[0m\033[K\r\n");
    gui_printf(" \033[1;30m[X] Flip Board | [Y] Switch Sides | [L] Time Control Type\033[0m\033[K\r\n");
    gui_printf(" \033[1;30m[R] Adjust Value | [SELECT] Exit to Homebrew\033[0m\033[K\r\n\r\n");
    
    gui_printf(" \033[1;37mEngine Status:\033[0m (%s)", (engine_running) ? "\033[1;32mActive\033[0m" : "\033[1;31mOffline\033[0m");
    if (engine_running) {
        if (engine_nps > 0) {
            if (engine_nps >= 1000000) {
                gui_printf(" | NPS: %.2fM", (double)engine_nps / 1000000.0);
            } else if (engine_nps >= 1000) {
                gui_printf(" | NPS: %.1fk", (double)engine_nps / 1000.0);
            } else {
                gui_printf(" | NPS: %lld", engine_nps);
            }
        }
        if (engine_score_type == 0) {
            gui_printf(" | \033[1;36mEval:\033[0m %+.2f", (double)engine_score_val / 100.0);
        } else if (engine_score_type == 1) {
            if (engine_score_val > 0) {
                gui_printf(" | \033[1;31mEval: +M%d\033[0m", engine_score_val);
            } else if (engine_score_val < 0) {
                gui_printf(" | \033[1;31mEval: -M%d\033[0m", -engine_score_val);
            } else {
                gui_printf(" | \033[1;31mEval: M0\033[0m");
            }
        }
    }
    gui_printf("\033[K\r\n\033[J");
    fflush(stdout);
}

void print_side_panel_line(int panel_row) {
    gui_printf(" ");
    int total_full_moves = (history_count + 1) / 2;
    if (total_full_moves == 0) return;
    
    int start_move = 1;
    if (total_full_moves > 10) {
        start_move = total_full_moves - 9;
    }
    int display = start_move + panel_row;
    if (display > total_full_moves) return;

    int w_idx = (display - 1) * 2;
    int b_idx = w_idx + 1;

    gui_printf("   %2d. ", display);
    if (w_idx < history_count) {
        char w_str[10];
        gui_move_to_uci(move_history[w_idx], w_str);
        gui_printf("%-6s", w_str);
    } else {
        gui_printf("------");
    }

    gui_printf(" ");

    if (b_idx < history_count) {
        char b_str[10];
        gui_move_to_uci(move_history[b_idx], b_str);
        gui_printf("%-6s", b_str);
    } else {
        if (w_idx < history_count) gui_printf("...");
        else gui_printf("------");
    }
}

int get_promo_choice() {
    gui_printf("\r\n \033[1;33mPromote pawn: [A] Queen, [B] Rook, [X] Bishop, [Y] Knight\033[0m");
    fflush(stdout);
    int choice = 5;
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_A) { choice = 5; break; }
        if (kDown & KEY_B) { choice = 4; break; }
        if (kDown & KEY_X) { choice = 3; break; }
        if (kDown & KEY_Y) { choice = 2; break; }
        gspWaitForVBlank();
    }
    gui_printf("\r\033[K\033[A\033[K");
    fflush(stdout);
    return choice;
}

void handle_select() {
    if (!has_legal_moves(&current_state) || current_state.halfmoves >= 100 || count_repetitions(&current_state) >= 3) {
        return;
    }

    int sq = screen_to_board_sq(cursor_r, cursor_c);
    if (selected_sq == -1) {
        int p = current_state.board[sq];
        if (p != 0 && ((current_state.turn == 1 && p > 0) || (current_state.turn == -1 && p < 0))) {
            selected_sq = sq;
        }
    } else {
        GuiMove m = {selected_sq, sq, 0};
        int p = current_state.board[selected_sq];
        int is_promo = (abs(p) == 1 && (sq / 8 == 0 || sq / 8 == 7));
        if (is_promo) m.promo = 5; 

        if (is_legal_move(&current_state, m)) {
            if (is_promo) {
                m.promo = get_promo_choice();
            }
            push_state(&current_state, m);
            BoardState next;
            make_move(&current_state, &next, m);
            current_state = next;
            selected_sq = -1;
            
            engine_nps = 0;
            engine_score_type = -1;
            engine_score_val = 0;
        } else {
            int target = current_state.board[sq];
            if (target != 0 && ((current_state.turn == 1 && target > 0) || (current_state.turn == -1 && target < 0))) {
                selected_sq = sq; 
            } else {
                selected_sq = -1; 
            }
        }
    }
}

void handle_undo() {
    if (engine_thinking) {
        send_to_engine("stop\n");
        engine_thinking = 0;
    }
    engine_nps = 0;
    engine_score_type = -1;
    engine_score_val = 0;
    int step_back = (user_side == 1 || user_side == -1) ? 2 : 1;
    while (step_back > 0 && history_count > 0) {
        history_count--;
        current_state = history[history_count];
        step_back--;
    }
    selected_sq = -1;
}

void handle_reset_board() {
    if (engine_thinking) {
        send_to_engine("stop\n");
        engine_thinking = 0;
    }
    engine_nps = 0;
    engine_score_type = -1;
    engine_score_val = 0;
    init_board(&current_state);
    history_count = 0;
    selected_sq = -1;
    cursor_r = 6;
    cursor_c = 4;
    if (engine_running) {
        send_to_engine("ucinewgame\nisready\n");
    }
}

void handle_switch_sides() {
    if (engine_thinking) {
        send_to_engine("stop\n");
        engine_thinking = 0;
    }
    if (user_side == 1) user_side = -1;
    else if (user_side == -1) user_side = 0;
    else if (user_side == 0) user_side = 2;
    else user_side = 1;
}

void adjust_time_control() {
    if (time_control_type == 0) { 
        int time_list[] = {100, 250, 500, 1000, 1500, 2000, 3000, 5000};
        int time_count = sizeof(time_list) / sizeof(time_list[0]);
        int found_idx = -1;
        for (int i = 0; i < time_count; i++) {
            if (time_control_val == time_list[i]) {
                found_idx = i;
                break;
            }
        }
        if (found_idx == -1 || found_idx == time_count - 1) {
            time_control_val = time_list[0];
        } else {
            time_control_val = time_list[found_idx + 1];
        }
    } else if (time_control_type == 1) { 
        time_control_val = (time_control_val % 10) + 1; 
    } else { 
        int nodes_list[] = {256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
        int nodes_count = sizeof(nodes_list) / sizeof(nodes_list[0]);
        int found_idx = -1;
        for (int i = 0; i < nodes_count; i++) {
            if (time_control_val == nodes_list[i]) {
                found_idx = i;
                break;
            }
        }
        if (found_idx == -1 || found_idx == nodes_count - 1) {
            time_control_val = nodes_list[0];
        } else {
            time_control_val = nodes_list[found_idx + 1];
        }
    }
}

void handle_3ds_input() {
    hidScanInput();
    u32 kDown = hidKeysDown();
    u32 kHeld = hidKeysHeld();

    static int hold_timer = 0;
    u32 input_active = kDown;

    if (kHeld & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT)) {
        hold_timer++;
        if (hold_timer > 10) {
            input_active |= (kHeld & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT));
            hold_timer = 7;
        }
    } else {
        hold_timer = 0;
    }

    if (input_active & (KEY_DUP | KEY_CPAD_UP)) {
        if (cursor_r > 0) cursor_r--;
    }
    if (input_active & (KEY_DDOWN | KEY_CPAD_DOWN)) {
        if (cursor_r < 7) cursor_r++;
    }
    if (input_active & (KEY_DLEFT | KEY_CPAD_LEFT)) {
        if (cursor_c > 0) cursor_c--;
    }
    if (input_active & (KEY_DRIGHT | KEY_CPAD_RIGHT)) {
        if (cursor_c < 7) cursor_c++;
    }

    if (kDown & KEY_A)      handle_select();
    if (kDown & KEY_B)      handle_undo();
    if (kDown & KEY_START)  handle_reset_board();
    if (kDown & KEY_X)      board_orientation = -board_orientation;
    if (kDown & KEY_Y)      handle_switch_sides();
    if (kDown & KEY_L) {
        time_control_type = (time_control_type + 1) % 3;
        time_control_val = (time_control_type == 0) ? 500 : (time_control_type == 1 ? 1 : 256);
    }
    if (kDown & KEY_R)      adjust_time_control();
    
    if (kDown & KEY_SELECT) { 
        if (engine_running) {
            send_to_engine("quit\n");
            pthread_join(engine_thread, NULL);
        }
        exit(0);
    }
}

void init_board(BoardState *state) {
    int start[64] = {
        -4, -2, -3, -5, -6, -3, -2, -4,
        -1, -1, -1, -1, -1, -1, -1, -1,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         1,  1,  1,  1,  1,  1,  1,  1,
         4,  2,  3,  5,  6,  3,  2,  4
    };
    memcpy(state->board, start, sizeof(start));
    state->turn = 1;
    state->castle = 15;
    state->ep = -1;
    state->halfmoves = 0;
    state->fullmoves = 1;
}

int main() {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL); 

    init_board(&current_state);
    start_engine();

    while (aptMainLoop()) {
        int engine_active = 0;
        if (has_legal_moves(&current_state) && current_state.halfmoves < 100 && count_repetitions(&current_state) < 3) {
            if (user_side == 2) engine_active = 1;
            else if (user_side == 1 && current_state.turn == -1) engine_active = 1;
            else if (user_side == -1 && current_state.turn == 1) engine_active = 1;
        }

        if (engine_active && !engine_thinking && engine_running) {
            engine_thinking = 1;
            trigger_engine_move();
        }

        read_from_engine();
        draw_ui();
        handle_3ds_input();

        gfxFlushBuffers();
        gfxSwapBuffers();

        gspWaitForVBlank(); 
    }

    gfxExit();
    return 0;
}
