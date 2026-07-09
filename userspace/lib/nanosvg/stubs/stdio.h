/* Stub stdio.h for NanoSVG freestanding build.
 * nsvgParseFromFile is unused — we use nsvgParse from memory.
 * These stubs satisfy the compiler; the function will never be called. */

#ifndef _XOS_STUB_STDIO_H
#define _XOS_STUB_STDIO_H

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct _xos_FILE FILE;

static inline FILE *fopen(const char *path, const char *mode) { (void)path; (void)mode; return 0; }
static inline int fseek(FILE *f, long off, int whence) { (void)f; (void)off; (void)whence; return -1; }
static inline long ftell(FILE *f) { (void)f; return 0; }
static inline size_t fread(void *buf, size_t sz, size_t n, FILE *f) { (void)buf; (void)sz; (void)n; (void)f; return 0; }
static inline int fclose(FILE *f) { (void)f; return 0; }

int sscanf(const char *str, const char *fmt, ...);

#endif
