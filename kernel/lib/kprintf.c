#include "kernel/lib/kprintf.h"
#include "kernel/arch/x86_64/serial.h"
#include <stdint.h>
#include <stdbool.h>

bool g_verbose_boot = true;

void kputs(const char *s) { serial_write(s); }

static void put_uint(uint64_t v, unsigned base, bool upper) {
    char buf[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (v == 0) { buf[i++] = '0'; }
    while (v) { buf[i++] = digits[v % base]; v /= base; }
    while (i--) serial_putc(buf[i]);
}

static void put_int(int64_t v) {
    if (v < 0) { serial_putc('-'); put_uint((uint64_t)(-v), 10, false); }
    else put_uint((uint64_t)v, 10, false);
}

void kvprintf(const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { serial_putc(*fmt); continue; }
        fmt++;
        bool islong = false;
        while (*fmt == 'l') { islong = true; fmt++; }
        switch (*fmt) {
            case 'c': serial_putc((char)va_arg(ap, int)); break;
            case 's': {
                const char *s = va_arg(ap, const char *);
                serial_write(s ? s : "(null)");
                break;
            }
            case 'd': case 'i':
                if (islong) put_int(va_arg(ap, int64_t));
                else put_int(va_arg(ap, int));
                break;
            case 'u':
                put_uint(islong ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, false);
                break;
            case 'x':
                put_uint(islong ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, false);
                break;
            case 'X':
                put_uint(islong ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, true);
                break;
            case 'p':
                serial_write("0x");
                put_uint((uint64_t)(uintptr_t)va_arg(ap, void *), 16, false);
                break;
            case '%': serial_putc('%'); break;
            default:  serial_putc('%'); serial_putc(*fmt); break;
        }
    }
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

void kpanic(const char *fmt, ...) {
    serial_write("\n*** KERNEL PANIC: ");
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    serial_write(" ***\n");
    for (;;) __asm__ volatile("cli; hlt");
}
