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

#define MSG_QUEUE_SIZE 65536

typedef struct {
    char data[MSG_QUEUE_SIZE];
    int head;
    int tail;
    LightLock lock;
} MsgQueue;

static MsgQueue q_gui_to_engine;
static MsgQueue q_engine_to_gui;

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
    LightLock_Init(&q_gui_to_engine.lock);

    q_engine_to_gui.head = q_engine_to_gui.tail = 0;
    LightLock_Init(&q_engine_to_gui.lock);
    
    s_searchThreadActive = false;
}

static void queue_push_char(MsgQueue *q, char c) {
    int next_head = (q->head + 1) % MSG_QUEUE_SIZE;
    if (next_head == q->tail) {
        q->tail = (q->tail + 1) % MSG_QUEUE_SIZE;
    }
    q->data[q->head] = c;
    q->head = next_head;
}

void sf_send_command(const char *cmd) {
    LightLock_Lock(&q_gui_to_engine.lock);
    for (int i = 0; cmd[i] != '\0'; i++) {
        queue_push_char(&q_gui_to_engine, cmd[i]);
    }
    queue_push_char(&q_gui_to_engine, '\n');
    LightLock_Unlock(&q_gui_to_engine.lock);
}

void sf_recv_command(char *buf, size_t max_len) {
    while (1) {
        LightLock_Lock(&q_gui_to_engine.lock);
        
        if (q_gui_to_engine.head != q_gui_to_engine.tail) {
            int has_newline = 0;
            int temp_tail = q_gui_to_engine.tail;
            while (temp_tail != q_gui_to_engine.head) {
                if (q_gui_to_engine.data[temp_tail] == '\n') {
                    has_newline = 1;
                    break;
                }
                temp_tail = (temp_tail + 1) % MSG_QUEUE_SIZE;
            }

            if (has_newline) {
                size_t i = 0;
                int truncated = 0;
                
                while (q_gui_to_engine.head != q_gui_to_engine.tail) {
                    char c = q_gui_to_engine.data[q_gui_to_engine.tail];
                    q_gui_to_engine.tail = (q_gui_to_engine.tail + 1) % MSG_QUEUE_SIZE;
                    
                    if (c == '\n') {
                        break;
                    }
                    
                    if (i < max_len - 1) {
                        buf[i++] = c;
                    } else {
                        truncated = 1;
                    }
                }
                buf[i] = '\0';
                
                if (truncated) {
                    sf_printf("[BRIDGE_WARNING] Command exceeded buffer size and was safely truncated!\n");
                }

                LightLock_Unlock(&q_gui_to_engine.lock);
                return;
            }
        }
        
        LightLock_Unlock(&q_gui_to_engine.lock);
        
        // Use active thread sleep (1 millisecond) instead of yielding continuously.
        // This avoids running hot and chewing through battery when idle.
        threadSleep(1000); 
    }
}

int sf_printf(const char *format, ...) {
    char temp_buf[4096];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(temp_buf, sizeof(temp_buf), format, args);
    va_end(args);

    LightLock_Lock(&q_engine_to_gui.lock);
    for (int i = 0; temp_buf[i] != '\0'; i++) {
        queue_push_char(&q_engine_to_gui, temp_buf[i]);
    }
    LightLock_Unlock(&q_engine_to_gui.lock);
    
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

        LightLock_Lock(&q_engine_to_gui.lock);
        for (int i = 0; temp_buf[i] != '\0'; i++) {
            queue_push_char(&q_engine_to_gui, temp_buf[i]);
        }
        LightLock_Unlock(&q_engine_to_gui.lock);
        
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

        LightLock_Lock(&q_engine_to_gui.lock);
        for (int i = 0; temp_buf[i] != '\0'; i++) {
            queue_push_char(&q_engine_to_gui, temp_buf[i]);
        }
        LightLock_Unlock(&q_engine_to_gui.lock);
        
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
    LightLock_Lock(&q_engine_to_gui.lock);
    int i = 0;
    for (; str[i] != '\0'; i++) {
        queue_push_char(&q_engine_to_gui, str[i]);
    }
    queue_push_char(&q_engine_to_gui, '\n');
    LightLock_Unlock(&q_engine_to_gui.lock);
    
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
    LightLock_Lock(&q_engine_to_gui.lock);
    queue_push_char(&q_engine_to_gui, (char)character);
    LightLock_Unlock(&q_engine_to_gui.lock);
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
    
    LightLock_Lock(&q_engine_to_gui.lock);
    for (size_t i = 0; i < total_bytes; i++) {
        queue_push_char(&q_engine_to_gui, char_ptr[i]);
    }
    LightLock_Unlock(&q_engine_to_gui.lock);
    return nmemb;
}

