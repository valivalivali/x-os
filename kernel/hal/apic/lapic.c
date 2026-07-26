#include "kernel/hal/apic/lapic.h"
#include "kernel/hal/apic/smp.h"
#include "kernel/arch/x86_64/io.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/memory/pmm.h"
#include "kernel/lib/kprintf.h"
#include <stdint.h>

/* Default LAPIC MMIO physical base address.
 * On modern systems, this is fetched from the IA32_APIC_BASE MSR,
 * but for QEMU with Limine, the default 0xFEE00000 is always used. */
#define LAPIC_DEFAULT_PHYS  0xFEE00000ULL

/* IA32_APIC_BASE MSR (index 0x1B) */
#define MSR_IA32_APIC_BASE  0x1B
#define APIC_BASE_BSP       (1u << 8)
#define APIC_BASE_EXTD      (1u << 10)   /* x2APIC enable */
#define APIC_BASE_EN        (1u << 11)   /* xAPIC global enable */

/* x2APIC MSR base — register offset is added to this. */
#define X2APIC_MSR_BASE     0x800

static volatile uint32_t *lapic_regs = NULL;  /* xAPIC MMIO */
static bool x2apic_mode = false;

static inline uint32_t lapic_read(uint32_t offset) {
    if (x2apic_mode) {
        uint32_t low, high;
        __asm__ volatile("rdmsr" : "=a"(low), "=d"(high)
                         : "c"(X2APIC_MSR_BASE + (offset >> 4)));
        return low;
    }
    return lapic_regs[offset / 4];
}

static inline void lapic_write(uint32_t offset, uint32_t val) {
    if (x2apic_mode) {
        __asm__ volatile("wrmsr" : : "a"(val), "d"(0),
                         "c"(X2APIC_MSR_BASE + (offset >> 4)));
        return;
    }
    lapic_regs[offset / 4] = val;
}

/* Some registers are 64-bit in x2APIC (ICR, self-IPI). */
static inline void lapic_write64(uint32_t offset, uint64_t val) {
    if (x2apic_mode) {
        __asm__ volatile("wrmsr" : : "a"((uint32_t)val),
                         "d"((uint32_t)(val >> 32)),
                         "c"(X2APIC_MSR_BASE + (offset >> 4)));
        return;
    }
    /* xAPIC: split into HI/LO 32-bit writes. */
    lapic_regs[(offset + 0x10) / 4] = (uint32_t)(val >> 32);
    lapic_regs[offset / 4] = (uint32_t)val;
}

static uint64_t get_apic_base(void) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(MSR_IA32_APIC_BASE));
    return ((uint64_t)high << 32) | low;
}

static void set_apic_base(uint64_t val) {
    __asm__ volatile("wrmsr" : : "a"((uint32_t)val), "d"((uint32_t)(val >> 32)),
                     "c"(MSR_IA32_APIC_BASE));
}

void lapic_init(uint64_t phys_base) {
    /* Enable x2APIC if the CPU supports it — MSR access is faster than
     * MMIO and avoids mapping a fixed physical page.  Falls back to
     * xAPIC MMIO on older hardware. */
    if (g_cpu.x2apic) {
        uint64_t base = get_apic_base();
        base |= APIC_BASE_EN | APIC_BASE_EXTD;
        set_apic_base(base);
        x2apic_mode = true;
        lapic_regs = NULL;  /* not used in x2APIC mode */
    } else {
        if (phys_base == 0)
            phys_base = get_apic_base() & 0xFFFFF000ULL;
        if (phys_base == 0)
            phys_base = LAPIC_DEFAULT_PHYS;

        /* Enable xAPIC global enable bit. */
        uint64_t base = get_apic_base();
        base |= APIC_BASE_EN;
        set_apic_base(base);

        lapic_regs = (volatile uint32_t *)phys_to_virt(phys_base);
        x2apic_mode = false;
    }

    /* Enable the LAPIC by setting the Spurious Interrupt Vector Register.
     * Bit 8 = APIC enable, vector = spurious vector (0xFF).
     * In x2APIC mode, SVR is accessed via MSR 0x80F. */
    uint32_t svr = LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR;
    lapic_write(LAPIC_SVR, svr);

    /* Set Task Priority to 0 (accept all interrupts).
     * In x2APIC, TPR is via MSR 0x808; the top 4 bits are the class. */
    lapic_write(LAPIC_TPR, 0);

    /* Set LINT0 to ExtINT mode for legacy PIC pass-through.
     * Delivery mode 7 (ExtINT), fixed, edge-triggered.
     * Only the BSP unmaskes LINT0 — APs mask it so legacy PIC
     * interrupts (keyboard IRQ1, mouse IRQ12) are delivered to exactly
     * one CPU.  Without this, multiple APs handle the same PS/2
     * interrupt concurrently, corrupting mouse packet state and
     * causing the cursor to teleport. */
    if (this_cpu()->is_bsp)
        lapic_write(LAPIC_LVT_LINT0, 0x700); /* ExtINT, unmasked */
    else
        lapic_write(LAPIC_LVT_LINT0, 0x700 | LAPIC_LVT_MASKED); /* ExtINT, masked */

    /* Set LINT1 to NMI delivery mode. */
    lapic_write(LAPIC_LVT_LINT1, 0x400); /* NMI delivery mode */

    /* Mask the error LVT for now. */
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);

    /* Clear any pending EOI. */
    lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_get_id(void) {
    if (x2apic_mode) {
        /* In x2APIC, the ID is the full 32-bit value (MSR 0x802). */
        return lapic_read(LAPIC_ID);
    }
    if (!lapic_regs) return 0;
    return lapic_read(LAPIC_ID) >> 24;
}

