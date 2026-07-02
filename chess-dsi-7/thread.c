/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2017 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

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

#include "thread.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/reent.h>
#include <calico/system/thread.h> // Correct path to Calico preemptive thread API
#include <nds.h>

#include "material.h"
#include "movegen.h"
#include "movepick.h"
#include "numa.h"
#include "pawns.h"
#include "search.h"
#include "settings.h"
#include "tt.h"
#include "uci.h"
#include "tbprobe.h"

/* ============================================================================
   PART 1: 3DS/DS BRIDGE IMPLEMENTATION (Formerly 3ds_bridge.c)
   ============================================================================ */

#define MSG_QUEUE_SIZE 65536u        // must stay a power of two
#define MSG_QUEUE_MASK (MSG_QUEUE_SIZE - 1u)

// Lock-free single-producer/single-consumer ring buffer.
//
// BUGFIX: previously guarded by a LightLock spinlock. On Calico's
// non-time-sliced-between-equal-priority scheduler, a spinlock is only
// safe if a thread that fails to acquire it is guaranteed to eventually
// yield to whoever holds it. Nothing guaranteed that here: if the GUI
// thread began spinning on q_engine_to_gui.lock at the exact moment the
// engine thread was holding it (e.g. interrupted mid-sf_printf() during
// uci_loop()'s startup banner), the GUI thread would spin forever without
// ever yielding, so the engine thread holding the lock could never be
// rescheduled to finish and release it - a permanent deadlock. This
// matches exactly the same root-cause category already documented and
// fixed once in this file for thread_idle_loop().
//
// Fix: each queue has exactly one producer and one consumer, so no lock is
// needed at all - a head/tail counter pair with memory barriers (the same
// __sync_synchronize() idiom already used throughout this file) is
// sufficient and cannot deadlock.
typedef struct {
    char data[MSG_QUEUE_SIZE];
    volatile uint32_t head; // producer-owned: total bytes ever written
    volatile uint32_t tail; // consumer-owned: total bytes ever read
} MsgQueue;

static MsgQueue q_gui_to_engine;   // producer: GUI thread    | consumer: engine/UCI thread
static MsgQueue q_engine_to_gui;   // producer: engine thread | consumer: GUI thread

// SAFETY NOTE: this queue is safe to use with a blocking
// threadBlock()/threadUnblockAllByValue() pair specifically because
// sf_recv_command()'s checker (the UCI command thread, MAIN_THREAD_PRIO)
// and sf_send_command()'s caller (the GUI thread, also MAIN_THREAD_PRIO)
// run at EQUAL priority, and Calico does not time-slice equal-priority
// threads. That means the GUI thread cannot preempt the UCI thread in the
// narrow gap between its "queue is empty" check and its threadBlock()
// call - it can only run once the UCI thread itself actually blocks. See
// the large comment on thread_idle_loop() below for the case where this
// reasoning does NOT hold and caused a real, reproducible permanent hang.
static ThrListNode s_guiToEngineQueue;

// Stack and Thread-Local Storage Configuration for Calico
#define SF_THREAD_STACK_SIZE (64 * 1024) // 64 KB Stack (safe for heavy chess evaluation)

static Thread s_searchThread;
static void* s_searchThreadStack = NULL;
static void* s_searchThreadTls = NULL;
static volatile bool s_searchThreadActive = false;
static void* (*s_startRoutine)(void*) = NULL;
static void* s_threadArg = NULL;

// Safely execute interrupt manipulations in ARM mode to prevent Thumb assembly syntax errors
__attribute__((target("arm"), noinline)) u32 ds_disable_interrupts(void) {
    u32 old;
    __asm__ volatile(
        "mrs %0, cpsr\n\t"
        "orr r12, %0, #0x80\n\t"
        "msr cpsr_c, r12" 
        : "=r"(old) 
        : 
        : "r12", "cc", "memory"
    );
    return old;
}

