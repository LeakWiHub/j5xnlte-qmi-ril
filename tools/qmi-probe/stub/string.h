/* Minimal freestanding <string.h> for qmi-probe (no libc is linked). */
#ifndef PROBE_STRING_H
#define PROBE_STRING_H
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
size_t strlen(const char *s);
#endif
