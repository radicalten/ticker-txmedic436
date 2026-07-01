// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#ifndef THREEDS_BRIDGE_H
#define THREEDS_BRIDGE_H

#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <pthread.h> // Always use the compiler's official pthread definitions

// On Nintendo DS/DSi, define 3DS compatibility wrappers
#ifdef __NDS__
#include <calico.h>

// Real thread-blocking LightLock utilizing Calico queues
typedef struct {
    volatile int state;
    ThrListNode queue;
} LightLock;

// Non-inlined ARM-state interrupt controllers (implemented in 3ds_bridge.c)
u32 ds_disable_interrupts(void);
void ds_restore_interrupts(u32 old);

static inline void LightLock_Init(LightLock* lock) {
    lock->state = 0;
    lock->queue.next = NULL;
    lock->queue.prev = NULL;
}

static inline void LightLock_Lock(LightLock* lock) {
    while (1) {
        u32 irq_state = ds_disable_interrupts();
        if (lock->state == 0) {
            lock->state = 1;
            ds_restore_interrupts(irq_state);
            break;
        }
        // Blocks this thread on the lock's queue.
        // Calico will switch context and restore interrupts on the next thread.
        threadBlock(&lock->queue, (u32)lock);
        ds_restore_interrupts(irq_state);
    }
}

static inline void LightLock_Unlock(LightLock* lock) {
    u32 irq_state = ds_disable_interrupts();
    lock->state = 0;
    // Wake up one thread waiting on this lock queue
    threadUnblockOneByValue(&lock->queue, (u32)lock);
    ds_restore_interrupts(irq_state);
}

static inline int LightLock_TryLock(LightLock* lock) {
    u32 irq_state = ds_disable_interrupts();
    if (lock->state == 0) {
        lock->state = 1;
        ds_restore_interrupts(irq_state);
        return 0; // Success (matches 3DS LightLock)
    }
    ds_restore_interrupts(irq_state);
    return 1; // Failed to acquire
}

// Nintendo DS replacement for 3DS osGetTime() in milliseconds utilizing Calico ticks
static inline unsigned long long osGetTime(void) {
    return (tickGetCount() * 1000ULL) / TICK_FREQ;
}

// Cooperative yield function mapped to Calico's preemptive yield
void ds_yield(void);

// 3DS Kernel SVC Compatibility Stubs mapped to Calico
#define CUR_THREAD_HANDLE 0

static inline void svcSetThreadPriority(int handle, int priority) {
    (void)handle;
    // Map priority safely within Calico's bounds (0x00 to 0x3F)
    if (priority < THREAD_MAX_PRIO) priority = THREAD_MAX_PRIO;
    if (priority > THREAD_MIN_PRIO) priority = THREAD_MIN_PRIO;
    threadSetPrio(threadGetSelf(), (u8)priority);
}

static inline void svcSleepThread(unsigned long long ns) {
    if (ns == 0) {
        threadYield();
    } else {
        u32 us = (u32)(ns / 1000ULL);
        if (us == 0) us = 1;
        
        // Calico threadSleep() expects ticks, calculated here using Calico's inline function
        u32 ticks = ticksFromUsec(us);
        if (ticks == 0) ticks = 1; // Ensure we sleep for at least one hardware tick
        threadSleep(ticks);
    }
}
#endif

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

#endif // THREEDS_BRIDGE_H
