#include "3ds_bridge.h"
#include <3ds.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

// Expanded to 64KB to easily handle heavy Stockfish PV search info outputs
#define MSG_QUEUE_SIZE 65536

typedef struct {
    char data[MSG_QUEUE_SIZE];
    int head;
    int tail;
    LightLock lock;
} MsgQueue;

static MsgQueue q_gui_to_engine;
static MsgQueue q_engine_to_gui;

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
}

// Thread-safe helper to push characters.
// Safely drops the oldest unread byte if the buffer fills up, preventing state corruption.
static void queue_push_char(MsgQueue *q, char c) {
    int next_head = (q->head + 1) % MSG_QUEUE_SIZE;
    if (next_head == q->tail) {
        // Buffer overflow! Discard the oldest character to keep pointers valid
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

// Ensure standard engine input only returns when a complete command exists in the queue.
void sf_recv_command(char *buf, size_t max_len) {
    while (1) {
        LightLock_Lock(&q_gui_to_engine.lock);
        
        if (q_gui_to_engine.head != q_gui_to_engine.tail) {
            // Scan queue to ensure a complete newline-terminated line is available
            int has_newline = 0;
            int temp_tail = q_gui_to_engine.tail;
            while (temp_tail != q_gui_to_engine.head) {
                if (q_gui_to_engine.data[temp_tail] == '\n') {
                    has_newline = 1;
                    break;
                }
                temp_tail = (temp_tail + 1) % MSG_QUEUE_SIZE;
            }

            // Only extract and return data if we have the complete line
            if (has_newline) {
                size_t i = 0;
                while (q_gui_to_engine.head != q_gui_to_engine.tail && i < max_len - 1) {
                    char c = q_gui_to_engine.data[q_gui_to_engine.tail];
                    q_gui_to_engine.tail = (q_gui_to_engine.tail + 1) % MSG_QUEUE_SIZE;
                    if (c == '\n') {
                        break;
                    }
                    buf[i++] = c;
                }
                buf[i] = '\0';
                LightLock_Unlock(&q_gui_to_engine.lock);
                return;
            }
        }
        
        LightLock_Unlock(&q_gui_to_engine.lock);
        svcSleepThread(2000000LL); // Sleep 2ms to prevent CPU core starvation
    }
}

int sf_printf(const char *format, ...) {
    char temp_buf[2048]; // Safe upper bound for engine info formats
    va_list args;
    va_start(args, format);
    int written = vsnprintf(temp_buf, sizeof(temp_buf), format, args);
    va_end(args);

    LightLock_Lock(&q_engine_to_gui.lock);
    for (int i = 0; temp_buf[i] != '\0'; i++) {
        queue_push_char(&q_engine_to_gui, temp_buf[i]);
    }
    LightLock_Unlock(&q_engine_to_gui.lock);
    return written;
}

int sf_fprintf(FILE *stream, const char *format, ...) {
    // If writing to standard out/err, redirect to our GUI message queue
    if (stream == stdout || stream == stderr) {
        char temp_buf[2048];
        va_list args;
        va_start(args, format);
        int written = vsnprintf(temp_buf, sizeof(temp_buf), format, args);
        va_end(args);

        LightLock_Lock(&q_engine_to_gui.lock);
        for (int i = 0; temp_buf[i] != '\0'; i++) {
            queue_push_char(&q_engine_to_gui, temp_buf[i]);
        }
        LightLock_Unlock(&q_engine_to_gui.lock);
        return written;
    }
    // Otherwise, write normally (e.g., to log files)
    va_list args;
    va_start(args, format);
    int written = vfprintf(stream, format, args);
    va_end(args);
    return written;
}

int sf_vfprintf(FILE *stream, const char *format, va_list arg) {
    if (stream == stdout || stream == stderr) {
        char temp_buf[2048];
        int written = vsnprintf(temp_buf, sizeof(temp_buf), format, arg);

        LightLock_Lock(&q_engine_to_gui.lock);
        for (int i = 0; temp_buf[i] != '\0'; i++) {
            queue_push_char(&q_engine_to_gui, temp_buf[i]);
        }
        LightLock_Unlock(&q_engine_to_gui.lock);
        return written;
    }
    return vfprintf(stream, format, arg);
}

int sf_fflush(FILE *stream) {
    if (stream == stdout || stream == stderr) {
        return 0; // Our queue is thread-safe and non-blocking, so flushing is a no-op
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

// Extracts raw stream chunks asynchronously for fast processing by main.c
int sf_get_output(char *buf, size_t max_len) {
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

int sf_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                      void *(*start_routine) (void *), void *arg) {
    pthread_attr_t local_attr;
    if (attr == NULL) {
        pthread_attr_init(&local_attr);
    } else {
        local_attr = *attr;
    }

    // Force a stable stack size (512 KB) for Stockfish search threads 
    // to prevent deep recursive searches from blowing up the 3DS stack limit.
    pthread_attr_setstacksize(&local_attr, 512 * 1024);

    return pthread_create(thread, &local_attr, start_routine, arg);
}