__attribute__((target("arm"), noinline)) void ds_restore_interrupts(u32 old) {
    __asm__ volatile(
        "msr cpsr_c, %0" 
        : 
        : "r"(old) 
        : "cc", "memory"
    );
}

// Adapt standard routine void* return to Calico's int return signature
static int calico_thread_entry(void* arg) {
    (void)arg;
    if (s_startRoutine) {
        s_startRoutine(s_threadArg);
    }
    s_searchThreadActive = false;
    return 0;
}

// Yield CPU time
void ds_yield(void) {
    threadYield();
}

// ============================================================================
// CRITICAL FIX: newlib malloc/calloc/realloc/free/memalign thread-safety.
//
// newlib's allocator is NOT thread-safe by default - it requires the
// platform to supply __malloc_lock()/__malloc_unlock() hooks, which nothing
// in this codebase was providing. Calico is a genuinely preemptive
// scheduler (a thread can be interrupted at an arbitrary instruction, e.g.
// on a VBlank IRQ), so without these hooks, two threads calling into malloc
// concurrently could corrupt the allocator's internal free-list. This does
// not crash immediately - it silently sits there until a LATER, completely
// unrelated allocation's free-list walk hits the corrupted region and spins
// forever. Because the engine's UCI thread runs at the SAME priority as the
// GUI thread (MAIN_THREAD_PRIO) and Calico does not time-slice
// equal-priority threads (see calico/system/thread.h's own module
// documentation), a stuck non-yielding loop in EITHER thread permanently
// starves the other.
//
// Fix: since the DSi's ARM9 is single-core, Calico can only ever preempt a
// running thread via an interrupt. Disabling IRQs for the (very short)
// duration of a malloc-family call is therefore sufficient to make it
// atomic with respect to every other thread, regardless of relative thread
// priority - no additional scheduler-aware lock primitive is needed.
// Depth-counted, and safe against same-thread re-entrancy (e.g. memalign()
// internally calling malloc() on some newlib builds), unlike a plain
// spinlock, since disabling already-disabled interrupts is a harmless
// no-op.
// ============================================================================
static u32 s_mallocLockSavedCPSR = 0;
static int s_mallocLockDepth = 0;

// Diagnostic-only: globally-numbered debug print, safe to call from any
// thread at any time, writing straight to the bottom screen (bypasses the
// engine<->GUI queue entirely). The sequence number lets us tell exactly
// which call happened last, regardless of which thread printed it or how
// the console scrolled.
extern PrintConsole bottomConsole;
static volatile int s_dbgSeq = 0;

void sf_dbg(const char *msg) {
    int n = ++s_dbgSeq;
    consoleSelect(&bottomConsole);
    printf("[%d] %s\n", n, msg);
    fflush(stdout);
}

void __malloc_lock(struct _reent *reent) {
    (void)reent;
    sf_dbg("malloc_lock enter");
    u32 old = ds_disable_interrupts();
    if (s_mallocLockDepth == 0) {
        s_mallocLockSavedCPSR = old;
    }
    s_mallocLockDepth++;
    sf_dbg("malloc_lock exit");
}

void __malloc_unlock(struct _reent *reent) {
    (void)reent;
    sf_dbg("malloc_unlock enter");
    s_mallocLockDepth--;
    if (s_mallocLockDepth == 0) {
        ds_restore_interrupts(s_mallocLockSavedCPSR);
    }
    sf_dbg("malloc_unlock exit");
}

// Undefine target library macros to avoid infinite loops inside overrides
#undef printf
#undef fprintf
#undef vfprintf
#undef fflush
#undef puts
#undef fputs
#undef putchar
#undef fputc
#undef putc
#undef fwrite
#undef pthread_create

void sf_bridge_init(void) {
    q_gui_to_engine.head = q_gui_to_engine.tail = 0;
    q_engine_to_gui.head = q_engine_to_gui.tail = 0;

    s_searchThreadActive = false;
    // s_guiToEngineQueue is a static ThrListNode and is therefore
    // zero-initialized by the BSS segment at program start, matching the
    // "empty queue" representation used elsewhere (see Thread's own
    // zero-initialized 'waiters' field).
}

