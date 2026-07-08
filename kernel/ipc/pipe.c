/* Kernel pipe implementation — ring buffer for shell pipelines.
 *
 * pipe() creates a pair of file descriptors: pipefd[0] for reading,
 * pipefd[1] for writing. Data written to the write end can be read
 * from the read end in FIFO order.
 *
 * We use the upper half of the XFS fd table (indices 64..95) for pipe fds
 * to avoid collision with file fds (0..31).
 */

#include "kernel/fs/xfs.h"
#include "kernel/sched/sched.h"
#include "kernel/memory/heap.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

#define PIPE_MAX      16
#define PIPE_BUF_SIZE 4096
#define PIPE_FD_BASE  64   /* pipe fds start at 64 */

typedef struct {
    bool     used;
    uint32_t owner_pid;
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t data_bytes;     /* bytes currently in buffer */
    bool     read_closed;
    bool     write_closed;
} pipe_t;

static pipe_t g_pipes[PIPE_MAX];

/* Find a pipe fd slot for a given fd number */
static pipe_t *pipe_lookup(int fd) {
    if (fd < PIPE_FD_BASE || fd >= PIPE_FD_BASE + PIPE_MAX * 2)
        return NULL;
    int idx = (fd - PIPE_FD_BASE) / 2;
    if (idx < 0 || idx >= PIPE_MAX) return NULL;
    if (!g_pipes[idx].used) return NULL;
    if (g_pipes[idx].owner_pid != proc_current()->pid) return NULL;
    return &g_pipes[idx];
}

/* Allocate a pipe and return its index, or -1 on failure */
static int pipe_alloc(void) {
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!g_pipes[i].used) {
            memset(&g_pipes[i], 0, sizeof(pipe_t));
            g_pipes[i].used = true;
            g_pipes[i].owner_pid = proc_current()->pid;
            return i;
        }
    }
    return -1;
}

int pipe_create(int pipefd[2]) {
    if (!pipefd) return -1;
    int idx = pipe_alloc();
    if (idx < 0) return -1;
    /* read fd = base + idx*2, write fd = base + idx*2 + 1 */
    pipefd[0] = PIPE_FD_BASE + idx * 2;       /* read end */
    pipefd[1] = PIPE_FD_BASE + idx * 2 + 1;   /* write end */
    return 0;
}

int pipe_read(int fd, void *buf, size_t count) {
    pipe_t *p = pipe_lookup(fd);
    if (!p) return -1;
    /* Only the read end (even offset) can read */
    if (((fd - PIPE_FD_BASE) & 1) != 0) return -1;
    if (!buf || count == 0) return 0;

    if (p->data_bytes == 0) {
        if (p->write_closed) return 0;  /* EOF */
        return -1;  /* would block (no blocking pipes yet) */
    }

    size_t to_read = count < p->data_bytes ? count : p->data_bytes;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < to_read; i++) {
        dst[i] = p->buf[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
    }
    p->data_bytes -= (uint32_t)to_read;
    return (int)to_read;
}

int pipe_write(int fd, const void *buf, size_t count) {
    pipe_t *p = pipe_lookup(fd);
    if (!p) return -1;
    /* Only the write end (odd offset) can write */
    if (((fd - PIPE_FD_BASE) & 1) != 1) return -1;
    if (p->read_closed) return -1;
    if (!buf || count == 0) return 0;

    size_t space = PIPE_BUF_SIZE - p->data_bytes;
    if (count > space) count = space;  /* partial write for now */

    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < count; i++) {
        p->buf[p->write_pos] = src[i];
        p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
    }
    p->data_bytes += (uint32_t)count;
    return (int)count;
}

void pipe_close(int fd) {
    pipe_t *p = pipe_lookup(fd);
    if (!p) return;
    if (((fd - PIPE_FD_BASE) & 1) == 0) {
        p->read_closed = true;
    } else {
        p->write_closed = true;
    }
    if (p->read_closed && p->write_closed) {
        p->used = false;
    }
}

/* Duplicate a pipe fd to a new fd number.
 * Returns new fd, or -1 on failure. */
int pipe_dup(int oldfd, int newfd) {
    pipe_t *p = pipe_lookup(oldfd);
    if (!p) return -1;

    /* For pipes, dup just returns the same end with a new fd number.
     * We don't support arbitrary newfd for pipes yet — just allocate
     * a new fd that points to the same pipe + end. */
    (void)newfd;
    /* For simplicity, return oldfd (both ends share the same buffer) */
    return oldfd;
}
