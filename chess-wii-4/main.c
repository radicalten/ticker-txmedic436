/*
  Stockfish - Wii Port
  Threading via devkitPPC Tuxedo KThread (thread.c / thread.h).
  Pure C. No C++.

  Inter-thread communication uses two FIFO ring queues:
    cmd_queue  : GUI -> Engine  (commands / UCI strings)
    rsp_queue  : Engine -> GUI  (UCI output lines)

  Queue index updates are protected with PPCIrqLockByMsr /
  PPCIrqUnlockByMsr (the correct Tuxedo primitive for critical
  sections on this single-core CPU).

  The engine thread blocks on cmd_q_waiters (a KThrQueue) when
  no command is available.  KThrQueueBlock and
  KThrQueueUnblockOneByValue handle their own IRQ locking
  internally, so we must NOT hold the IRQ lock when calling them.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* devkitPPC / libogc */
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/system.h>

/* Tuxedo */
#include <tuxedo/thread.h>
#include <tuxedo/types.h>
#include <tuxedo/tick.h>
#include <tuxedo/ppc/intrinsics.h>   /* PPCIrqLockByMsr / PPCIrqUnlockByMsr */

/* Stockfish engine headers */
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

/* ─────────────────────────────────────────────
   Compile-time constants
   ───────────────────────────────────────────── */
#define MAX_HISTORY          2048
#define PV_BUF_SIZE          1024
#define CMD_BUF_SIZE         32768   /* position string scratch buffer  */
#define CMD_QUEUE_SIZE       32      /* slots in GUI->Engine ring       */
#define CMD_ITEM_SIZE        512     /* max bytes per command           */
#define RSP_QUEUE_SIZE       128     /* slots in Engine->GUI ring       */
#define RSP_ITEM_SIZE        512     /* max bytes per response line     */
#define ENGINE_STACK_SIZE    (512 * 1024)

/* Input auto-repeat (in VIDEO_WaitVSync ticks, ~16 ms each) */
#define INPUT_INITIAL_DELAY  18
#define INPUT_REPEAT_DELAY    4

/* Token value used to signal "command available" */
#define CMD_TOKEN  ((uptr)1)

/* ─────────────────────────────────────────────
   Board / move types
   ───────────────────────────────────────────── */
typedef struct {
    int board[64]; /* +White -Black; 1=P 2=N 3=B 4=R 5=Q 6=K */
    int turn;      /* 1=White -1=Black */
    int castle;    /* bitmask 1=WK 2=WQ 4=BK 8=BQ */
    int ep;        /* en-passant square, -1=none */
    int halfmoves;
    int fullmoves;
} BoardState;

typedef struct {
    int from;
    int to;
    int promo;     /* 0=none 2=N 3=B 4=R 5=Q */
} GuiMove;

/* ─────────────────────────────────────────────
   Ring queue: GUI -> Engine  (commands)
   ───────────────────────────────────────────── */
static char      cmd_data[CMD_QUEUE_SIZE][CMD_ITEM_SIZE];
static int       cmd_head = 0;
static int       cmd_tail = 0;

/*
 * KThrQueue on which the engine thread sleeps when cmd_data is empty.
 * KThrQueueBlock / KThrQueueUnblockOneByValue manage their own IRQ
 * locking, so we never hold PPCIrqLockByMsr while calling them.
 */
static KThrQueue cmd_waiters;

/* ─────────────────────────────────────────────
   Ring queue: Engine -> GUI  (responses)
   ───────────────────────────────────────────── */
static char      rsp_data[RSP_QUEUE_SIZE][RSP_ITEM_SIZE];
static int       rsp_head = 0;
static int       rsp_tail = 0;

/* ─────────────────────────────────────────────
   Engine KThread
   ───────────────────────────────────────────── */
static KThread  engine_thread;
static u8       engine_stack[ENGINE_STACK_SIZE] __attribute__((aligned(8)));
static volatile bool engine_thread_alive = false;

/* ─────────────────────────────────────────────
   GUI state
   ───────────────────────────────────────────── */
static BoardState current_state;
static BoardState history[MAX_HISTORY];
static GuiMove    move_history[MAX_HISTORY];
static int        history_count = 0;

static int cursor_r = 6;
static int cursor_c = 4;
static int selected_sq = -1;

static int board_orientation = 1;  /* 1=White bottom -1=Black bottom */
static int user_side         = 1;  /* 1=White -1=Black 0=Hotseat 2=Watch */

static int time_control_type = 0;  /* 0=Time(ms) 1=Depth 2=Nodes */
static int time_control_val  = 100;

static int  engine_alive    = 0;
static int  engine_thinking = 0;

static bool engine_recvd_uciok   = false;
static bool engine_recvd_readyok = false;
static char engine_console_log[3][128];

/* Live metrics */
static long long engine_nps        = 0;
static int       engine_depth      = 0;
static int       engine_seldepth   = 0;
static int       engine_time_ms    = 0;
static long long engine_nodes      = 0;
static int       engine_hashfull   = 0;
static long long engine_tbhits     = 0;
static int       engine_score_type = -1;
static int       engine_score_val  = 0;
static char      engine_pv[PV_BUF_SIZE] = "";

/* ─────────────────────────────────────────────
   Wii video
   ───────────────────────────────────────────── */
static void       *xfb   = NULL;
static GXRModeObj *rmode = NULL;

/* ─────────────────────────────────────────────
   Forward declarations
   ───────────────────────────────────────────── */
static void    init_board(BoardState *s);
static int     is_legal_gui_move(const BoardState *s, GuiMove m);
static int     has_legal_moves(const BoardState *s);
static int     is_square_attacked(const BoardState *s, int sq, int attacker);
static void    make_gui_move(const BoardState *src, BoardState *dst, GuiMove m);
static void    send_to_engine(const char *cmd);
static int     find_king(const BoardState *s, int color);
static int     count_repetitions(const BoardState *s);
static int     get_promo_choice(void);
static void    move_to_pgn(const BoardState *s, GuiMove m, char *buf);
static void    reset_engine_metrics(void);
static void    log_engine_line(const char *line);
static void    trigger_engine_move(void);
static void    draw_ui(void);
static int     is_pseudo_legal_move(const BoardState *s, GuiMove m);
static void    move_to_uci(GuiMove m, char *buf);
static GuiMove uci_to_gui_move(const char *str);
static void    push_state(const BoardState *s, GuiMove m);
static void    process_engine_output(char *line);
static void    read_from_engine(void);
static int     screen_to_board_sq(int r, int c);