void lapic_eoi(void) {
    if (x2apic_mode) {
        lapic_write(LAPIC_EOI, 0);
        return;
    }
    if (!lapic_regs) return;
    lapic_write(LAPIC_EOI, 0);
}

void lapic_send_ipi(uint32_t vector, uint32_t dest_lapic_id) {
    if (x2apic_mode) {
        /* x2APIC: ICR is a single 64-bit MSR write (0x830).
         * Destination is in bits 31-0 (full 32-bit APIC ID). */
        uint64_t icr = (uint64_t)dest_lapic_id |
                       vector | LAPIC_ICR_FIXED |
                       LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_EDGE |
                       LAPIC_ICR_NO_SHORTHAND;
        lapic_write64(LAPIC_ICR_LO, icr);
        return;
    }
    if (!lapic_regs) return;
    /* xAPIC: set destination in ICR_HI (bits 24-31 = APIC ID). */
    lapic_write(LAPIC_ICR_HI, dest_lapic_id << 24);
    lapic_write(LAPIC_ICR_LO, vector | LAPIC_ICR_FIXED |
                 LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_EDGE |
                 LAPIC_ICR_NO_SHORTHAND);
}

void lapic_send_ipi_all_others(uint32_t vector) {
    if (x2apic_mode) {
        uint64_t icr = vector | LAPIC_ICR_FIXED |
                       LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_EDGE |
                       LAPIC_ICR_ALL_EXCL_SELF;
        lapic_write64(LAPIC_ICR_LO, icr);
        return;
    }
    if (!lapic_regs) return;
    lapic_write(LAPIC_ICR_HI, 0);
    lapic_write(LAPIC_ICR_LO, vector | LAPIC_ICR_FIXED |
                 LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_EDGE |
                 LAPIC_ICR_ALL_EXCL_SELF);
}

void lapic_send_ipi_all(uint32_t vector) {
    if (x2apic_mode) {
        uint64_t icr = vector | LAPIC_ICR_FIXED |
                       LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_EDGE |
                       LAPIC_ICR_ALL_INCL_SELF;
        lapic_write64(LAPIC_ICR_LO, icr);
        return;
    }
    if (!lapic_regs) return;
    lapic_write(LAPIC_ICR_HI, 0);
    lapic_write(LAPIC_ICR_LO, vector | LAPIC_ICR_FIXED |
                 LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_EDGE |
                 LAPIC_ICR_ALL_INCL_SELF);
}

void lapic_timer_init(uint32_t vector, uint32_t ticks) {
    if (x2apic_mode) {
        /* x2APIC: same register layout, accessed via MSR. */
        lapic_write(LAPIC_TIMER_DIV, 0xB);
        lapic_write(LAPIC_TIMER_INITCNT, ticks);
        lapic_write(LAPIC_LVT_TIMER, vector | LAPIC_LVT_PERIODIC);
        return;
    }
    if (!lapic_regs) return;
    lapic_write(LAPIC_TIMER_DIV, 0xB);
    lapic_write(LAPIC_TIMER_INITCNT, ticks);
    lapic_write(LAPIC_LVT_TIMER, vector | LAPIC_LVT_PERIODIC);
}

void lapic_timer_stop(void) {
    if (x2apic_mode) {
        lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
        lapic_write(LAPIC_TIMER_INITCNT, 0);
        return;
    }
    if (!lapic_regs) return;
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_INITCNT, 0);
}

volatile uint32_t *lapic_mmio(void) {
    return lapic_regs;
}

bool lapic_is_x2apic(void) {
    return x2apic_mode;
}
