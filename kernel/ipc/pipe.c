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
#include "kernel/hal/apic/spinlock.h"

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
static spinlock_t pipe_lock = SPINLOCK_INIT;

static pipe_t *pipe_lookup(int fd) {
    if (fd < PIPE_FD_BASE || fd >= PIPE_FD_BASE + PIPE_MAX * 2)
        return NULL;
    int idx = (fd - PIPE_FD_BASE) / 2;
    if (idx < 0 || idx >= PIPE_MAX) return NULL;
    if (!g_pipes[idx].used) return NULL;
    return &g_pipes[idx];
}

static int pipe_alloc(void) {
    uint64_t rflags = spinlock_acquire_irqsave(&pipe_lock);
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!g_pipes[i].used) {
            memset(&g_pipes[i], 0, sizeof(pipe_t));
            g_pipes[i].used = true;
            g_pipes[i].creator_pid = (uint32_t)proc_current()->pid;
            g_pipes[i].read_refs = 1;
            g_pipes[i].write_refs = 1;
            spinlock_release_irqrestore(&pipe_lock, rflags);
            return i;
        }
    }
    spinlock_release_irqrestore(&pipe_lock, rflags);
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
    uint64_t rflags = spinlock_acquire_irqsave(&pipe_lock);
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!g_pipes[i].used) continue;
        if (g_pipes[i].creator_pid != parent_pid) continue;
        if (g_pipes[i].read_refs)
            g_pipes[i].read_refs++;
        if (g_pipes[i].write_refs)
            g_pipes[i].write_refs++;
    }
    spinlock_release_irqrestore(&pipe_lock, rflags);
}

int pipe_read(int fd, void *buf, size_t count) {
    pipe_t *p = pipe_lookup(fd);
    if (!p) return -1;
    if (((fd - PIPE_FD_BASE) & 1) != 0) return -1;
    if (!buf || count == 0) return 0;

    for (;;) {
        uint64_t rflags = spinlock_acquire_irqsave(&pipe_lock);
        if (!p->used) {
            spinlock_release_irqrestore(&pipe_lock, rflags);
            return 0;
        }
        if (p->data_bytes > 0) {
            size_t to_read = count < p->data_bytes ? count : p->data_bytes;
            uint8_t *dst = (uint8_t *)buf;
            for (size_t i = 0; i < to_read; i++) {
                dst[i] = p->buf[p->read_pos];
                p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
            }
            p->data_bytes -= (uint32_t)to_read;
            spinlock_release_irqrestore(&pipe_lock, rflags);
            return (int)to_read;
        }
        if (p->write_refs == 0) {
            spinlock_release_irqrestore(&pipe_lock, rflags);
            return 0; /* EOF */
        }
        spinlock_release_irqrestore(&pipe_lock, rflags);
        sched_yield();
    }
}

int pipe_write(int fd, const void *buf, size_t count) {
    pipe_t *p = pipe_lookup(fd);
    if (!p) return -1;
    if (((fd - PIPE_FD_BASE) & 1) != 1) return -1;
    if (!buf || count == 0) return 0;

    const uint8_t *src = (const uint8_t *)buf;
    size_t written = 0;
    while (written < count) {
        uint64_t rflags = spinlock_acquire_irqsave(&pipe_lock);
        if (!p->used || p->read_refs == 0) {
            spinlock_release_irqrestore(&pipe_lock, rflags);
            return written ? (int)written : -1;
        }
        if (p->data_bytes < PIPE_BUF_SIZE) {
            size_t space = PIPE_BUF_SIZE - p->data_bytes;
            size_t chunk = count - written;
            if (chunk > space) chunk = space;
            for (size_t i = 0; i < chunk; i++) {
                p->buf[p->write_pos] = src[written + i];
                p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
            }
            p->data_bytes += (uint32_t)chunk;
            written += chunk;
            spinlock_release_irqrestore(&pipe_lock, rflags);
        } else {
            spinlock_release_irqrestore(&pipe_lock, rflags);
            sched_yield();
        }
    }
    return (int)written;
}

void pipe_close(int fd) {
    pipe_t *p = pipe_lookup(fd);
    if (!p) return;
    uint64_t rflags = spinlock_acquire_irqsave(&pipe_lock);
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
    spinlock_release_irqrestore(&pipe_lock, rflags);
}

int pipe_dup(int oldfd, int newfd) {
    pipe_t *p = pipe_lookup(oldfd);
    if (!p) return -1;
    (void)newfd;
    uint64_t rflags = spinlock_acquire_irqsave(&pipe_lock);
    if (((oldfd - PIPE_FD_BASE) & 1) == 0)
        p->read_refs++;
    else
        p->write_refs++;
    spinlock_release_irqrestore(&pipe_lock, rflags);
    return oldfd;
}

/* Poll helpers for select/poll. */
int pipe_readable(int fd) {
    pipe_t *p = pipe_lookup(fd);
    if (!p) return 0;
    if (((fd - PIPE_FD_BASE) & 1) != 0) return 0;  /* write end */
    uint64_t rflags = spinlock_acquire_irqsave(&pipe_lock);
    int ready = (p->data_bytes > 0 || !p->used) ? 1 : 0;
    spinlock_release_irqrestore(&pipe_lock, rflags);
    return ready;
}

int pipe_writable(int fd) {
    pipe_t *p = pipe_lookup(fd);
    if (!p) return 0;
    if (((fd - PIPE_FD_BASE) & 1) == 0) return 0;  /* read end */
    uint64_t rflags = spinlock_acquire_irqsave(&pipe_lock);
    int ready = (p->data_bytes < PIPE_BUF_SIZE) ? 1 : 0;
    spinlock_release_irqrestore(&pipe_lock, rflags);
    return ready;
}
