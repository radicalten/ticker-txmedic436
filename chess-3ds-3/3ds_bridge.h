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

// Custom stream overrides
int sf_printf(const char *format, ...);
int sf_puts(const char *str);
int sf_putchar(int character);
size_t sf_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

int sf_get_output(char *buf, size_t max_len);

#ifdef __cplusplus
}
#endif

// Redirect all standard console write operations
#define printf sf_printf
#define puts sf_puts
#define putchar sf_putchar
#define fwrite sf_fwrite

#endif // THREEDS_BRIDGE_H
