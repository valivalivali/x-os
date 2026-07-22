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
    void     *current_proc;    /* offset 8  — GS:8, current process */
    uint32_t  cpu_id;          /* offset 16 — GS:16, logical CPU index (0 = BSP) */
    uint32_t  lapic_id;        /* offset 20 — GS:20, Local APIC ID */
    bool      is_bsp;          /* offset 24 */
    bool      online;          /* offset 25 */

    /* Per-CPU GDT and TSS */
    void     *gdt;             /* pointer to this CPU's GDT array */
    void     *tss;             /* pointer to this CPU's TSS */

    /* Per-CPU idle stack */
    void     *idle_stack;

    /* Per-CPU timer ticks counter */
    uint64_t  local_ticks;
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

/* Per-CPU GDT/TSS setup — called once per CPU.
 * cpu: pointer to this CPU's data struct
 * gdt_ptr: pre-allocated GDT array (at least 8 uint64_t entries)
 * tss_ptr: pre-allocated TSS struct */
void smp_setup_cpu_gdt(cpu_data_t *cpu, void *gdt_ptr, void *tss_ptr);

/* AP entry point — called by Limine's goto_address on each AP. */
void ap_entry(struct handoff_cpu_info *info);

/* Global scheduler lock — protects the process table and ready queue. */
extern spinlock_t g_sched_lock;
