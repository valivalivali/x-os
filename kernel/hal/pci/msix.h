#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "kernel/hal/pci/pci.h"

/* MSI-X interrupt support for PCI devices.
 *
 * MSI-X allows each device to have multiple independent interrupt vectors,
 * each delivered as a memory write to the LAPIC.  This eliminates legacy
 * PIC IRQ sharing and gives per-queue interrupt routing.
 *
 * Vectors 0x40-0x7F are reserved for MSI-X (64 vectors max).
 * Each vector gets its own IDT entry and assembly stub.
 */

#define MSIX_VECTOR_BASE  0x40    /* first IDT vector for MSI-X */
#define MSIX_MAX_VECTORS  32      /* max MSI-X vectors we support */

/* PCI capability IDs */
#define PCI_CAP_MSIX      0x11

/* MSI-X capability register bits */
#define MSIX_ENABLE       (1u << 31)
#define MSIX_FUNC_MASK    (1u << 30)
#define MSIX_TBL_SIZE_MASK 0x7FF   /* low 11 bits + 1 = table size */

/* MSI-X table entry (written by kernel, read by device) */
typedef struct __attribute__((packed)) {
    uint32_t msg_addr_lo;   /* LAPIC address low  (0xFEE00000) */
    uint32_t msg_addr_hi;   /* LAPIC address high (0x00000000) */
    uint32_t msg_data;      /* vector number */
    uint32_t vector_ctrl;   /* bit 0 = mask */
} msix_table_entry_t;

/* MSI-X capability info for a device */
typedef struct {
    bool     present;           /* device has MSI-X capability */
    uint8_t  cap_offset;        /* config space offset of MSI-X cap */
    uint8_t  table_bar;         /* BAR index containing the MSI-X table */
    uint32_t table_offset;      /* offset within that BAR */
    uint8_t  pba_bar;           /* BAR index containing the PBA */
    uint32_t pba_offset;        /* offset within that BAR */
    uint16_t table_size;        /* number of table entries */
    volatile msix_table_entry_t *table;  /* mapped MSI-X table */
} msix_cap_t;

/* Interrupt handler type for MSI-X vectors */
typedef void (*msix_handler_t)(void *ctx);

/* Parse MSI-X capability from PCI config space.
 * Returns true if the device supports MSI-X. */
bool msix_parse(pci_dev_t *dev, msix_cap_t *out);

/* Install MSI-X interrupt stubs into the IDT.  Called once during boot. */
void msix_idt_init(void);

/* Enable MSI-X on a device (disables legacy INTx).
 * Must be called after msix_parse and after the BAR is mapped. */
void msix_enable(pci_dev_t *dev, msix_cap_t *cap);

/* Allocate a free MSI-X vector and register a handler.
 * Returns the vector number, or 0 on failure.
 * The vector is routed to the current CPU's LAPIC. */
uint8_t msix_alloc_vector(msix_handler_t handler, void *ctx);

/* Program an MSI-X table entry to deliver to a given vector.
 * entry_idx: index into the MSI-X table (0-based)
 * vector: IDT vector number from msix_alloc_vector
 * cpu: target CPU (LAPIC ID) */
void msix_program_entry(msix_cap_t *cap, int entry_idx,
                        uint8_t vector, uint8_t cpu);

/* Mask an MSI-X table entry (prevent interrupts). */
void msix_mask_entry(msix_cap_t *cap, int entry_idx);

/* Unmask an MSI-X table entry. */
void msix_unmask_entry(msix_cap_t *cap, int entry_idx);
