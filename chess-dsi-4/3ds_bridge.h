#ifndef THREEDS_BRIDGE_H
#define THREEDS_BRIDGE_H

#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <pthread.h> 

#ifdef __NDS__
#include <sys/time.h> 

typedef struct {
    int placeholder;
} LightLock;

static inline void LightLock_Init(LightLock* lock) { (void)lock; }
static inline void LightLock_Lock(LightLock* lock) { (void)lock; }
static inline void LightLock_Unlock(LightLock* lock) { (void)lock; }
static inline int LightLock_TryLock(LightLock* lock) { (void)lock; return 0; }

static inline unsigned long long osGetTime(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((unsigned long long)tv.tv_sec * 1000ULL) + (tv.tv_usec / 1000ULL);
}

void ds_yield(void);

#define CUR_THREAD_HANDLE 0
static inline void svcSetThreadPriority(int handle, int priority) { (void)handle; (void)priority; }
static inline void svcSleepThread(unsigned long long ns) { (void)ns; ds_yield(); }
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

// NEW: Input overrides to capture stdin
char *sf_fgets(char *str, int n, FILE *stream);
int sf_getchar(void);
int sf_fgetc(FILE *stream);

int sf_get_output(char *buf, size_t max_len);
int sf_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                      void *(*start_routine) (void *), void *arg);

#ifdef __cplusplus
}
#endif

// Redirection applies ONLY to the Stockfish/Engine files, NOT the GUI main.c
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

// NEW: Map standard input functions to our bridge
#define fgets sf_fgets
#define getchar sf_getchar
#define fgetc sf_fgetc
#endif

#endif // THREEDS_BRIDGE_H