/* ═══════════════════════════════════════════════════════════════
   QUEUE PRIMITIVES

   Index arithmetic is protected with PPCIrqLockByMsr so that
   the main thread and engine thread never corrupt head/tail.

   IMPORTANT: PPCIrqLockByMsr must be released BEFORE calling
   KThrQueueBlock or KThrQueueUnblockOneByValue — those functions
   acquire the same lock internally (see thread.c source).
   ═══════════════════════════════════════════════════════════════ */

/*
 * cmd_enqueue — called from the GUI (main) thread.
 * Copies str into the next free cmd_data slot, then wakes the
 * engine thread via KThrQueueUnblockOneByValue.
 */
static void cmd_enqueue(const char *str)
{
    /* ── Critical section: update indices ── */
    PPCIrqState st = PPCIrqLockByMsr();
    int next = (cmd_tail + 1) % CMD_QUEUE_SIZE;
    int full = (next == cmd_head);
    if (!full) {
        strncpy(cmd_data[cmd_tail], str, CMD_ITEM_SIZE - 1);
        cmd_data[cmd_tail][CMD_ITEM_SIZE - 1] = '\0';
        cmd_tail = next;
    }
    PPCIrqUnlockByMsr(st);
    /* ── End critical section ── */

    if (!full) {
        /*
         * Wake one waiting engine thread.
         * KThrQueueUnblockOneByValue locks IRQs itself — do NOT
         * hold st while calling it.
         */
        KThrQueueUnblockOneByValue(&cmd_waiters, CMD_TOKEN);
    }
    /* If full, drop the command silently. */
}

/*
 * cmd_dequeue_blocking — called from the engine thread.
 * Returns when a command is available.
 *
 * Pattern:
 *   1. Lock IRQs and check if data is present.
 *   2a. If yes: copy it out, unlock, return.
 *   2b. If no:  unlock IRQs, then call KThrQueueBlock (which
 *       locks IRQs again internally).  Loop back to step 1 after
 *       the block returns so we re-check (spurious wakeup guard).
 */
static void cmd_dequeue_blocking(char *out, int maxlen)
{
    for (;;) {
        PPCIrqState st = PPCIrqLockByMsr();

        if (cmd_head != cmd_tail) {
            strncpy(out, cmd_data[cmd_head], maxlen - 1);
            out[maxlen - 1] = '\0';
            cmd_head = (cmd_head + 1) % CMD_QUEUE_SIZE;
            PPCIrqUnlockByMsr(st);
            return;
        }

        /* Queue empty — release lock before blocking */
        PPCIrqUnlockByMsr(st);

        /*
         * KThrQueueBlock acquires the IRQ lock internally,
         * suspends this thread, and releases it before returning.
         * The token value is what cmd_enqueue passes to
         * KThrQueueUnblockOneByValue.
         */
        KThrQueueBlock(&cmd_waiters, CMD_TOKEN);

        /* Re-check at top of loop after wakeup */
    }
}

/*
 * rsp_enqueue — called from the engine thread.
 */
static void rsp_enqueue(const char *str)
{
    PPCIrqState st = PPCIrqLockByMsr();
    int next = (rsp_tail + 1) % RSP_QUEUE_SIZE;
    if (next != rsp_head) {
        strncpy(rsp_data[rsp_tail], str, RSP_ITEM_SIZE - 1);
        rsp_data[rsp_tail][RSP_ITEM_SIZE - 1] = '\0';
        rsp_tail = next;
    }
    /* else: queue full, drop */
    PPCIrqUnlockByMsr(st);
}

/*
 * rsp_dequeue — called from the GUI (main) thread.
 * Non-blocking. Returns 1 if a line was retrieved, 0 if empty.
 */
