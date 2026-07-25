#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "kernel/hal/apic/spinlock.h"

#define MAX_CPUS 64

/* Forward declaration — defined in boot/handoff/handoff.h */
struct handoff_cpu_info;

/* Per-CPU data structure.
 *
 * Each CPU's GS base register points directly to its cpu_data_t struct.
 * This is inspired by FreeBSD's pcpu struct and XNU's cpu_data.
 *
 * Access via: this_cpu() returns a pointer to the current CPU's data.
 * The GS base MSR (0xC0000101) holds the address of the struct.
 */
typedef struct cpu_data {
    uint64_t  rsp0;            /* offset 0  — GS:0, per-CPU kernel RSP0 */
    volatile void *current_proc;    /* offset 8  — GS:8, current process */
    uint32_t  cpu_id;          /* offset 16 — GS:16, logical CPU index (0 = BSP) */
    uint32_t  lapic_id;        /* offset 20 — GS:20, Local APIC ID */
    bool      is_bsp;          /* offset 24 */
    bool      online;          /* offset 25 */
    uint8_t   _pad[6];         /* align to 32 */

    /* Per-CPU user GPRs saved at syscall entry for fork().
     * Accessed via GS: in syscall_entry.S. */
    uint64_t  user_rbx;        /* offset 32 — GS:32 */
    uint64_t  user_rbp;        /* offset 40 — GS:40 */
    uint64_t  user_r12;        /* offset 48 — GS:48 */
    uint64_t  user_r13;        /* offset 56 — GS:56 */
    uint64_t  user_r14;        /* offset 64 — GS:64 */

    /* Per-CPU GDT and TSS */
    void     *gdt;             /* pointer to this CPU's GDT array */
    void     *tss;             /* pointer to this CPU's TSS */

    /* Per-CPU idle stack */
    void     *idle_stack;

    /* Per-CPU timer ticks counter */
    uint64_t  local_ticks;

    /* Saved rflags for sched_yield — context_switch changes stacks,
     * so we can't keep rflags as a local variable across the switch. */
    uint64_t  saved_rflags;

    /* Per-CPU TSS RSP0 stack top — the "idle" RSP0 used when no ring-3
     * process is running.  When switching to a kernel-only process (idle),
     * TSS RSP0 must be reset to this value so interrupts land on the
     * per-CPU TSS stack, not on a stale ring-3 process's kernel stack. */
    uint64_t  tss_rsp0;        /* offset 112 — GS:112 */

    /* Per-CPU idle proc (pid=0) — used when all real processes are
     * blocked.  Needed so sched_yield can context-switch to idle when
     * the current process calls proc_sleep. */
    void     *idle_proc;       /* offset 120 — GS:120 */

    /* Per-CPU need_resched flag — set by timer/IPI handlers, checked
     * at safe points (syscall return, interrupt return to userspace,
     * idle loop).  Follows the XNU AST / Linux TIF_NEED_RESCHED pattern:
     * the timer handler sets the flag instead of calling sched_yield_try(),
     * eliminating sched_lock contention from 8 CPUs × 1000Hz timer ticks
     * and preventing context switches while holding IPC/scheduler locks. */
    volatile uint64_t need_resched;  /* offset 128 — GS:128 */
} cpu_data_t;

/* Global CPU array — indexed by logical CPU ID. */
extern cpu_data_t *g_cpus[MAX_CPUS];
extern uint32_t    g_cpu_count;
extern bool        g_smp_enabled;

/* Initialize SMP subsystem — discovers CPUs from Limine handoff,
 * sets up per-CPU data structures, and starts all APs.
 * Must be called after: GDT, IDT, PMM, heap, VMM are initialized. */
void smp_init(void);

/* Get the current CPU's per-CPU data pointer (via GS base MSR). */
static inline cpu_data_t *this_cpu(void) {
    cpu_data_t *cpu;
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000101));
    cpu = (cpu_data_t *)(((uint64_t)high << 32) | low);
    return cpu;
}

/* Get the current CPU's logical ID. */
static inline uint32_t cpu_current_id(void) {
    uint32_t id;
    __asm__ volatile("movl %%gs:16, %0" : "=r"(id));
    return id;
}

/* Update the kernel RSP0 used by the CPU when an interrupt/syscall
 * arrives from ring-3.  This must update BOTH the per-CPU software
 * shadow (GS:0) AND the actual TSS hardware field — the CPU reads
 * RSP0 from the TSS descriptor, not from GS:0.
 *
 * TSS layout (packed): uint32_t reserved0; uint64_t rsp0; ...
 * So rsp0 is at byte offset 4 from the TSS pointer.
 *
 * For APs, cpu->tss points to the per-CPU TSS.
 * For the BSP, cpu->tss is NULL and we fall back to gdt_set_rsp0(). */
void gdt_set_rsp0(uint64_t rsp0);

static inline void cpu_set_rsp0(uint64_t rsp0) {
    cpu_data_t *cpu = this_cpu();
    cpu->rsp0 = rsp0;
    if (cpu->tss) {
        *(uint64_t *)((uint8_t *)cpu->tss + 4) = rsp0;
    } else {
        gdt_set_rsp0(rsp0);
    }
}

/* Per-CPU GDT/TSS setup — called once per CPU.
 * cpu: pointer to this CPU's data struct
 * gdt_ptr: pre-allocated GDT array (at least 8 uint64_t entries)
 * tss_ptr: pre-allocated TSS struct */
void smp_setup_cpu_gdt(cpu_data_t *cpu, void *gdt_ptr, void *tss_ptr);

/* AP entry point — called by Limine's goto_address on each AP. */
void ap_entry(struct handoff_cpu_info *info);

/* Global scheduler lock — protects the process table and ready queue. */
extern spinlock_t g_sched_lock;
