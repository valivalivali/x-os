#include "kernel/hal/pci/msix.h"
#include "kernel/hal/pci/pci.h"
#include "kernel/arch/x86_64/io.h"
#include "kernel/memory/pmm.h"
#include "kernel/memory/vmm.h"
#include "kernel/interrupts/idt.h"
#include "kernel/hal/apic/lapic.h"
#include "kernel/hal/apic/smp.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include <stdint.h>

/* Forward declaration — implemented in idt.c */
void set_gate_raw(int n, void *handler, uint8_t type_attr);

/* LAPIC MSI base address: writes to this address deliver an interrupt.
 * Bits [19:12] = destination APIC ID. */
#define MSI_ADDR_BASE  0xFEE00000u

/* PCI capability list walk: read byte at offset 0x34 for cap pointer,
 * then follow next-pointer at offset+1 of each cap. */
#define PCI_CAPABILITY_LIST  0x34
#define PCI_CAP_NEXT(offs)   ((offs) + 1)
#define PCI_CAP_ID(offs)     (offs)

/* Handler table for MSI-X vectors.  Indexed by (vector - MSIX_VECTOR_BASE). */
static msix_handler_t msix_handlers[MSIX_MAX_VECTORS];
static void          *msix_ctxs[MSIX_MAX_VECTORS];
static int            msix_next_vector = 0;  /* round-robin allocator */

/* Assembly stubs for MSI vectors (from msi_stubs.S).
 * Each stub calls msi_dispatch_asm(vector). */
extern void msi_isr_0(void),  msi_isr_1(void),  msi_isr_2(void),  msi_isr_3(void);
extern void msi_isr_4(void),  msi_isr_5(void),  msi_isr_6(void),  msi_isr_7(void);
extern void msi_isr_8(void),  msi_isr_9(void),  msi_isr_10(void), msi_isr_11(void);
extern void msi_isr_12(void), msi_isr_13(void), msi_isr_14(void), msi_isr_15(void);
extern void msi_isr_16(void), msi_isr_17(void), msi_isr_18(void), msi_isr_19(void);
extern void msi_isr_20(void), msi_isr_21(void), msi_isr_22(void), msi_isr_23(void);
extern void msi_isr_24(void), msi_isr_25(void), msi_isr_26(void), msi_isr_27(void);
extern void msi_isr_28(void), msi_isr_29(void), msi_isr_30(void), msi_isr_31(void);

static void *msi_stub_table[MSIX_MAX_VECTORS] = {
    msi_isr_0,  msi_isr_1,  msi_isr_2,  msi_isr_3,
    msi_isr_4,  msi_isr_5,  msi_isr_6,  msi_isr_7,
    msi_isr_8,  msi_isr_9,  msi_isr_10, msi_isr_11,
    msi_isr_12, msi_isr_13, msi_isr_14, msi_isr_15,
    msi_isr_16, msi_isr_17, msi_isr_18, msi_isr_19,
    msi_isr_20, msi_isr_21, msi_isr_22, msi_isr_23,
    msi_isr_24, msi_isr_25, msi_isr_26, msi_isr_27,
    msi_isr_28, msi_isr_29, msi_isr_30, msi_isr_31,
};

/* Called from assembly: dispatch an MSI vector to its handler.
 * EOI has already been sent to the LAPIC by the stub. */
void msi_dispatch_asm(int vector) {
    int idx = vector - MSIX_VECTOR_BASE;
    if (idx < 0 || idx >= MSIX_MAX_VECTORS) return;
    if (msix_handlers[idx])
        msix_handlers[idx](msix_ctxs[idx]);
}

/* Install all MSI stubs into the IDT.  Called once during boot. */
void msix_idt_init(void) {
    for (int i = 0; i < MSIX_MAX_VECTORS; i++)
        set_gate_raw(MSIX_VECTOR_BASE + i, msi_stub_table[i], 0x8E);
}