// Producer-side push. Never touches 'tail' (that belongs solely to the
// consumer) - if the queue is completely full we simply drop the
// character rather than overwrite, to preserve strict single-writer-per-
// index safety. In practice, at 64KB and drained every GUI frame, this
// should never actually happen.
static inline void queue_push_char(MsgQueue *q, char c) {
    uint32_t head = q->head;
    uint32_t tail = q->tail;
    if ((head - tail) >= MSG_QUEUE_SIZE) {
        return; // full - drop rather than corrupt consumer-owned state
    }
    q->data[head & MSG_QUEUE_MASK] = c;
    __sync_synchronize();      // publish the byte before advertising it
    q->head = head + 1;
}

void sf_send_command(const char *cmd) {
    for (int i = 0; cmd[i] != '\0'; i++) {
        queue_push_char(&q_gui_to_engine, cmd[i]);
    }
    queue_push_char(&q_gui_to_engine, '\n');

    // Safe: see s_guiToEngineQueue's declaration comment above. GUI thread
    // and the UCI command thread are equal priority, so this cannot race
    // against sf_recv_command()'s check-then-block sequence.
    threadUnblockAllByValue(&s_guiToEngineQueue, 0);
}

void sf_recv_command(char *buf, size_t max_len) {
    while (1) {
        uint32_t head = q_gui_to_engine.head;
        uint32_t tail = q_gui_to_engine.tail; // owned by us (consumer)

        if (head != tail) {
            int has_newline = 0;
            uint32_t scan = tail;
            while (scan != head) {
                if (q_gui_to_engine.data[scan & MSG_QUEUE_MASK] == '\n') {
                    has_newline = 1;
                    break;
                }
                scan++;
            }

            if (has_newline) {
                size_t i = 0;
                int truncated = 0;

                while (tail != head) {
                    char c = q_gui_to_engine.data[tail & MSG_QUEUE_MASK];
                    tail++;
                    if (c == '\n') break;
                    if (i < max_len - 1) buf[i++] = c;
                    else truncated = 1;
                }
                buf[i] = '\0';

                __sync_synchronize();
                q_gui_to_engine.tail = tail;

                if (truncated) {
                    sf_printf("[BRIDGE_WARNING] Command exceeded buffer size and was safely truncated!\n");
                }
                return;
            }
        }

        // Safe (equal-priority checker/setter - see notes above).
        threadBlock(&s_guiToEngineQueue, 0);
    }
}

int sf_printf(const char *format, ...) {
    char temp_buf[4096];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(temp_buf, sizeof(temp_buf), format, args);
    va_end(args);

    for (int i = 0; temp_buf[i] != '\0'; i++) {
        queue_push_char(&q_engine_to_gui, temp_buf[i]);
    }

    ds_yield(); // Give GUI immediate priority to read this buffer
    return written;
}

int sf_fprintf(FILE *stream, const char *format, ...) {
    if (stream == stdout || stream == stderr) {
        char temp_buf[4096];
        va_list args;
        va_start(args, format);
        int written = vsnprintf(temp_buf, sizeof(temp_buf), format, args);
        va_end(args);

        for (int i = 0; temp_buf[i] != '\0'; i++) {
            queue_push_char(&q_engine_to_gui, temp_buf[i]);
        }

        ds_yield();
        return written;
    }
    va_list args;
    va_start(args, format);
    int written = vfprintf(stream, format, args);
    va_end(args);
    return written;
}

int sf_vfprintf(FILE *stream, const char *format, va_list arg) {
    if (stream == stdout || stream == stderr) {
        char temp_buf[4096];
        int written = vsnprintf(temp_buf, sizeof(temp_buf), format, arg);

        for (int i = 0; temp_buf[i] != '\0'; i++) {
            queue_push_char(&q_engine_to_gui, temp_buf[i]);
        }

        ds_yield();
        return written;
    }
    return vfprintf(stream, format, arg);
}

