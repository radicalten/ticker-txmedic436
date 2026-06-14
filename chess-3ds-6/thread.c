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
#include "3ds_bridge.h"

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
  pthread_attr_setstacksize(&attr, 256 * 1024);
  
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
      svcSleepThread(2000000ULL); // Sleep 2ms
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
