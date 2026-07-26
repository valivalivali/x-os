#include "kernel/ipc/ipc.h"
#include "kernel/include/syscall.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/sched/sched.h"
#include "kernel/hal/apic/spinlock.h"

static port_t ports[IPC_MAX_PORTS];
static spinlock_t ipc_lock = SPINLOCK_INIT;

void ipc_init(void) {
    memset(ports, 0, sizeof(ports));
}

/* Dump port table for SYS_PORT_LIST introspection. Returns count filled. */
int ipc_port_list(void *out, int max) {
    if (!out || max <= 0) return 0;
    port_info_t *entries = (port_info_t *)out;
    uint64_t rflags = spinlock_acquire_irqsave(&ipc_lock);
    int n = 0;
    for (int i = 1; i < IPC_MAX_PORTS && n < max; i++) {
        if (!ports[i].used) continue;
        entries[n].handle    = (uint32_t)i;
        entries[n].used      = 1;
        entries[n].owner_pid = ports[i].owner_pid;
        entries[n].count     = ports[i].count;
        entries[n].depth     = IPC_PORT_DEPTH;
        n++;
    }
    spinlock_release_irqrestore(&ipc_lock, rflags);
    return n;
}

port_t *port_get(port_handle_t h) {
    if (h < 1 || h >= IPC_MAX_PORTS) return NULL;
    return ports[h].used ? &ports[h] : NULL;
}

port_handle_t port_create(uint64_t owner_pid) {
    uint64_t rflags = spinlock_acquire_irqsave(&ipc_lock);
    for (int i = 1; i < IPC_MAX_PORTS; i++) {
        if (!ports[i].used) {
            ports[i].used = true;
            ports[i].owner_pid = owner_pid;
            ports[i].head = 0;
            ports[i].tail = 0;
            ports[i].count = 0;
            spinlock_release_irqrestore(&ipc_lock, rflags);
            return (port_handle_t)i;
        }
    }
    spinlock_release_irqrestore(&ipc_lock, rflags);
    return PORT_NULL;
}

bool port_send(port_handle_t h, const ipc_msg_t *msg) {
    uint64_t rflags = spinlock_acquire_irqsave(&ipc_lock);
    port_t *p = port_get(h);
    if (!p || !msg) { spinlock_release_irqrestore(&ipc_lock, rflags); return false; }
    if (p->count >= IPC_PORT_DEPTH) { spinlock_release_irqrestore(&ipc_lock, rflags); return false; }

    uint32_t idx = p->tail;
    p->buf[idx] = *msg;
    p->tail = (idx + 1) % IPC_PORT_DEPTH;
    p->count++;
    bool wake = p->recv_waiters > 0;
    const void *chan = &p->recvq;
    spinlock_release_irqrestore(&ipc_lock, rflags);

    /* Outside the IPC lock: sched_wake_chan takes sched_lock, and the
     * ordering rule is ipc_lock before sched_lock. */
    if (wake) sched_wake_chan(chan);
    return true;
}

bool port_recv(port_handle_t h, ipc_msg_t *out, bool block) {
    /* Capture the caller's interrupt state once.  The blocking path hands
     * ipc_lock to the scheduler and returns with interrupts still disabled,
     * so the usual acquire_irqsave/release_irqrestore pairing cannot put it
     * back — do it explicitly at the single exit point instead. */
    uint64_t entry_rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(entry_rflags));

    bool ok = false;
    for (;;) {
        spinlock_acquire(&ipc_lock);
        port_t *p = port_get(h);
        if (!p || !out) { spinlock_release(&ipc_lock); break; }

        if (p->count > 0) {
            uint32_t idx = p->head;
            *out = p->buf[idx];
            p->head = (idx + 1) % IPC_PORT_DEPTH;
            p->count--;
            bool wake = p->send_waiters > 0;
            const void *chan = &p->sendq;
            spinlock_release(&ipc_lock);
            if (wake) sched_wake_chan(chan);
            ok = true;
            break;
        }

        if (!block) { spinlock_release(&ipc_lock); break; }

        /* Sleep until a sender arrives.  sched_block_on releases ipc_lock
         * only after this process has been marked blocked, so a send that
         * lands in the gap still finds us on the wait channel. */
        p->recv_waiters++;
        sched_block_on(&p->recvq, &ipc_lock, 0);

        spinlock_acquire(&ipc_lock);
        p = port_get(h);
        if (p && p->recv_waiters) p->recv_waiters--;
        spinlock_release(&ipc_lock);

        /* Re-check: we may have been woken by the backstop timeout, or
         * another receiver may have taken the message first. */
    }

    if (entry_rflags & (1 << 9)) sti();
    return ok;
}

void port_close(port_handle_t h) {
    uint64_t rflags = spinlock_acquire_irqsave(&ipc_lock);
    port_t *p = port_get(h);
    const void *rq = NULL, *sq = NULL;
    if (p) {
        /* Release anyone parked on this port before the memory is reused,
         * or they would sleep until their backstop expires and then touch
         * a recycled port. */
        if (p->recv_waiters) rq = &p->recvq;
        if (p->send_waiters) sq = &p->sendq;
        memset(p, 0, sizeof(*p));
    }
    spinlock_release_irqrestore(&ipc_lock, rflags);
    if (rq) sched_wake_chan(rq);
    if (sq) sched_wake_chan(sq);
}

/* ---- Simple nameserver (well-known service IDs → ports) ---------------- */

#define NS_MAX_IDS 16
static port_handle_t ns_table[NS_MAX_IDS];

void ns_register(uint32_t id, port_handle_t port) {
    uint64_t rflags = spinlock_acquire_irqsave(&ipc_lock);
    if (id < NS_MAX_IDS) ns_table[id] = port;
    spinlock_release_irqrestore(&ipc_lock, rflags);
}

port_handle_t ns_lookup(uint32_t id) {
    uint64_t rflags = spinlock_acquire_irqsave(&ipc_lock);
    port_handle_t h = (id < NS_MAX_IDS) ? ns_table[id] : PORT_NULL;
    spinlock_release_irqrestore(&ipc_lock, rflags);
    return h;
}
