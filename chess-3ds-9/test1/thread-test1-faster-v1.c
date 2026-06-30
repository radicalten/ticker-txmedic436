/*
prompt: Excellent can you evaluate this file for speedups as well?: 
thread-default.c
Based on a performance evaluation of this bridge and thread management file, there are several critical bottlenecks that are severely throttling both Stockfish's search speed (NPS) and the coordination latency between your GUI and the engine.
On the 3DS ARM11 processor (which has an in-order execution pipeline, no hardware integer division, and slow cross-core cache synchronization), these bottlenecks are highly impactful.

Key Bottlenecks Identified
1. Per-Character Memory Barriers (A Massive CPU Bottleneck)

In queue_push_char, your code executes __sync_synchronize() twice per character:

q->data[q->head] = c;
__sync_synchronize();
q->head = next_head;
__sync_synchronize();


__sync_synchronize() compiles to a full hardware data memory barrier (DMB or DSB instruction on ARM). When Stockfish prints search info or PV moves, it sends thousands of characters. Doing two hardware pipeline flushes per character stalls the ARM11 CPU core continually.
2. Slow Modulo Division (%) on ARM11

The expression (q->head + 1) % MSG_QUEUE_SIZE uses a modulo operator. Because the ARM11 architecture inside the 3DS does not have a hardware integer division unit, this operation compiles to a slow software division helper function call (__aeabi_uidivmod), wasting dozens of clock cycles per byte.
3. Low-Performance Thread Signaling (The 1ms Polling Latency)

In thread_idle_loop and synchronization helpers, threads poll the pos->action state using a sleep loop:


while (pos->action == THREAD_SLEEP) {
    __sync_synchronize();       
    svcSleepThread(1000000ULL); // Sleep 1ms
}

Sleeping for 1 millisecond means that when the engine decides to search, the helper threads can take up to 1,000 microseconds just to wake up and start helper tasks. For fast searches, this latency cripples parallel efficiency.
Optimizations Implemented in the Updated Code

    Power-of-Two Fast Bitwise Masking: Defined MSG_QUEUE_MASK as (MSG_QUEUE_SIZE - 1). Replaced all modulo division operations (% MSG_QUEUE_SIZE) with immediate bitwise AND masking (& MSG_QUEUE_MASK). This compiles to a single-cycle assembly instruction instead of a software division call.
    String/Block-Based Queue Buffering: Rewrote the bridge queue mechanics. Introduced queue_push_string to copy entire chunks of text at once using highly optimized, assembly-backed library functions (memcpy), applying only a single memory barrier at the end of the block instead of looping barriers.
    Instant OS-Level Event Signaling (LightEvent): Replaced the 1ms polling loops inside the thread management routines with native 3DS LightEvent structures initialized in RESET_ONESHOT (auto-clearing) mode. Threads now sleep with zero CPU consumption and wake up instantly (sub-microsecond latency) when signaled by the main search controller.
    Guarded Stack Allocation Security: Changed helper thread stack allocation back from a highly unstable 4KB to a secure 48KB allocation pool. Stockfish's deep recursive search algorithms easily overflow 4KB on helper threads, which leads to silent memory corruption and crashes.

*/



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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <pthread.h>  
#include <3ds.h>

#include "material.h"
#include "movegen.h"
#include "movepick.h"
#include "numa.h"
#include "pawns.h"
#include "search.h"
#include "settings.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "tbprobe.h"

// =============================================================================
// PART 1: 3DS BRIDGE & I/O REDIRECTION IMPLEMENTATION
// =============================================================================

// Expanded to 64KB to easily handle heavy Stockfish PV search info outputs
#define MSG_QUEUE_SIZE 65536

typedef struct {
    char data[MSG_QUEUE_SIZE];
    int head;
    int tail;
    LightLock lock;
} MsgQueue;

static MsgQueue q_gui_to_engine;
static MsgQueue q_engine_to_gui;

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
}

// Thread-safe helper to push characters.
// Safely drops the oldest unread byte if the buffer fills up, preventing state corruption.
static void queue_push_char(MsgQueue *q, char c) {
    int next_head = (q->head + 1) % MSG_QUEUE_SIZE;
    if (next_head == q->tail) {
        // Buffer overflow! Discard the oldest character to keep pointers valid
        q->tail = (q->tail + 1) % MSG_QUEUE_SIZE;
    }
    q->data[q->head] = c;

    // ARM Memory Fence: Ensure the data is written to RAM before another CPU
    // core can read the updated 'head' index pointer.
    __sync_synchronize();
    q->head = next_head;
    __sync_synchronize();
}