static int rsp_dequeue(char *out, int maxlen)
{
    PPCIrqState st = PPCIrqLockByMsr();

    if (rsp_head == rsp_tail) {
        PPCIrqUnlockByMsr(st);
        return 0;
    }

    strncpy(out, rsp_data[rsp_head], maxlen - 1);
    out[maxlen - 1] = '\0';
    rsp_head = (rsp_head + 1) % RSP_QUEUE_SIZE;

    PPCIrqUnlockByMsr(st);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════
   ENGINE ↔ UCI BRIDGE

   uci.c must be patched (or wrapped via funopen) to call
   engine_bridge_getline() instead of fgets(stdin, ...) and
   engine_bridge_putline() instead of printf / puts for every
   line of UCI output it produces.
   ═══════════════════════════════════════════════════════════════ */

char *engine_bridge_getline(char *buf, int maxlen)
{
    if (!engine_thread_alive) return NULL;
    cmd_dequeue_blocking(buf, maxlen);
    return buf;
}

void engine_bridge_putline(const char *line)
{
    rsp_enqueue(line);
}

/* ═══════════════════════════════════════════════════════════════
   ENGINE THREAD ENTRY
   KThreadPrepare sets LR = KThreadExit, so returning from this
   function automatically calls KThreadExit(return_value).
   ═══════════════════════════════════════════════════════════════ */

static sptr engine_thread_entry(void *arg)
{
    (void)arg;

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

    /* uci_loop must use engine_bridge_getline / engine_bridge_putline */
    char *dummy_argv[] = { "ucichess", NULL };
    uci_loop(1, dummy_argv);

    threads_exit();
    TB_free();
    options_free();
    tt_free();
    pb_free();
#ifdef NNUE
    nnue_free();
#endif

    engine_thread_alive = false;
    return 0;
    /*
     * Returning here causes KThreadExit(0) to be called via the
     * LR set by KThreadPrepare — no explicit KThreadExit needed.
     */
}

/* ═══════════════════════════════════════════════════════════════
   ENGINE LIFECYCLE
   ═══════════════════════════════════════════════════════════════ */

static void start_engine(void)
{
    engine_recvd_uciok   = false;
    engine_recvd_readyok = false;
    memset(engine_console_log, 0, sizeof(engine_console_log));

    /* Zero the KThrQueue — it has no separate init function;
       zeroing is the correct initialisation (see thread_priv.h). */
    memset(&cmd_waiters, 0, sizeof(cmd_waiters));

    engine_thread_alive = true;
    engine_alive        = 1;

    /*
     * KThreadPrepare:
     *   - zeroes the KThread struct
     *   - sets up the PPC context (PC, LR=KThreadExit, GPRs, MSR)
     *   - sets state = KTHR_STATE_WAITING, suspend = 1
     *   - inserts the thread into the scheduler list
     *
     * Priority: one step below KTHR_MAIN_PRIO (0x3f) so the main
     * thread stays responsive.  The engine will run whenever the
     * main thread is blocked in VIDEO_WaitVSync or KThreadYield.
     */
    KThreadPrepare(
        &engine_thread,
        engine_thread_entry,
        NULL,
        engine_stack + ENGINE_STACK_SIZE,  /* stack top (grows down) */
        KTHR_MAIN_PRIO + 1                 /* 0x40 */
    );

    /* Remove the initial suspend so the thread can be scheduled */
    KThreadResume(&engine_thread);

    /* Send initial UCI handshake */
    log_engine_line("GUI -> uci");
    log_engine_line("GUI -> isready");
    send_to_engine("uci\n");
    send_to_engine("isready\n");
}

/* ═══════════════════════════════════════════════════════════════
   SEND / RECEIVE
   ═══════════════════════════════════════════════════════════════ */

static void send_to_engine(const char *cmd)
{
    if (!engine_alive) return;

    /* Log a stripped copy for the on-screen console */
    char clean[128];
    strncpy(clean, cmd, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    int len = (int)strlen(clean);
    while (len > 0 &&
           (clean[len-1] == '\n' || clean[len-1] == '\r'))
        clean[--len] = '\0';
    char log_buf[140];
    snprintf(log_buf, sizeof(log_buf), "GUI -> %s", clean);
    log_engine_line(log_buf);

    cmd_enqueue(cmd);
}

static void read_from_engine(void)
{
    char line[RSP_ITEM_SIZE];
    while (rsp_dequeue(line, sizeof(line))) {
        process_engine_output(line);
    }
}

/* ═══════════════════════════════════════════════════════════════
   ENGINE OUTPUT PARSER
   ═══════════════════════════════════════════════════════════════ */

static void process_engine_output(char *line)
{
    int len = (int)strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n' ||
                       line[len-1] == ' '  || line[len-1] == '\t'))
        line[--len] = '\0';

    if (strcmp(line, "uciok")   == 0) engine_recvd_uciok   = true;
    if (strcmp(line, "readyok") == 0) engine_recvd_readyok = true;

    if (strncmp(line, "info", 4) != 0 && len > 0) {
        char cl[140];
        snprintf(cl, sizeof(cl), "ENG -> %s", line);
        log_engine_line(cl);
    }

    if (strncmp(line, "info", 4) == 0) {
        char *ptr;
        if ((ptr = strstr(line, " nps ")))     { long long v; if (sscanf(ptr," nps %lld",     &v)  == 1) engine_nps      = v;  }
        if ((ptr = strstr(line, " score ")))   {
            char st[16]; int sv;
            if (sscanf(ptr," score %15s %d", st, &sv) == 2) {
                if (strcmp(st,"cp")   == 0) { engine_score_type = 0; engine_score_val = sv * current_state.turn; }
                if (strcmp(st,"mate") == 0) { engine_score_type = 1; engine_score_val = sv * current_state.turn; }
            }
        }
        if ((ptr = strstr(line, " depth ")))    { int d;       if (sscanf(ptr," depth %d",    &d)  == 1) engine_depth    = d;  }
        if ((ptr = strstr(line, " seldepth "))) { int sd;      if (sscanf(ptr," seldepth %d", &sd) == 1) engine_seldepth = sd; }
        if ((ptr = strstr(line, " time ")))     { int t;       if (sscanf(ptr," time %d",     &t)  == 1) engine_time_ms  = t;  }
        if ((ptr = strstr(line, " nodes ")))    { long long n; if (sscanf(ptr," nodes %lld",  &n)  == 1) engine_nodes    = n;  }
        if ((ptr = strstr(line, " hashfull "))) { int h;       if (sscanf(ptr," hashfull %d", &h)  == 1) engine_hashfull = h;  }
        if ((ptr = strstr(line, " tbhits ")))   { long long tb;if (sscanf(ptr," tbhits %lld", &tb) == 1) engine_tbhits   = tb; }
        if ((ptr = strstr(line, " pv "))) {
            strncpy(engine_pv, ptr + 4, PV_BUF_SIZE - 1);
            engine_pv[PV_BUF_SIZE - 1] = '\0';
        }
    }

    if (strncmp(line, "bestmove", 8) == 0) {
        char ms[16];
        if (sscanf(line, "bestmove %15s", ms) == 1 &&
            strcmp(ms, "(none)") != 0 &&
            strcmp(ms, "NULL")   != 0) {
            GuiMove m = uci_to_gui_move(ms);
            if (is_legal_gui_move(&current_state, m)) {
                push_state(&current_state, m);
                BoardState next;
                make_gui_move(&current_state, &next, m);
                current_state = next;
            }
        }
        engine_thinking = 0;
    }
}

/* ═══════════════════════════════════════════════════════════════
   UTILITY
   ═══════════════════════════════════════════════════════════════ */

static void log_engine_line(const char *line)
{
    strncpy(engine_console_log[2], engine_console_log[1], 127);
    engine_console_log[2][127] = '\0';
    strncpy(engine_console_log[1], engine_console_log[0], 127);
    engine_console_log[1][127] = '\0';
    strncpy(engine_console_log[0], line, 127);
    engine_console_log[0][127] = '\0';
}

static void reset_engine_metrics(void)
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

static int screen_to_board_sq(int r, int c)
{
    return (board_orientation == 1) ? (r * 8 + c) : ((7 - r) * 8 + (7 - c));
}

static void move_to_uci(GuiMove m, char *buf)
{
    int fc = m.from % 8, fr = 8 - (m.from / 8);
    int tc = m.to   % 8, tr = 8 - (m.to   / 8);
    sprintf(buf, "%c%d%c%d", 'a' + fc, fr, 'a' + tc, tr);
    if (m.promo) {
        char p = (m.promo == 2) ? 'n' :
                 (m.promo == 3) ? 'b' :
                 (m.promo == 4) ? 'r' : 'q';
        sprintf(buf + 4, "%c", p);
    }
}

static GuiMove uci_to_gui_move(const char *str)
{
    GuiMove m = {-1, -1, 0};
    if (strlen(str) < 4) return m;
    m.from = (8 - (str[1] - '0')) * 8 + (str[0] - 'a');
    m.to   = (8 - (str[3] - '0')) * 8 + (str[2] - 'a');
    if (strlen(str) == 5) {
        char p = str[4];
        m.promo = (p == 'n') ? 2 :
                  (p == 'b') ? 3 :
                  (p == 'r') ? 4 : 5;
    }
    return m;
}

