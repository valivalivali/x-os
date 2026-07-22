#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "kernel/arch/x86_64/io.h"

/* Simple ticket spinlock for SMP synchronization.
 *
 * Inspired by FreeBSD's mtx (mutex) and XNU's simple_lock.
 * Uses the x86 PAUSE instruction for efficiency in spin loops.
 *
 * Usage:
 *   static spinlock_t my_lock = SPINLOCK_INIT;
 *   spinlock_acquire(&my_lock);
 *   ... critical section ...
 *   spinlock_release(&my_lock);
 *
 * For IRQ-safe contexts, use spinlock_acquire_irqsave/release_irqrestore
 * which also disable interrupts to prevent deadlock with IRQ handlers.
 */

typedef struct {
    volatile uint32_t next_ticket;
    volatile uint32_t serving_ticket;
} spinlock_t;

#define SPINLOCK_INIT { 0, 0 }

static inline void spinlock_acquire(spinlock_t *lock) {
    uint32_t ticket;
    __asm__ volatile(
        "lock xaddl %0, %1\n"
        : "=r"(ticket), "+m"(lock->next_ticket)
        : "0"(1)
        : "memory"
    );
    while (lock->serving_ticket != ticket) {
        __asm__ volatile("pause");
    }
}

static inline void spinlock_release(spinlock_t *lock) {
    __asm__ volatile(
        "incl %0\n"
        : "+m"(lock->serving_ticket)
        :: "memory"
    );
}

/* Save RFLAGS and disable interrupts before acquiring the lock. */
static inline uint64_t spinlock_acquire_irqsave(spinlock_t *lock) {
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags));
    spinlock_acquire(lock);
    return rflags;
}

/* Restore interrupt state after releasing the lock. */
static inline void spinlock_release_irqrestore(spinlock_t *lock, uint64_t rflags) {
    spinlock_release(lock);
    if (rflags & (1 << 9))
        sti();
}

static inline bool spinlock_try(spinlock_t *lock) {
    uint32_t ticket = lock->next_ticket;
    uint32_t expected = lock->serving_ticket;
    __asm__ volatile(
        "lock cmpxchgl %2, %0\n"
        : "+m"(lock->next_ticket), "+a"(expected)
        : "r"(ticket + 1)
        : "memory", "cc"
    );
    if (expected == ticket) {
        while (lock->serving_ticket != ticket)
            __asm__ volatile("pause");
        return true;
    }
    return false;
}