int sf_fflush(FILE *stream) {
    if (stream == stdout || stream == stderr) {
        return 0;
    }
    return fflush(stream);
}

int sf_puts(const char *str) {
    int i = 0;
    for (; str[i] != '\0'; i++) {
        queue_push_char(&q_engine_to_gui, str[i]);
    }
    queue_push_char(&q_engine_to_gui, '\n');

    ds_yield();
    return i + 1;
}

int sf_fputs(const char *str, FILE *stream) {
    if (stream == stdout || stream == stderr) {
        return sf_puts(str);
    }
    return fputs(str, stream);
}

int sf_putchar(int character) {
    queue_push_char(&q_engine_to_gui, (char)character);
    return character;
}

int sf_fputc(int character, FILE *stream) {
    if (stream == stdout || stream == stderr) {
        return sf_putchar(character);
    }
    return fputc(character, stream);
}

int sf_putc(int character, FILE *stream) {
    if (stream == stdout || stream == stderr) {
        return sf_putchar(character);
    }
    return putc(character, stream);
}

size_t sf_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t total_bytes = size * nmemb;
    const char *char_ptr = (const char *)ptr;

    for (size_t i = 0; i < total_bytes; i++) {
        queue_push_char(&q_engine_to_gui, char_ptr[i]);
    }
    return nmemb;
}

int sf_get_output(char *buf, size_t max_len) {
    ds_yield(); // Give engine thread a turn to push outputs

    uint32_t head = q_engine_to_gui.head;
    uint32_t tail = q_engine_to_gui.tail; // owned by us (consumer)

    if (head == tail) return 0;

    size_t i = 0;
    while (tail != head && i < max_len - 1) {
        buf[i++] = q_engine_to_gui.data[tail & MSG_QUEUE_MASK];
        tail++;
    }
    buf[i] = '\0';

    __sync_synchronize();
    q_engine_to_gui.tail = tail;

    return (int)i;
}

// Preemptive Thread Spawner mapping POSIX definitions to libcalico
int sf_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                      void *(*start_routine) (void *), void *arg) {
    (void)attr;

    // Recursive thread guard (matches original design for single search engines)
    if (s_searchThreadActive) {
        if (thread != NULL) {
            *thread = (pthread_t)999;
        }
        return 0; 
    }

    // Allocate Stack and TLS on the first creation run to prevent runtime fragmentation
    if (!s_searchThreadStack) {
        s_searchThreadStack = malloc(SF_THREAD_STACK_SIZE);
        if (!s_searchThreadStack) {
            return -1;
        }

        size_t tls_size = threadGetLocalStorageSize();
        if (tls_size > 0) {
            s_searchThreadTls = malloc(tls_size);
            if (!s_searchThreadTls) {
                free(s_searchThreadStack);
                s_searchThreadStack = NULL;
                return -1;
            }
        }
    }

    s_startRoutine = start_routine;
    s_threadArg = arg;
    s_searchThreadActive = true;

    // Align stack base boundary up 8-bytes
    uintptr_t stack_base = (uintptr_t)s_searchThreadStack;
    uintptr_t stack_top_aligned = (stack_base + SF_THREAD_STACK_SIZE) & ~7;

    // Setup Calico thread metadata
    threadPrepare(&s_searchThread, 
                  calico_thread_entry, 
                  NULL, 
                  (void*)stack_top_aligned, 
                  MAIN_THREAD_PRIO);

    // Register C stdlib Thread-Local Storage pointer (essential to prevent crashes in sub-threads)
    if (s_searchThreadTls) {
        threadAttachLocalStorage(&s_searchThread, s_searchThreadTls);
    }

    // Launch execution context in the Calico scheduler
    threadStart(&s_searchThread);

    if (thread != NULL) {
        *thread = (pthread_t)&s_searchThread;
    }

    return 0;
}


/* ============================================================================
   PART 2: ENGINE THREADING IMPLEMENTATION (Formerly thread.c)
   ============================================================================ */

static void thread_idle_loop(Position *pos);

