#pragma once
#include <stdarg.h>
#include <stdbool.h>

/* Minimal kernel logging to the serial port.
 * Supports: %c %s %d %i %u %x %p %% and length modifier 'l' (%lu %lx %ld). */
void kputs(const char *s);
void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list ap);
__attribute__((noreturn)) void kpanic(const char *fmt, ...);

/* Verbose boot flag.  Set to false to suppress [boot] diagnostic prints.
 * Default true; may be toggled by kernel command line in future. */
extern bool g_verbose_boot;

/* Conditional boot logging.  Only prints when g_verbose_boot is true. */
#define boot_log(fmt, ...) do { \
    if (g_verbose_boot) kprintf("[boot] " fmt, ##__VA_ARGS__); \
} while (0)
#define boot_puts(msg) do { \
    if (g_verbose_boot) kputs("[boot] " msg); \
} while (0)
