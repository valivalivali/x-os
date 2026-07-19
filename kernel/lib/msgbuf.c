#include "kernel/lib/msgbuf.h"
#include "kernel/lib/string.h"

static char g_buf[MSGBUF_SIZE];
static size_t g_head;   /* next write index */
static size_t g_count;  /* bytes currently stored (≤ MSGBUF_SIZE) */
static int g_ready;

void msgbuf_init(void) {
    g_head = 0;
    g_count = 0;
    g_ready = 1;
}

void msgbuf_putc(char c) {
    if (!g_ready)
        msgbuf_init();
    g_buf[g_head] = c;
    g_head = (g_head + 1) % MSGBUF_SIZE;
    if (g_count < MSGBUF_SIZE)
        g_count++;
}

void msgbuf_write(const char *s, size_t n) {
    if (!s) return;
    for (size_t i = 0; i < n; i++)
        msgbuf_putc(s[i]);
}

size_t msgbuf_len(void) {
    return g_count;
}

size_t msgbuf_copy(char *dst, size_t max) {
    if (!dst || max == 0 || g_count == 0)
        return 0;
    size_t n = g_count;
    if (n > max)
        n = max;

    /* Oldest byte index when buffer is full: g_head. When not full: 0. */
    size_t start = (g_count == MSGBUF_SIZE) ? g_head : 0;
    for (size_t i = 0; i < n; i++)
        dst[i] = g_buf[(start + i) % MSGBUF_SIZE];
    return n;
}
