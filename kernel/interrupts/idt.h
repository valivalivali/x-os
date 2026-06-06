#pragma once
#include <stdint.h>

/* Hardware-pushed interrupt frame (no error code). */
struct interrupt_frame {
    uint64_t ip;
    uint64_t cs;
    uint64_t flags;
    uint64_t sp;
    uint64_t ss;
};

typedef void (*irq_handler_t)(void);

void idt_init(void);                          /* loads IDT, remaps + masks PIC */
void irq_install(int irq, irq_handler_t fn);  /* registers + unmasks an IRQ    */
void irq_uninstall(int irq);
