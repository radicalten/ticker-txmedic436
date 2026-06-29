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
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// ============================================================
// IMPORTANT: Standard headers MUST come before types.h
// because types.h redefines printf/fgets/getline as macros.
// We include standard headers first, then undef the macros
// so this file uses real stdio functions for the GUI.
// ============================================================
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
#include <fat.h>

// Engine headers - types.h will redefine printf/fgets/getline
// for the engine's use. We undef them immediately after so
// this file (main.c / GUI code) uses real stdio.
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

// Restore real stdio for the GUI - engine files keep the macros
#undef printf
#undef fgets
#undef getline

// ============================================================
// Hook function pointer DEFINITIONS (declared extern in types.h)
// Exactly one .c file must define these - that is main.c
// ============================================================
char*   (*engine_fgets_hook)(char *str, int num, FILE *stream) = NULL;
int     (*engine_printf_hook)(const char *format, ...)         = NULL;
ssize_t (*engine_getline_hook)(char **lineptr, size_t *n,
                                FILE *stream)                   = NULL;

// ============================================================
// cfish_getline: fallback getline for Wii (no POSIX getline)
// Required by types.h macro on GEKKO/Wii target
// ============================================================
ssize_t cfish_getline(char **lineptr, size_t *n, FILE *stream)
{
    if (!lineptr || !n || !stream) return -1;
    if (*lineptr == NULL || *n == 0) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }
    int i = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if (i >= (int)*n - 1) {
            size_t new_n   = *n * 2;
            char  *new_ptr = realloc(*lineptr, new_n);
            if (!new_ptr) {
                free(*lineptr);
                *lineptr = NULL;
                *n = 0;
                return -1;
            }
            *lineptr = new_ptr;
            *n       = new_n;
        }
        (*lineptr)[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return -1;
    (*lineptr)[i] = '\0';
    return i;
}

// ============================================================
// Board / GUI types
// ============================================================
#define MAX_HISTORY 2048

typedef struct {
    int board[64]; // P=1,N=2,B=3,R=4,Q=5,K=6 (+White/-Black)
    int turn;      // 1=White, -1=Black
    int castle;    // Bitmask: 1=WK,2=WQ,4=BK,8=BQ
    int ep;        // En-passant target square (0-63), -1 if none
    int halfmoves;
    int fullmoves;
} BoardState;

typedef struct {
    int from;
    int to;
    int promo; // 0=None,2=N,3=B,4=R,5=Q
} GuiMove;

// ============================================================
// Global GUI state
// ============================================================
BoardState current_state;
BoardState history[MAX_HISTORY];
GuiMove    move_history[MAX_HISTORY];
int        history_count = 0;

int cursor_r    = 6;
int cursor_c    = 4;
int selected_sq = -1;

int board_orientation = 1;
int user_side         = 1;

int time_control_type = 0;
int time_control_val  = 1;

int  engine_thinking      = 0;

// FIXED: gui_mode_active starts true so engine_printf routes to
// the FIFO from the very first call, avoiding a race condition
// where the engine thread output would go to stdout instead.
bool gui_mode_active      = true;

bool engine_recvd_uciok   = false;
bool engine_recvd_readyok = false;

long long engine_nps        = 0;
int       engine_depth      = 0;
int       engine_seldepth   = 0;
int       engine_time_ms    = 0;
long long engine_nodes      = 0;
int       engine_hashfull   = 0;
long long engine_tbhits     = 0;
int       engine_score_type = -1;
int       engine_score_val  = 0;
char      engine_pv[1024]   = "";

// ============================================================
// Thread-safe in-memory FIFO (emulates OS pipe between threads)
// ============================================================
#define FIFO_SIZE 16384

typedef struct {
    char   data[FIFO_SIZE];
    int    head;
    int    tail;
    LOCK_T lock; // LOCK_T defined by thread.h / platform headers
} SimpleFIFO;

SimpleFIFO gui_to_eng_pipe;
SimpleFIFO eng_to_gui_pipe;

// Engine thread handle and stack
// KThread type comes from the Wii threading layer included via thread.h
KThread engine_thread;
void   *engine_thread_stack = NULL;

// FIXED: engine_running flag lets engine_fgets/engine_getline exit
// cleanly when the engine is told to stop, preventing infinite loops.
static volatile bool engine_running = false;

// ============================================================
// FIFO operations
// ============================================================

void fifo_init(SimpleFIFO *f)
{
    // FIXED: memset entire struct BEFORE LOCK_INIT.
    // LOCK_INIT writing into garbage = write to 0x00000008 (your crash).
    memset(f, 0, sizeof(SimpleFIFO));
    LOCK_INIT(f->lock);
}

void fifo_write(SimpleFIFO *f, const char *str, int len)
{
    if (!f || !str || len <= 0) return;
    LOCK(f->lock);
    for (int i = 0; i < len; i++) {
        int next = (f->head + 1) % FIFO_SIZE;
        if (next != f->tail) { // Drop silently when full
            f->data[f->head] = str[i];
            f->head          = next;
        }
    }
    UNLOCK(f->lock);
}

int fifo_read(SimpleFIFO *f, char *out, int max_len)
{
    if (!f || !out || max_len <= 0) return 0;
    LOCK(f->lock);
    int n = 0;
    while (f->tail != f->head && n < max_len) {
        out[n++]  = f->data[f->tail];
        f->tail   = (f->tail + 1) % FIFO_SIZE;
    }
    UNLOCK(f->lock);
    return n;
}

// ============================================================
// Redirect hooks: engine calls printf/fgets/getline which
// resolve to these functions via the macros in types.h
// ============================================================

