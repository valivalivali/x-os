#include "kernel/hal/apic/lapic.h"
#include "kernel/arch/x86_64/io.h"
#include "kernel/memory/pmm.h"
#include "kernel/lib/kprintf.h"
#include <stdint.h>

/* Default LAPIC MMIO physical base address.
 * On modern systems, this is fetched from the IA32_APIC_BASE MSR,
 * but for QEMU with Limine, the default 0xFEE00000 is always used. */
#define LAPIC_DEFAULT_PHYS  0xFEE00000ULL

static volatile uint32_t *lapic_regs = NULL;
static uint64_t lapic_phys = 0;

static inline uint32_t lapic_read(uint32_t offset) {
    return lapic_regs[offset / 4];
}

static inline void lapic_write(uint32_t offset, uint32_t val) {
    lapic_regs[offset / 4] = val;
}

static uint64_t get_apic_base(void) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(27));
    return ((uint64_t)high << 32) | (low & 0xFFFFF000ULL);
}

void lapic_init(uint64_t phys_base) {
    if (phys_base == 0)
        phys_base = get_apic_base() & 0xFFFFF000ULL;
    if (phys_base == 0)
        phys_base = LAPIC_DEFAULT_PHYS;

    lapic_phys = phys_base;

    /* Map the LAPIC MMIO region if not already mapped.
     * The kernel PML4 should have a mapping for this physical address.
     * We use phys_to_virt() which works through the HHDM. */
    lapic_regs = (volatile uint32_t *)phys_to_virt(phys_base);

    /* Enable the LAPIC by setting the Spurious Interrupt Vector Register.
     * Bit 8 = APIC enable, vector = spurious vector (0xFF). */
    uint32_t svr = LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR;
    lapic_write(LAPIC_SVR, svr);

    /* Set Task Priority to 0 (accept all interrupts). */
    lapic_write(LAPIC_TPR, 0);

    /* Mask LINT0 and LINT1 (legacy PIC lines — not used in APIC mode). */
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);

    /* Mask the error LVT for now. */
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);

    /* Clear any pending EOI. */
    lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_get_id(void) {
    if (!lapic_regs) return 0;
    return lapic_read(LAPIC_ID) >> 24;
}

void lapic_eoi(void) {
    if (!lapic_regs) return;
    lapic_write(LAPIC_EOI, 0);
}

void lapic_send_ipi(uint32_t vector, uint32_t dest_lapic_id) {
    if (!lapic_regs) return;
    /* Set destination in ICR_HI (bits 24-31 = destination APIC ID). */
    lapic_write(LAPIC_ICR_HI, dest_lapic_id << 24);
    /* Send the IPI: fixed delivery, edge-triggered, assert level. */
    lapic_write(LAPIC_ICR_LO, vector | LAPIC_ICR_FIXED |
                 LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_EDGE |
                 LAPIC_ICR_NO_SHORTHAND);
}

void lapic_send_ipi_all_others(uint32_t vector) {
    if (!lapic_regs) return;
    lapic_write(LAPIC_ICR_HI, 0);
    lapic_write(LAPIC_ICR_LO, vector | LAPIC_ICR_FIXED |
                 LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_EDGE |
                 LAPIC_ICR_ALL_EXCL_SELF);
}

void lapic_send_ipi_all(uint32_t vector) {
    if (!lapic_regs) return;
    lapic_write(LAPIC_ICR_HI, 0);
    lapic_write(LAPIC_ICR_LO, vector | LAPIC_ICR_FIXED |
                 LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_EDGE |
                 LAPIC_ICR_ALL_INCL_SELF);
}

void lapic_timer_init(uint32_t vector, uint32_t ticks) {
    if (!lapic_regs) return;
    /* Set divide value to 1 (0xB = divide by 1). */
    lapic_write(LAPIC_TIMER_DIV, 0xB);
    /* Set initial count. */
    lapic_write(LAPIC_TIMER_INITCNT, ticks);
    /* Configure LVT timer: periodic, unmasked, given vector. */
    lapic_write(LAPIC_LVT_TIMER, vector | LAPIC_LVT_PERIODIC);
}

void lapic_timer_stop(void) {
    if (!lapic_regs) return;
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_INITCNT, 0);
}

volatile uint32_t *lapic_mmio(void) {
    return lapic_regs;
}