static void push_state(const BoardState *s, GuiMove m)
{
    if (history_count < MAX_HISTORY - 1) {
        history[history_count]      = *s;
        move_history[history_count] = m;
        history_count++;
    }
}

/* ═══════════════════════════════════════════════════════════════
   GAME RULES
   ═══════════════════════════════════════════════════════════════ */

static int find_king(const BoardState *s, int color)
{
    for (int i = 0; i < 64; i++)
        if (s->board[i] == color * 6) return i;
    return -1;
}

static int is_square_attacked(const BoardState *s, int sq, int attacker)
{
    int r = sq / 8, c = sq % 8;

    static const int kr[] = {-2,-2,-1,-1, 1, 1, 2, 2};
    static const int kc[] = {-1, 1,-2, 2,-2, 2,-1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kr[i], nc = c + kc[i];
        if ((unsigned)nr < 8u && (unsigned)nc < 8u &&
            s->board[nr*8+nc] == attacker * 2) return 1;
    }

    static const int gr[] = {-1,-1,-1, 0, 0, 1, 1, 1};
    static const int gc[] = {-1, 0, 1,-1, 1,-1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + gr[i], nc = c + gc[i];
        if ((unsigned)nr < 8u && (unsigned)nc < 8u &&
            s->board[nr*8+nc] == attacker * 6) return 1;
    }

    int po = (attacker == 1) ? 1 : -1;
    for (int dc = -1; dc <= 1; dc += 2) {
        int nr = r + po, nc = c + dc;
        if ((unsigned)nr < 8u && (unsigned)nc < 8u &&
            s->board[nr*8+nc] == attacker) return 1;
    }

    static const int bdr[] = {-1,-1, 1, 1};
    static const int bdc[] = {-1, 1,-1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + bdr[i], nc = c + bdc[i];
        while ((unsigned)nr < 8u && (unsigned)nc < 8u) {
            int t = s->board[nr*8+nc];
            if (t) { if (t == attacker*3 || t == attacker*5) return 1; break; }
            nr += bdr[i]; nc += bdc[i];
        }
    }

    static const int rsr[] = {-1, 1, 0, 0};
    static const int rsc[] = { 0, 0,-1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + rsr[i], nc = c + rsc[i];
        while ((unsigned)nr < 8u && (unsigned)nc < 8u) {
            int t = s->board[nr*8+nc];
            if (t) { if (t == attacker*4 || t == attacker*5) return 1; break; }
            nr += rsr[i]; nc += rsc[i];
        }
    }
    return 0;
}

static int is_pseudo_legal_move(const BoardState *s, GuiMove m)
{
    int p      = s->board[m.from];
    int target = s->board[m.to];
    int turn   = s->turn;

    if (!p || m.from == m.to) return 0;
    if ((turn ==  1 && p < 0) || (turn == -1 && p > 0)) return 0;
    if (target && ((turn == 1 && target > 0) || (turn == -1 && target < 0))) return 0;

    int fr = m.from / 8, fc = m.from % 8;
    int tr = m.to   / 8, tc = m.to   % 8;
    int dr = tr - fr,    dc = tc - fc;
    int adr = abs(dr),   adc = abs(dc);

    switch (abs(p)) {
    case 1: {
        int dir = (turn == 1) ? -1 : 1;
        if (dc == 0 && dr == dir && !target) return 1;
        int sr2 = (turn == 1) ? 6 : 1;
        if (dc == 0 && fr == sr2 && dr == 2*dir &&
            !s->board[(fr+dir)*8+fc] && !target) return 1;
        if (adc == 1 && dr == dir && (target || m.to == s->ep)) return 1;
        return 0;
    }
    case 2:
        return (adr == 2 && adc == 1) || (adr == 1 && adc == 2);
    case 3: {
        if (adr != adc) return 0;
        int sr2 = (dr > 0) ? 1 : -1, sc2 = (dc > 0) ? 1 : -1;
        int r2 = fr + sr2, c2 = fc + sc2;
        while (r2 != tr) { if (s->board[r2*8+c2]) return 0; r2 += sr2; c2 += sc2; }
        return 1;
    }
    case 4: {
        if (dr && dc) return 0;
        int sr2 = !dr ? 0 : (dr > 0 ? 1 : -1);
        int sc2 = !dc ? 0 : (dc > 0 ? 1 : -1);
        int r2 = fr + sr2, c2 = fc + sc2;
        while (r2 != tr || c2 != tc) { if (s->board[r2*8+c2]) return 0; r2 += sr2; c2 += sc2; }
        return 1;
    }
    case 5: {
        if (adr != adc && dr && dc) return 0;
        int sr2 = !dr ? 0 : (dr > 0 ? 1 : -1);
        int sc2 = !dc ? 0 : (dc > 0 ? 1 : -1);
        if (adr == adc) { sr2 = (dr > 0) ? 1 : -1; sc2 = (dc > 0) ? 1 : -1; }
        int r2 = fr + sr2, c2 = fc + sc2;
        while (r2 != tr || c2 != tc) { if (s->board[r2*8+c2]) return 0; r2 += sr2; c2 += sc2; }
        return 1;
    }
    case 6: {
        if (adr <= 1 && adc <= 1) return 1;
        if (dr != 0 || adc != 2)  return 0;
        if (turn == 1 && fr == 7 && fc == 4) {
            if (m.to == 62 && (s->castle & 1) &&
                !s->board[61] && !s->board[62] &&
                !is_square_attacked(s,60,-1) &&
                !is_square_attacked(s,61,-1) &&
                !is_square_attacked(s,62,-1)) return 1;
            if (m.to == 58 && (s->castle & 2) &&
                !s->board[59] && !s->board[58] && !s->board[57] &&
                !is_square_attacked(s,60,-1) &&
                !is_square_attacked(s,59,-1) &&
                !is_square_attacked(s,58,-1)) return 1;
        }
        if (turn == -1 && fr == 0 && fc == 4) {
            if (m.to == 6 && (s->castle & 4) &&
                !s->board[5] && !s->board[6] &&
                !is_square_attacked(s,4,1) &&
                !is_square_attacked(s,5,1) &&
                !is_square_attacked(s,6,1)) return 1;
            if (m.to == 2 && (s->castle & 8) &&
                !s->board[3] && !s->board[2] && !s->board[1] &&
                !is_square_attacked(s,4,1) &&
                !is_square_attacked(s,3,1) &&
                !is_square_attacked(s,2,1)) return 1;
        }
        return 0;
    }
    }
    return 0;
}

