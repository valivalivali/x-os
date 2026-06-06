#include "kernel/hal/input/input.h"
#include "kernel/lib/string.h"

#define QSIZE 256

static input_event_t  queue[QSIZE];
static volatile uint32_t qhead = 0, qtail = 0;

static volatile int32_t mx = 0, my = 0;
static volatile uint8_t mbtn = 0;
static int32_t bw = 1280, bh = 800;

void input_init(int screen_w, int screen_h) {
    bw = screen_w; bh = screen_h;
    mx = screen_w / 2; my = screen_h / 2;
    mbtn = 0; qhead = qtail = 0;
}

void input_push(const input_event_t *e) {
    uint32_t n = (qhead + 1) % QSIZE;
    if (n == qtail) return;           /* full: drop oldest-preserving */
    queue[qhead] = *e;
    qhead = n;
}

bool input_poll(input_event_t *out) {
    bool got = false;
    __asm__ volatile("cli");
    if (qtail != qhead) {
        *out = queue[qtail];
        qtail = (qtail + 1) % QSIZE;
        got = true;
    }
    __asm__ volatile("sti");
    return got;
}

void input_update_mouse(int dx, int dy, uint8_t buttons) {
    /* Linear speed multiplier. With 200 Hz sample rate, each packet
     * carries a small delta; 2x gives responsive movement without
     * teleportation. Adjust if cursor feels too slow or too fast. */
    const int speed = 2;
    int sdx = dx * speed;
    int sdy = dy * speed;

    int32_t nx = mx + sdx, ny = my + sdy;
    if (nx < 0) nx = 0; if (nx > bw - 1) nx = bw - 1;
    if (ny < 0) ny = 0; if (ny > bh - 1) ny = bh - 1;
    mx = nx; my = ny;

    input_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EV_MOUSE_MOVE;
    e.x = nx; e.y = ny; e.dx = sdx; e.dy = sdy; e.buttons = buttons;
    input_push(&e);

    uint8_t changed = buttons ^ mbtn;
    for (uint8_t b = 0; b < 3; b++) {
        uint8_t mask = (uint8_t)(1u << b);
        if (changed & mask) {
            input_event_t be;
            memset(&be, 0, sizeof(be));
            be.type = (buttons & mask) ? EV_MOUSE_DOWN : EV_MOUSE_UP;
            be.x = nx; be.y = ny; be.button = mask; be.buttons = buttons;
            input_push(&be);
        }
    }
    mbtn = buttons;
}

int32_t input_mouse_x(void)       { return mx; }
int32_t input_mouse_y(void)       { return my; }
