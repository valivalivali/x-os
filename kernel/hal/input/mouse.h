#pragma once
#include "kernel/hal/apic/spinlock.h"

/* PS/2 mouse: enables streaming, installs IRQ12, decodes 3-byte packets. */
void mouse_init(void);

/* Polling fallback: called from timer tick to check for mouse data
 * when IRQ12 isn't being delivered (QEMU cocoa display issue). */
void mouse_poll(void);

/* Shared PS/2 lock — protects concurrent keyboard + mouse port access. */
extern spinlock_t ps2_lock;
