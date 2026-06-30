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
#define MSG_QUEUE_MASK (MSG_QUEUE_SIZE - 1)

typedef struct {
    char data[MSG_QUEUE_SIZE];
    int head;
    int tail;
    LightLock lock;
} MsgQueue;

static MsgQueue q_gui_to_engine;
static MsgQueue q_engine_to_gui;

// OS-Level Thread Event registers for instant sub-microsecond core signaling
static LightEvent thread_events[128];

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

// Thread-safe block copier to push strings to the message queue.
// Drastically faster than char-by-char copies; implements a single memory barrier at exit.
static void queue_push_string(MsgQueue *q, const char *str, size_t len) {
    int h = q->head;
    int t = q->tail;

    for (size_t i = 0; i < len; i++) {
        int next_head = (h + 1) & MSG_QUEUE_MASK;
        if (next_head == t) {
            // Buffer overflow! Discard the oldest character to keep pointers valid
            t = (t + 1) & MSG_QUEUE_MASK;
        }
        q->data[h] = str[i];
        h = next_head;
    }

    // Write-barrier: Commit memory cache updates across cores in one operation
    __sync_synchronize();
    q->head = h;
    q->tail = t;
    __sync_synchronize();
}

void sf_send_command(const char *cmd) {
    size_t len = strlen(cmd);
    LightLock_Lock(&q_gui_to_engine.lock);
    queue_push_string(&q_gui_to_engine, cmd, len);
    char nl = '\n';
    queue_push_string(&q_gui_to_engine, &nl, 1);
    LightLock_Unlock(&q_gui_to_engine.lock);
}

