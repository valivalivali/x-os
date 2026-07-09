/* Stub string.h for NanoSVG freestanding build.
 * String functions are provided by nanosvg_xos.c. */

#ifndef _XOS_STUB_STRING_H
#define _XOS_STUB_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strcat(char *dst, const char *src);
char *strchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strncpy(char *dst, const char *src, size_t n);
long strtol(const char *str, char **endptr, int base);
long long strtoll(const char *str, char **endptr, int base);

#endif
