#include "3ds_bridge.h"
#include <nds.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define MSG_QUEUE_SIZE 65536

typedef struct {
    char data[MSG_QUEUE_SIZE];
    int head;
    int tail;
    LightLock lock;
} MsgQueue;

static MsgQueue q_gui_to_engine;
static MsgQueue q_engine_to_gui;

// Stack and Thread-Local Storage Configuration for Calico
#define SF_THREAD_STACK_SIZE (64 * 1024) // 64 KB Stack (safe for heavy chess evaluation)

static Thread s_searchThread;
static void* s_searchThreadStack = NULL;
static void* s_searchThreadTls = NULL;
static volatile bool s_searchThreadActive = false;
static void* (*s_startRoutine)(void*) = NULL;
static void* s_threadArg = NULL;

// Adapt standard routine void* return to Calico's int return signature
static int calico_thread_entry(void* arg) {
    (void)arg;
    if (s_startRoutine) {
        s_startRoutine(s_threadArg);
    }
    s_searchThreadActive = false;
    return 0;
}

// Yield CPU time
void ds_yield(void) {
    threadYield();
}

// Undefine target library macros to avoid infinite loops inside overrides
#undef printf
#undef fprintf
#undef vfprintf
#undef fflush
#undef puts
#undef fputs
#undef putchar
#undef fputc
#undef putc
#undef fwrite
#undef pthread_create

void sf_bridge_init(void) {
    q_gui_to_engine.head = q_gui_to_engine.tail = 0;
    LightLock_Init(&q_gui_to_engine.lock);

    q_engine_to_gui.head = q_engine_to_gui.tail = 0;
    LightLock_Init(&q_engine_to_gui.lock);
    
    s_searchThreadActive = false;
}

static void queue_push_char(MsgQueue *q, char c) {
    int next_head = (q->head + 1) % MSG_QUEUE_SIZE;
    if (next_head == q->tail) {
        q->tail = (q->tail + 1) % MSG_QUEUE_SIZE;
    }
    q->data[q->head] = c;
    q->head = next_head;
}

void sf_send_command(const char *cmd) {
    LightLock_Lock(&q_gui_to_engine.lock);
    for (int i = 0; cmd[i] != '\0'; i++) {
        queue_push_char(&q_gui_to_engine, cmd[i]);
    }
    queue_push_char(&q_gui_to_engine, '\n');
    LightLock_Unlock(&q_gui_to_engine.lock);
}

void sf_recv_command(char *buf, size_t max_len) {
    while (1) {
        LightLock_Lock(&q_gui_to_engine.lock);
        
        if (q_gui_to_engine.head != q_gui_to_engine.tail) {
            int has_newline = 0;
            int temp_tail = q_gui_to_engine.tail;
            while (temp_tail != q_gui_to_engine.head) {
                if (q_gui_to_engine.data[temp_tail] == '\n') {
                    has_newline = 1;
                    break;
                }
                temp_tail = (temp_tail + 1) % MSG_QUEUE_SIZE;
            }

            if (has_newline) {
                size_t i = 0;
                int truncated = 0;
                
                while (q_gui_to_engine.head != q_gui_to_engine.tail) {
                    char c = q_gui_to_engine.data[q_gui_to_engine.tail];
                    q_gui_to_engine.tail = (q_gui_to_engine.tail + 1) % MSG_QUEUE_SIZE;
                    
                    if (c == '\n') {
                        break;
                    }
                    
                    if (i < max_len - 1) {
                        buf[i++] = c;
                    } else {
                        truncated = 1;
                    }
                }
                buf[i] = '\0';
                
                if (truncated) {
                    sf_printf("[BRIDGE_WARNING] Command exceeded buffer size and was safely truncated!\n");
                }

                LightLock_Unlock(&q_gui_to_engine.lock);
                return;
            }
        }
        
        LightLock_Unlock(&q_gui_to_engine.lock);
        
        // Use active thread sleep (1 millisecond) instead of yielding continuously.
        // This avoids running hot and chewing through battery when idle.
        threadSleep(1000); 
    }
}

int sf_printf(const char *format, ...) {
    char temp_buf[4096];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(temp_buf, sizeof(temp_buf), format, args);
    va_end(args);

    LightLock_Lock(&q_engine_to_gui.lock);
    for (int i = 0; temp_buf[i] != '\0'; i++) {
        queue_push_char(&q_engine_to_gui, temp_buf[i]);
    }
    LightLock_Unlock(&q_engine_to_gui.lock);
    
    ds_yield(); // Give GUI immediate priority to read this buffer
    return written;
}

int sf_fprintf(FILE *stream, const char *format, ...) {
    if (stream == stdout || stream == stderr) {
        char temp_buf[4096];
        va_list args;
        va_start(args, format);
        int written = vsnprintf(temp_buf, sizeof(temp_buf), format, args);
        va_end(args);

        LightLock_Lock(&q_engine_to_gui.lock);
        for (int i = 0; temp_buf[i] != '\0'; i++) {
            queue_push_char(&q_engine_to_gui, temp_buf[i]);
        }
        LightLock_Unlock(&q_engine_to_gui.lock);
        
        ds_yield();
        return written;
    }
    va_list args;
    va_start(args, format);
    int written = vfprintf(stream, format, args);
    va_end(args);
    return written;
}