bool msix_parse(pci_dev_t *dev, msix_cap_t *out) {
    memset(out, 0, sizeof(*out));

    /* Walk the PCI capability list. */
    uint8_t cap_ptr = (uint8_t)pci_read(dev->bus, dev->dev, dev->func,
                                        PCI_CAPABILITY_LIST);
    while (cap_ptr != 0 && cap_ptr < 0xFF) {
        uint32_t cap_hdr = pci_read(dev->bus, dev->dev, dev->func, cap_ptr);
        uint8_t  cap_id  = cap_hdr & 0xFF;
        uint8_t  next    = (cap_hdr >> 8) & 0xFF;

        if (cap_id == PCI_CAP_MSIX) {
            /* MSI-X capability layout (4-byte header):
             *   [15:0]  = Message Control (cap_id at [7:0], next at [15:8])
             *   [31:16] = Table Offset / Table BIR
             *   [47:32] = PBA Offset / PBA BIR
             * Read the full 16-byte capability. */
            uint32_t msg_ctrl = (cap_hdr >> 16) & 0xFFFF;
            uint32_t tbl_info = pci_read(dev->bus, dev->dev, dev->func,
                                         cap_ptr + 4);
            uint32_t pba_info = pci_read(dev->bus, dev->dev, dev->func,
                                         cap_ptr + 8);

            out->present     = true;
            out->cap_offset  = cap_ptr;
            out->table_size  = (msg_ctrl & MSIX_TBL_SIZE_MASK) + 1;
            out->table_bar   = tbl_info & 0x07;
            out->table_offset= tbl_info & 0xFFFFFFF8;
            out->pba_bar     = pba_info & 0x07;
            out->pba_offset  = pba_info & 0xFFFFFFF8;

            /* Map the MSI-X table via the BAR's physical address. */
            if (out->table_bar < 6 && dev->bar_valid[out->table_bar]) {
                uint64_t bar_addr = dev->bar[out->table_bar];
                uint64_t table_phys = bar_addr + out->table_offset;
                out->table = (volatile msix_table_entry_t *)
                    phys_to_virt(table_phys);
            }
            return true;
        }
        cap_ptr = next;
    }
    return false;
}

void msix_enable(pci_dev_t *dev, msix_cap_t *cap) {
    if (!cap->present) return;

    /* Enable MSI-X and mask all vectors while we set up. */
    uint32_t cap_hdr = pci_read(dev->bus, dev->dev, dev->func, cap->cap_offset);
    uint32_t msg_ctrl = (cap_hdr >> 16) & 0xFFFF;
    msg_ctrl |= MSIX_ENABLE | MSIX_FUNC_MASK;
    pci_write(dev->bus, dev->dev, dev->func, cap->cap_offset,
              (cap_hdr & 0xFFFF) | (msg_ctrl << 16));

    /* Mask all table entries. */
    if (cap->table) {
        for (int i = 0; i < cap->table_size; i++)
            cap->table[i].vector_ctrl |= 1;
    }

    /* Disable legacy INTx by clearing the interrupt disable bit.
     * In practice, enabling MSI-X implicitly disables INTx, but we
     * also set the INTx disable bit in the command register. */
    uint32_t cmd = pci_read(dev->bus, dev->dev, dev->func, PCI_COMMAND);
    cmd |= (1u << 10);  /* INTx disable */
    pci_write(dev->bus, dev->dev, dev->func, PCI_COMMAND, cmd);

    /* Unmask the function-level mask. */
    msg_ctrl &= ~MSIX_FUNC_MASK;
    pci_write(dev->bus, dev->dev, dev->func, cap->cap_offset,
              (cap_hdr & 0xFFFF) | (msg_ctrl << 16));
}

uint8_t msix_alloc_vector(msix_handler_t handler, void *ctx) {
    if (msix_next_vector >= MSIX_MAX_VECTORS) return 0;
    int idx = msix_next_vector++;
    msix_handlers[idx] = handler;
    msix_ctxs[idx] = ctx;
    return MSIX_VECTOR_BASE + idx;
}

void msix_program_entry(msix_cap_t *cap, int entry_idx,
                        uint8_t vector, uint8_t cpu) {
    if (!cap->table || entry_idx >= cap->table_size) return;

    /* Mask the entry while programming. */
    cap->table[entry_idx].vector_ctrl |= 1;

    /* Program the message address and data.
     * Address: 0xFEE00000 | (cpu << 12) — delivers to cpu's LAPIC.
     * Data: vector number. */
    cap->table[entry_idx].msg_addr_hi = 0;
    cap->table[entry_idx].msg_addr_lo = MSI_ADDR_BASE | ((uint32_t)cpu << 12);
    cap->table[entry_idx].msg_data    = vector;

    /* Unmask the entry. */
    cap->table[entry_idx].vector_ctrl &= ~1u;
}

void msix_mask_entry(msix_cap_t *cap, int entry_idx) {
    if (cap->table && entry_idx < cap->table_size)
        cap->table[entry_idx].vector_ctrl |= 1;
}

void msix_unmask_entry(msix_cap_t *cap, int entry_idx) {
    if (cap->table && entry_idx < cap->table_size)
        cap->table[entry_idx].vector_ctrl &= ~1u;
}
