#pragma once
#include <stddef.h>
#include <stdint.h>

/* XNU/BSD-style kernel message buffer (dmesg). Ring of MSGBUF_SIZE bytes. */

#define MSGBUF_SIZE 16384

void msgbuf_init(void);
void msgbuf_putc(char c);
void msgbuf_write(const char *s, size_t n);

/* Copy up to `max` bytes of log into `dst` (oldest→newest linear view).
 * Returns number of bytes written. */
size_t msgbuf_copy(char *dst, size_t max);

size_t msgbuf_len(void);