static int is_legal_gui_move(const BoardState *s, GuiMove m)
{
    if (!is_pseudo_legal_move(s, m)) return 0;
    BoardState next;
    make_gui_move(s, &next, m);
    int king = find_king(&next, s->turn);
    if (king == -1) return 0;
    return !is_square_attacked(&next, king, -s->turn);
}

static int has_legal_moves(const BoardState *s)
{
    for (int f = 0; f < 64; f++) {
        if (!s->board[f]) continue;
        if ((s->turn ==  1 && s->board[f] < 0) ||
            (s->turn == -1 && s->board[f] > 0)) continue;
        for (int t = 0; t < 64; t++) {
            GuiMove m = {f, t, 0};
            if (abs(s->board[f]) == 1 && (t/8 == 0 || t/8 == 7)) m.promo = 5;
            if (is_legal_gui_move(s, m)) return 1;
        }
    }
    return 0;
}

static int count_repetitions(const BoardState *s)
{
    int count = 1;
    for (int i = 0; i < history_count; i++) {
        if (s->turn   == history[i].turn   &&
            s->castle == history[i].castle &&
            s->ep     == history[i].ep     &&
            memcmp(s->board, history[i].board, sizeof(s->board)) == 0)
            count++;
    }
    return count;
}

static void make_gui_move(const BoardState *src, BoardState *dst, GuiMove m)
{
    *dst = *src;
    int p      = dst->board[m.from];
    int is_cap = (src->board[m.to] != 0) || (abs(p) == 1 && m.to == src->ep);

    dst->board[m.from] = 0;
    dst->board[m.to]   = m.promo ? (dst->turn * m.promo) : p;

    if (abs(p) == 1 && m.to == dst->ep) {
        int pd = (dst->turn == 1) ? 8 : -8;
        dst->board[m.to + pd] = 0;
    }

    dst->ep = -1;
    if (abs(p) == 1 && abs(m.from - m.to) == 16)
        dst->ep = m.from + ((dst->turn == 1) ? -8 : 8);

    if (abs(p) == 6) {
        if      (m.from == 60 && m.to == 62) { dst->board[61] = dst->board[63]; dst->board[63] = 0; }
        else if (m.from == 60 && m.to == 58) { dst->board[59] = dst->board[56]; dst->board[56] = 0; }
        else if (m.from ==  4 && m.to ==  6) { dst->board[5]  = dst->board[7];  dst->board[7]  = 0; }
        else if (m.from ==  4 && m.to ==  2) { dst->board[3]  = dst->board[0];  dst->board[0]  = 0; }
    }

    if (m.from == 60)               dst->castle &= ~(1|2);
    if (m.from ==  4)               dst->castle &= ~(4|8);
    if (m.from == 63 || m.to == 63) dst->castle &= ~1;
    if (m.from == 56 || m.to == 56) dst->castle &= ~2;
    if (m.from ==  7 || m.to ==  7) dst->castle &= ~4;
    if (m.from ==  0 || m.to ==  0) dst->castle &= ~8;

    dst->turn = -dst->turn;
    dst->halfmoves = (abs(p) == 1 || is_cap) ? 0 : dst->halfmoves + 1;
    if (dst->turn == 1) dst->fullmoves++;
}

/* ═══════════════════════════════════════════════════════════════
   PGN NOTATION
   ═══════════════════════════════════════════════════════════════ */

static void move_to_pgn(const BoardState *s, GuiMove m, char *buf)
{
    int p  = s->board[m.from];
    int ap = abs(p);
    int is_cap = (s->board[m.to] != 0) || (ap == 1 && m.to == s->ep);

    if (ap == 6 && abs(m.from - m.to) == 2) {
        strcpy(buf, (m.to > m.from) ? "O-O" : "O-O-O");
    } else {
        char *ptr = buf;
        if (ap == 1) {
            if (is_cap) { *ptr++ = 'a' + (m.from % 8); *ptr++ = 'x'; }
        } else {
            *ptr++ = (ap==2)?'N':(ap==3)?'B':(ap==4)?'R':(ap==5)?'Q':'K';
            if (ap >= 2 && ap <= 5) {
                int fc = 0, rc = 0, ce = 0;
                for (int sq = 0; sq < 64; sq++) {
                    if (sq == m.from || s->board[sq] != p) continue;
                    GuiMove t2 = {sq, m.to, 0};
                    if (is_legal_gui_move(s, t2)) {
                        ce = 1;
                        if (sq % 8 == m.from % 8) fc = 1;
                        if (sq / 8 == m.from / 8) rc = 1;
                    }
                }
                if (ce) {
                    if      (!fc) *ptr++ = 'a' + (m.from % 8);
                    else if (!rc) *ptr++ = '8' - (m.from / 8);
                    else        { *ptr++ = 'a' + (m.from % 8);
                                  *ptr++ = '8' - (m.from / 8); }
                }
            }
            if (is_cap) *ptr++ = 'x';
        }
        *ptr++ = 'a' + (m.to % 8);
        *ptr++ = '8' - (m.to / 8);
        if (ap == 1 && m.promo) {
            *ptr++ = '=';
            *ptr++ = (m.promo==5)?'Q':(m.promo==4)?'R':(m.promo==3)?'B':'N';
        }
        *ptr = '\0';
    }

    BoardState next;
    make_gui_move(s, &next, m);
    int ok = find_king(&next, next.turn);
    if (ok != -1 && is_square_attacked(&next, ok, -next.turn))
        strcat(buf, has_legal_moves(&next) ? "+" : "#");
}

/* ═══════════════════════════════════════════════════════════════
   BOARD INIT
   ═══════════════════════════════════════════════════════════════ */

static void init_board(BoardState *s)
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
    memcpy(s->board, start, sizeof(start));
    s->turn      =  1;
    s->castle    = 15;
    s->ep        = -1;
    s->halfmoves =  0;
    s->fullmoves =  1;
}

/* ═══════════════════════════════════════════════════════════════
   ENGINE TRIGGER
   ═══════════════════════════════════════════════════════════════ */

