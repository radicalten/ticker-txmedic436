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
        svcSleepThread(2000000LL); // Yield CPU to prevent starvation
    }
}

#undef printf
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
