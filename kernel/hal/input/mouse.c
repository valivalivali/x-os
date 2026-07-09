/* X OS PS/2 Mouse — clean rewrite
 *
 * IRQ12 fires once per byte. We collect 3 bytes into a packet:
 *   byte 0: flags (bit 3 always set, bits 4/5 = X/Y sign)
 *   byte 1: X delta
 *   byte 2: Y delta
 *
 * QEMU's cocoa display doesn't reliably deliver IRQ12 after init.
 * We also poll from the timer tick (1000 Hz) as a fallback.
 */

#include "kernel/hal/input/mouse.h"
#include "kernel/hal/input/ps2.h"
#include "kernel/hal/input/input.h"
#include "kernel/arch/x86_64/io.h"
#include "kernel/interrupts/idt.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64

static uint8_t cycle = 0;
static uint8_t packet[3];

static void mouse_process_byte(uint8_t data) {
    if (cycle == 0) {
        /* Byte 0 must have bit 3 set. Skip garbage until we see it. */
        if (!(data & 0x08)) return;
        packet[0] = data;
        cycle = 1;
    } else if (cycle == 1) {
        packet[1] = data;
        cycle = 2;
    } else {
        packet[2] = data;
        cycle = 0;

        uint8_t flags = packet[0];
        if (flags & 0xC0) return;   /* overflow — discard */

        int dx = packet[1];
        if (flags & 0x10) dx -= 256;

        int dy = packet[2];
        if (flags & 0x20) dy -= 256;

        /* Screen Y grows downward; PS/2 dy is up-positive. */
        input_update_mouse(dx, -dy, flags & 0x07);
    }
}

static void mouse_isr(void) {
    uint8_t data = inb(PS2_DATA);
    mouse_process_byte(data);
}

/* Called from timer tick (1000 Hz) as a polling fallback.
 * Checks the PS/2 status port for mouse data and processes it. */
void mouse_poll(void) {
    for (int i = 0; i < 16; i++) {
        uint8_t st = inb(PS2_STATUS);
        if (!(st & 0x01)) break;       /* output buffer empty */
        if (!(st & 0x20)) break;      /* keyboard data, not mouse */
        uint8_t data = inb(PS2_DATA);
        mouse_process_byte(data);
    }
}

void mouse_init(void) {
    ps2_mouse_write(0xF6);  /* reset to defaults */
    ps2_mouse_write(0xF3);  /* Set Sample Rate command */
    ps2_mouse_write(200);   /* 200 samples/sec */
    ps2_mouse_write(0xF4);  /* enable data reporting */
    irq_install(12, mouse_isr);
}