static void trigger_engine_move(void)
{
    reset_engine_metrics();

    static char cmd[CMD_BUF_SIZE];
    strcpy(cmd, "position startpos moves");
    int len = (int)strlen(cmd);

    for (int i = 0; i < history_count; i++) {
        char um[10];
        move_to_uci(move_history[i], um);
        int ml = (int)strlen(um);
        if (len + 1 + ml + 2 >= (int)sizeof(cmd)) break;
        cmd[len++] = ' ';
        strcpy(cmd + len, um);
        len += ml;
    }
    cmd[len++] = '\n';
    cmd[len]   = '\0';
    send_to_engine(cmd);

    char go[256];
    if      (time_control_type == 0) sprintf(go, "go movetime %d\n", time_control_val);
    else if (time_control_type == 1) sprintf(go, "go depth %d\n",    time_control_val);
    else                             sprintf(go, "go nodes %d\n",    time_control_val);
    send_to_engine(go);
}

/* ═══════════════════════════════════════════════════════════════
   CONTROLLER INPUT
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    int up, down, left, right;
    int select;
    int back;
    int undo;
    int reset;
    int flip;
    int sides;
    int ttype;
    int tval;
    int quit;
} InputState;

#define VBTN_UP     0
#define VBTN_DOWN   1
#define VBTN_LEFT   2
#define VBTN_RIGHT  3
#define VBTN_SELECT 4
#define VBTN_BACK   5
#define VBTN_COUNT  6

static u32 btn_held_ticks[VBTN_COUNT];

static int vbtn_fired(int idx, int held)
{
    if (!held) { btn_held_ticks[idx] = 0; return 0; }
    btn_held_ticks[idx]++;
    if (btn_held_ticks[idx] == 1) return 1;
    if (btn_held_ticks[idx] > INPUT_INITIAL_DELAY &&
        (btn_held_ticks[idx] - INPUT_INITIAL_DELAY) % INPUT_REPEAT_DELAY == 0)
        return 1;
    return 0;
}

static InputState read_input(void)
{
    InputState in;
    memset(&in, 0, sizeof(in));

    WPAD_ScanPads();
    PAD_ScanPads();

    u32 w_held = WPAD_ButtonsHeld(0);
    u32 w_down = WPAD_ButtonsDown(0);
    u32 g_held = PAD_ButtonsHeld(0);
    u32 g_down = PAD_ButtonsDown(0);

    WPADData *wd = WPAD_Data(0);
    bool has_cl  = (wd && wd->exp.type == WPAD_EXP_CLASSIC);
    u32 cl_held  = has_cl ? wd->exp.classic.btns_held : 0;

    int raw_up    = (w_held & WPAD_BUTTON_UP)    || (g_held & PAD_BUTTON_UP)    || (cl_held & CLASSIC_BUTTON_UP);
    int raw_down  = (w_held & WPAD_BUTTON_DOWN)  || (g_held & PAD_BUTTON_DOWN)  || (cl_held & CLASSIC_BUTTON_DOWN);
    int raw_left  = (w_held & WPAD_BUTTON_LEFT)  || (g_held & PAD_BUTTON_LEFT)  || (cl_held & CLASSIC_BUTTON_LEFT);
    int raw_right = (w_held & WPAD_BUTTON_RIGHT) || (g_held & PAD_BUTTON_RIGHT) || (cl_held & CLASSIC_BUTTON_RIGHT);

    s8 gx = PAD_StickX(0), gy = PAD_StickY(0);
    if (gx < -40) raw_left  = 1;
    if (gx >  40) raw_right = 1;
    if (gy >  40) raw_up    = 1;
    if (gy < -40) raw_down  = 1;

    in.up    = vbtn_fired(VBTN_UP,    raw_up);
    in.down  = vbtn_fired(VBTN_DOWN,  raw_down);
    in.left  = vbtn_fired(VBTN_LEFT,  raw_left);
    in.right = vbtn_fired(VBTN_RIGHT, raw_right);

    in.select = vbtn_fired(VBTN_SELECT,
                    (w_down & WPAD_BUTTON_A) || (g_down & PAD_BUTTON_A) ||
                    (has_cl && (cl_held & CLASSIC_BUTTON_A)));
    in.back   = vbtn_fired(VBTN_BACK,
                    (w_down & WPAD_BUTTON_B) || (g_down & PAD_BUTTON_B) ||
                    (has_cl && (cl_held & CLASSIC_BUTTON_B)));

    in.undo  = ((w_down & WPAD_BUTTON_MINUS) || (g_down & PAD_TRIGGER_Z))  ? 1 : 0;
    in.reset = (w_down & WPAD_BUTTON_HOME)                                   ? 1 : 0;
    in.flip  = (w_down & WPAD_BUTTON_1)                                      ? 1 : 0;
    in.sides = (w_down & WPAD_BUTTON_PLUS)                                   ? 1 : 0;
    in.ttype = (g_down & PAD_BUTTON_START)                                   ? 1 : 0;
    in.tval  = (g_down & PAD_BUTTON_Y)                                       ? 1 : 0;
    in.quit  = (g_down & PAD_BUTTON_X)                                       ? 1 : 0;

    return in;
}

/* ═══════════════════════════════════════════════════════════════
   PROMOTION CHOICE  (blocking controller poll)
   ═══════════════════════════════════════════════════════════════ */

static int get_promo_choice(void)
{
    printf("\n\n  PROMOTE PAWN:\n");
    printf("   UP=Queen  DOWN=Rook  LEFT=Bishop  RIGHT=Knight\n");
    fflush(stdout);

    for (;;) {
        VIDEO_WaitVSync();
        WPAD_ScanPads();
        PAD_ScanPads();
        u32 wd = WPAD_ButtonsDown(0);
        u32 gd = PAD_ButtonsDown(0);
        if ((wd & WPAD_BUTTON_UP)    || (gd & PAD_BUTTON_UP))    return 5;
        if ((wd & WPAD_BUTTON_DOWN)  || (gd & PAD_BUTTON_DOWN))  return 4;
        if ((wd & WPAD_BUTTON_LEFT)  || (gd & PAD_BUTTON_LEFT))  return 3;
        if ((wd & WPAD_BUTTON_RIGHT) || (gd & PAD_BUTTON_RIGHT)) return 2;
        if ((wd & WPAD_BUTTON_A)     || (gd & PAD_BUTTON_A))     return 5;
    }
}

/* ═══════════════════════════════════════════════════════════════
   DISPLAY  (plain ASCII, Wii CON_InitEx console)
   ═══════════════════════════════════════════════════════════════ */

