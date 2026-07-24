#include "kernel/ipc/ipc.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/sched/sched.h"
#include "kernel/hal/apic/spinlock.h"

static port_t ports[IPC_MAX_PORTS];
static spinlock_t ipc_lock = SPINLOCK_INIT;

void ipc_init(void) {
    memset(ports, 0, sizeof(ports));
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
    spinlock_release_irqrestore(&ipc_lock, rflags);
    return true;
}

bool port_recv(port_handle_t h, ipc_msg_t *out, bool block) {
    (void)block; /* Non-blocking for now until scheduler is fully hooked up */
    uint64_t rflags = spinlock_acquire_irqsave(&ipc_lock);
    port_t *p = port_get(h);
    if (!p || !out) { spinlock_release_irqrestore(&ipc_lock, rflags); return false; }
    if (p->count == 0) { spinlock_release_irqrestore(&ipc_lock, rflags); return false; }

    uint32_t idx = p->head;
    *out = p->buf[idx];
    p->head = (idx + 1) % IPC_PORT_DEPTH;
    p->count--;
    spinlock_release_irqrestore(&ipc_lock, rflags);
    return true;
}

void port_close(port_handle_t h) {
    uint64_t rflags = spinlock_acquire_irqsave(&ipc_lock);
    port_t *p = port_get(h);
    if (p) memset(p, 0, sizeof(*p));
    spinlock_release_irqrestore(&ipc_lock, rflags);
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
