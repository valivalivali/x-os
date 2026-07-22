#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Local APIC (Advanced Programmable Interrupt Controller) driver.
 *
 * On x86_64 SMP systems, each CPU has its own Local APIC mapped at a
 * physical address discovered via the MMIO base (default 0xFEE00000).
 * The LAPIC provides:
 *   - Inter-Processor Interrupts (IPIs)
 *   - Per-CPU timer (LAPIC timer)
 *   - End-of-Interrupt (EOI) acknowledgement
 *
 * Inspired by FreeBSD's local_apic.c and XNU's lapic.c.
 */

/* LAPIC register offsets (relative to MMIO base) */
#define LAPIC_ID               0x020
#define LAPIC_VERSION          0x030
#define LAPIC_TPR              0x080
#define LAPIC_EOI              0x0B0
#define LAPIC_LDR              0x0D0
#define LAPIC_SVR              0x0F0
#define LAPIC_ICR_LO           0x300
#define LAPIC_ICR_HI           0x310
#define LAPIC_LVT_TIMER        0x320
#define LAPIC_LVT_LINT0        0x350
#define LAPIC_LVT_LINT1        0x360
#define LAPIC_LVT_ERROR        0x370
#define LAPIC_TIMER_INITCNT    0x380
#define LAPIC_TIMER_CURRCNT    0x390
#define LAPIC_TIMER_DIV        0x3E0

/* SVR flags */
#define LAPIC_SVR_ENABLE       (1 << 8)
#define LAPIC_SVR_FOCUS        (1 << 9)
#define LAPIC_SVR_EOI_BROADCAST (1 << 12)

/* LVT timer modes */
#define LAPIC_LVT_ONESHOT      (0 << 17)
#define LAPIC_LVT_PERIODIC     (1 << 17)
#define LAPIC_LVT_TSC_DEADLINE (2 << 17)
#define LAPIC_LVT_MASKED       (1 << 16)

/* ICR delivery modes bits */
#define LAPIC_ICR_FIXED        (0 << 8)
#define LAPIC_ICR_LOWEST       (1 << 8)
#define LAPIC_ICR_SMI          (2 << 8)
#define LAPIC_ICR_NMI          (4 << 8)
#define LAPIC_ICR_INIT         (5 << 8)
#define LAPIC_ICR_STARTUP      (6 << 8)

/* ICR destination shorthand */
#define LAPIC_ICR_NO_SHORTHAND (0 << 18)
#define LAPIC_ICR_SELF         (1 << 18)
#define LAPIC_ICR_ALL_INCL_SELF (2 << 18)
#define LAPIC_ICR_ALL_EXCL_SELF (3 << 18)

/* ICR level/trigger */
#define LAPIC_ICR_LEVEL_ASSERT (1 << 14)
#define LAPIC_ICR_LEVEL_DEASS  (0 << 14)
#define LAPIC_ICR_EDGE         (0 << 15)
#define LAPIC_ICR_LEVEL        (1 << 15)

/* IPI vector numbers (in IDT) */
#define IPI_VECTOR_RESCHED     0xE0
#define IPI_VECTOR_TLB         0xE1
#define IPI_VECTOR_STOP        0xE2
#define IPI_VECTOR_CALL_FUNC   0xE3

/* LAPIC timer vector (per-CPU timer interrupt) */
#define LAPIC_TIMER_VECTOR     0xF0

/* Spurious vector */
#define LAPIC_SPURIOUS_VECTOR  0xFF

/* Initialize the LAPIC for the current CPU.
 * Must be called once per CPU after paging is enabled.
 * phys_base: physical MMIO address of the LAPIC (from CPUID or default). */
void lapic_init(uint64_t phys_base);

/* Get the LAPIC ID of the current CPU. */
uint32_t lapic_get_id(void);

/* Send an EOI (End of Interrupt) to the LAPIC. */
void lapic_eoi(void);

/* Send an IPI to a specific CPU by LAPIC ID.
 * vector: interrupt vector number
 * dest_lapic_id: destination LAPIC ID */
void lapic_send_ipi(uint32_t vector, uint32_t dest_lapic_id);

/* Send an IPI to all other CPUs (excluding self). */
void lapic_send_ipi_all_others(uint32_t vector);

/* Send an IPI to all CPUs (including self). */
void lapic_send_ipi_all(uint32_t vector);

/* Configure the LAPIC timer for periodic interrupts.
 * vector: interrupt vector for the timer
 * ticks: initial count for the timer period */
void lapic_timer_init(uint32_t vector, uint32_t ticks);

/* Stop the LAPIC timer (mask it). */
void lapic_timer_stop(void);

/* Get the virtual address of the LAPIC MMIO region. */
volatile uint32_t *lapic_mmio(void);