// Global objects
ThreadPool Threads;
MainThread mainThread;
CounterMoveHistoryStat **cmhTables = NULL;
int numCmhTables = 0;

// Track native Calico thread allocations
static Thread* s_calicoThreads[MAX_THREADS] = { NULL };
static void* s_threadStacks[MAX_THREADS] = { NULL };
static void* s_threadTls[MAX_THREADS] = { NULL };

// ==========================================
// WAIT QUEUES - see the important safety note on thread_idle_loop() below
// before adding any new blocking wait against a lower-priority thread.
// ==========================================
// s_sleepQueue[idx]: other threads block here in thread_wait_until_sleeping()
//                    waiting for worker 'idx' to finish and return to sleep.
//                    SAFE: checker priority (MAIN_THREAD_PRIO) is HIGHER
//                    than the worker (0x3D+idx) that eventually sets the
//                    condition and wakes this queue, so the worker can never
//                    preempt the checker mid check-then-block.
// s_initQueue:       the creating thread blocks here in thread_create(),
//                    woken once thread_init() finishes its one-time setup.
//                    SAFE for the same reason (checker is MAIN_THREAD_PRIO,
//                    setter is the new worker thread at a lower priority).
//
// NOTE: there used to also be an s_actionQueue[] here, used to let
// thread_idle_loop() block waiting for pos->action to leave THREAD_SLEEP.
// That one was REMOVED after causing a real, reproducible permanent hang
// ("engine never makes a move again") - see the comment on
// thread_idle_loop() for the full explanation. Do not reintroduce a
// blocking wait there without also solving that race (e.g. verified-safe
// use of ds_disable_interrupts()/ds_restore_interrupts() around the
// check-and-block, IF confirmed safe against Calico's actual context-switch
// implementation - not just assumed).
static ThrListNode s_sleepQueue[MAX_THREADS];
static ThrListNode s_initQueue;

// Small helper ensuring every one of thread_init()'s several exit paths
// (including all OOM failure branches) consistently pairs the
// "initializing = false" flag flip with waking thread_create()'s waiter.
static inline void thread_init_signal_done(void)
{
  Threads.initializing = false;
  threadUnblockAllByValue(&s_initQueue, 0);
}

// thread_init() is where a search thread starts and initializes itself.
static int thread_init(void *arg)
{
  int idx = (intptr_t)arg;

  // Use Calico priority limits to assign worker calculations to lower priority layers.
  // This keeps the master main thread highly responsive to check timers.
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3D + idx);

  int t = 0; // NUMA disabled for DS/DSi
  if (t >= numCmhTables) {
    int old = numCmhTables;
    numCmhTables = t + 16;
    cmhTables = realloc(cmhTables, numCmhTables * sizeof(CounterMoveHistoryStat *));
    if (!cmhTables) {
      sf_printf("\x1b[1;31m[OOM] Failed to allocate cmhTables list!\x1b[0m\n");
      sf_fflush(stdout);
      thread_init_signal_done();
      return 0;
    }
    while (old < numCmhTables)
      cmhTables[old++] = NULL;
  }
  
  if (!cmhTables[t]) {
    cmhTables[t] = calloc(1, sizeof(CounterMoveHistoryStat));
    if (!cmhTables[t]) {
      sf_printf("\x1b[1;31m[OOM] Failed to allocate CounterMoveHistoryStat table!\x1b[0m\n");
      sf_fflush(stdout);
      thread_init_signal_done();
      return 0;
    }
    for (int chk = 0; chk < 2; chk++)
      for (int c = 0; c < 2; c++)
        for (int j = 0; j < 16; j++)
          for (int k = 0; k < 64; k++)
            (*cmhTables[t])[chk][c][0][0][j][k] = CounterMovePruneThreshold - 1;
  }

  Position *pos = calloc(1, sizeof(Position));
  if (!pos) {
    sf_printf("\x1b[1;31m[OOM] Failed to allocate Position struct for Thread %d!\x1b[0m\n", idx);
    sf_fflush(stdout);
    thread_init_signal_done();
    return 0;
  }

