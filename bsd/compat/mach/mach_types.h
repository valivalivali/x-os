#pragma once
#include "../compat.h"

typedef proc_t *task_t;
typedef proc_t *thread_t;
typedef uint64_t vm_map_t;
typedef uint64_t host_t;
typedef uint64_t ipc_space_t;
typedef uint64_t ipc_port_t;
typedef uint64_t processor_t;
typedef uint64_t processor_set_t;

#define TASK_NULL    NULL
#define THREAD_NULL  NULL
#define VM_MAP_NULL  0
#define HOST_NULL    0
#define IPC_PORT_NULL 0

typedef kern_return_t (*thread_continue_t)(void *param, int code);