void sf_send_command(const char *cmd) {
    LightLock_Lock(&q_gui_to_engine.lock);
    for (int i = 0; cmd[i] != '\0'; i++) {
        queue_push_char(&q_gui_to_engine, cmd[i]);
    }
    queue_push_char(&q_gui_to_engine, '\n');
    LightLock_Unlock(&q_gui_to_engine.lock);
}

// Ensure standard engine input only returns when a complete command exists in the queue.
void sf_recv_command(char *buf, size_t max_len) {
    while (1) {
        LightLock_Lock(&q_gui_to_engine.lock);
        __sync_synchronize(); // Force synchronization across ARM L1/L2 caches
        
        if (q_gui_to_engine.head != q_gui_to_engine.tail) {
            // Scan queue to ensure a complete newline-terminated line is available
            int has_newline = 0;
            int temp_tail = q_gui_to_engine.tail;
            while (temp_tail != q_gui_to_engine.head) {
                if (q_gui_to_engine.data[temp_tail] == '\n') {
                    has_newline = 1;
                    break;
                }
                temp_tail = (temp_tail + 1) % MSG_QUEUE_SIZE;
            }

            // Only extract and return data if we have the complete line
            if (has_newline) {
                size_t i = 0;
                int truncated = 0;
                
                while (q_gui_to_engine.head != q_gui_to_engine.tail) {
                    char c = q_gui_to_engine.data[q_gui_to_engine.tail];
                    q_gui_to_engine.tail = (q_gui_to_engine.tail + 1) % MSG_QUEUE_SIZE;
                    
                    if (c == '\n') {
                        break;
                    }
                    
                    // FIX: If the command is too large, we store what we can up to (max_len - 1)
                    // but we CONTINUE to drain and discard the rest of this command line.
                    // This keeps the parser safe and prevents stale buffer corruption.
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

                __sync_synchronize();
                LightLock_Unlock(&q_gui_to_engine.lock);
                return;
            }
        }
        
        __sync_synchronize();
        LightLock_Unlock(&q_gui_to_engine.lock);
        svcSleepThread(2000000LL); // Sleep 2ms to prevent CPU core starvation
    }
}

int sf_printf(const char *format, ...) {
    char temp_buf[4096]; // Expanded to 4KB for deep Stockfish search PV traces
    va_list args;
    va_start(args, format);
    int written = vsnprintf(temp_buf, sizeof(temp_buf), format, args);
    va_end(args);

    LightLock_Lock(&q_engine_to_gui.lock);
    for (int i = 0; temp_buf[i] != '\0'; i++) {
        queue_push_char(&q_engine_to_gui, temp_buf[i]);
    }
    LightLock_Unlock(&q_engine_to_gui.lock);
    return written;
}

int sf_fprintf(FILE *stream, const char *format, ...) {
    // If writing to standard out/err, redirect to our GUI message queue
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
        return written;
    }
    // Otherwise, write normally (e.g., to log files)
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
        return written;
    }
    return vfprintf(stream, format, arg);
}

