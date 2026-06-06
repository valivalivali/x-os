#pragma once

/* Legacy 8259 PIC. We run in PIC mode (no APIC) for the MVP. */
void pic_remap(int offset1, int offset2);
void pic_eoi(int irq);
void pic_mask_all(void);
void pic_set_mask(int irq);     /* disable an IRQ line */
void pic_clear_mask(int irq);   /* enable an IRQ line  */