// engine_printf: routes engine UCI output into the GUI FIFO
// NOTE: This function must NOT use the macro-redirected printf
// because main.c has #undef printf above.
int engine_printf(const char *format, ...)
{
    char    buf[2048];
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

// engine_fgets: blocks until a full line arrives in the GUI->engine FIFO
char *engine_fgets(char *str, int num, FILE *stream)
{
    (void)stream;
    if (!str || num <= 1) return NULL;

    int n = 0;
    while (n < num - 1) {
        // FIXED: exit cleanly when engine is shutting down
        if (!engine_running) {
            if (n == 0) return NULL;
            break;
        }
        char c;
        if (fifo_read(&gui_to_eng_pipe, &c, 1) > 0) {
            str[n++] = c;
            if (c == '\n') break;
        } else {
            KThreadSleepMs(1);
        }
    }
    if (n == 0) return NULL;
    str[n] = '\0';
    return str;
}

// engine_getline: growable-buffer line reader for GUI->engine FIFO
ssize_t engine_getline(char **lineptr, size_t *n, FILE *stream)
{
    (void)stream;
    if (!lineptr || !n) return -1;

    if (*lineptr == NULL || *n == 0) {
        *n       = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }

    int i = 0;
    while (1) {
        // FIXED: exit cleanly when engine is shutting down
        if (!engine_running) {
            if (i == 0) return -1;
            break;
        }
        char c;
        if (fifo_read(&gui_to_eng_pipe, &c, 1) > 0) {
            if (i >= (int)*n - 1) {
                size_t  new_n   = *n * 2;
                char   *new_ptr = realloc(*lineptr, new_n);
                if (!new_ptr) {
                    // FIXED: free on realloc failure to prevent leak
                    free(*lineptr);
                    *lineptr = NULL;
                    *n       = 0;
                    return -1;
                }
                *lineptr = new_ptr;
                *n       = new_n;
            }
            (*lineptr)[i++] = c;
            if (c == '\n') break;
        } else {
            KThreadSleepMs(1);
        }
    }
    (*lineptr)[i] = '\0';
    return (ssize_t)i;
}

// ============================================================
// Forward declarations
// ============================================================
void init_board(BoardState *state);
int  is_legal_gui_move(const BoardState *state, GuiMove m);
int  has_legal_moves(const BoardState *state);
int  is_square_attacked(const BoardState *state, int sq, int attacker);
void make_gui_move(const BoardState *src, BoardState *dst, GuiMove m);
void print_side_panel_line(int panel_row);
void print_recent_moves(int row);
void send_to_engine(const char *cmd);
int  find_king(const BoardState *state, int color);
int  count_repetitions(const BoardState *state);
int  get_promo_choice(void);
void move_to_pgn(const BoardState *state, GuiMove m, char *buf);
void reset_engine_metrics(void);
void adjust_time_control(void);

// ============================================================
// Wii console initialisation
// ============================================================

void gui_cleanup(void)
{
    printf("\033[?25h\033[2J\033[H");
    fflush(stdout);
}

void init_wii_console(void)
{
    VIDEO_Init();
    WPAD_Init();

    // fatInitDefault returns bool - log failure but continue
    // (engine will run without NNUE/opening book if SD fails)
    if (!fatInitDefault()) {
        // Cannot log yet - console not up. Will be visible after
        // console_init below on next printf.
    }

    GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
    void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20,
                 rmode->fbWidth, rmode->xfbHeight,
                 rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
}

// ============================================================
// Engine thread
// ============================================================

// sptr is the thread return type defined by the Wii threading layer
static sptr engine_thread_main(void *arg)
{
    (void)arg;

    // Step-by-step logging so we can see which init crashes on Wii
    printf("[engine] psqt_init\n");      fflush(stdout);
    psqt_init();
    printf("[engine] bitboards_init\n"); fflush(stdout);
    bitboards_init();
    printf("[engine] zob_init\n");       fflush(stdout);
    zob_init();
    printf("[engine] bitbases_init\n");  fflush(stdout);
    bitbases_init();
#ifndef NNUE_PURE
    printf("[engine] endgames_init\n");  fflush(stdout);
    endgames_init();
#endif
    printf("[engine] threads_init\n");   fflush(stdout);
    threads_init();
    printf("[engine] options_init\n");   fflush(stdout);
    options_init();
    printf("[engine] search_clear\n");   fflush(stdout);
    search_clear();
    printf("[engine] uci_loop\n");       fflush(stdout);

    char *engine_argv[] = {"ucichess", NULL};
    uci_loop(1, engine_argv);

    // Signal fgets/getline to exit their read loops
    engine_running = false;
    return 0;
}

void send_to_engine(const char *cmd)
{
    if (!engine_running || !cmd) return;
    fifo_write(&gui_to_eng_pipe, cmd, (int)strlen(cmd));
}

void start_engine(void)
{
    engine_recvd_uciok   = false;
    engine_recvd_readyok = false;

    // FIXED: Init FIFOs BEFORE hooks or thread - each does memset+LOCK_INIT
    fifo_init(&gui_to_eng_pipe);
    fifo_init(&eng_to_gui_pipe);

    // Install redirect hooks after FIFOs are ready
    engine_fgets_hook   = engine_fgets;
    engine_printf_hook  = engine_printf;
    engine_getline_hook = engine_getline;

    // FIXED: Set flag BEFORE thread starts so fgets doesn't
    // return NULL on the very first call
    engine_running = true;

    // FIXED: 512KB stack - Stockfish recurses to MAX_PLY depth
    // 256KB was too small and caused silent stack overflow corruption
    const size_t STACK_SIZE = 512 * 1024;
    engine_thread_stack = memalign(32, STACK_SIZE);
    if (!engine_thread_stack) {
        printf("FATAL: Cannot allocate engine stack (%u bytes)\n",
               (unsigned)STACK_SIZE);
        fflush(stdout);
        engine_running = false;
        return;
    }
    memset(engine_thread_stack, 0, STACK_SIZE);

    // FIXED: Zero KThread struct before KThreadPrepare reads it.
    // KThreadPrepare writing to a NULL KThread ptr = write to 0x8.
    memset(&engine_thread, 0, sizeof(KThread));

    // FIXED: PowerPC stack grows DOWNWARD - pass TOP of buffer
    // Passing stack_base caused first frame to write before buffer
    void *stack_top = (char *)engine_thread_stack + STACK_SIZE;

    // Priority 64: engine runs below GUI (~40) so display stays fluid
    int result = KThreadPrepare(&engine_thread,
                                engine_thread_main,
                                NULL,
                                stack_top,
                                64);
    if (result != 0) {
        printf("FATAL: KThreadPrepare failed (ret=%d)\n", result);
        fflush(stdout);
        free(engine_thread_stack);
        engine_thread_stack = NULL;
        engine_running      = false;
        return;
    }

    result = KThreadResume(&engine_thread);
    if (result != 0) {
        printf("FATAL: KThreadResume failed (ret=%d)\n", result);
        fflush(stdout);
        free(engine_thread_stack);
        engine_thread_stack = NULL;
        engine_running      = false;
        return;
    }

    // Give engine time to reach uci_loop before sending commands
    KThreadSleepMs(200);
    send_to_engine("uci\nisready\n");
}

// ============================================================
// Board coordinate helpers
// ============================================================

int screen_to_board_sq(int r, int c)
{
    if (board_orientation == 1) return r * 8 + c;
    else                        return (7 - r) * 8 + (7 - c);
}

void move_to_uci(GuiMove m, char *buf)
{
    int f_col = m.from % 8, f_row = 8 - (m.from / 8);
    int t_col = m.to   % 8, t_row = 8 - (m.to   / 8);
    // FIXED: use snprintf, not sprintf, for safety
    snprintf(buf, 6, "%c%d%c%d",
             'a' + f_col, f_row,
             'a' + t_col, t_row);
    if (m.promo != 0) {
        char p = 'q';
        if      (m.promo == 2) p = 'n';
        else if (m.promo == 3) p = 'b';
        else if (m.promo == 4) p = 'r';
        buf[4] = p;
        buf[5] = '\0';
    }
}

// ============================================================
// Chess logic helpers
// ============================================================

int find_king(const BoardState *state, int color)
{
    int target = color * 6;
    for (int i = 0; i < 64; i++)
        if (state->board[i] == target) return i;
    return -1;
}

int is_square_attacked(const BoardState *state, int sq, int attacker)
{
    // FIXED: bounds check sq before any array access
    if (sq < 0 || sq >= 64) return 0;

    int r = sq / 8, c = sq % 8;

    // Knights
    static const int kn_r[] = {-2,-2,-1,-1, 1, 1, 2, 2};
    static const int kn_c[] = {-1, 1,-2, 2,-2, 2,-1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn_r[i], nc = c + kn_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8)
            if (state->board[nr*8+nc] == attacker * 2) return 1;
    }

    // King
    static const int kg_r[] = {-1,-1,-1, 0, 0, 1, 1, 1};
    static const int kg_c[] = {-1, 0, 1,-1, 1,-1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kg_r[i], nc = c + kg_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8)
            if (state->board[nr*8+nc] == attacker * 6) return 1;
    }

    // Pawns: attacker==1 (White) attacks from row below (r+1)
    //        attacker==-1 (Black) attacks from row above (r-1)
    int p_dr = (attacker == 1) ? 1 : -1;
    for (int dc = -1; dc <= 1; dc += 2) {
        int nr = r + p_dr, nc = c + dc;
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8)
            if (state->board[nr*8+nc] == attacker * 1) return 1;
    }

    // Diagonal sliders: bishop (3) and queen (5)
    static const int d_r[] = {-1,-1, 1, 1};
    static const int d_c[] = {-1, 1,-1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + d_r[i], nc = c + d_c[i];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int t = state->board[nr*8+nc];
            if (t != 0) {
                if (t == attacker*3 || t == attacker*5) return 1;
                break;
            }
            nr += d_r[i]; nc += d_c[i];
        }
    }

    // Straight sliders: rook (4) and queen (5)
    static const int s_r[] = {-1, 1, 0, 0};
    static const int s_c[] = { 0, 0,-1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + s_r[i], nc = c + s_c[i];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int t = state->board[nr*8+nc];
            if (t != 0) {
                if (t == attacker*4 || t == attacker*5) return 1;
                break;
            }
            nr += s_r[i]; nc += s_c[i];
        }
    }
    return 0;
}

