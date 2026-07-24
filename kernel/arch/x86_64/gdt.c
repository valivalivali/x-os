#include "kernel/arch/x86_64/gdt.h"
#include "kernel/memory/pmm.h"
#include "kernel/lib/string.h"
#include <stdint.h>

/* Segments */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x18
#define SEL_UDATA 0x20
#define SEL_UCODE2 0x28
#define SEL_TSS    0x30

static uint64_t gdt[8];   /* null, kcode, kdata, ucode, udata, ucode2, tss-lo, tss-hi */

static struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdtr;

/* x86_64 TSS — only RSP0 and ISTs matter.  Must be at least 104 bytes. */
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

static tss_t tss;
static uint8_t tss_stack[16384] __attribute__((aligned(16))); /* 16 KiB RSP0 stack */

static void load_gdt(void) {
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
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        ::: "rax", "memory");
}

static void load_tss(void) {
    __asm__ volatile("ltr %w0" : : "r"((uint16_t)SEL_TSS)); /* 0x30 */
}

/* Build a TSS descriptor (16 bytes, spanning two GDT slots).
 * Limit is sizeof(tss_t) - 1.  Base is the 64-bit TSS address. */
static void set_tss_desc(uint64_t *g, uint64_t base, uint32_t limit) {
    /* First 8 bytes (standard descriptor format) */
    uint64_t low  = (limit & 0xFFFF)
                  | ((base & 0xFFFFFFULL) << 16)
                  | ((uint64_t)0x89 << 40)          /* P=1, DPL=0, S=0, type=1001 */
                  | ((uint64_t)((limit >> 16) & 0xF) << 48)
                  | ((base >> 24) & 0xFF) << 56;
    /* Second 8 bytes (x86_64 extension) */
    uint64_t high = (base >> 32);

    g[0] = low;
    g[1] = high;
}

void gdt_init(void) {
    memset(gdt, 0, sizeof(gdt));
    memset(&tss, 0, sizeof(tss));

    gdt[0] = 0;                         /* null */
    gdt[1] = 0x00AF9A000000FFFFULL;     /* kernel code (64-bit, ring 0) */
    gdt[2] = 0x00CF92000000FFFFULL;     /* kernel data (ring 0) */
    gdt[3] = 0x00AFFA000000FFFFULL;     /* user code  (64-bit, ring 3) */
    gdt[4] = 0x00CFF2000000FFFFULL;     /* user data  (ring 3) */
    gdt[5] = 0x00AFFA000000FFFFULL;     /* user code 2 (64-bit, ring 3) for Intel sysret */

    tss.rsp0 = (uint64_t)(tss_stack + sizeof(tss_stack));
    tss.iopb_offset = sizeof(tss_t);
    set_tss_desc(&gdt[6], (uint64_t)&tss, sizeof(tss_t) - 1);

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)&gdt[0];

    load_gdt();
    load_tss();
}

void gdt_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}

void *gdt_get_tss(void) {
    return &tss;
}