#ifndef NNUE_PURE
  pos->pawnTable = calloc(PAWN_ENTRIES, sizeof(PawnEntry));
  pos->materialTable = calloc(8192, sizeof(MaterialEntry));
  if (!pos->pawnTable || !pos->materialTable) {
    sf_printf("\x1b[1;31m[OOM] Failed to allocate pawn or material tables for Thread %d!\x1b[0m\n", idx);
    sf_fflush(stdout);
    thread_init_signal_done();
    return 0;
  }
#endif

  pos->counterMoves = calloc(1, sizeof(CounterMoveStat));
  pos->mainHistory = calloc(1, sizeof(ButterflyHistory));
  pos->captureHistory = calloc(1, sizeof(CapturePieceToHistory));
  pos->lowPlyHistory = calloc(1, sizeof(LowPlyHistory));
  pos->rootMoves = calloc(1, sizeof(RootMoves));
  pos->stackAllocation = calloc(63 + (MAX_PLY + 110), sizeof(Stack));
  pos->moveList = calloc(10000, sizeof(ExtMove));

  if (!pos->counterMoves || !pos->mainHistory || !pos->captureHistory || 
      !pos->lowPlyHistory || !pos->rootMoves || !pos->stackAllocation || !pos->moveList) {
    sf_printf("\x1b[1;31m[OOM] Thread %d failed to allocate history/search arrays!\x1b[0m\n", idx);
    sf_fflush(stdout);
    thread_init_signal_done();
    return 0;
  }

  pos->stack = (Stack *)(((uintptr_t)pos->stackAllocation + 0x3f) & ~0x3f);
  pos->threadIdx = idx;
  pos->counterMoveHistory = cmhTables[t];

  atomic_store(&pos->resetCalls, false);
  pos->selDepth = pos->callsCnt = 0;

  // Store the initialized Position structural handle
  Threads.pos[idx] = pos;

  // GCC Hardware Barrier: Force Thread mapping completions. Kept as a
  // one-time, startup-only cost for defense in depth around the
  // Threads.pos[idx] publish.
  __sync_synchronize();

  // Signal completion to parent thread (thread_create()'s blocked waiter).
  thread_init_signal_done();

  thread_idle_loop(pos);

  return 0;
}

// thread_create() launches a new Calico preemptive thread.
static void thread_create(int idx)
{
  Threads.initializing = true;

  // Allocate Calico thread handle
  s_calicoThreads[idx] = malloc(sizeof(Thread));

  // Stack Allocation (256 KB to protect search recursion from overflowing) 32kb was min for 3DS
  size_t stack_size = 64 * 1024;
  s_threadStacks[idx] = malloc(stack_size);
  if (!s_threadStacks[idx]) {
    sf_printf("\x1b[1;31m[ERROR] Stack allocation failed for Thread %d!\x1b[0m\n", idx);
    return;
  }

  // Calculate 8-byte aligned stack top pointer
  uintptr_t stack_base = (uintptr_t)s_threadStacks[idx];
  uintptr_t stack_top_aligned = (stack_base + stack_size) & ~7;

  // Allocate C Standard library TLS to support memory safety
  size_t tls_size = threadGetLocalStorageSize();
  if (tls_size > 0) {
    s_threadTls[idx] = malloc(tls_size);
  } else {
    s_threadTls[idx] = NULL;
  }

  // Bind thread configurations inside the Calico scheduler context
  threadPrepare(s_calicoThreads[idx], 
                thread_init, 
                (void *)(intptr_t)idx, 
                (void*)stack_top_aligned, 
                MAIN_THREAD_PRIO);

  // Bind Standard Library local states to bypass race crashes
  if (s_threadTls[idx]) {
    threadAttachLocalStorage(s_calicoThreads[idx], s_threadTls[idx]);
  }

  // Wake up thread execution immediately
  threadStart(s_calicoThreads[idx]);
  
  // Safe: checker here (this function's caller) runs at MAIN_THREAD_PRIO,
  // strictly higher than the worker thread being waited on (0x3D+idx), so
  // the worker cannot preempt us mid check-then-block. See thread_idle_loop
  // for the case where this reasoning does NOT hold.
  while (Threads.initializing) {
    threadBlock(&s_initQueue, 0);
  }
}

