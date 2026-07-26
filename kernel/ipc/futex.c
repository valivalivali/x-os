/* Futex implementation for userspace synchronization.
 *
 * Supports FUTEX_WAIT and FUTEX_WAKE, the two operations needed by
 * pthread mutexes, condition variables, and barriers.  Uses the
 * scheduler's block_on/wake_chan infrastructure.
 *
 * The wait channel is the kernel virtual address of the futex word,
 * which is stable across threads sharing the same address space.
 */

#include "kernel/sched/sched.h"
#include "kernel/memory/vmm.h"
#include "kernel/memory/pmm.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/lib/kprintf.h"
#include "kernel/include/syscall.h"
#include <stdint.h>

#define FUTEX_MAX_WAITERS 256

/* A futex waiter entry.  We use a static table since the number of
 * concurrent futex waiters is bounded by SCHED_MAX_PROCS. */
typedef struct {
    proc_t *proc;        /* waiting process, NULL = free slot */
    uint64_t uaddr;      /* userspace address of the futex word */
} futex_waiter_t;

static futex_waiter_t futex_waiters[FUTEX_MAX_WAITERS];
static spinlock_t futex_lock = SPINLOCK_INIT;

/* Resolve a userspace virtual address to a kernel-accessible physical
 * address for atomic comparison.  Returns 0 if the page is not mapped. */
static uint32_t *futex_resolve(proc_t *p, uint64_t uaddr) {
    if (!p || !p->pml4_virt) return NULL;
    if (uaddr & 3) return NULL;  /* must be 4-byte aligned */
    uint64_t pa = vmm_virt_to_phys(p->pml4_virt, uaddr);
    if (!pa) return NULL;
    return (uint32_t *)phys_to_virt(pa);
}

int sys_futex_impl(uint32_t *uaddr, int op, uint32_t val,
                   uint64_t timeout_ns) {
    proc_t *cur = proc_current();
    if (!cur) return -1;

    /* Validate the futex word is readable before doing anything. */
    uint32_t *kaddr = futex_resolve(cur, (uint64_t)uaddr);
    if (!kaddr) return -1;

    switch (op) {
    case FUTEX_WAIT: {
        /* Atomic compare: if *uaddr != val, return EAGAIN. */
        uint32_t current = *kaddr;
        if (current != val) return -11;  /* EAGAIN */

        /* Find a free waiter slot. */
        uint64_t rflags = spinlock_acquire_irqsave(&futex_lock);
        int slot = -1;
        for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
            if (futex_waiters[i].proc == NULL) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            spinlock_release_irqrestore(&futex_lock, rflags);
            return -12;  /* ENOMEM */
        }
        futex_waiters[slot].proc = cur;
        futex_waiters[slot].uaddr = (uint64_t)uaddr;
        spinlock_release_irqrestore(&futex_lock, rflags);

        /* Block on the futex address as the wait channel.
         * Use a timeout if provided, otherwise default backstop. */
        uint64_t timeout_ms = timeout_ns ? timeout_ns / 1000000 : 0;
        sched_block_on((const void *)uaddr, NULL, timeout_ms);

        /* Woken up — remove our waiter entry. */
        rflags = spinlock_acquire_irqsave(&futex_lock);
        for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
            if (futex_waiters[i].proc == cur) {
                futex_waiters[i].proc = NULL;
                break;
            }
        }
        spinlock_release_irqrestore(&futex_lock, rflags);
        return 0;
    }

    case FUTEX_WAKE: {
        int woken = 0;
        uint64_t rflags = spinlock_acquire_irqsave(&futex_lock);
        for (int i = 0; i < FUTEX_MAX_WAITERS && woken < (int)val; i++) {
            if (futex_waiters[i].proc &&
                futex_waiters[i].uaddr == (uint64_t)uaddr) {
                /* Mark slot free and wake the process. */
                futex_waiters[i].proc = NULL;
                sched_wake_chan((const void *)uaddr);
                woken++;
            }
        }
        spinlock_release_irqrestore(&futex_lock, rflags);
        return woken;
    }

    default:
        return -38;  /* ENOSYS */
    }
}