int is_pseudo_legal_move(const BoardState *state, GuiMove m)
{
    // FIXED: validate indices before any array access
    if (m.from < 0 || m.from >= 64 || m.to < 0 || m.to >= 64) return 0;

    int p      = state->board[m.from];
    int target = state->board[m.to];
    int turn   = state->turn;

    if (p == 0)                                                    return 0;
    if ((turn ==  1 && p < 0) || (turn == -1 && p > 0))           return 0;
    if (m.from == m.to)                                            return 0;
    if (target != 0 &&
        ((turn ==  1 && target > 0) || (turn == -1 && target < 0))) return 0;

    int fr = m.from / 8, fc = m.from % 8;
    int tr = m.to   / 8, tc = m.to   % 8;
    int dr = tr - fr,    dc = tc - fc;
    int abs_dr = abs(dr), abs_dc = abs(dc);

    switch (abs(p)) {
    case 1: { // Pawn
        int dir     = (turn ==  1) ? -1 :  1;
        int start_r = (turn ==  1) ?  6 :  1;
        // One square forward
        if (dc == 0 && dr == dir && target == 0) return 1;
        // Two squares from start
        if (dc == 0 && fr == start_r && dr == 2 * dir &&
            state->board[(fr + dir) * 8 + fc] == 0 && target == 0) return 1;
        // Capture (diagonal)
        if (abs_dc == 1 && dr == dir) {
            if (target != 0) return 1;
            // FIXED: guard ep with != -1 before comparing
            if (state->ep != -1 && m.to == state->ep) return 1;
        }
        return 0;
    }
    case 2: // Knight
        return (abs_dr == 2 && abs_dc == 1) || (abs_dr == 1 && abs_dc == 2);
    case 3: { // Bishop
        if (abs_dr != abs_dc) return 0;
        int sr = (dr > 0) ? 1 : -1, sc = (dc > 0) ? 1 : -1;
        for (int nr = fr+sr, nc = fc+sc; nr != tr; nr += sr, nc += sc)
            if (state->board[nr*8+nc] != 0) return 0;
        return 1;
    }
    case 4: { // Rook
        if (dr != 0 && dc != 0) return 0;
        int sr = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
        int sc = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);
        for (int nr = fr+sr, nc = fc+sc; nr != tr || nc != tc;
             nr += sr, nc += sc)
            if (state->board[nr*8+nc] != 0) return 0;
        return 1;
    }
    case 5: { // Queen
        if (abs_dr != abs_dc && dr != 0 && dc != 0) return 0;
        int sr, sc;
        if (abs_dr == abs_dc) {
            sr = (dr > 0) ? 1 : -1;
            sc = (dc > 0) ? 1 : -1;
        } else {
            sr = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
            sc = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);
        }
        for (int nr = fr+sr, nc = fc+sc; nr != tr || nc != tc;
             nr += sr, nc += sc)
            if (state->board[nr*8+nc] != 0) return 0;
        return 1;
    }
    case 6: { // King
        if (abs_dr <= 1 && abs_dc <= 1) return 1;
        // Castling
        if (dr == 0 && abs_dc == 2) {
            if (turn == 1 && fr == 7 && fc == 4) {
                if (m.to == 62 && (state->castle & 1) &&
                    state->board[61] == 0 && state->board[62] == 0 &&
                    !is_square_attacked(state, 60, -1) &&
                    !is_square_attacked(state, 61, -1) &&
                    !is_square_attacked(state, 62, -1)) return 1;
                if (m.to == 58 && (state->castle & 2) &&
                    state->board[59] == 0 && state->board[58] == 0 &&
                    state->board[57] == 0 &&
                    !is_square_attacked(state, 60, -1) &&
                    !is_square_attacked(state, 59, -1) &&
                    !is_square_attacked(state, 58, -1)) return 1;
            }
            if (turn == -1 && fr == 0 && fc == 4) {
                if (m.to == 6 && (state->castle & 4) &&
                    state->board[5] == 0 && state->board[6] == 0 &&
                    !is_square_attacked(state, 4, 1) &&
                    !is_square_attacked(state, 5, 1) &&
                    !is_square_attacked(state, 6, 1)) return 1;
                if (m.to == 2 && (state->castle & 8) &&
                    state->board[3] == 0 && state->board[2] == 0 &&
                    state->board[1] == 0 &&
                    !is_square_attacked(state, 4, 1) &&
                    !is_square_attacked(state, 3, 1) &&
                    !is_square_attacked(state, 2, 1)) return 1;
            }
        }
        return 0;
    }
    }
    return 0;
}

