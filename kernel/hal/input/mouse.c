/* X OS PS/2 Mouse — clean rewrite
 *
 * IRQ12 fires once per byte. We collect 3 bytes into a packet:
 *   byte 0: flags (bit 3 always set, bits 4/5 = X/Y sign)
 *   byte 1: X delta
 *   byte 2: Y delta
 *
 * We do NOT check port 0x64 status bits. In QEMU the "mouse data"
 * bit (0x20) is unreliable; checking it drops real mouse bytes
 * and causes packet desync (teleportation). IRQ12 means mouse data.
 */

#include "kernel/hal/input/mouse.h"
#include "kernel/hal/input/ps2.h"
#include "kernel/hal/input/input.h"
#include "kernel/arch/x86_64/io.h"
#include "kernel/interrupts/idt.h"

static uint8_t cycle = 0;
static uint8_t packet[3];

static void mouse_isr(void) {
    uint8_t data = inb(0x60);

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

void mouse_init(void) {
    ps2_mouse_write(0xF6);  /* reset to defaults */
    /* Set sample rate to 200 Hz for smoother cursor movement.
     * Default is ~60 Hz which causes large per-packet deltas.
     * 200 Hz gives 5ms granularity — much smoother at the cost
     * of slightly more IRQ traffic. */
    ps2_mouse_write(0xF3);  /* Set Sample Rate command */
    ps2_mouse_write(200);   /* 200 samples/sec */
    ps2_mouse_write(0xF4);  /* enable data reporting */
    irq_install(12, mouse_isr);
}