int sf_get_output(char *buf, size_t max_len) {
    ds_yield(); // Give engine thread a turn to push outputs

    LightLock_Lock(&q_engine_to_gui.lock);
    if (q_engine_to_gui.head == q_engine_to_gui.tail) {
        LightLock_Unlock(&q_engine_to_gui.lock);
        return 0;
    }

    size_t i = 0;
    while (q_engine_to_gui.head != q_engine_to_gui.tail && i < max_len - 1) {
        char c = q_engine_to_gui.data[q_engine_to_gui.tail];
        q_engine_to_gui.tail = (q_engine_to_gui.tail + 1) % MSG_QUEUE_SIZE;
        buf[i++] = c;
    }
    buf[i] = '\0';

    LightLock_Unlock(&q_engine_to_gui.lock);
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
      Threads.initializing = false;
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
      Threads.initializing = false;
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
    Threads.initializing = false;
    return 0;
  }

#ifndef NNUE_PURE
  pos->pawnTable = calloc(PAWN_ENTRIES, sizeof(PawnEntry));
  pos->materialTable = calloc(8192, sizeof(MaterialEntry));
  if (!pos->pawnTable || !pos->materialTable) {
    sf_printf("\x1b[1;31m[OOM] Failed to allocate pawn or material tables for Thread %d!\x1b[0m\n", idx);
    sf_fflush(stdout);
    Threads.initializing = false;
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
    Threads.initializing = false;
    return 0;
  }

  pos->stack = (Stack *)(((uintptr_t)pos->stackAllocation + 0x3f) & ~0x3f);
  pos->threadIdx = idx;
  pos->counterMoveHistory = cmhTables[t];

  atomic_store(&pos->resetCalls, false);
  pos->selDepth = pos->callsCnt = 0;

  // Store the initialized Position structural handle
  Threads.pos[idx] = pos;

  // GCC Hardware Barrier: Force Thread mapping completions
  __sync_synchronize();

  // Signal completion to parent thread
  Threads.initializing = false;

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
  size_t stack_size = 256 * 1024;
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
  
  // Safe yield loop preventing startup freezes
  while (Threads.initializing) {
    __sync_synchronize();       // Force memory sync across cores
    threadSleep(1000);          // Yield 1 millisecond using preemptive sleeps
  }
}

// thread_destroy() waits for thread termination before returning.
static void thread_destroy(Position *pos)
{
  int idx = pos->threadIdx;
  pos->action = THREAD_EXIT;
  __sync_synchronize();
  
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

// thread_wait_for_search_finished() waits until not searching.
void thread_wait_until_sleeping(Position *pos)
{
  // High-performance cooperative yield loop with compile-time memory barriers
  while (pos->action != THREAD_SLEEP) {
    __sync_synchronize();       // Force memory synchronization across CPU cores
    threadSleep(1000);          // Sleep 1ms to allow execution slots
  }

  if (pos->threadIdx == 0)
    Threads.searching = false;
}

// thread_wait() waits until condition is true.
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
static void thread_idle_loop(Position *pos)
{
  while (true) {
    // Cooperative park loop
    while (pos->action == THREAD_SLEEP) {
      __sync_synchronize();       // Force core-to-core synchronicity checks
      threadSleep(2000);          // Sleep 2ms to prevent CPU starvation
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
    __sync_synchronize();         // Commit the state change globally immediately
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
