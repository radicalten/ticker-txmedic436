#ifndef CTRU_3DS_BRIDGE_H
#define CTRU_3DS_BRIDGE_H

#include <stdio.h>
#include <sys/types.h>
#include <pthread.h>

// Struct for thread-safe circular communication queues
typedef struct {
    char buffer[16384];
    int head;
    int tail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} SafeQueue;

extern SafeQueue gui_to_engine;
extern SafeQueue engine_to_gui;

// Intercept Standard POSIX functions strictly for the Chess Engine
#ifdef ENGINE_MODE
  ssize_t cfish_getline(char **lineptr, size_t *n, FILE *stream);
  int cfish_printf(const char *format, ...);
  int cfish_putchar(int c);
  int cfish_puts(const char *s);

  #define stdin  ((FILE*)0x1)
  #define stdout ((FILE*)0x2)
  #define stderr ((FILE*)0x3)

  #define getline(lineptr, n, stream) cfish_getline(lineptr, n, stream)
  #define printf(...) cfish_printf(__VA_ARGS__)
  #define putchar(c) cfish_putchar(c)
  #define puts(s) cfish_puts(s)
  #define fflush(stream) ((void)0)
  #define flockfile(stream) ((void)0)
  #define funlockfile(stream) ((void)0)
#endif

#endif // CTRU_3DS_BRIDGE_H
