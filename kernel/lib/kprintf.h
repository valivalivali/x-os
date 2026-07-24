#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/* Minimal kernel logging to the serial port.
 * Supports: %c %s %d %i %u %x %p %% and length modifier 'l' (%lu %lx %ld). */
void kputs(const char *s);
void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list ap);
/* Write a length-delimited string with the console lock held.
 * Used by sys_debug_log so userspace log messages are atomic. */
void kwrite(const char *s, size_t n);
__attribute__((noreturn)) void kpanic(const char *fmt, ...);

/* Verbose boot flag.  Set by boot-args `-v` (XNU-style). Silent by default. */
extern bool g_verbose_boot;

/* Conditional boot logging.  Only prints when g_verbose_boot is true. */
#define boot_log(fmt, ...) do { \
    if (g_verbose_boot) kprintf("[boot] " fmt, ##__VA_ARGS__); \
} while (0)
#define boot_puts(msg) do { \
    if (g_verbose_boot) kputs("[boot] " msg); \
} while (0)