int sf_vfprintf(FILE *stream, const char *format, va_list arg) {
    if (stream == stdout || stream == stderr) {
        char temp_buf[4096];
        int written = vsnprintf(temp_buf, sizeof(temp_buf), format, arg);

        LightLock_Lock(&q_engine_to_gui.lock);
        for (int i = 0; temp_buf[i] != '\0'; i++) {
            queue_push_char(&q_engine_to_gui, temp_buf[i]);
        }
        LightLock_Unlock(&q_engine_to_gui.lock);
        
        ds_yield();
        return written;
    }
    return vfprintf(stream, format, arg);
}

int sf_fflush(FILE *stream) {
    if (stream == stdout || stream == stderr) {
        return 0;
    }
    return fflush(stream);
}

int sf_puts(const char *str) {
    LightLock_Lock(&q_engine_to_gui.lock);
    int i = 0;
    for (; str[i] != '\0'; i++) {
        queue_push_char(&q_engine_to_gui, str[i]);
    }
    queue_push_char(&q_engine_to_gui, '\n');
    LightLock_Unlock(&q_engine_to_gui.lock);
    
    ds_yield();
    return i + 1;
}

int sf_fputs(const char *str, FILE *stream) {
    if (stream == stdout || stream == stderr) {
        return sf_puts(str);
    }
    return fputs(str, stream);
}

int sf_putchar(int character) {
    LightLock_Lock(&q_engine_to_gui.lock);
    queue_push_char(&q_engine_to_gui, (char)character);
    LightLock_Unlock(&q_engine_to_gui.lock);
    return character;
}

int sf_fputc(int character, FILE *stream) {
    if (stream == stdout || stream == stderr) {
        return sf_putchar(character);
    }
    return fputc(character, stream);
}

int sf_putc(int character, FILE *stream) {
    if (stream == stdout || stream == stderr) {
        return sf_putchar(character);
    }
    return putc(character, stream);
}

size_t sf_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t total_bytes = size * nmemb;
    const char *char_ptr = (const char *)ptr;
    
    LightLock_Lock(&q_engine_to_gui.lock);
    for (size_t i = 0; i < total_bytes; i++) {
        queue_push_char(&q_engine_to_gui, char_ptr[i]);
    }
    LightLock_Unlock(&q_engine_to_gui.lock);
    return nmemb;
}

int sf_get_output(char *buf, size_t max_len) {
    ds_yield(); // Give engine thread a turn to push outputs

    LightLock_Lock(&q_engine_to_gui.lock);
    if (q_engine_to_gui.head == q_engine_to_gui.tail) {
        LightLock_Unlock(&q_engine_to_gui.lock);
        return 0;
    }

    size_t i = 0;
    while (q_engine_to_gui.head != q_engine_to_gui.tail && i < max_len - 1) {
        char c = q_engine_to_gui.data[q_engine_to_gui.tail];
        q_engine_to_gui.tail = (q_engine_to_gui.tail + 1) % MSG_QUEUE_SIZE;
        buf[i++] = c;
    }
    buf[i] = '\0';

    LightLock_Unlock(&q_engine_to_gui.lock);
    return (int)i;
}

// Preemptive Thread Spawner mapping POSIX definitions to libcalico
int sf_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                      void *(*start_routine) (void *), void *arg) {
    (void)attr;

    // Recursive thread guard (matches original design for single search engines)
    if (s_searchThreadActive) {
        if (thread != NULL) {
            *thread = (pthread_t)999;
        }
        return 0; 
    }

    // Allocate Stack and TLS on the first creation run to prevent runtime fragmentation
    if (!s_searchThreadStack) {
        s_searchThreadStack = malloc(SF_THREAD_STACK_SIZE);
        if (!s_searchThreadStack) {
            return -1;
        }

        size_t tls_size = threadGetLocalStorageSize();
        if (tls_size > 0) {
            s_searchThreadTls = malloc(tls_size);
            if (!s_searchThreadTls) {
                free(s_searchThreadStack);
                s_searchThreadStack = NULL;
                return -1;
            }
        }
    }

    s_startRoutine = start_routine;
    s_threadArg = arg;
    s_searchThreadActive = true;

    // Align stack base boundary up 8-bytes
    uintptr_t stack_base = (uintptr_t)s_searchThreadStack;
    uintptr_t stack_top_aligned = (stack_base + SF_THREAD_STACK_SIZE) & ~7;

    // Setup Calico thread metadata
    threadPrepare(&s_searchThread, 
                  calico_thread_entry, 
                  NULL, 
                  (void*)stack_top_aligned, 
                  MAIN_THREAD_PRIO);

    // Register C stdlib Thread-Local Storage pointer (essential to prevent crashes in sub-threads)
    if (s_searchThreadTls) {
        threadAttachLocalStorage(&s_searchThread, s_searchThreadTls);
    }

    // Launch execution context in the Calico scheduler
    threadStart(&s_searchThread);

    if (thread != NULL) {
        *thread = (pthread_t)&s_searchThread;
    }

    return 0;
}
