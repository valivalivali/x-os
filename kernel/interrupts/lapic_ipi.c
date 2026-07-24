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

/* Reschedule IPI — wake up the target CPU so it runs sched_yield. */
void ipi_resched_handler(void) {
    /* Respect no_preempt — the current process may be in a critical
     * initialization section (e.g., composer GPU init).  Skipping the
     * yield here is safe: the next timer tick will reschedule. */
    proc_t *cur = proc_current();
    if (cur && cur->no_preempt) return;
    /* Use try-lock to avoid deadlock if another CPU holds sched_lock. */
    sched_yield_try();
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
 * IMPORTANT: Use try-lock for sched_yield.  If another CPU is in the
 * middle of a context_switch (holding sched_lock), spinning here would
 * block all further timer interrupts on this CPU, freezing timer_ticks()
 * and causing all sleeps to hang permanently. */
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

    /* Preemptive scheduling: yield on every tick — unless the current
     * process requested no preemption (e.g., composer during GPU init).
     * Use try-lock to avoid freezing all timer ticks if another CPU
     * is holding sched_lock during a context_switch. */
    proc_t *cur = proc_current();
    if (!cur || !cur->no_preempt)
        sched_yield_try();
}
