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

// FIXED: Increased from 256KB to 512KB.
// Stockfish search recurses to MAX_PLY (246) depth.
// 256KB caused silent stack overflow and memory corruption.
#define WII_THREAD_STACK_SIZE (512 * 1024)

ThreadPool Threads;
MainThread mainThread;
CounterMoveHistoryStat **cmhTables = NULL;
int numCmhTables = 0;

static sptr thread_init(void *arg)
{
    int idx = (intptr_t)arg;

    // FIXED: Was hardcoded to 0, meaning ALL threads shared cmhTables[0]
    // and corrupted each other's counter move history data.
    // Must use idx so each thread gets its own independent table.
    int t = idx;

    if (t >= numCmhTables) {
        int old = numCmhTables;
        numCmhTables = t + 16;
        cmhTables = realloc(cmhTables,
                            numCmhTables * sizeof(CounterMoveHistoryStat *));
        // FIXED: NULL check realloc - Wii heap is very limited (~24MB)
        if (!cmhTables) {
            atomic_store(&Threads.initializing, false);
            return 0;
        }
        while (old < numCmhTables)
            cmhTables[old++] = NULL;
    }

    if (!cmhTables[t]) {
        cmhTables[t] = calloc(1, sizeof(CounterMoveHistoryStat));
        // FIXED: NULL check calloc
        if (!cmhTables[t]) {
            atomic_store(&Threads.initializing, false);
            return 0;
        }
        for (int chk = 0; chk < 2; chk++)
            for (int c = 0; c < 2; c++)
                for (int j = 0; j < 16; j++)
                    for (int k = 0; k < 64; k++)
                        (*cmhTables[t])[chk][c][0][0][j][k] =
                            CounterMovePruneThreshold - 1;
    }

    Position *pos = calloc(1, sizeof(Position));
    // FIXED: NULL check every allocation - Wii has limited heap
    if (!pos) {
        atomic_store(&Threads.initializing, false);
        return 0;
    }

#ifndef NNUE_PURE
    pos->pawnTable     = calloc(PAWN_ENTRIES, sizeof(PawnEntry));
    pos->materialTable = calloc(8192, sizeof(MaterialEntry));
    if (!pos->pawnTable || !pos->materialTable) {
        free(pos->pawnTable);
        free(pos->materialTable);
        free(pos);
        atomic_store(&Threads.initializing, false);
        return 0;
    }
#endif

    pos->counterMoves    = calloc(1, sizeof(CounterMoveStat));
    pos->mainHistory     = calloc(1, sizeof(ButterflyHistory));
    pos->captureHistory  = calloc(1, sizeof(CapturePieceToHistory));
    pos->lowPlyHistory   = calloc(1, sizeof(LowPlyHistory));
    pos->rootMoves       = calloc(1, sizeof(RootMoves));
    pos->stackAllocation = calloc(63 + (MAX_PLY + 110), sizeof(Stack));
    pos->moveList        = calloc(10000, sizeof(ExtMove));

    // FIXED: Check all secondary allocations together
    if (!pos->counterMoves   ||
        !pos->mainHistory    ||
        !pos->captureHistory ||
        !pos->lowPlyHistory  ||
        !pos->rootMoves      ||
        !pos->stackAllocation||
        !pos->moveList) {
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
        atomic_store(&Threads.initializing, false);
        return 0;
    }

    // Align stack pointer to 64-byte boundary within allocation
    pos->stack = (Stack *)(((uintptr_t)pos->stackAllocation + 0x3f) & ~0x3f);
    pos->threadIdx          = idx;
    // FIXED: uses t=idx now, not hardcoded 0
    pos->counterMoveHistory = cmhTables[t];

    atomic_store(&pos->resetCalls, false);
    pos->selDepth = pos->callsCnt = 0;
    pos->action   = THREAD_SLEEP;

    Threads.pos[idx] = pos;

    // Signal creator that we are fully initialised
    atomic_store(&Threads.initializing, false);

    thread_idle_loop(pos);
    return 0;
}

