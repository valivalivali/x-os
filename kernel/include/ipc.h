#pragma once
#include <stdint.h>
#include <stdbool.h>

/* X OS Microkernel — IPC message format and capability types.
 * This header is shared between kernel and userspace.
 */

#define IPC_MSG_MAX_PAYLOAD 256
#define IPC_CAP_MAX_PER_MSG 4

typedef uint64_t cap_handle_t;
#define CAP_NULL ((cap_handle_t)0)

typedef uint64_t port_handle_t;
#define PORT_NULL ((port_handle_t)0)

typedef enum {
    IPC_MSG_REQUEST  = 0,
    IPC_MSG_RESPONSE = 1,
    IPC_MSG_EVENT    = 2,
} ipc_msg_type_t;

typedef struct {
    uint64_t    type;      /* ipc_msg_type_t */
    uint64_t    sender_pid;
    cap_handle_t caps[IPC_CAP_MAX_PER_MSG];
    uint32_t    cap_count;
    uint32_t    payload_len;
    uint8_t     payload[IPC_MSG_MAX_PAYLOAD];
} ipc_msg_t;

/* Well-known service ports (registered by init at boot).
 * Apps and services look these up by name through the nameserver port.
 */
#define PORT_NS_DISPLAY       1
#define PORT_NS_FS            2
#define PORT_NS_COMPOSER      3
#define PORT_NS_INPUT         4
#define PORT_NS_SHELL_BRIDGE  5

/* Kernel-side API (kernel/ipc/ipc.c) */
void ipc_init(void);
port_handle_t port_create(uint64_t owner_pid);
bool port_send(port_handle_t port, const ipc_msg_t *msg);
bool port_send_blocking(port_handle_t port, const ipc_msg_t *msg);
bool port_recv(port_handle_t port, ipc_msg_t *out, bool block);
void port_close(port_handle_t port);
