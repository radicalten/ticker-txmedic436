#include "3ds_bridge.h"
#include <3ds.h>
#include <string.h>

#define MSG_QUEUE_SIZE 16384

typedef struct {
    char data[MSG_QUEUE_SIZE];
    int head;
    int tail;
    LightLock lock;
} MsgQueue;

static MsgQueue q_gui_to_engine;
static MsgQueue q_engine_to_gui;

void sf_bridge_init(void) {
    q_gui_to_engine.head = q_gui_to_engine.tail = 0;
    LightLock_Init(&q_gui_to_engine.lock);

    q_engine_to_gui.head = q_engine_to_gui.tail = 0;
    LightLock_Init(&q_engine_to_gui.lock);
}

void sf_send_command(const char *cmd) {
    LightLock_Lock(&q_gui_to_engine.lock);
    for (int i = 0; cmd[i] != '\0'; i++) {
        q_gui_to_engine.data[q_gui_to_engine.head] = cmd[i];
        q_gui_to_engine.head = (q_gui_to_engine.head + 1) % MSG_QUEUE_SIZE;
    }
    q_gui_to_engine.data[q_gui_to_engine.head] = '\n';
    q_gui_to_engine.head = (q_gui_to_engine.head + 1) % MSG_QUEUE_SIZE;
    LightLock_Unlock(&q_gui_to_engine.lock);
}

void sf_recv_command(char *buf, size_t max_len) {
    while (1) {
        LightLock_Lock(&q_gui_to_engine.lock);
        if (q_gui_to_engine.head != q_gui_to_engine.tail) {
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
        LightLock_Unlock(&q_gui_to_engine.lock);
        svcSleepThread(2000000LL); 
    }
}

#undef printf
#undef puts
#undef putchar
#undef fwrite

int sf_printf(const char *format, ...) {
    char temp_buf[1024];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(temp_buf, sizeof(temp_buf), format, args);
    va_end(args);

    LightLock_Lock(&q_engine_to_gui.lock);
    for (int i = 0; temp_buf[i] != '\0'; i++) {
        q_engine_to_gui.data[q_engine_to_gui.head] = temp_buf[i];
        q_engine_to_gui.head = (q_engine_to_gui.head + 1) % MSG_QUEUE_SIZE;
    }
    LightLock_Unlock(&q_engine_to_gui.lock);
    return written;
}

int sf_puts(const char *str) {
    LightLock_Lock(&q_engine_to_gui.lock);
    int i = 0;
    for (; str[i] != '\0'; i++) {
        q_engine_to_gui.data[q_engine_to_gui.head] = str[i];
        q_engine_to_gui.head = (q_engine_to_gui.head + 1) % MSG_QUEUE_SIZE;
    }
    q_engine_to_gui.data[q_engine_to_gui.head] = '\n';
    q_engine_to_gui.head = (q_engine_to_gui.head + 1) % MSG_QUEUE_SIZE;
    LightLock_Unlock(&q_engine_to_gui.lock);
    return i + 1;
}

int sf_putchar(int character) {
    LightLock_Lock(&q_engine_to_gui.lock);
    q_engine_to_gui.data[q_engine_to_gui.head] = (char)character;
    q_engine_to_gui.head = (q_engine_to_gui.head + 1) % MSG_QUEUE_SIZE;
    LightLock_Unlock(&q_engine_to_gui.lock);
    return character;
}

size_t sf_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t total_bytes = size * nmemb;
    const char *char_ptr = (const char *)ptr;
    
    LightLock_Lock(&q_engine_to_gui.lock);
    for (size_t i = 0; i < total_bytes; i++) {
        q_engine_to_gui.data[q_engine_to_gui.head] = char_ptr[i];
        q_engine_to_gui.head = (q_engine_to_gui.head + 1) % MSG_QUEUE_SIZE;
    }
    LightLock_Unlock(&q_engine_to_gui.lock);
    return nmemb;
}

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
        if (c == '\n') {
            break;
        }
    }
    buf[i] = '\0';

    LightLock_Unlock(&q_engine_to_gui.lock);
    return (int)i;
}

#undef pthread_create
int sf_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                      void *(*start_routine) (void *), void *arg) {
    pthread_attr_t local_attr;
    if (attr == NULL) {
        pthread_attr_init(&local_attr);
    } else {
        local_attr = *attr;
    }

    // Force a stable stack size (128 KB) for Stockfish search threads 
    // to prevent deep recursive searches from blowing up the 3DS stack limit.
    pthread_attr_setstacksize(&local_attr, 128 * 1024);

    // Assign Stockfish search threads to Core 1 (Sys Core / Secondary CPU Core)
    // to prevent CPU calculation spikes from blocking the GUI thread on Core 0.
    pthread_attr_setprocessor_np(&local_attr, 1);

    return pthread_create(thread, &local_attr, start_routine, arg);
}
