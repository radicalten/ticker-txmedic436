/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty ofg
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <malloc.h>

// Nintendo Wii DevkitPro Hardware Headers
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <fat.h> // Added for SD/USB filesystem initialization

#include "bitboard.h"
#include "endgame.h"
#include "pawns.h"
#include "polybook.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "tbprobe.h"

// Restore standard printing and file operations inside the GUI's main thread
#undef printf
#undef fgets
#undef getline

#define MAX_HISTORY 2048

// Board representation
typedef struct {
    int board[64]; // P=1, N=2, B=3, R=4, Q=5, K=6 (Positive=White, Negative=Black)
    int turn;      // 1 = White, -1 = Black
    int castle;    // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep;        // En-passant square (0-63), -1 if none
    int halfmoves; // For 50-move rule
    int fullmoves;
} BoardState;

typedef struct {
    int from;
    int to;
    int promo; // 0=None, 2=N, 3=B, 4=R, 5=Q
} GuiMove;

// Global GUI state
BoardState current_state;
BoardState history[MAX_HISTORY];
GuiMove move_history[MAX_HISTORY];
int history_count = 0;

int cursor_r = 6;  
int cursor_c = 4;  
int selected_sq = -1;

int board_orientation = 1; 
int user_side = 1;         

int time_control_type = 0;   
int time_control_val = 1;    

int engine_thinking = 0;
bool gui_mode_active = true; // FIXED: gui_mode_active starts true so engine_printf routes to the FIFO from the very first call, avoiding a race condition

// Handshake Tracking
bool engine_recvd_uciok = false;
bool engine_recvd_readyok = false;

// Engine live performance metrics
long long engine_nps = 0;      
int engine_depth = 0;          
int engine_seldepth = 0;       
int engine_time_ms = 0;        
long long engine_nodes = 0;    
int engine_hashfull = 0;       
long long engine_tbhits = 0;   

int engine_score_type = -1;  
int engine_score_val = 0;    

char engine_pv[1024] = "";   

// Thread-safe In-Memory FIFOs to emulate standard OS pipes
#define FIFO_SIZE 16384
typedef struct {
    char data[FIFO_SIZE];
    int head;
    int tail;
    LOCK_T lock;
} SimpleFIFO;

SimpleFIFO gui_to_eng_pipe;
SimpleFIFO eng_to_gui_pipe;

KThread engine_thread;
void* engine_thread_stack = NULL;
static volatile bool engine_running = false; // FIXED: engine_running flag lets engine_fgets/engine_getline exit cleanly when the engine is told to stop, preventing infinite loops.

void fifo_init(SimpleFIFO *f) {
    memset(f, 0, sizeof(SimpleFIFO));
    LOCK_INIT(f->lock);
}

void fifo_write(SimpleFIFO *f, const char *str, int len) {
    if (!f || !str || len <= 0) return;
    LOCK(f->lock);
    for (int i = 0; i < len; i++) {
        int next = (f->head + 1) % FIFO_SIZE;
        if (next != f->tail) {
            f->data[f->head] = str[i];
            f->head = next;
        }
    }
    UNLOCK(f->lock);
}

int fifo_read(SimpleFIFO *f, char *out, int max_len) {
   if (!f || !out || max_len <= 0) return 0;
    LOCK(f->lock);
    int bytes_read = 0;
    while (f->tail != f->head && bytes_read < max_len) {
        out[bytes_read++] = f->data[f->tail];
        f->tail = (f->tail + 1) % FIFO_SIZE;
    }
    UNLOCK(f->lock);
    return bytes_read;
}

// Redirected standard output writer for Cfish UCI thread
int engine_printf(const char *format, ...) {
    char buf[2048];
    va_list args;
    va_start(args, format);
    int ret = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    if (gui_mode_active) {
        fifo_write(&eng_to_gui_pipe, buf, ret);
    } else {
        fwrite(buf, 1, ret, stdout);
        fflush(stdout);
    }
    return ret;
}

