#ifndef __SHIM_STRING_H__
#define __SHIM_STRING_H__
#include <stddef.h>
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
#endif