static void thread_create(int idx)
{
    atomic_store(&Threads.initializing, true);

    // FIXED: NULL check malloc.
    // If this returns NULL and we pass it to KThreadPrepare,
    // it writes to NULL+8 = 0x00000008 which is exactly the
    // crash: "Invalid write to 0x00000008"
    KThread *thread = malloc(sizeof(KThread));
    if (!thread) {
        printf("FATAL: malloc(KThread) failed for idx=%d\n", idx);
        fflush(stdout);
        atomic_store(&Threads.initializing, false);
        return;
    }

    // FIXED: Zero KThread struct before KThreadPrepare reads it
    memset(thread, 0, sizeof(KThread));

    void *stack_base = memalign(32, WII_THREAD_STACK_SIZE);
    if (!stack_base) {
        printf("FATAL: memalign(stack) failed for idx=%d\n", idx);
        fflush(stdout);
        free(thread);
        atomic_store(&Threads.initializing, false);
        return;
    }

    // FIXED: Zero stack before use to prevent uninitialised data crashes
    memset(stack_base, 0, WII_THREAD_STACK_SIZE);

    Threads.threads[idx]       = thread;
    Threads.thread_stacks[idx] = stack_base;

    // FIXED: Zero the wait queue before any blocking operations use it
    memset(&Threads.waitQueues[idx], 0, sizeof(KThrQueue));

    // FIXED: PowerPC/Broadway stack grows DOWNWARD.
    // KThreadPrepare expects the TOP (highest address) of the stack.
    // Passing stack_base (lowest address) caused the first stack frame
    // to be written BEFORE the allocated buffer, corrupting heap memory.
    void *stack_top = (char *)stack_base + WII_THREAD_STACK_SIZE;

    // FIXED: KThreadPrepare returns void on devkitPPC.
    // Assigning its return value caused:
    // "error: void value not ignored as it ought to be"
    KThreadPrepare(thread,
                   thread_init,
                   (void *)(intptr_t)idx,
                   stack_top,
                   0x55); // Priority 85: below GUI (~64), non-blocking UI

    // FIXED: KThreadResume returns void on devkitPPC.
    // Assigning its return value caused:
    // "error: invalid use of void expression"
    KThreadResume(thread);

    // Spin-wait for thread_init to signal completion.
    // FIXED: Added timeout so we don't hang forever if init crashes.
    int timeout_ms = 5000;
    while (atomic_load(&Threads.initializing) && timeout_ms > 0) {
        KThreadSleepMs(1);
        timeout_ms--;
    }

    if (timeout_ms <= 0) {
        printf("WARNING: Thread %d init timed out\n", idx);
        fflush(stdout);
    }
}

static void thread_destroy(Position *pos)
{
    int idx = pos->threadIdx;

    // Signal thread to exit its idle loop
    pos->action = THREAD_EXIT;

    // Wake the thread so it sees the EXIT action
    KThrQueueUnblockAllByValue(&Threads.waitQueues[idx], 0);

    // Wait for thread to finish before freeing its stack
    KThreadJoin(Threads.threads[idx]);

    free(Threads.threads[idx]);
    free(Threads.thread_stacks[idx]);
    Threads.threads[idx]       = NULL;
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

    // FIXED: Use sleep-spin instead of KThrQueueBlock to avoid
    // missed-wakeup race: if the thread sets THREAD_SLEEP and calls
    // UnblockAll BEFORE we call Block, we would block forever.
    while (pos->action != THREAD_SLEEP) {
        KThreadSleepMs(1);
    }

    if (idx == 0)
        Threads.searching = false;
}

void thread_wait(Position *pos, atomic_bool *condition)
{
    // FIXED: Same missed-wakeup race as thread_wait_until_sleeping.
    // Use sleep-spin for safety.
    while (!atomic_load(condition)) {
        KThreadSleepMs(1);
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

        // FIXED: Wake any threads waiting for this thread to finish
        KThrQueueUnblockAllByValue(&Threads.waitQueues[idx], 0);
    }

    // FIXED: Signal exit completion for thread_destroy's KThreadJoin
    KThrQueueUnblockAllByValue(&Threads.waitQueues[idx], 0);
}

void threads_init(void)
{
    // FIXED: Zero the entire ThreadPool before use to prevent stale
    // pointer or index values from causing undefined behaviour
    memset(&Threads, 0, sizeof(ThreadPool));

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
                // FIXED: NULL after free to prevent double-free
                cmhTables[i] = NULL;
            }
        }
        free(cmhTables);
        cmhTables    = NULL;
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
