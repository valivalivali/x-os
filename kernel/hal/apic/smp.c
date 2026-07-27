#include "kernel/hal/apic/smp.h"
#include "kernel/hal/apic/lapic.h"
#include "kernel/hal/apic/spinlock.h"
#include "kernel/arch/x86_64/gdt.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/interrupts/idt.h"
#include "kernel/sched/sched.h"
#include "kernel/memory/pmm.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/vmm.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "boot/handoff/handoff.h"
#include <stdint.h>

/* Global state */
cpu_data_t  *g_cpus[MAX_CPUS];
uint32_t     g_cpu_count = 1;
bool         g_smp_enabled = false;
spinlock_t   g_sched_lock = SPINLOCK_INIT;

/* Per-CPU storage arrays — allocated statically to avoid heap dependency
 * during early AP startup. Each CPU gets its own GDT, TSS, and stack. */

/* GDT: 8 entries × 8 bytes = 64 bytes per CPU */
static uint64_t  per_cpu_gdt[MAX_CPUS][8] __attribute__((aligned(16)));

/* TSS: 104 bytes per CPU */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} tss_t;

static tss_t      per_cpu_tss[MAX_CPUS];
static cpu_data_t per_cpu_data[MAX_CPUS];

/* Per-CPU 32 KiB RSP0 stacks */
static uint8_t    per_cpu_tss_stack[MAX_CPUS][32768] __attribute__((aligned(16)));

/* Per-CPU idle stacks (for AP idle loops) */
static uint8_t    per_cpu_idle_stack[MAX_CPUS][32768] __attribute__((aligned(16)));

/* Build a TSS descriptor (16 bytes, spanning two GDT slots). */
static void set_tss_desc(uint64_t *g, uint64_t base, uint32_t limit) {
    uint64_t low  = (limit & 0xFFFF)
                  | ((base & 0xFFFFFFULL) << 16)
                  | ((uint64_t)0x89 << 40)
                  | ((uint64_t)((limit >> 16) & 0xF) << 48)
                  | ((base >> 24) & 0xFF) << 56;
    uint64_t high = (base >> 32);
    g[0] = low;
    g[1] = high;
}

static void load_gdt_for_cpu(uint64_t *gdt_base) {
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } gdtr;
    gdtr.limit = 8 * 8 - 1;  /* 8 entries */
    gdtr.base = (uint64_t)gdt_base;
    __asm__ volatile("lgdt %0" : : "m"(gdtr));
    __asm__ volatile(
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        ::: "rax", "memory");
}

static void load_tss_for_cpu(void) {
    __asm__ volatile("ltr %w0" : : "r"((uint16_t)0x30));
}

void smp_setup_cpu_gdt(cpu_data_t *cpu, void *gdt_ptr, void *tss_ptr) {
    uint64_t *gdt = (uint64_t *)gdt_ptr;
    tss_t *tss = (tss_t *)tss_ptr;
    uint32_t cpu_id = cpu->cpu_id;

    memset(gdt, 0, 64);
    memset(tss, 0, sizeof(tss_t));

    gdt[0] = 0;                         /* null */
    gdt[1] = 0x00AF9A000000FFFFULL;     /* kernel code (64-bit, ring 0) */
    gdt[2] = 0x00CF92000000FFFFULL;     /* kernel data (ring 0) */
    gdt[3] = 0x00AFFA000000FFFFULL;     /* user code  (64-bit, ring 3) */
    gdt[4] = 0x00CFF2000000FFFFULL;     /* user data  (ring 3) */
    gdt[5] = 0x00AFFA000000FFFFULL;     /* user code 2 (Intel sysret) */

    /* Set up TSS with per-CPU RSP0 stack */
    tss->rsp0 = (uint64_t)(per_cpu_tss_stack[cpu_id] + sizeof(per_cpu_tss_stack[cpu_id]));
    tss->iopb_offset = sizeof(tss_t);
    set_tss_desc(&gdt[6], (uint64_t)tss, sizeof(tss_t) - 1);

    cpu->gdt = gdt_ptr;
    cpu->tss = tss_ptr;
    cpu->rsp0 = tss->rsp0;
    cpu->tss_rsp0 = tss->rsp0;  /* save idle RSP0 for non-ring-3 switches */
}

