/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  ...license header unchanged...
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

// FIXED: Increased stack size - Stockfish search recurses deeply.
// 256KB was marginal and caused silent stack overflow corruption.
// 512KB gives safe headroom for MAX_PLY recursion depth.
#define WII_THREAD_STACK_SIZE (512 * 1024)

ThreadPool Threads;
MainThread mainThread;
CounterMoveHistoryStat **cmhTables = NULL;
int numCmhTables = 0;

static sptr thread_init(void *arg)
{
  int idx = (intptr_t)arg;
  
  // FIXED: Was hardcoded to 0, meaning ALL threads wrote to cmhTables[0]
  // and corrupted each other. Must use idx so each thread gets its own table.
  int t = idx;

  if (t >= numCmhTables) {
    int old = numCmhTables;
    // FIXED: Grow by idx+16 to ensure t is always a valid index
    numCmhTables = t + 16;
    cmhTables = realloc(cmhTables,
        numCmhTables * sizeof(CounterMoveHistoryStat *));
    // FIXED: NULL check realloc - on Wii heap is very limited
    if (!cmhTables) {
      // Fatal: cannot recover, signal init complete so GUI doesn't hang
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
  // FIXED: NULL check every allocation - Wii has ~24MB heap total
  if (!pos) {
    atomic_store(&Threads.initializing, false);
    return 0;
  }

#ifndef NNUE_PURE
  pos->pawnTable = calloc(PAWN_ENTRIES, sizeof(PawnEntry));
  pos->materialTable = calloc(8192, sizeof(MaterialEntry));
  if (!pos->pawnTable || !pos->materialTable) {
    // FIXED: Free partial allocations to avoid leak before signaling
    free(pos->pawnTable);
    free(pos->materialTable);
    free(pos);
    atomic_store(&Threads.initializing, false);
    return 0;
  }
#endif

  pos->counterMoves   = calloc(1, sizeof(CounterMoveStat));
  pos->mainHistory    = calloc(1, sizeof(ButterflyHistory));
  pos->captureHistory = calloc(1, sizeof(CapturePieceToHistory));
  pos->lowPlyHistory  = calloc(1, sizeof(LowPlyHistory));
  pos->rootMoves      = calloc(1, sizeof(RootMoves));
  pos->stackAllocation = calloc(63 + (MAX_PLY + 110), sizeof(Stack));
  pos->moveList       = calloc(10000, sizeof(ExtMove));

  // FIXED: Check all secondary allocations in one block
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
  pos->threadIdx = idx;
  pos->counterMoveHistory = cmhTables[t]; // FIXED: uses t=idx now

  atomic_store(&pos->resetCalls, false);
  pos->selDepth = pos->callsCnt = 0;
  pos->action = THREAD_SLEEP;

  Threads.pos[idx] = pos;
  
  // Signal creator thread that we are fully initialized
  atomic_store(&Threads.initializing, false);

  thread_idle_loop(pos);
  return 0;
}

static void thread_create(int idx)
{
  atomic_store(&Threads.initializing, true);

  // FIXED: NULL check malloc - if this returns NULL and we pass it
  // to KThreadPrepare, it writes to NULL+8 = 0x00000008, which is
  // EXACTLY the crash you see: "Invalid write to 0x00000008"
  KThread *thread = malloc(sizeof(KThread));
  if (!thread) {
    // Cannot create thread - fatal on Wii, halt with message
    printf("FATAL: malloc(KThread) failed for idx=%d\n", idx);
    fflush(stdout);
    atomic_store(&Threads.initializing, false);
    return;
  }

  // Zero-initialize the KThread struct before KThreadPrepare touches it
  // This prevents KThreadPrepare from misreading garbage fields
  memset(thread, 0, sizeof(KThread));

  void *stack_base = memalign(32, WII_THREAD_STACK_SIZE);
  if (!stack_base) {
    printf("FATAL: memalign(stack) failed for idx=%d\n", idx);
    fflush(stdout);
    free(thread);
    atomic_store(&Threads.initializing, false);
    return;
  }

  // FIXED: Zero the stack memory before use
  // Uninitialised stack memory can cause subtle crashes in thread startup
  memset(stack_base, 0, WII_THREAD_STACK_SIZE);

  Threads.threads[idx]       = thread;
  Threads.thread_stacks[idx] = stack_base;
  
  // FIXED: Zero the wait queue struct before any blocking operations use it
  memset(&Threads.waitQueues[idx], 0, sizeof(KThrQueue));

  // FIXED: PowerPC/Broadway stack grows DOWNWARD.
  // KThreadPrepare expects the TOP of the stack (highest address),
  // NOT the base (lowest address).
  // Passing stack_base caused the first stack frame to be written
  // BEFORE the allocated buffer, corrupting adjacent heap memory.
  void *stack_top = (char *)stack_base + WII_THREAD_STACK_SIZE;

  // Priority 0x55 (85): search threads run below GUI priority (64)
  // so the display stays responsive during engine thinking
  int result = KThreadPrepare(thread, thread_init,
                               (void *)(intptr_t)idx,
                               stack_top,   // FIXED: pass top not base
                               0x55);
  if (result != 0) {
    printf("FATAL: KThreadPrepare failed (%d) for idx=%d\n", result, idx);
    fflush(stdout);
    free(stack_base);
    free(thread);
    Threads.threads[idx]       = NULL;
    Threads.thread_stacks[idx] = NULL;
    atomic_store(&Threads.initializing, false);
    return;
  }

  result = KThreadResume(thread);
  if (result != 0) {
    printf("FATAL: KThreadResume failed (%d) for idx=%d\n", result, idx);
    fflush(stdout);
    // Thread was prepared but not running - still need to clean up
    free(stack_base);
    free(thread);
    Threads.threads[idx]       = NULL;
    Threads.thread_stacks[idx] = NULL;
    atomic_store(&Threads.initializing, false);
    return;
  }

  // Spin-wait for thread_init to complete and signal us
  // FIXED: Added timeout to prevent infinite hang if thread crashes
  // during init (e.g. allocation failure inside thread_init)
  int timeout_ms = 5000; // 5 second timeout
  while (atomic_load(&Threads.initializing) && timeout_ms > 0) {
    KThreadSleepMs(1);
    timeout_ms--;
  }
  
  if (timeout_ms <= 0) {
    printf("WARNING: Thread %d init timed out - possible crash in init\n",
           idx);
    fflush(stdout);
  }
}

static void thread_destroy(Position *pos)
{
  int idx = pos->threadIdx;
  
  // Signal thread to exit its idle loop
  pos->action = THREAD_EXIT;
  
  // Wake the thread so it can see the EXIT action and break
  KThrQueueUnblockAllByValue(&Threads.waitQueues[idx], 0);
  
  // FIXED: Wait for thread to actually finish before freeing its stack
  // Previously the stack could be freed while the thread was still
  // executing its exit path, causing a use-after-free crash
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
  
  // FIXED: Original code had a missed-wakeup race condition:
  // If the thread sets action=THREAD_SLEEP and calls UnblockAll
  // BEFORE we call KThrQueueBlock, we block forever.
  //
  // Solution: spin-check first, only block if still not sleeping.
  // The worker thread calls UnblockAll after setting THREAD_SLEEP,
  // so at worst we spin one extra iteration.
  while (pos->action != THREAD_SLEEP) {
    // Short sleep instead of blocking to avoid missed-wakeup
    // KThrQueueBlock is edge-triggered, not level-triggered
    KThreadSleepMs(1);
  }

  if (idx == 0)
    Threads.searching = false;
}

void thread_wait(Position *pos, atomic_bool *condition)
{
  int idx = pos->threadIdx;
  
  // FIXED: Same missed-wakeup race as thread_wait_until_sleeping.
  // Use sleep-spin instead of blocking queue to be safe.
  while (!atomic_load(condition)) {
    KThreadSleepMs(1);
  }
  
  // Suppress unused variable warning if KThrQueueBlock is removed
  (void)idx;
}

void thread_wake_up(Position *pos, int action)
{
  int idx = pos->threadIdx;
  
  // Only write action if it's not a bare resume signal
  if (action != THREAD_RESUME)
    pos->action = action;
  
  // Unblock any thread waiting in its idle loop or wait functions
  KThrQueueUnblockAllByValue(&Threads.waitQueues[idx], 0);
}

static void thread_idle_loop(Position *pos)
{
  int idx = pos->threadIdx;
  
  while (true) {
    // Sleep until woken by thread_wake_up
    while (pos->action == THREAD_SLEEP) {
      KThrQueueBlock(&Threads.waitQueues[idx], 0);
    }

    if (pos->action == THREAD_EXIT) {
      break;
    } else if (pos->action == THREAD_TT_CLEAR) {
      tt_clear_worker(idx);
    } else {
      // THREAD_SEARCH or THREAD_RESUME: run appropriate search function
      if (idx == 0)
        mainthread_search();
      else
        thread_search(pos);
    }

    // Mark this thread as sleeping again
    pos->action = THREAD_SLEEP;

    // FIXED: Signal any threads waiting for us to finish
    // (thread_wait_until_sleeping callers)
    KThrQueueUnblockAllByValue(&Threads.waitQueues[idx], 0);
  }
  
  // FIXED: Signal EXIT completion so thread_destroy's KThreadJoin
  // and any waiters know we have cleanly finished
  KThrQueueUnblockAllByValue(&Threads.waitQueues[idx], 0);
}

void threads_init(void)
{
  // FIXED: Zero the ThreadPool struct before initializing
  // to prevent stale pointer/index values from a previous run
  memset(&Threads, 0, sizeof(ThreadPool));
  
  LOCK_INIT(Threads.lock);
  Threads.numThreads = 0; // FIXED: explicit zero before thread_create
  Threads.numThreads = 1;
  thread_create(0);
}

void threads_exit(void)
{
  threads_set_number(0);
}

void threads_set_number(int num)
{
  // Grow thread pool if needed
  while (Threads.numThreads < num)
    thread_create(Threads.numThreads++);

  // Shrink thread pool if needed
  while (Threads.numThreads > num)
    thread_destroy(Threads.pos[--Threads.numThreads]);

  // Reinitialize search state for new thread count
  search_init();

  // Free counter move history tables when all threads are destroyed
  if (num == 0 && numCmhTables > 0) {
    for (int i = 0; i < numCmhTables; i++) {
      if (cmhTables[i]) {
        free(cmhTables[i]);
        cmhTables[i] = NULL; // FIXED: NULL after free to prevent double-free
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