int is_legal_gui_move(const BoardState *state, GuiMove m)
{
    if (!is_pseudo_legal_move(state, m)) return 0;
    BoardState next;
    make_gui_move(state, &next, m);
    int king = find_king(&next, state->turn);
    if (king == -1) return 0;
    return !is_square_attacked(&next, king, -state->turn);
}

int has_legal_moves(const BoardState *state)
{
    for (int f = 0; f < 64; f++) {
        int p = state->board[f];
        if (p == 0) continue;
        if ((state->turn ==  1 && p < 0) ||
            (state->turn == -1 && p > 0)) continue;
        for (int t = 0; t < 64; t++) {
            GuiMove m = {f, t, 0};
            // FIXED: only set promo for correct color reaching back rank
            if (abs(p) == 1) {
                int tr = t / 8;
                if ((state->turn ==  1 && tr == 0) ||
                    (state->turn == -1 && tr == 7))
                    m.promo = 5;
            }
            if (is_legal_gui_move(state, m)) return 1;
        }
    }
    return 0;
}

int count_repetitions(const BoardState *state)
{
    int count = 1;
    for (int i = 0; i < history_count; i++) {
        // Only compare positions with the same side to move
        if (history[i].turn != state->turn)   continue;
        if (history[i].castle != state->castle) continue;
        if (history[i].ep    != state->ep)    continue;
        if (memcmp(state->board, history[i].board,
                   sizeof(state->board)) == 0)
            count++;
    }
    return count;
}

void make_gui_move(const BoardState *src, BoardState *dst, GuiMove m)
{
    *dst = *src;
    int p = dst->board[m.from];

    // Capture detection uses src state (before modification)
    int is_capture = (src->board[m.to] != 0) ||
                     (abs(p) == 1 && src->ep != -1 && m.to == src->ep);

    dst->board[m.from] = 0;

    // Place piece (or promoted piece) on destination
    if (m.promo != 0) {
        // FIXED: use src->turn - dst->turn not yet flipped
        dst->board[m.to] = src->turn * m.promo;
    } else {
        dst->board[m.to] = p;
    }

    // En-passant: remove captured pawn
    if (abs(p) == 1 && src->ep != -1 && m.to == src->ep) {
        // FIXED: use src->turn for direction (not yet flipped)
        // White pawn moves up (decreasing index): captured pawn is below ep
        // Black pawn moves down (increasing index): captured pawn is above ep
        int cap_sq = m.to + (src->turn == 1 ? 8 : -8);
        if (cap_sq >= 0 && cap_sq < 64)
            dst->board[cap_sq] = 0;
    }

    // Set new en-passant square
    dst->ep = -1;
    if (abs(p) == 1 && abs(m.from - m.to) == 16) {
        // FIXED: use src->turn (before flip)
        dst->ep = m.from + (src->turn == 1 ? -8 : 8);
    }

    // Castling: relocate the rook
    if (abs(p) == 6) {
        if      (m.from == 60 && m.to == 62) {
            dst->board[61] = dst->board[63]; dst->board[63] = 0;
        } else if (m.from == 60 && m.to == 58) {
            dst->board[59] = dst->board[56]; dst->board[56] = 0;
        } else if (m.from == 4  && m.to == 6) {
            dst->board[5]  = dst->board[7];  dst->board[7]  = 0;
        } else if (m.from == 4  && m.to == 2) {
            dst->board[3]  = dst->board[0];  dst->board[0]  = 0;
        }
    }

    // Update castling rights
    if (m.from == 60) dst->castle &= ~(1 | 2);
    if (m.from ==  4) dst->castle &= ~(4 | 8);
    if (m.from == 63 || m.to == 63) dst->castle &= ~1;
    if (m.from == 56 || m.to == 56) dst->castle &= ~2;
    if (m.from ==  7 || m.to ==  7) dst->castle &= ~4;
    if (m.from ==  0 || m.to ==  0) dst->castle &= ~8;

    // Flip side to move
    dst->turn = -dst->turn;

    // Halfmove clock
    if (abs(p) == 1 || is_capture) dst->halfmoves = 0;
    else                            dst->halfmoves++;

    // Fullmove counter increments after Black's move
    if (dst->turn == 1) dst->fullmoves++;
}

void push_state(const BoardState *state, GuiMove m)
{
    if (history_count < MAX_HISTORY - 1) {
        history[history_count]      = *state;
        move_history[history_count] = m;
        history_count++;
    }
}

void reset_engine_metrics(void)
{
    engine_nps        = 0;
    engine_score_type = -1;
    engine_score_val  = 0;
    engine_pv[0]      = '\0';
    engine_depth      = 0;
    engine_seldepth   = 0;
    engine_time_ms    = 0;
    engine_nodes      = 0;
    engine_hashfull   = 0;
    engine_tbhits     = 0;
}

