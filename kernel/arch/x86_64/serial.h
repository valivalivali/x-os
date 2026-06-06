#pragma once

/* COM1 serial debug output (visible via QEMU -serial stdio). */
void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);
