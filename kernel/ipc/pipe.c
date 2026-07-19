/* Kernel pipe implementation — ring buffer for shell pipelines.
 *
 * pipe() creates a pair of file descriptors: pipefd[0] for reading,
 * pipefd[1] for writing. Data written to the write end can be read
 * from the read end in FIFO order.
 *
 * Ends are refcounted so fork()+close() matches POSIX: parent closing
 * its write fd must not mark the pipe write-closed while the child
 * still holds a copy (zsh's execcmd_fork sync pipe depends on this).
 */

#include "kernel/ipc/pipe.h"
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
    uint32_t creator_pid;
    uint32_t read_refs;
    uint32_t write_refs;
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t data_bytes;
} pipe_t;

static pipe_t g_pipes[PIPE_MAX];

static pipe_t *pipe_lookup(int fd) {
    if (fd < PIPE_FD_BASE || fd >= PIPE_FD_BASE + PIPE_MAX * 2)
        return NULL;
    int idx = (fd - PIPE_FD_BASE) / 2;
    if (idx < 0 || idx >= PIPE_MAX) return NULL;
    if (!g_pipes[idx].used) return NULL;
    return &g_pipes[idx];
}

static int pipe_alloc(void) {
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!g_pipes[i].used) {
            memset(&g_pipes[i], 0, sizeof(pipe_t));
            g_pipes[i].used = true;
            g_pipes[i].creator_pid = (uint32_t)proc_current()->pid;
            g_pipes[i].read_refs = 1;
            g_pipes[i].write_refs = 1;
            return i;
        }
    }
    return -1;
}

int pipe_create(int pipefd[2]) {
    if (!pipefd) return -1;
    int idx = pipe_alloc();
    if (idx < 0) return -1;
    pipefd[0] = PIPE_FD_BASE + idx * 2;       /* read end */
    pipefd[1] = PIPE_FD_BASE + idx * 2 + 1;   /* write end */
    return 0;
}

/* After fork, child inherits open pipe ends — bump refs on parent's pipes. */
void pipe_fork_inherit(uint32_t parent_pid) {
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!g_pipes[i].used) continue;
        if (g_pipes[i].creator_pid != parent_pid) continue;
        if (g_pipes[i].read_refs)
            g_pipes[i].read_refs++;
        if (g_pipes[i].write_refs)
            g_pipes[i].write_refs++;
    }
}

int pipe_read(int fd, void *buf, size_t count) {
    pipe_t *p = pipe_lookup(fd);
    if (!p) return -1;
    if (((fd - PIPE_FD_BASE) & 1) != 0) return -1;
    if (!buf || count == 0) return 0;

    for (;;) {
        if (p->data_bytes > 0)
            break;
        if (p->write_refs == 0)
            return 0; /* EOF */
        sched_yield();
        if (!p->used)
            return 0;
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
    if (((fd - PIPE_FD_BASE) & 1) != 1) return -1;
    if (p->read_refs == 0) return -1;
    if (!buf || count == 0) return 0;

    const uint8_t *src = (const uint8_t *)buf;
    size_t written = 0;
    while (written < count) {
        while (p->data_bytes >= PIPE_BUF_SIZE) {
            if (p->read_refs == 0) return written ? (int)written : -1;
            sched_yield();
            if (!p->used) return -1;
        }
        size_t space = PIPE_BUF_SIZE - p->data_bytes;
        size_t chunk = count - written;
        if (chunk > space) chunk = space;
        for (size_t i = 0; i < chunk; i++) {
            p->buf[p->write_pos] = src[written + i];
            p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
        }
        p->data_bytes += (uint32_t)chunk;
        written += chunk;
    }
    return (int)written;
}

void pipe_close(int fd) {
    pipe_t *p = pipe_lookup(fd);
    if (!p) return;
    if (((fd - PIPE_FD_BASE) & 1) == 0) {
        if (p->read_refs > 0)
            p->read_refs--;
    } else {
        if (p->write_refs > 0)
            p->write_refs--;
    }
    if (p->read_refs == 0 && p->write_refs == 0) {
        p->used = false;
    }
}

int pipe_dup(int oldfd, int newfd) {
    pipe_t *p = pipe_lookup(oldfd);
    if (!p) return -1;
    (void)newfd;
    if (((oldfd - PIPE_FD_BASE) & 1) == 0)
        p->read_refs++;
    else
        p->write_refs++;
    return oldfd;
}