void trigger_engine_move(void)
{
    reset_engine_metrics();

    // FIXED: 16384 bytes - enough for MAX_HISTORY=2048 moves * ~6 chars
    // Old 8192 could overflow silently and corrupt adjacent memory
    static char cmd[16384];
    cmd[0] = '\0';

    const char *header = "position startpos moves";
    // Use strncpy then force-terminate
    strncpy(cmd, header, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    int len = (int)strlen(cmd);

    for (int i = 0; i < history_count; i++) {
        char uci_m[8];
        move_to_uci(move_history[i], uci_m);
        int mlen = (int)strlen(uci_m);

        // Reserve room for ' ' + move + '\n' + '\0'
        if (len + 1 + mlen + 2 >= (int)sizeof(cmd)) break;

        cmd[len++] = ' ';
        memcpy(cmd + len, uci_m, (size_t)mlen);
        len += mlen;
    }

    // FIXED: safe termination - always room for \n\0
    if (len < (int)sizeof(cmd) - 2) {
        cmd[len++] = '\n';
        cmd[len]   = '\0';
    } else {
        cmd[sizeof(cmd) - 2] = '\n';
        cmd[sizeof(cmd) - 1] = '\0';
    }

    send_to_engine(cmd);

    char go_cmd[256];
    if (time_control_type == 0) {
        snprintf(go_cmd, sizeof(go_cmd),
                 "go movetime %d\n", time_control_val);
    } else if (time_control_type == 1) {
        snprintf(go_cmd, sizeof(go_cmd),
                 "go depth %d\n", time_control_val);
    } else {
        snprintf(go_cmd, sizeof(go_cmd),
                 "go nodes %d\n", time_control_val);
    }
    send_to_engine(go_cmd);
}

GuiMove uci_to_gui_move(const char *str)
{
    GuiMove m = {-1, -1, 0};
    if (!str || strlen(str) < 4) return m;

    int f_col = str[0] - 'a';
    int f_row = 8 - (str[1] - '0');
    int t_col = str[2] - 'a';
    int t_row = 8 - (str[3] - '0');

    // FIXED: validate before use
    if (f_col < 0 || f_col > 7 || f_row < 0 || f_row > 7 ||
        t_col < 0 || t_col > 7 || t_row < 0 || t_row > 7)
        return m;

    m.from = f_row * 8 + f_col;
    m.to   = t_row * 8 + t_col;

    if (strlen(str) >= 5) {
        switch (str[4]) {
        case 'n': m.promo = 2; break;
        case 'b': m.promo = 3; break;
        case 'r': m.promo = 4; break;
        default:  m.promo = 5; break;
        }
    }
    return m;
}

void process_engine_output(char *line)
{
    if (!line) return;

    // Strip trailing whitespace
    int len = (int)strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n' ||
                       line[len-1] == ' '  || line[len-1] == '\t'))
        line[--len] = '\0';
    if (len == 0) return;

    if (strcmp(line, "uciok") == 0)    { engine_recvd_uciok   = true; return; }
    if (strcmp(line, "readyok") == 0)  { engine_recvd_readyok = true; return; }

    if (strncmp(line, "info", 4) == 0) {
        char *ptr;

        // FIXED: advance past the token keyword before sscanf
        if ((ptr = strstr(line, " nps "))) {
            long long v = 0;
            if (sscanf(ptr + 5, "%lld", &v) == 1) engine_nps = v;
        }
        if ((ptr = strstr(line, " score "))) {
            char score_type[16] = {0};
            int  score_val      = 0;
            if (sscanf(ptr + 7, "%15s %d", score_type, &score_val) == 2) {
                if (strcmp(score_type, "cp") == 0) {
                    engine_score_type = 0;
                    engine_score_val  = score_val * current_state.turn;
                } else if (strcmp(score_type, "mate") == 0) {
                    engine_score_type = 1;
                    engine_score_val  = score_val * current_state.turn;
                }
            }
        }
        if ((ptr = strstr(line, " depth "))) {
            int d = 0;
            if (sscanf(ptr + 7, "%d", &d) == 1) engine_depth = d;
        }
        if ((ptr = strstr(line, " seldepth "))) {
            int sd = 0;
            if (sscanf(ptr + 10, "%d", &sd) == 1) engine_seldepth = sd;
        }
        if ((ptr = strstr(line, " time "))) {
            int t = 0;
            if (sscanf(ptr + 6, "%d", &t) == 1) engine_time_ms = t;
        }
        if ((ptr = strstr(line, " nodes "))) {
            long long n = 0;
            if (sscanf(ptr + 7, "%lld", &n) == 1) engine_nodes = n;
        }
        if ((ptr = strstr(line, " hashfull "))) {
            int h = 0;
            if (sscanf(ptr + 10, "%d", &h) == 1) engine_hashfull = h;
        }
        if ((ptr = strstr(line, " tbhits "))) {
            long long tb = 0;
            if (sscanf(ptr + 8, "%lld", &tb) == 1) engine_tbhits = tb;
        }
        if ((ptr = strstr(line, " pv "))) {
            strncpy(engine_pv, ptr + 4, sizeof(engine_pv) - 1);
            engine_pv[sizeof(engine_pv) - 1] = '\0';
        }
    }

    if (strncmp(line, "bestmove", 8) == 0) {
        char move_str[16] = {0};
        if (sscanf(line + 9, "%15s", move_str) == 1) {
            if (strcmp(move_str, "(none)") == 0 ||
                strcmp(move_str, "NULL")   == 0) {
                engine_thinking = 0;
                return;
            }
            GuiMove m = uci_to_gui_move(move_str);
            // FIXED: validate before calling is_legal
            if (m.from >= 0 && m.from < 64 &&
                m.to   >= 0 && m.to   < 64 &&
                is_legal_gui_move(&current_state, m)) {
                push_state(&current_state, m);
                BoardState next;
                make_gui_move(&current_state, &next, m);
                current_state = next;
            }
            engine_thinking = 0;
        }
    }
}

// ============================================================
// Engine output reader (called from main loop)
// ============================================================
static char engine_buffer[8192];
static int  engine_buf_len = 0;