// Redirected standard input reader for Cfish UCI thread (fgets)
char* engine_fgets(char* str, int num, FILE* stream) {
    (void)stream;
    if (!str || num <= 1) return NULL;
    int bytes_read = 0;
    while (bytes_read < num - 1) {
            if (!engine_running) {
            if (bytes_read == 0) return NULL;
            break;
          }
        char c;
        if (fifo_read(&gui_to_eng_pipe, &c, 1) > 0) {
            str[bytes_read++] = c;
            if (c == '\n') break;
        } else {
            KThreadSleepMs(1); // FIXED: Prevents 100% CPU starvation spinning
        }
    }
    if (bytes_read == 0) return NULL;
    str[bytes_read] = '\0';
    return str;
}

// Redirected standard input reader for Cfish UCI thread (getline)
ssize_t engine_getline(char **lineptr, size_t *n, FILE *stream) {
    (void)stream;
    if (!lineptr || !n) return -1;
    if (*lineptr == NULL || *n == 0) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }

    int i = 0;
    while (1) {
      if (!engine_running) {
            if (i == 0) return -1;
            break;
        }
        char c;
        if (fifo_read(&gui_to_eng_pipe, &c, 1) > 0) {
            if (i >= (int)*n - 1) {
                *n *= 2;
                char *new_ptr = realloc(*lineptr, *n);
                if (new_ptr == NULL) return -1;
                *lineptr = new_ptr;
            }
            (*lineptr)[i++] = c;
            if (c == '\n') break;
        } else {
            KThreadSleepMs(1); // FIXED: Prevents 100% CPU starvation spinning
        }
    }
    (*lineptr)[i] = '\0';
    return i;
}

void init_board(BoardState *state);
int is_legal_gui_move(const BoardState *state, GuiMove m);
int has_legal_moves(const BoardState *state);
int is_square_attacked(const BoardState *state, int sq, int attacker);
void make_gui_move(const BoardState *src, BoardState *dst, GuiMove m);
void print_side_panel_line(int panel_row);
void print_recent_moves(int row);
void send_to_engine(const char *cmd);
int find_king(const BoardState *state, int color);
int count_repetitions(const BoardState *state);
int get_promo_choice();
void move_to_pgn(const BoardState *state, GuiMove m, char *buf);
void reset_engine_metrics();
void adjust_time_control();

void gui_cleanup() {
    printf("\033[?25h\033[2J\033[H"); 
    fflush(stdout);
}

void init_wii_console() {
    VIDEO_Init();
    WPAD_Init();
    fatInitDefault(); // FIXED: Initialise SD/USB FAT storage for NNUE & book files
    
    GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
    void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
}

static sptr engine_thread_main(void *arg) {
    (void)arg;
    
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

    char *engine_argv[] = {"ucichess", NULL};
    uci_loop(1, engine_argv);

    return 0;
}

void start_engine() {
    engine_recvd_uciok = false;
    engine_recvd_readyok = false;

    fifo_init(&gui_to_eng_pipe);
    fifo_init(&eng_to_gui_pipe);

    // Setup redirect hooks
    engine_fgets_hook = engine_fgets;
    engine_printf_hook = engine_printf;
    engine_getline_hook = engine_getline;

    engine_thread_stack = memalign(32, 512 * 1024);
    
    KThreadPrepare(&engine_thread, engine_thread_main, NULL, engine_thread_stack, 0x3f);
    KThreadResume(&engine_thread);

    send_to_engine("uci\nisready\n");
}

void send_to_engine(const char *cmd) {
    fifo_write(&gui_to_eng_pipe, cmd, strlen(cmd));
}

int screen_to_board_sq(int r, int c) {
    if (board_orientation == 1) {
        return r * 8 + c;
    } else {
        return (7 - r) * 8 + (7 - c);
    }
}

