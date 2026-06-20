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
    volatile int head; // Volatile prevents compiler caching of register values across context switches
    volatile int tail; // Volatile prevents compiler caching of register values across context switches
    LightLock lock;
} MsgQueue;

static MsgQueue q_gui_to_engine;
static MsgQueue q_engine_to_gui;

// Cooperative Thread Scheduler Definitions
typedef struct {
    uint32_t sp;
    int active;
    void* stack_alloc;
} DS_Thread;

static DS_Thread main_thread;
static DS_Thread search_thread;
static DS_Thread* current_thread = &main_thread;

// Forward declare to prevent LTO issues and keep compiler optimizations happy
__attribute__((used)) __attribute__((noinline)) void thread_exit(void);
void thread_launcher(void);

// Naked assembly context switcher (ARM Mode)
// Fixed: Restores 10 registers (r3-r11, lr/pc) to keep stack 8-byte aligned at all times
// Added: __attribute__((noinline)) to prevent stack corruption when compiled under -O2/-O3 optimization levels
__attribute__((naked)) __attribute__((noinline)) __attribute__((target("arm")))
void ds_switch_context(uint32_t* current_sp, uint32_t next_sp) {
    __asm__ volatile (
        "push {r3-r11, lr}\n\t"  // Save callee-saved registers (10 registers total)
        "str sp, [r0]\n\t"       // Save old SP into *current_sp (r0)
        "mov sp, r1\n\t"         // Load new SP (r1) into SP register
        "pop {r3-r11, pc}\n\t"   // Restore callee-saved registers and jump to new context PC
    );
}

// Naked assembly launcher for starting a thread (ARM Mode)
__attribute__((naked)) __attribute__((noinline)) __attribute__((target("arm")))
void thread_launcher(void) {
    __asm__ volatile (
        "mov r0, r5\n\t"           // Setup arg (r5) as first argument in r0
        "blx r4\n\t"               // Branch to start_routine (r4)
        "ldr r1, =thread_exit\n\t" // Safely load pointer to thread_exit
        "bx r1\n\t"                // Safe ARM-to-Thumb Branch and Exchange
        ".ltorg\n\t"               // Dump literal pool for LDR instruction
    );
}

// Thread destruction routine
__attribute__((used)) __attribute__((noinline))
void thread_exit(void) {
    search_thread.active = 0;
    current_thread = &main_thread;
    ds_switch_context(&search_thread.sp, main_thread.sp);
}

// Yield CPU time between GUI and Engine
void ds_yield(void) {
    if (!search_thread.active) return; // Nothing to switch to

    DS_Thread* old = current_thread;
    if (current_thread == &main_thread) {
        current_thread = &search_thread;
    } else {
        current_thread = &main_thread;
    }
    ds_switch_context(&old->sp, current_thread->sp);
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
#undef fgets
#undef getchar
#undef fgetc
#undef pthread_create
#undef pthread_join

void sf_bridge_init(void) {
    q_gui_to_engine.head = q_gui_to_engine.tail = 0;
    LightLock_Init(&q_gui_to_engine.lock);

    q_engine_to_gui.head = q_engine_to_gui.tail = 0;
    LightLock_Init(&q_engine_to_gui.lock);
    
    main_thread.active = 1;
    search_thread.active = 0;
    current_thread = &main_thread;
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
        ds_yield(); // Yield CPU back to the GUI thread
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
    
    ds_yield(); // Let GUI thread parse this immediately
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

// Redirect standard fgets to our cooperative non-blocking command line reader
char *sf_fgets(char *str, int n, FILE *stream) {
    if (stream == stdin) {
        sf_recv_command(str, (size_t)n);
        size_t len = strlen(str);
        if (len < (size_t)n - 1) {
            str[len] = '\n';
            str[len + 1] = '\0';
        }
        return str;
    }
    return fgets(str, n, stream);
}

// Redirect standard getchar to the non-blocking engine buffer
int sf_getchar(void) {
    char buf[2];
    sf_recv_command(buf, 2);
    return (int)buf[0];
}

// Redirect standard fgetc to handle stdin stream mapping
int sf_fgetc(FILE *stream) {
    if (stream == stdin) {
        return sf_getchar();
    }
    return fgetc(stream);
}

int sf_get_output(char *buf, size_t max_len) {
    ds_yield(); // Give engine thread a turn to run and populate output

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

// Cooperative Thread Spawner for DSi
int sf_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                      void *(*start_routine) (void *), void *arg) {
    (void)attr;

    // Recursive search thread safety guard
    if (search_thread.active) {
        if (thread != NULL) {
            *thread = (pthread_t)999;
        }
        return 0; // Return dummy success code
    }

    // Allocate 128 KB of stack memory to cleanly support recursive minimax evaluations
    size_t stacksize = 128 * 1024; 

    void* stack_alloc = malloc(stacksize);
    if (!stack_alloc) {
        return -1; // Out of memory
    }

    search_thread.stack_alloc = stack_alloc;
    search_thread.active = 1;

    // Align the top stack limit
    uint32_t* sp = (uint32_t*)((char*)stack_alloc + stacksize);
    
    // Build context frame mapping for assembly switcher
    *(--sp) = (uint32_t)thread_launcher; // pc
    *(--sp) = 0;                         // r11
    *(--sp) = 0;                         // r10
    *(--sp) = 0;                         // r9
    *(--sp) = 0;                         // r8
    *(--sp) = 0;                         // r7
    *(--sp) = 0;                         // r6
    *(--sp) = (uint32_t)arg;             // r5 (arg passed to launcher)
    *(--sp) = (uint32_t)start_routine;   // r4 (start_routine passed to launcher)
    *(--sp) = 0;                         // r3 (dummy padding register to keep stack 8-byte aligned)

    search_thread.sp = (uint32_t)sp;
    
    if (thread != NULL) {
        *thread = (pthread_t)1;
    }

    return 0;
}

// Intercept pthread_join to safely free stack memory allocation and prevent memory leaks
int sf_pthread_join(pthread_t thread, void **retval) {
    (void)thread;
    (void)retval;

    // Wait until the cooperative search thread has executed thread_exit
    while (search_thread.active) {
        ds_yield();
    }

    // Cleanly free allocated stack space
    if (search_thread.stack_alloc != NULL) {
        free(search_thread.stack_alloc);
        search_thread.stack_alloc = NULL;
    }

    return 0;
}