void read_from_engine(void)
{
    char tmp[2048];
    int  n;
    while ((n = fifo_read(&eng_to_gui_pipe, tmp, (int)sizeof(tmp) - 1)) > 0) {
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
                    // Buffer full: flush and restart
                    engine_buffer[engine_buf_len] = '\0';
                    process_engine_output(engine_buffer);
                    engine_buf_len = 0;
                    engine_buffer[engine_buf_len++] = tmp[i];
                }
            }
        }
    }
}

// ============================================================
// PGN move notation
// ============================================================
void move_to_pgn(const BoardState *state, GuiMove m, char *buf)
{
    int p      = state->board[m.from];
    int abs_p  = abs(p);
    int target = state->board[m.to];
    int is_cap = (target != 0) ||
                 (abs_p == 1 && state->ep != -1 && m.to == state->ep);

    if (abs_p == 6 && abs(m.from - m.to) == 2) {
        strcpy(buf, (m.to > m.from) ? "O-O" : "O-O-O");
    } else {
        char *ptr = buf;
        if (abs_p == 1) {
            if (is_cap) { *ptr++ = 'a' + (m.from % 8); *ptr++ = 'x'; }
        } else {
            static const char piece_ch[] = " PNBRQK";
            *ptr++ = piece_ch[abs_p];

            if (abs_p >= 2 && abs_p <= 5) {
                int file_conflict = 0, rank_conflict = 0, conflict = 0;
                for (int sq = 0; sq < 64; sq++) {
                    if (sq == m.from || state->board[sq] != p) continue;
                    GuiMove t = {sq, m.to, 0};
                    if (is_legal_gui_move(state, t)) {
                        conflict = 1;
                        if (sq % 8 == m.from % 8) file_conflict = 1;
                        if (sq / 8 == m.from / 8) rank_conflict = 1;
                    }
                }
                if (conflict) {
                    if      (!file_conflict) *ptr++ = 'a' + (m.from % 8);
                    else if (!rank_conflict) *ptr++ = '8' - (m.from / 8);
                    else {
                        *ptr++ = 'a' + (m.from % 8);
                        *ptr++ = '8' - (m.from / 8);
                    }
                }
            }
            if (is_cap) *ptr++ = 'x';
        }

        *ptr++ = 'a' + (m.to % 8);
        *ptr++ = '8' - (m.to / 8);

        if (abs_p == 1 && m.promo != 0) {
            static const char promo_ch[] = " PNBRQK";
            *ptr++ = '=';
            *ptr++ = (m.promo < 6) ? promo_ch[m.promo] : 'Q';
        }
        *ptr = '\0';
    }

    // Append check / checkmate symbol
    BoardState next;
    make_gui_move(state, &next, m);
    int opp_king = find_king(&next, next.turn);
    if (opp_king != -1 &&
        is_square_attacked(&next, opp_king, -next.turn)) {
        strcat(buf, has_legal_moves(&next) ? "+" : "#");
    }
}

// ============================================================
// Frame cache: compute expensive state once per draw_ui call
// ============================================================
typedef struct {
    int has_moves;
    int is_check;
    int king_in_check_sq;
    int repetitions;
} FrameCache;

static FrameCache build_frame_cache(void)
{
    FrameCache fc;
    fc.has_moves       = has_legal_moves(&current_state);
    fc.repetitions     = count_repetitions(&current_state);
    fc.is_check        = 0;
    fc.king_in_check_sq = -1;

    int wk = find_king(&current_state,  1);
    int bk = find_king(&current_state, -1);
    if (wk != -1 && is_square_attacked(&current_state, wk, -1)) {
        fc.is_check        = 1;
        fc.king_in_check_sq = wk;
    } else if (bk != -1 && is_square_attacked(&current_state, bk, 1)) {
        fc.is_check        = 1;
        fc.king_in_check_sq = bk;
    }
    return fc;
}

// ============================================================
// UI drawing
// ============================================================
void print_recent_moves(int row)
{
    int total = (history_count + 1) / 2;
    if (total == 0) return;

    int start = (total > 10) ? total - 9 : 1;
    int disp  = start + row;
    if (disp > total) return;

    int w = (disp - 1) * 2;
    int b = w + 1;

    printf("   %2d. ", disp);
    if (w < history_count) {
        char pgn[16];
        move_to_pgn(&history[w], move_history[w], pgn);
        printf("%-6s", pgn);
    } else {
        printf("------");
    }
    printf(" ");
    if (b < history_count) {
        char pgn[16];
        move_to_pgn(&history[b], move_history[b], pgn);
        printf("%-6s", pgn);
    } else {
        printf("%s", (w < history_count) ? "..." : "------");
    }
}

void print_side_panel_line(int panel_row)
{
    printf(" ");
    print_recent_moves(panel_row);
}