void move_to_uci(GuiMove m, char *buf) {
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

void move_to_pgn(const BoardState *state, GuiMove m, char *buf) {
    int p = state->board[m.from];
    int abs_p = abs(p);
    int target = state->board[m.to];
    int is_cap = (target != 0) || (abs_p == 1 && m.to == state->ep);

    if (abs_p == 6 && abs(m.from - m.to) == 2) {
        if (m.to > m.from) {
            strcpy(buf, "O-O");
        } else {
            strcpy(buf, "O-O-O");
        }
    } else {
        char *ptr = buf;
        if (abs_p == 1) {
            if (is_cap) {
                *ptr++ = 'a' + (m.from % 8);
                *ptr++ = 'x';
            }
        } else {
            if (abs_p == 2) *ptr++ = 'N';
            else if (abs_p == 3) *ptr++ = 'B';
            else if (abs_p == 4) *ptr++ = 'R';
            else if (abs_p == 5) *ptr++ = 'Q';
            else if (abs_p == 6) *ptr++ = 'K';

            if (abs_p >= 2 && abs_p <= 5) {
                int file_conflict = 0;
                int rank_conflict = 0;
                int conflict_exists = 0;
                for (int sq = 0; sq < 64; sq++) {
                    if (sq == m.from) continue;
                    if (state->board[sq] == p) { 
                        GuiMove test_m = {sq, m.to, 0};
                        if (is_legal_gui_move(state, test_m)) {
                            conflict_exists = 1;
                            if (sq % 8 == m.from % 8) file_conflict = 1;
                            if (sq / 8 == m.from / 8) rank_conflict = 1;
                        }
                    }
                }
                if (conflict_exists) {
                    if (!file_conflict) {
                        *ptr++ = 'a' + (m.from % 8);
                    } else if (!rank_conflict) {
                        *ptr++ = '8' - (m.from / 8);
                    } else {
                        *ptr++ = 'a' + (m.from % 8);
                        *ptr++ = '8' - (m.from / 8);
                    }
                }
            }

            if (is_cap) {
                *ptr++ = 'x';
            }
        }

        *ptr++ = 'a' + (m.to % 8);
        *ptr++ = '8' - (m.to / 8);

        if (abs_p == 1 && m.promo != 0) {
            *ptr++ = '=';
            if (m.promo == 5) *ptr++ = 'Q';
            else if (m.promo == 4) *ptr++ = 'R';
            else if (m.promo == 3) *ptr++ = 'B';
            else if (m.promo == 2) *ptr++ = 'N';
        }
        *ptr = '\0';
    }

    BoardState next;
    make_gui_move(state, &next, m);
    int opp_king = find_king(&next, next.turn);
    if (opp_king != -1 && is_square_attacked(&next, opp_king, -next.turn)) {
        if (!has_legal_moves(&next)) {
            strcat(buf, "#");
        } else {
            strcat(buf, "+");
        }
    }
}

GuiMove uci_to_gui_move(const char *str) {
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

void reset_engine_metrics() {
    engine_nps = 0;
    engine_score_type = -1;
    engine_score_val = 0;
    engine_pv[0] = '\0';
    engine_depth = 0;
    engine_seldepth = 0;
    engine_time_ms = 0;
    engine_nodes = 0;
    engine_hashfull = 0;
    engine_tbhits = 0;
}

void trigger_engine_move() {
    reset_engine_metrics();

    static char cmd[16384]; 
    cmd[0] = '\0';
    strcpy(cmd, "position startpos moves");
    
    int len = strlen(cmd);
    for (int i = 0; i < history_count; i++) {
        char uci_m[10];
        move_to_uci(move_history[i], uci_m);
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

    if (strcmp(line, "uciok") == 0) {
        engine_recvd_uciok = true;
    }
    if (strcmp(line, "readyok") == 0) {
        engine_recvd_readyok = true;
    }

    if (strncmp(line, "info", 4) == 0) {
        char *ptr;

        if ((ptr = strstr(line, " nps "))) {
            long long val;
            if (sscanf(ptr, " nps %lld", &val) == 1) engine_nps = val;
        }

        if ((ptr = strstr(line, " score "))) {
            int score_val = 0;
            char score_type[16];
            if (sscanf(ptr, " score %15s %d", score_type, &score_val) == 2) {
                if (strcmp(score_type, "cp") == 0) {
                    engine_score_type = 0;
                    engine_score_val = score_val * current_state.turn;
                } else if (strcmp(score_type, "mate") == 0) {
                    engine_score_type = 1;
                    engine_score_val = score_val * current_state.turn;
                }
            }
        }

        if ((ptr = strstr(line, " depth "))) {
            int d = 0;
            if (sscanf(ptr, " depth %d", &d) == 1) engine_depth = d;
        }

        if ((ptr = strstr(line, " seldepth "))) {
            int sd = 0;
            if (sscanf(ptr, " seldepth %d", &sd) == 1) engine_seldepth = sd;
        }

        if ((ptr = strstr(line, " time "))) {
            int t = 0;
            if (sscanf(ptr, " time %d", &t) == 1) engine_time_ms = t;
        }

        if ((ptr = strstr(line, " nodes "))) {
            long long n = 0;
            if (sscanf(ptr, " nodes %lld", &n) == 1) engine_nodes = n;
        }

        if ((ptr = strstr(line, " hashfull "))) {
            int h = 0;
            if (sscanf(ptr, " hashfull %d", &h) == 1) engine_hashfull = h;
        }

        if ((ptr = strstr(line, " tbhits "))) {
            long long tb = 0;
            if (sscanf(ptr, " tbhits %lld", &tb) == 1) engine_tbhits = tb;
        }

        if ((ptr = strstr(line, " pv "))) {
            strncpy(engine_pv, ptr + 4, sizeof(engine_pv) - 1);
            engine_pv[sizeof(engine_pv) - 1] = '\0';
        }
    }

    if (strncmp(line, "bestmove", 8) == 0) {
        char move_str[16];
        if (sscanf(line, "bestmove %15s", move_str) == 1) {
            if (strcmp(move_str, "(none)") == 0 || strcmp(move_str, "NULL") == 0) {
                engine_thinking = 0;
                return;
            }
            GuiMove m = uci_to_gui_move(move_str);
            if (is_legal_gui_move(&current_state, m)) {
                push_state(&current_state, m);
                BoardState next;
                make_gui_move(&current_state, &next, m);
                current_state = next;
            }
            engine_thinking = 0;
        }
    }
}

char engine_buffer[8192];
int engine_buf_len = 0;

void read_from_engine() {
    char tmp[2048];
    int n;
    while ((n = fifo_read(&eng_to_gui_pipe, tmp, sizeof(tmp) - 1)) > 0) {
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

int is_legal_gui_move(const BoardState *state, GuiMove m) {
    if (!is_pseudo_legal_move(state, m)) return 0;
    BoardState next;
    make_gui_move(state, &next, m);
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
            if (is_legal_gui_move(state, m)) return 1;
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

void make_gui_move(const BoardState *src, BoardState *dst, GuiMove m) {
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

void draw_ui() {
    printf("\033[H\r\n"); 

    const char *turn_str = (current_state.turn == 1) ? "\033[1;33mWhite\033[0m" : "\033[1;35mBlack\033[0m";
    int king = find_king(&current_state, current_state.turn);
    int is_ch = is_square_attacked(&current_state, king, -current_state.turn);
    int has_mov = has_legal_moves(&current_state);
    const char *w_play = (user_side == 1 || user_side == 0) ? "Hum" : "Eng";
    const char *b_play = (user_side == -1 || user_side == 0) ? "Hum" : "Eng";
    int repetitions = count_repetitions(&current_state);

    printf("  ");
    if (current_state.halfmoves >= 100) {
        printf("\033[1;36mDRAW (50-move rule)\033[0m");
    } else if (repetitions >= 3) {
        printf("\033[1;36mDRAW (threefold repetition)\033[0m");
    } else if (!has_mov) {
        if (is_ch) printf("\033[1;31mCHECKMATE!\033[0m");
        else printf("\033[1;36mSTALEMATE!\033[0m");
    } else if (is_ch) {
        printf("%s (\033[1;31mCHECK!\033[0m)", turn_str);
    } else {
        printf("%s's Turn", turn_str);
    }
    printf(" | W:%s B:%s", w_play, b_play);

    const char *types[] = {"Time-Limit", "Depth-Limit", "Node-Limit"};
    printf(" | %s", types[time_control_type]);
    if (time_control_type == 0) {
        printf(" (%d ms)", time_control_val);
    } else if (time_control_type == 1) {
        printf(" (depth %d)", time_control_val);
    } else {
        printf(" (%d nodes)", time_control_val);
    }
    printf("\033[K\r\n\r\n");

    if (board_orientation == 1) {
        printf("     a  b  c  d  e  f  g  h    ");
    } else {
        printf("     h  g  f  e  d  c  b  a    ");
    }
    print_side_panel_line(0);
    printf("\033[K\r\n");

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
        printf("  %d ", rank_lbl);

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
                if (is_legal_gui_move(&current_state, test_m)) {
                    is_legal_dest = 1;
                }
            }

            if (is_cursor) {
                bg_color = "\033[48;5;208m"; 
            } else if (is_selected) {
                bg_color = "\033[48;5;34m";  
            } else if (sq == king_in_check) {
                bg_color = "\033[48;5;196m"; 
            } else if (is_prev_move) {
                bg_color = is_light ? "\033[48;5;75m" : "\033[48;5;68m"; 
            } else if (is_legal_dest) {
                bg_color = is_light ? "\033[48;5;151m" : "\033[48;5;108m"; 
            } else {
                bg_color = is_light ? "\033[48;5;180m" : "\033[48;5;94m"; 
            }

            const char *piece_str = " ";
            const char *fg_color = "\033[38;5;232m"; 
            if (p != 0) {
                if (p > 0) fg_color = "\033[38;5;255m\033[1m"; 
                switch (abs(p)) {
                    case 1: piece_str = "P"; break; 
                    case 2: piece_str = "N"; break;
                    case 3: piece_str = "B"; break;
                    case 4: piece_str = "R"; break;
                    case 5: piece_str = "Q"; break;
                    case 6: piece_str = "K"; break;
                }
            }

            printf("%s%s %s \033[0m", bg_color, fg_color, piece_str);
        }

        printf(" %d ", rank_lbl);
        print_side_panel_line(r + 1);
        printf("\033[K\r\n"); 
    }

    if (board_orientation == 1) {
        printf("     a  b  c  d  e  f  g  h    ");
    } else {
        printf("     h  g  f  e  d  c  b  a    ");
    }
    print_side_panel_line(9);
    printf("\033[K\r\n\r\n");

    // UPDATED: Controls description to map '2' to perspective toggling
    printf(" \033[38;5;245m[D-PAD] Move | [A] Select | [B] Undo | [1] Cycle Level | [2] Flip | [+] Sides | [-] Cycle Type | [HOME] Quit\033[0m\033[K\r\n");
    
    // ==========================================
    // STREAMLINED ENGINE HUD (PV, NPS, EVAL)
    // ==========================================
    printf(" \033[1;34mEngine State:\033[0m ");
    if (engine_thinking) {
        printf("\033[1;32mThinking\033[0m");
    } else if (engine_recvd_readyok) {
        printf("\033[1;30mIdle (Ready)\033[0m");
    } else {
        printf("\033[1;31mConnecting...\033[0m");
    }

    // Nodes Per Second (NPS) Display
    if (engine_nps > 0) {
        if (engine_nps >= 1000000)      printf(" | \033[38;5;250mSpeed:\033[0m %.2fM nps", (double)engine_nps / 1000000.0);
        else if (engine_nps >= 1000)    printf(" | \033[38;5;250mSpeed:\033[0m %.1fk nps", (double)engine_nps / 1000.0);
        else                            printf(" | \033[38;5;250mSpeed:\033[0m %lld nps", engine_nps);
    } else {
        printf(" | \033[38;5;250mSpeed:\033[0m 0 nps");
    }

    // Evaluation Rating Display
    if (engine_score_type == 0) {
        if (engine_score_val > 0) {
            printf(" | \033[38;5;250mEval:\033[0m \033[1;32m%+.2f\033[0m", (double)engine_score_val / 100.0);
        } else if (engine_score_val < 0) {
            printf(" | \033[38;5;250mEval:\033[0m \033[1;31m%+.2f\033[0m", (double)engine_score_val / 100.0);
        } else {
            printf(" | \033[38;5;250mEval:\033[0m 0.00");
        }
    } else if (engine_score_type == 1) {
        if (engine_score_val > 0) {
            printf(" | \033[38;5;250mEval:\033[0m \033[1;32m+M%d\033[0m", engine_score_val);
        } else if (engine_score_val < 0) {
            printf(" | \033[38;5;250mEval:\033[0m \033[1;31m-M%d\033[0m", -engine_score_val);
        } else {
            printf(" | \033[38;5;250mEval:\033[0m M0");
        }
    } else {
        printf(" | \033[38;5;250mEval:\033[0m -");
    }
    printf("\033[K\r\n");

    // Principal Variation (PV) Display
    if (strlen(engine_pv) > 0) {
        printf(" \033[1;33mBest Line:\033[0m \033[38;5;250m%s\033[0m\033[K\r\n", engine_pv);
    } else {
        printf(" \033[1;33mBest Line:\033[0m \033[38;5;243m-\033[0m\033[K\r\n");
    }
    
    printf("\033[J"); 
    fflush(stdout);
}

void print_side_panel_line(int panel_row) {
    printf(" ");
    print_recent_moves(panel_row);
}

void print_recent_moves(int row) {
    int total_full_moves = (history_count + 1) / 2;
    if (total_full_moves == 0) {
        return; 
    }
    int start_move = 1;
    if (total_full_moves > 10) {
        start_move = total_full_moves - 9;
    }
    int display = start_move + row;
    if (display > total_full_moves) {
        return; 
    }

    int w_idx = (display - 1) * 2;
    int b_idx = w_idx + 1;

    printf("   %2d. ", display);
    if (w_idx < history_count) {
        char pgn_str[16];
        move_to_pgn(&history[w_idx], move_history[w_idx], pgn_str);
        printf("%-6s", pgn_str);
    } else {
        printf("------");
    }

    printf(" ");

    if (b_idx < history_count) {
        char pgn_str[16];
        move_to_pgn(&history[b_idx], move_history[b_idx], pgn_str);
        printf("%-6s", pgn_str);
    } else {
        if (w_idx < history_count) printf("...");
        else printf("------");
    }
}

int get_promo_choice() {
    printf("\r\n \033[1;33mPromote pawn to: [A] Queen, [B] Rook, [+] Bishop, [-] Knight\033[0m");
    fflush(stdout);
    
    int choice = 5;
    while (1) {
        WPAD_ScanPads();
        u32 pressed = WPAD_ButtonsDown(0);
        if (pressed & WPAD_BUTTON_A) { choice = 5; break; }
        if (pressed & WPAD_BUTTON_B) { choice = 4; break; }
        if (pressed & WPAD_BUTTON_PLUS) { choice = 3; break; }
        if (pressed & WPAD_BUTTON_MINUS) { choice = 2; break; }
        VIDEO_WaitVSync();
    }
    printf("\r\033[K\033[A\033[K");
    fflush(stdout);
    return choice;
}

void handle_select() {
    if (!has_legal_moves(&current_state) || current_state.halfmoves >= 100 || count_repetitions(&current_state) >= 3) {
        return;
    }

    // Block inputs if engine is searching, in engine-vs-engine mode, or on opponent's turn.
    if (engine_thinking) {
        return;
    }
    if (user_side == 2) { 
        return;
    }
    if (user_side == 1 && current_state.turn != 1) { 
        return;
    }
    if (user_side == -1 && current_state.turn != -1) { 
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
        if (is_promo) {
            m.promo = 5; 
        }

        if (is_legal_gui_move(&current_state, m)) {
            if (is_promo) {
                m.promo = get_promo_choice();
            }
            push_state(&current_state, m);
            BoardState next;
            make_gui_move(&current_state, &next, m);
            current_state = next;
            selected_sq = -1;
            reset_engine_metrics();
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
    reset_engine_metrics();
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
    reset_engine_metrics();
    init_board(&current_state);
    history_count = 0;
    selected_sq = -1;
    cursor_r = 6;
    cursor_c = 4;
    send_to_engine("ucinewgame\nisready\n");
}

void handle_switch_sides() {
    if (engine_thinking) {
        send_to_engine("stop\n");
        engine_thinking = 0;
    }
    selected_sq = -1; 
    if (user_side == 1) user_side = -1;
    else if (user_side == -1) user_side = 0;
    else if (user_side == 0) user_side = 2;
    else user_side = 1;
}

void adjust_time_control() {
    if (time_control_type == 0) { 
        int time_list[] = {1, 10, 50, 100, 500, 1000, 1500, 2000, 3000, 5000, 10000};
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
        time_control_val = (time_control_val % 20) + 1;
    } else { 
        int nodes_list[] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576, 2097152, 4194304, 8388608};
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

void handle_wii_input() {
    WPAD_ScanPads();
    u32 pressed = WPAD_ButtonsDown(0);

    if (pressed & WPAD_BUTTON_UP) {
        if (cursor_r > 0) cursor_r--;
    }
    if (pressed & WPAD_BUTTON_DOWN) {
        if (cursor_r < 7) cursor_r++;
    }
    if (pressed & WPAD_BUTTON_LEFT) {
        if (cursor_c > 0) cursor_c--;
    }
    if (pressed & WPAD_BUTTON_RIGHT) {
        if (cursor_c < 7) cursor_c++;
    }
    if (pressed & WPAD_BUTTON_A) { // UPDATED: Excluded button 2 so it doesn't double-trigger
        handle_select();
    }
    if (pressed & WPAD_BUTTON_B) { // SWAPPED: B strictly handles Undo actions
        handle_undo();
    }
    if (pressed & WPAD_BUTTON_1) { // SWAPPED: 1 cycles levels of current time control 
        adjust_time_control();
    }
    if (pressed & WPAD_BUTTON_2) { // UPDATED: Added mapping to toggle chessboard perspective
        board_orientation = (board_orientation == 1) ? -1 : 1;
    }
    if (pressed & WPAD_BUTTON_PLUS) {
        handle_switch_sides();
    }
    if (pressed & WPAD_BUTTON_MINUS) {
        time_control_type = (time_control_type + 1) % 3;
        time_control_val = (time_control_type == 0) ? 1 : (time_control_type == 1 ? 1 : 512);
    }
    if (pressed & WPAD_BUTTON_HOME) {
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

int run_gui_mode() {
    init_board(&current_state);
    start_engine();

    while (1) {
        int engine_active = 0;
        if (has_legal_moves(&current_state) && current_state.halfmoves < 100 && count_repetitions(&current_state) < 3) {
            if (user_side == 2) engine_active = 1;
            else if (user_side == 1 && current_state.turn == -1) engine_active = 1;
            else if (user_side == -1 && current_state.turn == 1) engine_active = 1;
        }

        if (engine_active && !engine_thinking) {
            engine_thinking = 1;
            trigger_engine_move();
        }

        draw_ui();
        handle_wii_input();
        read_from_engine();
        
        KThreadSleepMs(16); 
    }
    return 0;
}

int main(int argc, char **argv)
{
    init_wii_console();
    bool force_gui = true; 
    bool force_cli = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cli") == 0 || strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "uci") == 0) {
            force_cli = true;
            force_gui = false;
        }
    }

    if (force_cli) {
        gui_mode_active = false;
        print_engine_info(false);

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

        uci_loop(argc, argv);

        threads_exit();
        TB_free();
        options_free();
        tt_free();
        pb_free();
#ifdef NNUE
        nnue_free();
#endif
        return 0;
    } else {
        gui_mode_active = true;
        return run_gui_mode();
    }
}
