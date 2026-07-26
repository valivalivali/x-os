#include "kernel/hal/input/keyboard.h"
#include "kernel/hal/input/input.h"
#include "kernel/hal/input/ps2.h"
#include "kernel/arch/x86_64/io.h"
#include "kernel/interrupts/idt.h"
#include "kernel/lib/string.h"

/* Set-1 scancode -> ASCII (unshifted). Index = make code. */
static const char map_lower[128] = {
    0,   27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' ',
};

static const char map_upper[128] = {
    0,   27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,  ' ',
};

static bool shift = false;
static bool e0    = false;
static volatile uint32_t kbd_count = 0;

/* Shared byte handler — called from both the IRQ1 ISR and the timer
 * poll path.  Not re-entrant: the ISR can't fire while the poll is
 * running because both are on the BSP and the poll runs with
 * interrupts disabled (timer context). */
void keyboard_handle_byte(uint8_t sc) {
    kbd_count++;

    if (sc == 0xE0) { e0 = true; return; }

    bool release = (sc & 0x80) != 0;
    uint8_t code = sc & 0x7F;

    if (e0) {
        e0 = false;
        uint16_t key = 0;
        switch (code) {
            case 0x48: key = KEY_UP;    break;
            case 0x50: key = KEY_DOWN;  break;
            case 0x4B: key = KEY_LEFT;  break;
            case 0x4D: key = KEY_RIGHT; break;
            default: return;
        }
        input_event_t e;
        memset(&e, 0, sizeof(e));
        e.type = release ? EV_KEY_UP : EV_KEY_DOWN;
        e.key = key; e.scancode = code;
        input_push(&e);
        return;
    }

    if (code == 0x2A || code == 0x36) { shift = !release; return; }

    char ch = shift ? map_upper[code] : map_lower[code];
    input_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = release ? EV_KEY_UP : EV_KEY_DOWN;
    e.scancode = code;
    e.ch = ch;
    input_push(&e);
}

static void keyboard_isr(void) {
    /* Check OBF before reading — the timer poll path (keyboard_poll in
     * timer.c) may have already consumed the byte, leaving a spurious
     * IRQ1 pending.  Reading 0x60 with OBF=0 returns the last byte
     * (the port is a latch), which would duplicate the previous key. */
    uint8_t st = inb(0x64);
    if (!(st & 0x01)) return;       /* output buffer empty — spurious IRQ */
    if (st & 0x20) return;          /* mouse data, not keyboard */
    keyboard_handle_byte(inb(0x60));
}

void keyboard_init(void) {
    ps2_write_data(0xF4);     /* enable scanning */
    (void)ps2_read_data();    /* ACK */
    irq_install(1, keyboard_isr);
}

uint32_t keyboard_get_count(void) { return kbd_count; }
