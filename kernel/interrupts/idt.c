#include "kernel/interrupts/idt.h"
#include "kernel/interrupts/pic.h"
#include "kernel/lib/kprintf.h"
#include <stdint.h>

#define IDT_ENTRIES 256
#define KCODE_SEL   0x08
#define IRQ_BASE    0x20

struct __attribute__((packed)) idt_entry {
    uint16_t off_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t off_mid;
    uint32_t off_high;
    uint32_t zero;
};

static struct idt_entry idt[IDT_ENTRIES];
static struct __attribute__((packed)) { uint16_t limit; uint64_t base; } idtr;

static irq_handler_t handlers[16];

static void set_gate(int n, void *handler, uint8_t type_attr) {
    uint64_t a = (uint64_t)handler;
    idt[n].off_low   = a & 0xFFFF;
    idt[n].selector  = KCODE_SEL;
    idt[n].ist       = 0;
    idt[n].type_attr = type_attr;          /* 0x8E = present, ring0, int gate */
    idt[n].off_mid   = (a >> 16) & 0xFFFF;
    idt[n].off_high  = (a >> 32) & 0xFFFFFFFF;
    idt[n].zero      = 0;
}

/* ---- CPU exceptions ---------------------------------------------------- */

static const char *exc_name(int v) {
    switch (v) {
        case 0:  return "#DE divide error";
        case 6:  return "#UD invalid opcode";
        case 8:  return "#DF double fault";
        case 13: return "#GP general protection";
        case 14: return "#PF page fault";
        default: return "exception";
    }
}

static void exc_report(int vec, uint64_t err, struct interrupt_frame *f) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    kprintf("\n*** CPU EXCEPTION %d (%s) err=%lx\n", vec, exc_name(vec), err);
    kprintf("    ip=%lx cs=%lx flags=%lx sp=%lx cr2=%lx\n",
            f->ip, f->cs, f->flags, f->sp, cr2);
    for (;;) __asm__ volatile("cli; hlt");
}

#define EXC_N(n) __attribute__((interrupt)) \
    static void exc##n(struct interrupt_frame *f) { exc_report(n, 0, f); }
#define EXC_E(n) __attribute__((interrupt)) \
    static void exc##n(struct interrupt_frame *f, uint64_t e) { exc_report(n, e, f); }

EXC_N(0)  EXC_N(1)  EXC_N(2)  EXC_N(3)  EXC_N(4)  EXC_N(5)  EXC_N(6)  EXC_N(7)
EXC_E(8)  EXC_N(9)  EXC_E(10) EXC_E(11) EXC_E(12) EXC_E(13) EXC_E(14) EXC_N(15)
EXC_N(16) EXC_E(17) EXC_N(18) EXC_N(19) EXC_N(20) EXC_E(21) EXC_N(22) EXC_N(23)
EXC_N(24) EXC_N(25) EXC_N(26) EXC_N(27) EXC_N(28) EXC_N(29) EXC_E(30) EXC_N(31)

static void *exc_table[32] = {
    exc0,  exc1,  exc2,  exc3,  exc4,  exc5,  exc6,  exc7,
    exc8,  exc9,  exc10, exc11, exc12, exc13, exc14, exc15,
    exc16, exc17, exc18, exc19, exc20, exc21, exc22, exc23,
    exc24, exc25, exc26, exc27, exc28, exc29, exc30, exc31,
};

/* ---- IRQs -------------------------------------------------------------- */

static void irq_dispatch(int irq) {
    if (handlers[irq]) handlers[irq]();
    pic_eoi(irq);
}

void irq_dispatch_asm(int irq) {
    irq_dispatch(irq);
}

/* Assembly stubs from kernel/interrupts/irq_stubs.S */
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

static void *irq_table[16] = {
    irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
    irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15,
};

void idt_init(void) {
    for (int i = 0; i < 32; i++)  set_gate(i, exc_table[i], 0x8E);
    for (int i = 0; i < 16; i++)  set_gate(IRQ_BASE + i, irq_table[i], 0x8E);

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt[0];
    __asm__ volatile("lidt %0" : : "m"(idtr));

    pic_remap(IRQ_BASE, IRQ_BASE + 8);
    pic_mask_all();
}

void idt_reload(void) {
    __asm__ volatile("lidt %0" : : "m"(idtr));
}

void irq_install(int irq, irq_handler_t fn) {
    if (irq < 0 || irq > 15) return;
    handlers[irq] = fn;
    pic_clear_mask(irq);
}

void irq_uninstall(int irq) {
    if (irq < 0 || irq > 15) return;
    pic_set_mask(irq);
    handlers[irq] = 0;
}
