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
#include <malloc.h>
#include <string.h>

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

static void thread_idle_loop(Position *pos);

#define WII_THREAD_STACK_SIZE (256 * 1024)

ThreadPool Threads;
MainThread mainThread;
CounterMoveHistoryStat **cmhTables = NULL;
int numCmhTables = 0;

static sptr thread_init(void *arg)
{
  int idx = (intptr_t)arg;
  int t = 0; 

  if (t >= numCmhTables) {
    int old = numCmhTables;
    numCmhTables = t + 16;
    cmhTables = realloc(cmhTables,
        numCmhTables * sizeof(CounterMoveHistoryStat *));
    while (old < numCmhTables)
      cmhTables[old++] = NULL;
  }
  if (!cmhTables[t]) {
    cmhTables[t] = calloc(1, sizeof(CounterMoveHistoryStat));
    for (int chk = 0; chk < 2; chk++)
      for (int c = 0; c < 2; c++)
        for (int j = 0; j < 16; j++)
          for (int k = 0; k < 64; k++)
            (*cmhTables[t])[chk][c][0][0][j][k] = CounterMovePruneThreshold - 1;
  }

  Position *pos = calloc(1, sizeof(Position));
#ifndef NNUE_PURE
  pos->pawnTable = calloc(PAWN_ENTRIES,  sizeof(PawnEntry));
  pos->materialTable = calloc(8192, sizeof(MaterialEntry));
#endif
  pos->counterMoves = calloc(1, sizeof(CounterMoveStat));
  pos->mainHistory = calloc(1, sizeof(ButterflyHistory));
  pos->captureHistory = calloc(1, sizeof(CapturePieceToHistory));
  pos->lowPlyHistory = calloc(1, sizeof(LowPlyHistory));
  pos->rootMoves = calloc(1, sizeof(RootMoves));
  pos->stackAllocation = calloc(63 + (MAX_PLY + 110), sizeof(Stack));
  pos->moveList = calloc(10000, sizeof(ExtMove));

  pos->stack = (Stack *)(((uintptr_t)pos->stackAllocation + 0x3f) & ~0x3f);
  pos->threadIdx = idx;
  pos->counterMoveHistory = cmhTables[t];

  atomic_store(&pos->resetCalls, false);
  pos->selDepth = pos->callsCnt = 0;
  
  pos->action = THREAD_SLEEP; 

  Threads.pos[idx] = pos;
  atomic_store(&Threads.initializing, false);

  thread_idle_loop(pos);
  return 0;
}

static void thread_create(int idx)
{
  atomic_store(&Threads.initializing, true);

  KThread* thread = malloc(sizeof(KThread));
  void* stack_base = memalign(32, WII_THREAD_STACK_SIZE);

  Threads.threads[idx] = thread;
  Threads.thread_stacks[idx] = stack_base;
  
  // IMPLEMENTED: Properly initialize Tuxedo circular double-linked list head.
  // This completely resolves the unhandled page fault / null pointer kernel panic.
  Threads.waitQueues[idx].next = (KThread*)&Threads.waitQueues[idx];
  Threads.waitQueues[idx].prev = (KThread*)&Threads.waitQueues[idx];

  // IMPLEMENTED: Correctly calculate the highest address of the stack frame and align to 32 bytes
  void* stack_top = (char*)stack_base + WII_THREAD_STACK_SIZE - 32;

  // IMPLEMENTED: Lower heavy search threads to Priority 85 (0x55)
  KThreadPrepare(thread, thread_init, (void *)(intptr_t)idx, stack_top, 0x55);
  KThreadResume(thread);

  while (atomic_load(&Threads.initializing)) {
    KThreadSleepMs(1);
  }
}

static void thread_destroy(Position *pos)
{
  int idx = pos->threadIdx;
  pos->action = THREAD_EXIT;
  
  KThrQueueUnblockAllByValue(&Threads.waitQueues[idx], 0);
  KThreadJoin(Threads.threads[idx]);

  free(Threads.threads[idx]);
  free(Threads.thread_stacks[idx]);
  Threads.threads[idx] = NULL;
  Threads.thread_stacks[idx] = NULL;

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
  int idx = pos->threadIdx;
  while (pos->action != THREAD_SLEEP) {
    KThrQueueBlock(&Threads.waitQueues[idx], 0);
  }

  if (idx == 0)
    Threads.searching = false;
}

void thread_wait(Position *pos, atomic_bool *condition)
{
  int idx = pos->threadIdx;
  while (!atomic_load(condition)) {
    KThrQueueBlock(&Threads.waitQueues[idx], 0);
  }
}

void thread_wake_up(Position *pos, int action)
{
  int idx = pos->threadIdx;
  if (action != THREAD_RESUME)
    pos->action = action;
    
  KThrQueueUnblockAllByValue(&Threads.waitQueues[idx], 0);
}

static void thread_idle_loop(Position *pos)
{
  int idx = pos->threadIdx;
  while (true) {
    while (pos->action == THREAD_SLEEP) {
      KThrQueueBlock(&Threads.waitQueues[idx], 0);
    }

    if (pos->action == THREAD_EXIT) {
      break;
    } else if (pos->action == THREAD_TT_CLEAR) {
      tt_clear_worker(idx);
    } else {
      if (idx == 0)
        mainthread_search();
      else
        thread_search(pos);
    }

    pos->action = THREAD_SLEEP;
  }
}

void threads_init(void)
{
  LOCK_INIT(Threads.lock);
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

  while (Threads.numThreads > num)
    thread_destroy(Threads.pos[--Threads.numThreads]);

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

  if (num == 0)
    Threads.searching = false;
}

uint64_t threads_nodes_searched(void)
{
  uint64_t nodes = 0;
  for (int idx = 0; idx < Threads.numThreads; idx++)
    nodes += Threads.pos[idx]->nodes;
  return nodes;
}

uint64_t threads_tb_hits(void)
{
  uint64_t hits = 0;
  for (int idx = 0; idx < Threads.numThreads; idx++)
    hits += Threads.pos[idx]->tbHits;
  return hits;
}
