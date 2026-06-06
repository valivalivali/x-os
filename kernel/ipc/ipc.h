#pragma once
#include "kernel/include/ipc.h"

/* Kernel-side IPC implementation.
 * Ports are kernel-managed mailboxes. Capabilities are unforgeable handles
 * stored in a per-process capability table.
 */

#define IPC_MAX_PORTS 64

typedef struct {
    bool used;
    uint64_t owner_pid;
    /* Simple circular buffer per port. */
    ipc_msg_t buf[8];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} port_t;

void ipc_init(void);
port_t *port_get(port_handle_t h);

/* Simple nameserver: well-known service IDs → port handles. */
void ns_register(uint32_t id, port_handle_t port);
port_handle_t ns_lookup(uint32_t id);