/* Set the GS base register to point to per-CPU data.
 * On x86_64, the GS base is set via the IA32_KERNEL_GS_BASE MSR (0xC0000102)
 * for the kernel, and IA32_GS_BASE (0xC0000101) for the active GS base.
 * We write to GS_BASE since we're in kernel mode. */
static void set_gs_base(void *addr) {
    uint64_t val = (uint64_t)addr;
    __asm__ volatile(
        "wrmsr"
        :
        : "c"(0xC0000101), "a"((uint32_t)val), "d"((uint32_t)(val >> 32))
    );
}

/* AP entry point — called by Limine for each AP.
 * At this point, the AP is in 64-bit mode with paging enabled,
 * but has its own GDT and no per-CPU state set up. */
void ap_entry(struct handoff_cpu_info *info) {
    /* info->extra_argument contains the cpu_id set by smp_init */
    uint32_t cpu_id = (uint32_t)info->extra_argument;
    cpu_data_t *cpu = &per_cpu_data[cpu_id];

    /* Set up per-CPU GDT and TSS */
    smp_setup_cpu_gdt(cpu, per_cpu_gdt[cpu_id], &per_cpu_tss[cpu_id]);
    load_gdt_for_cpu(per_cpu_gdt[cpu_id]);
    load_tss_for_cpu();

    /* Set GS base to point to our per-CPU data */
    set_gs_base(cpu);

    /* Initialize LAPIC for this CPU */
    lapic_init(0);

    /* Reload the IDT (shared across all CPUs) */
    extern void idt_reload(void);
    idt_reload();

    /* Enable SSE (same as BSP does in kmain) */
    extern void enable_sse(void);
    enable_sse();

    /* Apply the same CR4/EFER/XCR0 feature set the BSP enabled.  Must run
     * before syscall_init_ap(), which does a read-modify-write of EFER. */
    cpu_enable_features();

    /* Configure syscall MSRs (EFER.SCE, STAR, LSTAR, SFMASK) for this CPU. */
    extern void syscall_init_ap(void);
    syscall_init_ap();

    /* Initialize per-CPU scheduler state (idle proc). */
    extern void sched_init_ap(void);
    sched_init_ap();

    /* Mark CPU as online */
    cpu->online = true;

    kprintf("[smp] CPU %u (APIC ID %u) online\n", cpu_id, cpu->lapic_id);

    /* Enable LAPIC timer for scheduling preemption (1000 Hz).
     * QEMU LAPIC timer runs at ~1GHz with divide-by-1, so initial
     * count = 1000000 for 1ms period (1000 Hz). */
    lapic_timer_init(LAPIC_TIMER_VECTOR, 4000000); /* 4ms = 250Hz */

    /* Enable interrupts and enter scheduler idle loop.
     * The LAPIC timer will fire periodically and call sched_yield,
     * which will pick processes from the shared ready queue.
     * IPI_RESCHED from other CPUs will also wake us up. */
    __asm__ volatile("sti");

    /* AP idle loop — shared with the BSP.  It consumes need_resched and
     * calls sched_yield(), which is what actually dispatches work.  A bare
     * hlt loop here would silently strand every runnable process. */
    sched_idle_loop();
}

