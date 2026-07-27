#include "kernel/hal/input/input.h"
#include "kernel/lib/string.h"
#include "kernel/hal/apic/spinlock.h"

#define QSIZE 256

static input_event_t  queue[QSIZE];
static volatile uint32_t qhead = 0, qtail = 0;

/* Protects the input queue and mouse position state from concurrent
 * access by multiple CPUs' timer handlers and the composer's sys_input_poll. */
static spinlock_t input_lock = SPINLOCK_INIT;

static volatile int32_t mx = 0, my = 0;
static volatile uint8_t mbtn = 0;
static int32_t bw = 1280, bh = 800;

void input_init(int screen_w, int screen_h) {
    bw = screen_w; bh = screen_h;
    mx = screen_w / 2; my = screen_h / 2;
    mbtn = 0; qhead = qtail = 0;
}

void input_push(const input_event_t *e) {
    if (!e) return;
    uint64_t rflags = spinlock_acquire_irqsave(&input_lock);

    /* Coalesce consecutive mouse moves — a noisy PS/2/QEMU stream otherwise
     * fills the queue and drops keyboard events (shell looks "stuck"). */
    if (e->type == EV_MOUSE_MOVE && qhead != qtail) {
        uint32_t last = (qhead + QSIZE - 1) % QSIZE;
        if (queue[last].type == EV_MOUSE_MOVE) {
            queue[last] = *e;
            spinlock_release_irqrestore(&input_lock, rflags);
            return;
        }
    }

    uint32_t n = (qhead + 1) % QSIZE;
    if (n == qtail) {
        /* Full: never drop keys; drop this mouse event instead. */
        if (e->type == EV_MOUSE_MOVE || e->type == EV_MOUSE_DOWN ||
            e->type == EV_MOUSE_UP) {
            spinlock_release_irqrestore(&input_lock, rflags);
            return;
        }
        /* Make room for a key by discarding the oldest event. */
        qtail = (qtail + 1) % QSIZE;
        n = (qhead + 1) % QSIZE;
    }
    queue[qhead] = *e;
    qhead = n;
    spinlock_release_irqrestore(&input_lock, rflags);
}

bool input_poll(input_event_t *out) {
    uint64_t rflags = spinlock_acquire_irqsave(&input_lock);
    bool got = false;
    if (qtail != qhead) {
        *out = queue[qtail];
        qtail = (qtail + 1) % QSIZE;
        got = true;
    }
    spinlock_release_irqrestore(&input_lock, rflags);
    return got;
}

void input_update_mouse(int dx, int dy, uint8_t buttons) {
    /* Linear speed multiplier. With 200 Hz sample rate, each packet
     * carries a small delta; 2x gives responsive movement without
     * teleportation. Adjust if cursor feels too slow or too fast. */
    const int speed = 2;
    int sdx = dx * speed;
    int sdy = dy * speed;

    uint64_t rflags = spinlock_acquire_irqsave(&input_lock);
    int32_t nx = mx + sdx, ny = my + sdy;
    if (nx < 0) nx = 0; if (nx > bw - 1) nx = bw - 1;
    if (ny < 0) ny = 0; if (ny > bh - 1) ny = bh - 1;
    mx = nx; my = ny;
    uint8_t old_btn = mbtn;
    mbtn = buttons;
    spinlock_release_irqrestore(&input_lock, rflags);

    input_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EV_MOUSE_MOVE;
    e.x = nx; e.y = ny; e.dx = sdx; e.dy = sdy; e.buttons = buttons;
    input_push(&e);

    uint8_t changed = buttons ^ old_btn;
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
}

void input_warp_mouse(int32_t x, int32_t y) {
    if (x < 0) x = 0; if (x > bw - 1) x = bw - 1;
    if (y < 0) y = 0; if (y > bh - 1) y = bh - 1;
    uint64_t rflags = spinlock_acquire_irqsave(&input_lock);
    int32_t odx = x - mx, ody = y - my;
    mx = x; my = y;
    uint8_t btn = mbtn;
    spinlock_release_irqrestore(&input_lock, rflags);

    input_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EV_MOUSE_MOVE;
    e.x = x; e.y = y; e.dx = odx; e.dy = ody; e.buttons = btn;
    input_push(&e);
}

void input_mouse_wheel(int dy) {
    if (dy == 0) return;
    input_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = EV_MOUSE_WHEEL;
    uint64_t rflags = spinlock_acquire_irqsave(&input_lock);
    e.x = mx;
    e.y = my;
    e.buttons = mbtn;
    spinlock_release_irqrestore(&input_lock, rflags);
    e.dy = dy;
    input_push(&e);
}

int32_t input_mouse_x(void) {
    uint64_t rflags = spinlock_acquire_irqsave(&input_lock);
    int32_t x = mx;
    spinlock_release_irqrestore(&input_lock, rflags);
    return x;
}

int32_t input_mouse_y(void) {
    uint64_t rflags = spinlock_acquire_irqsave(&input_lock);
    int32_t y = my;
    spinlock_release_irqrestore(&input_lock, rflags);
    return y;
}
