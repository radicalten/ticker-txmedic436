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

#ifndef THREAD_H
#define THREAD_H

#include <stdatomic.h>
#include "types.h"

#if defined(__wii__) || defined(GEKKO)
#include <tuxedo/thread.h>
#endif

#define MAX_THREADS 4

typedef struct {
  atomic_bool locked;
} TuxedoMutex;

#define LOCK_T TuxedoMutex
#define LOCK_INIT(x) atomic_store(&(x).locked, false)
#define LOCK_DESTROY(x) ((void)0)
#define LOCK(x) do { while (atomic_exchange(&(x).locked, true)) { KThreadYield(); } } while (0)
#define UNLOCK(x) atomic_store(&(x).locked, false)

enum {
  THREAD_SLEEP, THREAD_SEARCH, THREAD_TT_CLEAR, THREAD_EXIT, THREAD_RESUME
};

struct Position;
typedef struct Position Position;

void thread_search(Position *pos);
void thread_wake_up(Position *pos, int action);
void thread_wait_until_sleeping(Position *pos);
void thread_wait(Position *pos, atomic_bool *b);

struct MainThread {
  double previousTimeReduction;
  Value previousScore;
  Value iterValue[4];
};
typedef struct MainThread MainThread;

extern MainThread mainThread;
void mainthread_search(void);

struct ThreadPool {
  Position *pos[MAX_THREADS];
  int numThreads;
  
  KThread* threads[MAX_THREADS];
  void* thread_stacks[MAX_THREADS];
  KThrQueue waitQueues[MAX_THREADS];
  
  atomic_bool initializing;
  bool searching, sleeping, stopOnPonderhit;
  atomic_bool ponder, stop, increaseDepth;
  LOCK_T lock;
};
typedef struct ThreadPool ThreadPool;

void threads_init(void);
void threads_exit(void);
void threads_start_thinking(Position *pos, LimitsType *);
void threads_set_number(int num);
uint64_t threads_nodes_searched(void);
uint64_t threads_tb_hits(void);

extern ThreadPool Threads;

INLINE Position *threads_main(void)
{
  return Threads.pos[0];
}

extern CounterMoveHistoryStat **cmhTables;
extern int numCmhTables;

#endif // THREAD_H
