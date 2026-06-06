#include "kernel/hal/input/ps2.h"
#include "kernel/arch/x86_64/io.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

void ps2_wait_write(void) { for (int i = 0; i < 100000; i++) if (!(inb(PS2_STATUS) & 0x02)) return; }
void ps2_wait_read(void)  { for (int i = 0; i < 100000; i++) if (inb(PS2_STATUS) & 0x01)  return; }

void ps2_command(uint8_t cmd)    { ps2_wait_write(); outb(PS2_CMD, cmd); }
void ps2_write_data(uint8_t d)   { ps2_wait_write(); outb(PS2_DATA, d); }
uint8_t ps2_read_data(void)      { ps2_wait_read();  return inb(PS2_DATA); }

void ps2_mouse_write(uint8_t d) {
    ps2_command(0xD4);       /* next byte goes to the auxiliary (mouse) port */
    ps2_write_data(d);
    (void)ps2_read_data();   /* consume ACK (0xFA) */
}

void ps2_init(void) {
    /* Disable both devices while we configure. */
    ps2_command(0xAD);
    ps2_command(0xA7);

    /* Flush the output buffer. */
    while (inb(PS2_STATUS) & 0x01) (void)inb(PS2_DATA);

    /* Configuration byte: enable IRQ1 + IRQ12, enable both clocks,
     * keep scancode translation on (so we always read set 1). */
    ps2_command(0x20);
    uint8_t cfg = ps2_read_data();
    cfg |=  0x01;   /* port 1 (keyboard) interrupt */
    cfg |=  0x02;   /* port 2 (mouse) interrupt    */
    cfg &= ~0x10;   /* enable port 1 clock         */
    cfg &= ~0x20;   /* enable port 2 clock         */
    cfg |=  0x40;   /* first-port translation      */
    ps2_command(0x60);
    ps2_write_data(cfg);

    /* Re-enable both devices. */
    ps2_command(0xAE);
    ps2_command(0xA8);
}