// thread_destroy() waits for thread termination before returning.
static void thread_destroy(Position *pos)
{
  int idx = pos->threadIdx;
  pos->action = THREAD_EXIT;
  __sync_synchronize();

  // NOTE: no blocking wakeup call here on purpose. thread_idle_loop() waits
  // for pos->action via polling (see explanation there), so there is no
  // wait-queue registration for it to wake here anymore.

  // Explicitly wait for thread termination
  if (s_calicoThreads[idx]) {
    threadJoin(s_calicoThreads[idx]);
    free(s_calicoThreads[idx]);
    s_calicoThreads[idx] = NULL;
  }

  // Free stack and TLS heap segments
  if (s_threadStacks[idx]) {
    free(s_threadStacks[idx]);
    s_threadStacks[idx] = NULL;
  }
  if (s_threadTls[idx]) {
    free(s_threadTls[idx]);
    s_threadTls[idx] = NULL;
  }

#ifndef NNUE_PURE
  free(pos->pawnTable);
  free(pos->materialTable);
#endif
  free(pos->counterMoves);
  free(pos->mainHistory);
  free(pos->captureHistory);
  free(pos->lowPlyHistory);
  free(pos->rootMoves);
  free(pos->stackAllocation);
  free(pos->moveList);
  free(pos);
}

// thread_wait_until_sleeping() waits until not searching.
void thread_wait_until_sleeping(Position *pos)
{
  // Safe: checker here runs at MAIN_THREAD_PRIO (called only from
  // search.c/uci.c on the UCI command thread), strictly higher priority
  // than the worker thread (0x3D+idx) whose action we're waiting on. The
  // worker cannot preempt us mid check-then-block, so it cannot race ahead
  // and complete its own transition (setting pos->action = THREAD_SLEEP and
  // waking this queue) in the gap between our check and our threadBlock()
  // call - it simply doesn't get to run until we've actually blocked.
  while (pos->action != THREAD_SLEEP) {
    threadBlock(&s_sleepQueue[pos->threadIdx], 0);
  }

  if (pos->threadIdx == 0)
    Threads.searching = false;
}

// thread_wait() waits until condition is true.
//
// NOTE: intentionally NOT converted to a blocking wait. This function is
// generic over an arbitrary caller-supplied atomic_bool*, and safely
// converting it requires also adding a matching threadUnblockAllByValue()
// call at every call site that does `atomic_store(condition, true)` - those
// call sites live in uci.c's "stop"/"quit"/"ponderhit" handlers. In
// practice this GUI never sends "go infinite" or "ponder", so this wait
// path is never reached at all with the current GUI - left as polling
// rather than guessed at, since it costs nothing to leave alone.
void thread_wait(Position *pos, atomic_bool *condition)
{
  while (!atomic_load(condition)) {
    __sync_synchronize();       // Force hardware memory coherence update
    threadSleep(1000);          // Sleep 1ms
  }
}

void thread_wake_up(Position *pos, int action)
{
  if (action != THREAD_RESUME) {
    pos->action = action;
    __sync_synchronize();       // Force instant cache line flush
  }
}