void draw_ui(void)
{
    // FIXED: all expensive calls made once and cached
    FrameCache fc = build_frame_cache();

    printf("\033[H\r\n");

    const char *turn_str = (current_state.turn == 1)
        ? "\033[1;33mWhite\033[0m"
        : "\033[1;35mBlack\033[0m";
    const char *w_play = (user_side ==  1 || user_side == 0) ? "Hum" : "Eng";
    const char *b_play = (user_side == -1 || user_side == 0) ? "Hum" : "Eng";

    printf("  ");
    if (current_state.halfmoves >= 100) {
        printf("\033[1;36mDRAW (50-move rule)\033[0m");
    } else if (fc.repetitions >= 3) {
        printf("\033[1;36mDRAW (threefold repetition)\033[0m");
    } else if (!fc.has_moves) {
        printf(fc.is_check ? "\033[1;31mCHECKMATE!\033[0m"
                           : "\033[1;36mSTALEMATE!\033[0m");
    } else if (fc.is_check) {
        printf("%s (\033[1;31mCHECK!\033[0m)", turn_str);
    } else {
        printf("%s's Turn", turn_str);
    }

    printf(" | W:%s B:%s", w_play, b_play);
    static const char *tc_names[] = {
        "Time-Limit", "Depth-Limit", "Node-Limit"
    };
    printf(" | %s", tc_names[time_control_type]);
    if      (time_control_type == 0) printf(" (%d ms)",    time_control_val);
    else if (time_control_type == 1) printf(" (depth %d)", time_control_val);
    else                             printf(" (%d nodes)",  time_control_val);
    printf("\033[K\r\n\r\n");

    // Column labels
    printf((board_orientation == 1)
           ? "     a  b  c  d  e  f  g  h    "
           : "     h  g  f  e  d  c  b  a    ");
    print_side_panel_line(0);
    printf("\033[K\r\n");

    for (int r = 0; r < 8; r++) {
        int rank_lbl = (board_orientation == 1) ? (8 - r) : (r + 1);
        printf("  %d ", rank_lbl);

        for (int c = 0; c < 8; c++) {
            int sq = screen_to_board_sq(r, c);
            int p  = current_state.board[sq];
            int is_light = ((sq / 8) + (sq % 8)) % 2 == 0;

            int is_prev = 0;
            if (history_count > 0) {
                GuiMove lm = move_history[history_count - 1];
                is_prev = (sq == lm.from || sq == lm.to);
            }

            int is_legal_dest = 0;
            if (selected_sq != -1) {
                GuiMove tm = {selected_sq, sq, 0};
                int sp = current_state.board[selected_sq];
                if (abs(sp) == 1) {
                    int tr = sq / 8;
                    if ((current_state.turn ==  1 && tr == 0) ||
                        (current_state.turn == -1 && tr == 7))
                        tm.promo = 5;
                }
                if (is_legal_gui_move(&current_state, tm)) is_legal_dest = 1;
            }

            const char *bg;
            if      (r == cursor_r && c == cursor_c)  bg = "\033[48;5;208m";
            else if (sq == selected_sq)                bg = "\033[48;5;34m";
            else if (sq == fc.king_in_check_sq)        bg = "\033[48;5;196m";
            else if (is_prev)   bg = is_light ? "\033[48;5;75m"  : "\033[48;5;68m";
            else if (is_legal_dest) bg = is_light ? "\033[48;5;151m": "\033[48;5;108m";
            else                bg = is_light ? "\033[48;5;180m" : "\033[48;5;94m";

            const char *fg  = (p > 0) ? "\033[38;5;255m\033[1m"
                                       : "\033[38;5;232m";
            static const char *glyphs[] = {" ","P","N","B","R","Q","K"};
            const char *g = (p != 0 && abs(p) <= 6) ? glyphs[abs(p)] : " ";

            printf("%s%s %s \033[0m", bg, fg, g);
        }

        printf(" %d ", rank_lbl);
        print_side_panel_line(r + 1);
        printf("\033[K\r\n");
    }

    // Bottom column labels
    printf((board_orientation == 1)
           ? "     a  b  c  d  e  f  g  h    "
           : "     h  g  f  e  d  c  b  a    ");
    print_side_panel_line(9);
    printf("\033[K\r\n\r\n");

    // Controls hint
    printf(" \033[38;5;245m[D-PAD] Move | [A] Select | [B] Undo | "
           "[1] Cycle Level | [2] Flip | [+] Sides | "
           "[-] Cycle Type | [HOME] Quit\033[0m\033[K\r\n");

    // Engine HUD
    printf(" \033[1;34mEngine:\033[0m ");
    if      (engine_thinking)      printf("\033[1;32mThinking\033[0m");
    else if (engine_recvd_readyok) printf("\033[1;30mIdle\033[0m");
    else                           printf("\033[1;31mConnecting\033[0m");

    if (engine_nps >= 1000000)
        printf(" | \033[38;5;250mSpeed:\033[0m %.2fM nps",
               (double)engine_nps / 1000000.0);
    else if (engine_nps >= 1000)
        printf(" | \033[38;5;250mSpeed:\033[0m %.1fk nps",
               (double)engine_nps / 1000.0);
    else
        printf(" | \033[38;5;250mSpeed:\033[0m %lld nps", engine_nps);

    if (engine_score_type == 0) {
        printf(" | \033[38;5;250mEval:\033[0m %s%+.2f\033[0m",
               (engine_score_val > 0) ? "\033[1;32m" :
               (engine_score_val < 0) ? "\033[1;31m" : "",
               (double)engine_score_val / 100.0);
    } else if (engine_score_type == 1) {
        if      (engine_score_val > 0)
            printf(" | \033[38;5;250mEval:\033[0m \033[1;32m+M%d\033[0m",
                   engine_score_val);
        else if (engine_score_val < 0)
            printf(" | \033[38;5;250mEval:\033[0m \033[1;31m-M%d\033[0m",
                   -engine_score_val);
        else
            printf(" | \033[38;5;250mEval:\033[0m M0");
    } else {
        printf(" | \033[38;5;250mEval:\033[0m -");
    }
    printf("\033[K\r\n");

    printf(" \033[1;33mBest Line:\033[0m \033[38;5;250m%s\033[0m\033[K\r\n",
           (engine_pv[0] != '\0') ? engine_pv : "-");

    printf("\033[J");
    fflush(stdout);
}

// ============================================================
// Input handling
// ============================================================
int get_promo_choice(void)
{
    printf("\r\n \033[1;33mPromote: [A]=Queen [B]=Rook [+]=Bishop [-]=Knight\033[0m");
    fflush(stdout);
    int choice = 5;
    for (;;) {
        WPAD_ScanPads();
        u32 p = WPAD_ButtonsDown(0);
        if (p & WPAD_BUTTON_A)     { choice = 5; break; }
        if (p & WPAD_BUTTON_B)     { choice = 4; break; }
        if (p & WPAD_BUTTON_PLUS)  { choice = 3; break; }
        if (p & WPAD_BUTTON_MINUS) { choice = 2; break; }
        VIDEO_WaitVSync();
    }
    printf("\r\033[K\033[A\033[K");
    fflush(stdout);
    return choice;
}

void handle_select(void)
{
    if (engine_thinking)                           return;
    if (current_state.halfmoves >= 100)            return;
    if (count_repetitions(&current_state) >= 3)    return;
    if (!has_legal_moves(&current_state))          return;
    if (user_side == 2)                            return;
    if (user_side ==  1 && current_state.turn !=  1) return;
    if (user_side == -1 && current_state.turn != -1) return;

    int sq = screen_to_board_sq(cursor_r, cursor_c);

    if (selected_sq == -1) {
        int p = current_state.board[sq];
        if (p != 0 &&
            ((current_state.turn ==  1 && p > 0) ||
             (current_state.turn == -1 && p < 0)))
            selected_sq = sq;
    } else {
        GuiMove m = {selected_sq, sq, 0};
        int p = current_state.board[selected_sq];

        // FIXED: promotion rank is color-dependent
        int tr      = sq / 8;
        int is_prom = (abs(p) == 1) &&
                      ((current_state.turn ==  1 && tr == 0) ||
                       (current_state.turn == -1 && tr == 7));
        if (is_prom) m.promo = 5; // Queen for legality check

        if (is_legal_gui_move(&current_state, m)) {
            if (is_prom) m.promo = get_promo_choice();
            push_state(&current_state, m);
            BoardState next;
            make_gui_move(&current_state, &next, m);
            current_state = next;
            selected_sq   = -1;
            reset_engine_metrics();
        } else {
            int t = current_state.board[sq];
            selected_sq = (t != 0 &&
                           ((current_state.turn ==  1 && t > 0) ||
                            (current_state.turn == -1 && t < 0)))
                          ? sq : -1;
        }
    }
}

