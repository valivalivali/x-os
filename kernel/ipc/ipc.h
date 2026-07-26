#pragma once
#include "kernel/include/ipc.h"

/* Kernel-side IPC implementation.
 * Ports are kernel-managed mailboxes. Capabilities are unforgeable handles
 * stored in a per-process capability table.
 */

#define IPC_MAX_PORTS 64
#define IPC_PORT_DEPTH 64

typedef struct {
    bool used;
    uint64_t owner_pid;
    /* Simple circular buffer per port. */
    ipc_msg_t buf[IPC_PORT_DEPTH];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    /* Wait channels.  Receivers sleep on &recvq until a message arrives;
     * senders sleep on &sendq until the queue drains.  The addresses are
     * only ever used as identities for sched_block_on/sched_wake_chan. */
    uint32_t recv_waiters;
    uint32_t send_waiters;
    char recvq;
    char sendq;
} port_t;

void ipc_init(void);
port_t *port_get(port_handle_t h);
int ipc_port_list(void *out, int max); /* for SYS_PORT_LIST */

/* Simple nameserver: well-known service IDs → port handles. */
void ns_register(uint32_t id, port_handle_t port);
port_handle_t ns_lookup(uint32_t id);
