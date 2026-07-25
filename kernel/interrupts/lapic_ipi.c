#include "kernel/hal/apic/lapic.h"
#include "kernel/hal/apic/smp.h"
#include "kernel/sched/sched.h"
#include "kernel/lib/kprintf.h"
#include "kernel/memory/vmm.h"
#include "kernel/hal/timers/timer.h"
#include "kernel/hal/input/mouse.h"
#include <stdint.h>

/* IPI and LAPIC timer C handlers.
 * These are called from lapic_stubs.S after all GPRs are saved.
 * The assembly stub sends LAPIC EOI after the handler returns.
 */

/* Reschedule IPI — set need_resched flag so the target CPU reschedules
 * at the next safe point (syscall return, interrupt return, idle loop).
 * This follows the XNU cause_ast_check / Linux smp_send_reschedule pattern:
 * the IPI just signals that a reschedule is needed, it doesn't do the
 * actual context switch (which could deadlock if the current process
 * holds IPC or scheduler locks). */
void ipi_resched_handler(void) {
    /* Never discard a remote reschedule request.  The safe return path will
     * consume it after any no_preempt critical section ends. */
    this_cpu()->need_resched = 1;
}

/* TLB shootdown IPI — flush the TLB on this CPU. */
void ipi_tlb_handler(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
}

/* Stop IPI — halt this CPU. */
void ipi_stop_handler(void) {
    kprintf("[smp] CPU %u stopping\n", cpu_current_id());
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* Call-function IPI — placeholder for future SMP function calls. */
void ipi_call_func_handler(void) {
    /* Not yet implemented — just return. */
}

/* LAPIC timer handler — per-CPU scheduling tick.
 * Called on APs when the LAPIC timer fires.
 * BSP uses the PIT timer for device polling and scheduling.
 *
 * Deferred preemption: set need_resched flag instead of calling
 * sched_yield_try().  The flag is checked at safe points (syscall
 * return, interrupt return to userspace, idle loop).  This eliminates
 * sched_lock contention from timer handlers and prevents context
 * switches while a process holds IPC or scheduler locks. */
void lapic_timer_handler(void) {
    cpu_data_t *cpu = this_cpu();
    cpu->local_ticks++;
    timer_tick_global();

    /* Poll PS/2 mouse — but only from one CPU at a time to avoid
     * contention on ps2_lock and PS/2 I/O ports.  The BSP (cpu 0)
     * always polls; APs poll every 4th tick as a fallback in case
     * the BSP's PIT stopped (which happens intermittently in SMP).
     * Without this, mouse input freezes when the PIT stops. */
    if (cpu->is_bsp || (cpu->local_ticks & 3) == 0)
        mouse_poll();

    /* Check kernel stack canaries on APs too — the BSP PIT handler
     * checks canaries, but APs only run the LAPIC timer. */
    extern void sched_check_canaries(void);
    sched_check_canaries();

    /* Set need_resched flag — the actual reschedule happens at the
     * next safe point (syscall return, interrupt return, idle loop).
     * Also wake up expired sleepers so NSLEEP works without requiring
     * another process to voluntarily yield. */
    extern void sched_wake_sleepers(void);
    sched_wake_sleepers();
    /* Timer expiry records a deferred request even while preemption is
     * disabled; sched_check_resched() consumes it at the first safe point. */
    cpu->need_resched = 1;
}