void handle_undo(void)
{
    if (engine_thinking) {
        send_to_engine("stop\n");
        engine_thinking = 0;
    }
    reset_engine_metrics();
    int steps = (user_side == 1 || user_side == -1) ? 2 : 1;
    while (steps-- > 0 && history_count > 0) {
        history_count--;
        current_state = history[history_count];
    }
    selected_sq = -1;
}

void handle_reset_board(void)
{
    if (engine_thinking) {
        send_to_engine("stop\n");
        engine_thinking = 0;
    }
    reset_engine_metrics();
    init_board(&current_state);
    history_count = 0;
    selected_sq   = -1;
    cursor_r      = 6;
    cursor_c      = 4;
    send_to_engine("ucinewgame\nisready\n");
}

void handle_switch_sides(void)
{
    if (engine_thinking) {
        send_to_engine("stop\n");
        engine_thinking = 0;
    }
    selected_sq = -1;
    if      (user_side ==  1) user_side = -1;
    else if (user_side == -1) user_side =  0;
    else if (user_side ==  0) user_side =  2;
    else                      user_side =  1;
}

void adjust_time_control(void)
{
    if (time_control_type == 0) {
        static const int tlist[] = {
            1,10,50,100,500,1000,1500,2000,3000,5000,10000
        };
        int cnt = (int)(sizeof(tlist)/sizeof(tlist[0]));
        int idx = -1;
        for (int i = 0; i < cnt; i++)
            if (time_control_val == tlist[i]) { idx = i; break; }
        time_control_val = (idx == -1 || idx == cnt-1) ? tlist[0]
                                                       : tlist[idx+1];
    } else if (time_control_type == 1) {
        time_control_val = (time_control_val % 20) + 1;
    } else {
        static const int nlist[] = {
            512,1024,2048,4096,8192,16384,32768,
            65536,131072,262144,524288,1048576,
            2097152,4194304,8388608
        };
        int cnt = (int)(sizeof(nlist)/sizeof(nlist[0]));
        int idx = -1;
        for (int i = 0; i < cnt; i++)
            if (time_control_val == nlist[i]) { idx = i; break; }
        time_control_val = (idx == -1 || idx == cnt-1) ? nlist[0]
                                                       : nlist[idx+1];
    }
}

void handle_wii_input(void)
{
    WPAD_ScanPads();
    u32 p = WPAD_ButtonsDown(0);

    if (p & WPAD_BUTTON_UP)    { if (cursor_r > 0) cursor_r--; }
    if (p & WPAD_BUTTON_DOWN)  { if (cursor_r < 7) cursor_r++; }
    if (p & WPAD_BUTTON_LEFT)  { if (cursor_c > 0) cursor_c--; }
    if (p & WPAD_BUTTON_RIGHT) { if (cursor_c < 7) cursor_c++; }
    if (p & WPAD_BUTTON_A)     handle_select();
    if (p & WPAD_BUTTON_B)     handle_undo();
    if (p & WPAD_BUTTON_1)     adjust_time_control();
    if (p & WPAD_BUTTON_2)     board_orientation = -board_orientation;
    if (p & WPAD_BUTTON_PLUS)  handle_switch_sides();
    if (p & WPAD_BUTTON_MINUS) {
        time_control_type = (time_control_type + 1) % 3;
        time_control_val  = (time_control_type == 0) ?    1 :
                            (time_control_type == 1) ?    1 : 512;
    }
    if (p & WPAD_BUTTON_HOME) exit(0);
}

// ============================================================
// Board initialisation
// ============================================================
void init_board(BoardState *state)
{
    static const int start[64] = {
        -4,-2,-3,-5,-6,-3,-2,-4,
        -1,-1,-1,-1,-1,-1,-1,-1,
         0, 0, 0, 0, 0, 0, 0, 0,
         0, 0, 0, 0, 0, 0, 0, 0,
         0, 0, 0, 0, 0, 0, 0, 0,
         0, 0, 0, 0, 0, 0, 0, 0,
         1, 1, 1, 1, 1, 1, 1, 1,
         4, 2, 3, 5, 6, 3, 2, 4
    };
    memcpy(state->board, start, sizeof(start));
    state->turn      =  1;
    state->castle    = 15;
    state->ep        = -1;
    state->halfmoves =  0;
    state->fullmoves =  1;
}

// ============================================================
// Main game loop
// ============================================================
int run_gui_mode(void)
{
    init_board(&current_state);
    start_engine();

    for (;;) {
        // Determine whether engine should move
        int eng_active = 0;
        if (has_legal_moves(&current_state)    &&
            current_state.halfmoves < 100       &&
            count_repetitions(&current_state) < 3) {
            if (user_side == 2)                                  eng_active = 1;
            else if (user_side ==  1 && current_state.turn == -1) eng_active = 1;
            else if (user_side == -1 && current_state.turn ==  1) eng_active = 1;
        }
        if (eng_active && !engine_thinking) {
            engine_thinking = 1;
            trigger_engine_move();
        }

        draw_ui();
        handle_wii_input();
        read_from_engine();
        KThreadSleepMs(16); // ~60 fps
    }
    return 0;
}

// ============================================================
// Entry point
// ============================================================
int main(int argc, char **argv)
{
    init_wii_console();

    bool force_cli = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cli") == 0 ||
            strcmp(argv[i], "-c")    == 0 ||
            strcmp(argv[i], "uci")   == 0) {
            force_cli = true;
        }
    }

    if (force_cli) {
        gui_mode_active = false;

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
    }

    // gui_mode_active already true - no race condition
    return run_gui_mode();
}
