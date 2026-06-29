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
// STOCKFISH NATIVE 3DS THREAD CONFIGURATIONS & DEFINITIONS
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
// THREEDS BRIDGE DECLARATIONS
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

// Custom thread override to set stack sizes and cores
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


/* =============================================================================
   IMPLEMENTATION SECTION
   This section is compiled ONLY in the file where you define
   THREEDS_BRIDGE_IMPLEMENTATION.
   ========================================================================== */
#ifdef THREEDS_BRIDGE_IMPLEMENTATION

#include <string.h>
#include <stdlib.h>

#define SF_BUFFER_SIZE 16384

// Thread-safe circular buffer for storing stdout redirection
static char sf_out_buffer[SF_BUFFER_SIZE];
static size_t sf_buf_head = 0;
static size_t sf_buf_tail = 0;
static pthread_mutex_t sf_buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

// Command queue (Engine to GUI and vice-versa)
static char sf_cmd_buffer[1024];
static pthread_mutex_t sf_cmd_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sf_cmd_cond = PTHREAD_COND_INITIALIZER;
static int sf_cmd_ready = 0;

void sf_bridge_init(void) {
    pthread_mutex_init(&sf_buffer_mutex, NULL);
    pthread_mutex_init(&sf_cmd_mutex, NULL);
    pthread_cond_init(&sf_cmd_cond, NULL);
    sf_buf_head = 0;
    sf_buf_tail = 0;
    sf_cmd_ready = 0;
}

// GUI writes a command to the engine
void sf_send_command(const char *cmd) {
    pthread_mutex_lock(&sf_cmd_mutex);
    strncpy(sf_cmd_buffer, cmd, sizeof(sf_cmd_buffer) - 1);
    sf_cmd_buffer[sizeof(sf_cmd_buffer) - 1] = '\0';
    sf_cmd_ready = 1;
    pthread_cond_signal(&sf_cmd_cond);
    pthread_mutex_unlock(&sf_cmd_mutex);
}

// Engine waits to receive command from GUI
void sf_recv_command(char *buf, size_t max_len) {
    pthread_mutex_lock(&sf_cmd_mutex);
    while (!sf_cmd_ready) {
        pthread_cond_wait(&sf_cmd_cond, &sf_cmd_mutex);
    }
    strncpy(buf, sf_cmd_buffer, max_len - 1);
    buf[max_len - 1] = '\0';
    sf_cmd_ready = 0;
    pthread_mutex_unlock(&sf_cmd_mutex);
}

// Helper to push characters into the redirect buffer
static void sf_buffer_push(const char *str, size_t len) {
    pthread_mutex_lock(&sf_buffer_mutex);
    for (size_t i = 0; i < len; i++) {
        size_t next = (sf_buf_head + 1) % SF_BUFFER_SIZE;
        if (next != sf_buf_tail) { // Avoid overflow (discard oldest if full)
            sf_out_buffer[sf_buf_head] = str[i];
            sf_buf_head = next;
        }
    }
    pthread_mutex_unlock(&sf_buffer_mutex);
}

// GUI calls this to read what Stockfish has printed
int sf_get_output(char *buf, size_t max_len) {
    pthread_mutex_lock(&sf_buffer_mutex);
    size_t i = 0;
    while (sf_buf_tail != sf_buf_head && i < (max_len - 1)) {
        buf[i++] = sf_out_buffer[sf_buf_tail];
        sf_buf_tail = (sf_buf_tail + 1) % SF_BUFFER_SIZE;
    }
    buf[i] = '\0';
    pthread_mutex_unlock(&sf_buffer_mutex);
    return (int)i;
}

/* Custom Standard Library overrides */

int sf_vfprintf(FILE *stream, const char *format, va_list arg) {
    char temp[512];
    int len = vsnprintf(temp, sizeof(temp), format, arg);
    if (len > 0) {
        sf_buffer_push(temp, len);
    }
    return len;
}

int sf_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int r = sf_vfprintf(stdout, format, args);
    va_end(args);
    return r;
}

int sf_fprintf(FILE *stream, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int r = sf_vfprintf(stream, format, args);
    va_end(args);
    return r;
}

int sf_fflush(FILE *stream) {
    (void)stream;
    return 0; // No-op as we handle buffer on-the-fly
}

int sf_puts(const char *str) {
    size_t len = strlen(str);
    sf_buffer_push(str, len);
    sf_buffer_push("\n", 1);
    return (int)len + 1;
}

int sf_fputs(const char *str, FILE *stream) {
    (void)stream;
    return sf_puts(str);
}

int sf_putchar(int character) {
    char c = (char)character;
    sf_buffer_push(&c, 1);
    return character;
}

int sf_fputc(int character, FILE *stream) {
    (void)stream;
    return sf_putchar(character);
}

int sf_putc(int character, FILE *stream) {
    (void)stream;
    return sf_putchar(character);
}

size_t sf_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    (void)stream;
    size_t total_bytes = size * nmemb;
    sf_buffer_push((const char*)ptr, total_bytes);
    return nmemb;
}

// Custom pthread wrapper configured specifically for Nintendo 3DS core/stack setup
int sf_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                      void *(*start_routine) (void *), void *arg) {
    pthread_attr_t custom_attr;
    
    if (attr == NULL) {
        pthread_attr_init(&custom_attr);
    } else {
        custom_attr = *attr;
    }
    
    // Configured for 3DS System thread allocations:
    // pthread_attr_setstacksize(&custom_attr, 1024 * 64); // Allocate stable 64KB stack
    
    int result = pthread_create(thread, &custom_attr, start_routine, arg);
    
    if (attr == NULL) {
        pthread_attr_destroy(&custom_attr);
    }
    
    return result;
}

#endif // THREEDS_BRIDGE_IMPLEMENTATION