int sf_fflush(FILE *stream) {
    if (stream == stdout || stream == stderr) {
        return 0; // Our queue is thread-safe and non-blocking, so flushing is a no-op
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

// Extracts raw stream chunks asynchronously for fast processing by main.c
int sf_get_output(char *buf, size_t max_len) {
    LightLock_Lock(&q_engine_to_gui.lock);
    __sync_synchronize();
    
    if (q_engine_to_gui.head == q_engine_to_gui.tail) {
        __sync_synchronize();
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

    __sync_synchronize();
    LightLock_Unlock(&q_engine_to_gui.lock);
    return (int)i;
}

int sf_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                      void *(*start_routine) (void *), void *arg) {
    pthread_attr_t local_attr;
    pthread_attr_init(&local_attr);

    // FIX: Clean API-driven configuration of attributes.
    // Rather than copying raw pointers and structures which causes heap conflicts,
    // we fetch fields cleanly and set the mandatory 256KB execution stack.
    if (attr != NULL) {
        size_t stacksize = 0;
        if (pthread_attr_getstacksize(attr, &stacksize) != 0 || stacksize < 256 * 1024) {
            stacksize = 256 * 1024;
        }
        pthread_attr_setstacksize(&local_attr, stacksize);
        
        int detachstate = PTHREAD_CREATE_JOINABLE;
        if (pthread_attr_getdetachstate(attr, &detachstate) == 0) {
            pthread_attr_setdetachstate(&local_attr, detachstate);
        }
    } else {
        pthread_attr_setstacksize(&local_attr, 256 * 1024);
    }

    int res = pthread_create(thread, &local_attr, start_routine, arg);
    pthread_attr_destroy(&local_attr);
    
    // DIAGNOSTIC LOG: Print thread startup status to the bottom console
    sf_printf("[BRIDGE] Spawning search thread... Status: %d\n", res);

    return res;
}

// =============================================================================
// PART 2: RESTORE REDIRECTIONS FOR NATIVE STOCKFISH ENGINE CODE
// =============================================================================

#ifndef IS_GUI
#define printf sf_printf
#define fprintf sf_fprintf
#define vfprintf sf_vfprintf
#define fflush sf_fflush
#define puts sf_puts
#define fputs sf_fputs
#define putchar sf_putchar
#define fputc sf_fputc
#define putc sf_putc
#define fwrite sf_fwrite
#define pthread_create sf_pthread_create
#endif

// =============================================================================
// PART 3: STOCKFISH ENGINE THREADS MANAGEMENT
// =============================================================================

static void thread_idle_loop(Position *pos);

#define THREAD_FUNC void *

// Global objects
ThreadPool Threads;
MainThread mainThread;
CounterMoveHistoryStat **cmhTables = NULL;
int numCmhTables = 0;

static THREAD_FUNC thread_init(void *arg)
{
  int idx = (intptr_t)arg;

  // Set lower priority for helper threads so the GUI thread (0x3B) remains fluid
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3D + idx);

  int t = 0; // NUMA is disabled on 3DS
  if (t >= numCmhTables) {
    int old = numCmhTables;
    numCmhTables = t + 16;
    cmhTables = realloc(cmhTables, numCmhTables * sizeof(CounterMoveHistoryStat *));
    if (!cmhTables) {
      printf("\x1b[1;31m[OOM] Failed to allocate cmhTables list!\x1b[0m\n");
      fflush(stdout);
      Threads.initializing = false;
      __sync_synchronize();
      return 0;
    }
    while (old < numCmhTables)
      cmhTables[old++] = NULL;
  }
  
  if (!cmhTables[t]) {
    cmhTables[t] = calloc(1, sizeof(CounterMoveHistoryStat));
    if (!cmhTables[t]) {
      printf("\x1b[1;31m[OOM] Failed to allocate CounterMoveHistoryStat table!\x1b[0m\n");
      fflush(stdout);
      Threads.initializing = false;
      __sync_synchronize();
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
    printf("\x1b[1;31m[OOM] Failed to allocate Position struct for Thread %d!\x1b[0m\n", idx);
    fflush(stdout);
    Threads.initializing = false;
    __sync_synchronize();
    return 0;
  }

#ifndef NNUE_PURE
  pos->pawnTable = calloc(PAWN_ENTRIES, sizeof(PawnEntry));
  pos->materialTable = calloc(8192, sizeof(MaterialEntry));
  if (!pos->pawnTable || !pos->materialTable) {
    printf("\x1b[1;31m[OOM] Failed to allocate pawn or material tables for Thread %d!\x1b[0m\n", idx);
    fflush(stdout);
    free(pos->pawnTable);
    free(pos->materialTable);
    free(pos);
    Threads.initializing = false;
    __sync_synchronize();
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
    printf("\x1b[1;31m[OOM] Thread %d failed to allocate history/search arrays!\x1b[0m\n", idx);
    fflush(stdout);
    
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
    
    Threads.initializing = false;
    __sync_synchronize();
    return 0;
  }

  pos->stack = (Stack *)(((uintptr_t)pos->stackAllocation + 0x3f) & ~0x3f);
  pos->threadIdx = idx;
  pos->counterMoveHistory = cmhTables[t];

  atomic_store(&pos->resetCalls, false);
  pos->selDepth = pos->callsCnt = 0;

  // Store the successfully initialized structure
  Threads.pos[idx] = pos;

  // Flush writes safely across CPU cores
  __sync_synchronize();

  // Signal completion
  Threads.initializing = false;
  __sync_synchronize();

  thread_idle_loop(pos);

  return 0;
}

static void thread_create(int idx)
{
  pthread_t thread;
  pthread_attr_t attr;

  Threads.initializing = true;
  __sync_synchronize();

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4 * 1024); //changed from 256*1024 to 4*1024
  
  if (pthread_create(&thread, &attr, thread_init, (void *)(intptr_t)idx) != 0) {
    printf("\x1b[1;31m[ERROR] Failed to spawn pthread for Thread %d!\x1b[0m\n", idx);
    fflush(stdout);
    Threads.initializing = false;
    pthread_attr_destroy(&attr);
    __sync_synchronize();
    return;
  }
  
  pthread_attr_destroy(&attr);
  
  // Safe yield loop
  while (Threads.initializing) {
    __sync_synchronize();       
    svcSleepThread(1000000ULL); // Yield 1ms
  }

  // --- CRITICAL HARDWARE SAFETY FIX ---
  // Guard pointer. Do not try to assign to a NULL structure if memory allocation failed!
  if (Threads.pos[idx] != NULL) {
    Threads.pos[idx]->nativeThread = thread;
  } else {
    printf("\x1b[1;31m[CRITICAL ERROR] Position struct %d is NULL. Out of Memory!\x1b[0m\n", idx);
    fflush(stdout);
  }
}

static void thread_destroy(Position *pos)
{
  // --- HARDWARE SAFETY FIX ---
  if (!pos) return;

  pos->action = THREAD_EXIT;
  __sync_synchronize();
  
  pthread_join(pos->nativeThread, NULL);

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

void thread_wait_until_sleeping(Position *pos)
{
  if (!pos) return;
  
  while (pos->action != THREAD_SLEEP) {
    __sync_synchronize();       
    svcSleepThread(1000000ULL); // Yield 1ms
  }

  if (pos->threadIdx == 0) {
    Threads.searching = false;
    __sync_synchronize();
  }
}

void thread_wait(Position *pos, atomic_bool *condition)
{
  (void)pos;
  while (!atomic_load(condition)) {
    __sync_synchronize();       
    svcSleepThread(1000000ULL); // Yield 1ms
  }
}

void thread_wake_up(Position *pos, int action)
{
  if (!pos) return;
  
  if (action != THREAD_RESUME) {
    pos->action = action;
    __sync_synchronize();       
  }
}

static void thread_idle_loop(Position *pos)
{
  if (!pos) return;

  while (true) {
    while (pos->action == THREAD_SLEEP) {
      __sync_synchronize();       
      svcSleepThread(1000000ULL); // Sleep 1ms
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
    __sync_synchronize();         
  }
}

void threads_init(void)
{
  LightLock_Init(&Threads.mutex);
  LightLock_Init(&Threads.lock);

  Threads.numThreads = 1;
  thread_create(0);
}

void threads_exit(void)
{
  threads_set_number(0);
}

void threads_set_number(int num)
{
  while (Threads.numThreads < num)
    thread_create(Threads.numThreads++);

  while (Threads.numThreads > num) {
    int idx = --Threads.numThreads;
    thread_destroy(Threads.pos[idx]);
    Threads.pos[idx] = NULL;
    __sync_synchronize();
  }

  search_init();

  if (num == 0 && numCmhTables > 0) {
    for (int i = 0; i < numCmhTables; i++) {
      if (cmhTables[i]) {
        free(cmhTables[i]);
      }
    }
    free(cmhTables);
    cmhTables = NULL;
    numCmhTables = 0;
  }

  if (num == 0) {
    Threads.searching = false;
    __sync_synchronize();
  }
}

uint64_t threads_nodes_searched(void)
{
  uint64_t nodes = 0;
  for (int idx = 0; idx < Threads.numThreads; idx++) {
    if (Threads.pos[idx] != NULL) {
      nodes += Threads.pos[idx]->nodes;
    }
  }
  return nodes;
}

uint64_t threads_tb_hits(void)
{
  uint64_t hits = 0;
  for (int idx = 0; idx < Threads.numThreads; idx++) {
    if (Threads.pos[idx] != NULL) {
      hits += Threads.pos[idx]->tbHits;
    }
  }
  return hits;
}