void smp_init(void) {
    const handoff_t *h = handoff_get();

    g_cpu_count = h->cpu_count;
    if (g_cpu_count > MAX_CPUS)
        g_cpu_count = MAX_CPUS;

    kprintf("[smp] %u CPU(s) detected (BSP LAPIC ID %u)\n",
            g_cpu_count, h->bsp_lapic_id);

    /* Initialize BSP (CPU 0) per-CPU data.
     * We don't reload the BSP's GDT or enable the LAPIC here — the BSP
     * already has a working GDT from gdt_init() and uses the legacy PIC
     * for interrupts. Enabling the LAPIC on the BSP would require
     * changing the EOI sequence, which we avoid for now. */
    memset(&per_cpu_data[0], 0, sizeof(cpu_data_t));
    per_cpu_data[0].cpu_id = 0;
    per_cpu_data[0].lapic_id = h->bsp_lapic_id;
    per_cpu_data[0].is_bsp = true;
    per_cpu_data[0].online = true;
    per_cpu_data[0].idle_stack = per_cpu_idle_stack[0];
    per_cpu_data[0].rsp0 = (uint64_t)(per_cpu_tss_stack[0] + sizeof(per_cpu_tss_stack[0]));
    per_cpu_data[0].tss_rsp0 = per_cpu_data[0].rsp0;  /* save idle RSP0 */
    per_cpu_data[0].tss = gdt_get_tss();  /* BSP uses gdt.c's TSS, not per_cpu_tss */
    g_cpus[0] = &per_cpu_data[0];

    /* Set GS base for BSP so this_cpu() works */
    set_gs_base(&per_cpu_data[0]);

    if (g_cpu_count <= 1) {
        kprintf("[smp] single CPU — no APs to start\n");
        g_smp_enabled = false;
        return;
    }

    /* Start all APs using Limine's SMP goto_address mechanism.
     * Limine has already brought all CPUs into 64-bit mode; we just
     * need to set the goto_address for each AP and it will jump there. */
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        uint32_t lapic_id = h->cpus[i]->lapic_id;

        if (lapic_id == h->bsp_lapic_id) {
            /* Skip BSP — it's already running */
            continue;
        }

        /* Find the logical CPU ID for this AP.
         * We assign logical IDs in order, skipping the BSP. */
        uint32_t logical_id = 0;
        for (uint32_t j = 0, next_id = 0; j < g_cpu_count; j++) {
            if (h->cpus[j]->lapic_id == h->bsp_lapic_id)
                continue;
            if (h->cpus[j]->lapic_id == lapic_id) {
                logical_id = next_id + 1; /* BSP is 0, APs start at 1 */
                break;
            }
            next_id++;
        }

        if (logical_id >= MAX_CPUS)
            continue;

        /* Initialize per-CPU data for this AP */
        memset(&per_cpu_data[logical_id], 0, sizeof(cpu_data_t));
        per_cpu_data[logical_id].cpu_id = logical_id;
        per_cpu_data[logical_id].lapic_id = lapic_id;
        per_cpu_data[logical_id].is_bsp = false;
        per_cpu_data[logical_id].online = false;
        per_cpu_data[logical_id].idle_stack = per_cpu_idle_stack[logical_id];
        g_cpus[logical_id] = &per_cpu_data[logical_id];

        /* Set extra_argument to the logical CPU ID so ap_entry knows which CPU it is */
        h->cpus[i]->extra_argument = logical_id;

        /* Set the goto_address — Limine will call this function on the AP.
         * The AP will start executing at ap_entry() in 64-bit mode. */
        h->cpus[i]->goto_address = (void (*)(struct handoff_cpu_info *))ap_entry;

        kprintf("[smp] starting AP logical %u (LAPIC ID %u)\n",
                logical_id, lapic_id);
    }

    /* Wait for all APs to come online */
    int wait_ms = 0;
    uint32_t online_count = 1; /* BSP is already online */
    while (online_count < g_cpu_count && wait_ms < 5000) {
        online_count = 1;
        for (uint32_t i = 1; i < g_cpu_count; i++) {
            if (per_cpu_data[i].online)
                online_count++;
        }
        if (online_count < g_cpu_count) {
            /* Small delay — use port 0x80 wait */
            for (volatile int j = 0; j < 100000; j++);
            wait_ms++;
        }
    }

    kprintf("[smp] %u/%u CPUs online\n", online_count, g_cpu_count);
    g_smp_enabled = (online_count > 1);
}