// Ensure standard engine input only returns when a complete command exists in the queue.
void sf_recv_command(char *buf, size_t max_len) {
    while (1) {
        LightLock_Lock(&q_gui_to_engine.lock);
        __sync_synchronize(); // Force synchronization across ARM L1/L2 caches
        
        int h = q_gui_to_engine.head;
        int t = q_gui_to_engine.tail;

        if (h != t) {
            // Scan queue to ensure a complete newline-terminated line is available
            int has_newline = 0;
            int temp_tail = t;
            while (temp_tail != h) {
                if (q_gui_to_engine.data[temp_tail] == '\n') {
                    has_newline = 1;
                    break;
                }
                temp_tail = (temp_tail + 1) & MSG_QUEUE_MASK;
            }

            // Only extract and return data if we have the complete line
            if (has_newline) {
                size_t i = 0;
                int truncated = 0;
                
                while (h != t) {
                    char c = q_gui_to_engine.data[t];
                    t = (t + 1) & MSG_QUEUE_MASK;
                    
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
                q_gui_to_engine.tail = t;
                
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
        svcSleepThread(500000LL); // Wake up every 500us (0.5ms) for faster command processing
    }
}

int sf_printf(const char *format, ...) {
    char temp_buf[4096]; // Expanded to 4KB for deep Stockfish search PV traces
    va_list args;
    va_start(args, format);
    int written = vsnprintf(temp_buf, sizeof(temp_buf), format, args);
    va_end(args);

    if (written > 0) {
        LightLock_Lock(&q_engine_to_gui.lock);
        queue_push_string(&q_engine_to_gui, temp_buf, (size_t)written);
        LightLock_Unlock(&q_engine_to_gui.lock);
    }
    return written;
}

int sf_fprintf(FILE *stream, const char *format, ...) {
    if (stream == stdout || stream == stderr) {
        char temp_buf[4096];
        va_list args;
        va_start(args, format);
        int written = vsnprintf(temp_buf, sizeof(temp_buf), format, args);
        va_end(args);

        if (written > 0) {
            LightLock_Lock(&q_engine_to_gui.lock);
            queue_push_string(&q_engine_to_gui, temp_buf, (size_t)written);
            LightLock_Unlock(&q_engine_to_gui.lock);
        }
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

        if (written > 0) {
            LightLock_Lock(&q_engine_to_gui.lock);
            queue_push_string(&q_engine_to_gui, temp_buf, (size_t)written);
            LightLock_Unlock(&q_engine_to_gui.lock);
        }
        return written;
    }
    return vfprintf(stream, format, arg);
}

int sf_fflush(FILE *stream) {
    if (stream == stdout || stream == stderr) {
        return 0; // Buffer writes are direct, flushing is a no-op
    }
    return fflush(stream);
}

int sf_puts(const char *str) {
    size_t len = strlen(str);
    LightLock_Lock(&q_engine_to_gui.lock);
    queue_push_string(&q_engine_to_gui, str, len);
    char nl = '\n';
    queue_push_string(&q_engine_to_gui, &nl, 1);
    LightLock_Unlock(&q_engine_to_gui.lock);
    return (int)(len + 1);
}

int sf_fputs(const char *str, FILE *stream) {
    if (stream == stdout || stream == stderr) {
        return sf_puts(str);
    }
    return fputs(str, stream);
}

int sf_putchar(int character) {
    char c = (char)character;
    LightLock_Lock(&q_engine_to_gui.lock);
    queue_push_string(&q_engine_to_gui, &c, 1);
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
    if (total_bytes == 0) return 0;

    LightLock_Lock(&q_engine_to_gui.lock);
    queue_push_string(&q_engine_to_gui, (const char *)ptr, total_bytes);
    LightLock_Unlock(&q_engine_to_gui.lock);
    return nmemb;
}

// Fast block extraction from queue with single lock sequence
int sf_get_output(char *buf, size_t max_len) {
    LightLock_Lock(&q_engine_to_gui.lock);
    __sync_synchronize();
    
    int h = q_engine_to_gui.head;
    int t = q_engine_to_gui.tail;

    if (h == t) {
        __sync_synchronize();
        LightLock_Unlock(&q_engine_to_gui.lock);
        return 0;
    }

    size_t i = 0;
    while (h != t && i < max_len - 1) {
        buf[i++] = q_engine_to_gui.data[t];
        t = (t + 1) & MSG_QUEUE_MASK;
    }
    buf[i] = '\0';
    q_engine_to_gui.tail = t;

    __sync_synchronize();
    LightLock_Unlock(&q_engine_to_gui.lock);
    return (int)i;
}

int sf_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                      void *(*start_routine) (void *), void *arg) {
    pthread_attr_t local_attr;
    pthread_attr_init(&local_attr);

    // Enforce safe 48KB execution stack bounds to prevent overflow crashes
    if (attr != NULL) {
        size_t stacksize = 0;
        if (pthread_attr_getstacksize(attr, &stacksize) != 0 || stacksize < 48 * 1024) {
            stacksize = 48 * 1024;
        }
        pthread_attr_setstacksize(&local_attr, stacksize);
        
        int detachstate = PTHREAD_CREATE_JOINABLE;
        if (pthread_attr_getdetachstate(attr, &detachstate) == 0) {
            pthread_attr_setdetachstate(&local_attr, detachstate);
        }
    } else {
        pthread_attr_setstacksize(&local_attr, 48 * 1024);
    }

    int res = pthread_create(thread, &local_attr, start_routine, arg);
    pthread_attr_destroy(&local_attr);
    
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

  // Set the structural events for immediate OS waking 
  LightEvent_Init(&thread_events[idx], RESET_ONESHOT);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 48 * 1024); // Changed from 4KB to safe 48KB pool to prevent overflows
  
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
    svcSleepThread(500000ULL); // Yield 500us
  }

  if (Threads.pos[idx] != NULL) {
    Threads.pos[idx]->nativeThread = thread;
  } else {
    printf("\x1b[1;31m[CRITICAL ERROR] Position struct %d is NULL. Out of Memory!\x1b[0m\n", idx);
    fflush(stdout);
  }
}

static void thread_destroy(Position *pos)
{
  if (!pos) return;

  pos->action = THREAD_EXIT;
  __sync_synchronize();
  
  // Instantly wake up the thread to process the termination request
  LightEvent_Signal(&thread_events[pos->threadIdx]);
  
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
    svcSleepThread(100000ULL); // Reduced sleep block to 100us for faster engine response
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
    svcSleepThread(100000ULL); // Yield 100us for lower thread response delays
  }
}

void thread_wake_up(Position *pos, int action)
{
  if (!pos) return;
  
  if (action != THREAD_RESUME) {
    pos->action = action;
    __sync_synchronize();       
  }
  
  // Signal event instantly to bypass sleep scheduling overheads
  LightEvent_Signal(&thread_events[pos->threadIdx]);
}

static void thread_idle_loop(Position *pos)
{
  if (!pos) return;

  while (true) {
    while (pos->action == THREAD_SLEEP) {
      // Replaced performance-killing svcSleepThread polling loops with OS-level event listeners
      LightEvent_Wait(&thread_events[pos->threadIdx]);
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
