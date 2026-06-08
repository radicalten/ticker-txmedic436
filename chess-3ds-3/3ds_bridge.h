#ifndef THREEDS_BRIDGE_H
#define THREEDS_BRIDGE_H

#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void sf_bridge_init(void);
void sf_send_command(const char *cmd);
void sf_recv_command(char *buf, size_t max_len);
int sf_printf(const char *format, ...);
int sf_get_output(char *buf, size_t max_len);

#ifdef __cplusplus
}
#endif

// Redirect standard printf to write to our thread-safe buffer
#define printf sf_printf

#endif // THREEDS_BRIDGE_H
