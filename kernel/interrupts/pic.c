#include "kernel/interrupts/pic.h"
#include "kernel/arch/x86_64/io.h"
#include <stdint.h>

#define PIC1       0x20
#define PIC1_DATA  0x21
#define PIC2       0xA0
#define PIC2_DATA  0xA1
#define ICW1_INIT  0x10
#define ICW1_ICW4  0x01
#define ICW4_8086  0x01
#define PIC_EOI    0x20

void pic_remap(int offset1, int offset2) {
    outb(PIC1, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC1_DATA, (uint8_t)offset1); io_wait();  /* master vector offset  */
    outb(PIC2_DATA, (uint8_t)offset2); io_wait();  /* slave vector offset   */
    outb(PIC1_DATA, 4); io_wait();                 /* slave at IRQ2         */
    outb(PIC2_DATA, 2); io_wait();                 /* slave cascade id      */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();
}

void pic_eoi(int irq) {
    if (irq >= 8) outb(PIC2, PIC_EOI);
    outb(PIC1, PIC_EOI);
}

void pic_mask_all(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_set_mask(int irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(irq < 8 ? irq : irq - 8);
    outb(port, inb(port) | (uint8_t)(1u << bit));
}

void pic_clear_mask(int irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(irq < 8 ? irq : irq - 8);
    outb(port, inb(port) & (uint8_t)~(1u << bit));
    if (irq >= 8) /* ensure the cascade line (IRQ2) is enabled */
        outb(PIC1_DATA, inb(PIC1_DATA) & (uint8_t)~(1u << 2));
}
