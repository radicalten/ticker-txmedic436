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

#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <pthread.h>
#include <3ds.h>

#include "types.h"

// =============================================================================
// STOCKFISH NATIVE THREAD CONFIGURATIONS
// =============================================================================

// Max search threads scaled to 4 for optimal performance on New 3DS models
#define MAX_THREADS 4

// Redirect locks to highly-stable native 3DS LightLocks
#define LOCK_T LightLock
#define LOCK_INIT(x) LightLock_Init(&(x))
#define LOCK_DESTROY(x) do {} while (0)
#define LOCK(x) LightLock_Lock(&(x))
#define UNLOCK(x) LightLock_Unlock(&(x))

enum {
  THREAD_SLEEP, THREAD_SEARCH, THREAD_TT_CLEAR, THREAD_EXIT, THREAD_RESUME
};

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
  LightLock mutex;
  volatile bool initializing;
  volatile bool searching;
  volatile bool sleeping;
  volatile bool stopOnPonderhit;
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

static inline Position *threads_main(void)
{
  return Threads.pos[0];
}

extern CounterMoveHistoryStat **cmhTables;
extern int numCmhTables;


// =============================================================================
// 3DS BRIDGE REDIRECTION DECLARATIONS
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif

void sf_bridge_init(void);
void sf_send_command(const char *cmd);
void sf_recv_command(char *buf, size_t max_len);

// Custom stream overrides
int sf_printf(const char *format, ...);
int sf_fprintf(FILE *stream, const char *format, ...);
int sf_vfprintf(FILE *stream, const char *format, va_list arg);
int sf_fflush(FILE *stream);
int sf_puts(const char *str);
int sf_fputs(const char *str, FILE *stream);
int sf_putchar(int character);
int sf_fputc(int character, FILE *stream);
int sf_putc(int character, FILE *stream);
size_t sf_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

int sf_get_output(char *buf, size_t max_len);

// Custom thread override to set execution stack size
int sf_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                      void *(*start_routine) (void *), void *arg);

#ifdef __cplusplus
}
#endif

// Redirection applies ONLY to the Stockfish engine files, NOT the GUI main.c
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

#endif // THREAD_H