static const char *piece_to_char(int p)
{
    switch (p) {
    case  6: return "K"; case  5: return "Q"; case  4: return "R";
    case  3: return "B"; case  2: return "N"; case  1: return "P";
    case -1: return "p"; case -2: return "n"; case -3: return "b";
    case -4: return "r"; case -5: return "q"; case -6: return "k";
    default: return ".";
    }
}

static void draw_ui(void)
{
    printf("\033[H\033[2J");

    /* Status */
    {
        int king   = find_king(&current_state, current_state.turn);
        int is_ch  = (king != -1) && is_square_attacked(&current_state, king, -current_state.turn);
        int has_mv = has_legal_moves(&current_state);
        int reps   = count_repetitions(&current_state);
        const char *tn = (current_state.turn == 1) ? "White" : "Black";

        if      (current_state.halfmoves >= 100) printf("** DRAW (50-move rule) **\n");
        else if (reps >= 3)                       printf("** DRAW (threefold repetition) **\n");
        else if (!has_mv && is_ch)                printf("** CHECKMATE! **\n");
        else if (!has_mv)                         printf("** STALEMATE! **\n");
        else if (is_ch)                           printf("%s to move  CHECK!\n", tn);
        else                                      printf("%s to move\n", tn);
    }

    {
        const char *wp       = (user_side ==  1 || user_side == 0) ? "Human" : "Eng";
        const char *bp       = (user_side == -1 || user_side == 0) ? "Human" : "Eng";
        const char *tc_names[] = {"Time(ms)", "Depth", "Nodes"};
        printf("W:%s B:%s | %s:%d\n\n",
               wp, bp, tc_names[time_control_type], time_control_val);
    }

    /* Column labels */
    printf("    ");
    for (int c = 0; c < 8; c++) {
        int col = (board_orientation == 1) ? c : (7 - c);
        printf("  %c ", 'a' + col);
    }
    printf("\n");

    /* Check square */
    int in_check_sq = -1;
    {
        int wk = find_king(&current_state,  1);
        int bk = find_king(&current_state, -1);
        if (wk != -1 && is_square_attacked(&current_state, wk, -1)) in_check_sq = wk;
        if (bk != -1 && is_square_attacked(&current_state, bk,  1)) in_check_sq = bk;
    }

    /* Board rows */
    for (int r = 0; r < 8; r++) {
        int rank_lbl = (board_orientation == 1) ? (8 - r) : (r + 1);
        printf("  %d ", rank_lbl);

        for (int c = 0; c < 8; c++) {
            int sq  = screen_to_board_sq(r, c);
            int p   = current_state.board[sq];
            int isc = (r == cursor_r && c == cursor_c);
            int iss = (sq == selected_sq);

            int is_legal_dst = 0;
            if (selected_sq != -1) {
                GuiMove tm = {selected_sq, sq, 0};
                if (abs(current_state.board[selected_sq]) == 1 &&
                    (sq/8 == 0 || sq/8 == 7)) tm.promo = 5;
                if (is_legal_gui_move(&current_state, tm)) is_legal_dst = 1;
            }

            int is_prev = 0;
            if (history_count > 0) {
                GuiMove lm = move_history[history_count - 1];
                if (sq == lm.from || sq == lm.to) is_prev = 1;
            }

            const char *pc = piece_to_char(p);
            char cell[6];
            if      (isc && iss)          snprintf(cell, sizeof(cell), "(%s)", pc);
            else if (isc)                 snprintf(cell, sizeof(cell), "[%s]", pc);
            else if (iss)                 snprintf(cell, sizeof(cell), "{%s}", pc);
            else if (sq == in_check_sq)   snprintf(cell, sizeof(cell), "!%s!", pc);
            else if (is_legal_dst && p)   snprintf(cell, sizeof(cell), "*%s*", pc);
            else if (is_legal_dst)        snprintf(cell, sizeof(cell), " .  ");
            else if (is_prev)             snprintf(cell, sizeof(cell), "<%s>", pc);
            else                          snprintf(cell, sizeof(cell), " %s  ", pc);

            printf("%-4s", cell);
        }
        printf(" %d\n", rank_lbl);
    }

    /* Column footer */
    printf("    ");
    for (int c = 0; c < 8; c++) {
        int col = (board_orientation == 1) ? c : (7 - c);
        printf("  %c ", 'a' + col);
    }
    printf("\n\n");

    /* Controls */
    printf("D-Pad:Move  A:Select  B:Desel  -:Undo  HOME:Reset\n");
    printf("+:Sides  1:Flip  GC-Strt:TcType  GC-Y:TcVal  GC-X:Quit\n\n");

    /* Engine status */
    if (engine_alive) {
        if      (engine_recvd_readyok) printf("Engine: Ready");
        else if (engine_recvd_uciok)   printf("Engine: Connected");
        else                           printf("Engine: Connecting...");

        if (engine_thinking) {
            if (engine_nps)      printf("  Nps:%lld",   engine_nps);
            if (engine_depth)    printf("  D:%d/%d",    engine_depth, engine_seldepth);
            if (engine_nodes)    printf("  N:%lld",     engine_nodes);
            if (engine_time_ms)  printf("  T:%.1fs",    (double)engine_time_ms / 1000.0);
            if (engine_hashfull) printf("  H:%.1f%%",   (double)engine_hashfull / 10.0);
            if (engine_tbhits)   printf("  TB:%lld",    engine_tbhits);
        }
        if (engine_score_type == 0) printf("  Eval:%+.2f", (double)engine_score_val / 100.0);
        if (engine_score_type == 1) printf("  Mate:%d",    engine_score_val);
        if (!engine_thinking)       printf("  Idle");
        printf("\n");

        for (int i = 2; i >= 0; i--)
            if (engine_console_log[i][0])
                printf("  %s\n", engine_console_log[i]);

        if (engine_pv[0]) {
            char pv_short[60];
            strncpy(pv_short, engine_pv, 59);
            pv_short[59] = '\0';
            printf("  PV(d%d): %s\n", engine_depth, pv_short);
        }
    } else {
        printf("Engine: Offline\n");
    }

    /* Move list */
    printf("\n  Moves:\n");
    int total = (history_count + 1) / 2;
    int mstart = (total > 6) ? (total - 5) : 1;
    for (int mv = mstart; mv <= total; mv++) {
        int wi = (mv - 1) * 2, bi = wi + 1;
        char ws[16] = "---", bs[16] = "---";
        if (wi < history_count) move_to_pgn(&history[wi], move_history[wi], ws);
        if (bi < history_count) move_to_pgn(&history[bi], move_history[bi], bs);
        printf("  %2d. %-7s %-7s\n", mv, ws, bs);
    }

    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════
   ACTION HANDLERS
   ═══════════════════════════════════════════════════════════════ */

static void handle_select(void)
{
    if (!has_legal_moves(&current_state) ||
        current_state.halfmoves >= 100   ||
        count_repetitions(&current_state) >= 3) return;

    int sq = screen_to_board_sq(cursor_r, cursor_c);

    if (selected_sq == -1) {
        int p = current_state.board[sq];
        if (p && ((current_state.turn ==  1 && p > 0) ||
                  (current_state.turn == -1 && p < 0)))
            selected_sq = sq;
    } else {
        GuiMove m      = {selected_sq, sq, 0};
        int p          = current_state.board[selected_sq];
        int is_promo   = (abs(p) == 1 && (sq/8 == 0 || sq/8 == 7));
        if (is_promo) m.promo = 5;

        if (is_legal_gui_move(&current_state, m)) {
            if (is_promo) m.promo = get_promo_choice();
            push_state(&current_state, m);
            BoardState next;
            make_gui_move(&current_state, &next, m);
            current_state = next;
            selected_sq   = -1;
            reset_engine_metrics();
        } else {
            int t = current_state.board[sq];
            selected_sq =
                (t && ((current_state.turn ==  1 && t > 0) ||
                       (current_state.turn == -1 && t < 0)))
                ? sq : -1;
        }
    }
}

static void handle_undo(void)
{
    if (engine_thinking) { send_to_engine("stop\n"); engine_thinking = 0; }
    reset_engine_metrics();
    int step = (user_side == 1 || user_side == -1) ? 2 : 1;
    while (step-- > 0 && history_count > 0) {
        history_count--;
        current_state = history[history_count];
    }
    selected_sq = -1;
}

static void handle_reset_board(void)
{
    if (engine_thinking) { send_to_engine("stop\n"); engine_thinking = 0; }
    reset_engine_metrics();
    init_board(&current_state);
    history_count = 0;
    selected_sq   = -1;
    cursor_r      = 6;
    cursor_c      = 4;
    if (engine_alive) send_to_engine("ucinewgame\nisready\n");
}

static void handle_switch_sides(void)
{
    if (engine_thinking) { send_to_engine("stop\n"); engine_thinking = 0; }
    if      (user_side ==  1) user_side = -1;
    else if (user_side == -1) user_side =  0;
    else if (user_side ==  0) user_side =  2;
    else                      user_side =  1;
}

static void adjust_time_control(void)
{
    if (time_control_type == 0) {
        static const int tl[] = {1,10,50,100,500,1000,1500,2000,3000,5000,10000};
        int n = (int)(sizeof(tl)/sizeof(tl[0])), fi = -1;
        for (int i = 0; i < n; i++) if (time_control_val == tl[i]) { fi = i; break; }
        time_control_val = (fi < 0 || fi == n-1) ? tl[0] : tl[fi+1];
    } else if (time_control_type == 1) {
        time_control_val = (time_control_val % 20) + 1;
    } else {
        static const int nl[] = {512,1024,2048,4096,8192,16384,32768,65536,131072,262144,524288,1048576};
        int n = (int)(sizeof(nl)/sizeof(nl[0])), fi = -1;
        for (int i = 0; i < n; i++) if (time_control_val == nl[i]) { fi = i; break; }
        time_control_val = (fi < 0 || fi == n-1) ? nl[0] : nl[fi+1];
    }
}

static void handle_input_state(const InputState *in)
{
    if (in->up    && cursor_r > 0) cursor_r--;
    if (in->down  && cursor_r < 7) cursor_r++;
    if (in->left  && cursor_c > 0) cursor_c--;
    if (in->right && cursor_c < 7) cursor_c++;
    if (in->select) handle_select();
    if (in->back)   selected_sq = -1;
    if (in->undo)   handle_undo();
    if (in->reset)  handle_reset_board();
    if (in->flip)   board_orientation = -board_orientation;
    if (in->sides)  handle_switch_sides();
    if (in->ttype) {
        time_control_type = (time_control_type + 1) % 3;
        time_control_val  = (time_control_type == 0) ? 100 :
                            (time_control_type == 1) ?   1 : 512;
    }
    if (in->tval) adjust_time_control();
    if (in->quit) {
        if (engine_thinking) send_to_engine("stop\n");
        /* Signal engine thread to terminate */
        engine_thread_alive = false;
        KThrQueueUnblockOneByValue(&cmd_waiters, CMD_TOKEN);
        /* Wait for engine to finish before exit */
        KThreadJoin(&engine_thread);
        exit(0);
    }
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* ── Wii video init ── */
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb   = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    /* ── Console on framebuffer ── */
    CON_InitEx(rmode, 10, 10, rmode->fbWidth - 20, rmode->xfbHeight - 20);

    /* ── Controller init ── */
    WPAD_Init();
    PAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);

    /* ── Tuxedo threading init ──
       KThreadInit() sets up the main thread and idle thread.
       Must be called once before any other KThread function.     */
    KThreadInit();

    /* ── Game state ── */
    init_board(&current_state);
    memset(btn_held_ticks, 0, sizeof(btn_held_ticks));

    /* ── Start engine thread ── */
    start_engine();

    /* ── Main loop ── */
    for (;;) {
        /*
         * VIDEO_WaitVSync provides ~16 ms pacing.
         * While the main thread is blocked here the scheduler
         * will run the engine thread if it has work to do.
         */
        VIDEO_WaitVSync();

        /*
         * Explicitly yield so the engine thread can run even
         * when we return from VSync quickly.
         */
        KThreadYield();

        /* Pull any engine output lines */
        read_from_engine();

        /* Trigger engine calculation if needed */
        {
            int game_active =
                has_legal_moves(&current_state)         &&
                current_state.halfmoves < 100           &&
                count_repetitions(&current_state) < 3;

            int should_think =
                game_active      &&
                engine_alive     &&
                !engine_thinking &&
                ((user_side ==  2)                               ||
                 (user_side ==  1 && current_state.turn == -1)  ||
                 (user_side == -1 && current_state.turn ==  1));

            if (should_think) {
                engine_thinking = 1;
                trigger_engine_move();
            }
        }

        /* Handle controller input */
        {
            InputState in = read_input();
            handle_input_state(&in);
        }

        /* Redraw */
        draw_ui();
    }

    return 0;
}