// thread_idle_loop() is where the thread is parked when it has no work to do.
//
// *** IMPORTANT SAFETY NOTE - READ BEFORE CHANGING THIS FUNCTION ***
//
// The wait below (for pos->action to leave THREAD_SLEEP) MUST remain a
// polling loop, not a threadBlock()-based blocking wait. This was
// previously converted to threadBlock(&s_actionQueue[idx], 0), and that
// conversion caused a real, reproducible permanent hang ("engine never
// makes a move again") during on-device testing.
//
// Root cause: this loop's checker is the WORKER thread (priority
// 0x3D+idx). The condition it waits on is set by thread_wake_up() and
// thread_destroy(), both called from the UCI command thread (priority
// MAIN_THREAD_PRIO = 0x1c), which is strictly HIGHER priority than the
// worker. Because Calico preempts a running thread the instant a
// strictly-higher-priority thread becomes runnable (e.g. via a VBlank or
// other interrupt), the worker can be preempted in the exact gap between
// checking `pos->action == THREAD_SLEEP` and executing its threadBlock()
// call. If, during that preemption, the higher-priority thread runs
// thread_wake_up() (setting pos->action and calling
// threadUnblockAllByValue()), that wakeup finds nobody registered yet and
// is lost. When the worker resumes, it unconditionally calls threadBlock()
// next and blocks forever, since the one wakeup for that transition already
// happened. This is a low-probability-per-transition race (it requires an
// interrupt to land in a very narrow instruction window), which is why it
// did not appear immediately but eventually caused a permanent freeze
// during real play/testing.
//
// General rule for this codebase: a check-then-threadBlock() wait is only
// safe if the checking thread's priority is >= the priority of every thread
// that can change the awaited condition. It is UNSAFE if the checker has
// LOWER priority than the setter (this case), unless additional protection
// is used (e.g. a verified-safe critical section around the check-and-block
// using ds_disable_interrupts()/ds_restore_interrupts() - NOT attempted
// here since it would require confirming those primitives are safe to wrap
// around an actual threadBlock() call against Calico's real context-switch
// implementation, not just assumed).
static void thread_idle_loop(Position *pos)
{
  int idx = pos->threadIdx;
  while (true) {
    // Cooperative park loop - POLLING IS INTENTIONAL, see note above.
    while (pos->action == THREAD_SLEEP) {
      __sync_synchronize();       // Force core-to-core synchronicity checks
      threadSleep(1000);          // Sleep 1ms to prevent CPU starvation
    }

    if (pos->action == THREAD_EXIT) {
      break;
    } else if (pos->action == THREAD_TT_CLEAR) {
      tt_clear_worker(pos->threadIdx);
    } else {
      if (pos->threadIdx == 0)
        mainthread_search();
      else
        thread_search(pos);
    }

    pos->action = THREAD_SLEEP;
    // Safe to keep as a blocking wakeup: this is the producer side for
    // thread_wait_until_sleeping()'s consumers, which (per that function's
    // comment) are always higher-priority than this worker thread.
    threadUnblockAllByValue(&s_sleepQueue[idx], 0);
  }
}

// threads_init() creates and launches thread 0.
void threads_init(void)
{
  LightLock_Init(&Threads.mutex);
  LightLock_Init(&Threads.lock);

  Threads.numThreads = 1;
  thread_create(0);
}

// threads_exit() terminates threads before the program exits.
void threads_exit(void)
{
  threads_set_number(0);
}

// threads_set_number() creates/destroys threads to match the requested number.
void threads_set_number(int num)
{
  while (Threads.numThreads < num)
    thread_create(Threads.numThreads++);

  while (Threads.numThreads > num)
    thread_destroy(Threads.pos[--Threads.numThreads]);

  search_init();

  if (num == 0 && numCmhTables > 0) {
    for (int i = 0; i < numCmhTables; i++)
      if (cmhTables[i]) {
        free(cmhTables[i]);
      }
    free(cmhTables);
    cmhTables = NULL;
    numCmhTables = 0;
  }

  if (num == 0)
    Threads.searching = false;
}

// threads_nodes_searched() returns the number of nodes searched.
uint64_t threads_nodes_searched(void)
{
  uint64_t nodes = 0;
  for (int idx = 0; idx < Threads.numThreads; idx++)
    nodes += Threads.pos[idx]->nodes;
  return nodes;
}

// threads_tb_hits() returns the number of TB hits.
uint64_t threads_tb_hits(void)
{
  uint64_t hits = 0;
  for (int idx = 0; idx < Threads.numThreads; idx++)
    hits += Threads.pos[idx]->tbHits;
  return hits;
}
