#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Unified input events consumed by the compositor. Producers are the PS/2
 * keyboard and mouse drivers (which run in interrupt context). */

typedef enum {
    EV_MOUSE_MOVE = 1,
    EV_MOUSE_DOWN,
    EV_MOUSE_UP,
    EV_KEY_DOWN,
    EV_KEY_UP,
} event_type_t;

#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

/* Non-ASCII keys (input_event.key); ASCII arrives in input_event.ch. */
#define KEY_UP     0x101
#define KEY_DOWN   0x102
#define KEY_LEFT   0x103
#define KEY_RIGHT  0x104

typedef struct {
    event_type_t type;
    int32_t  x, y;       /* absolute mouse position (all mouse events) */
    int32_t  dx, dy;     /* mouse delta (move) */
    uint8_t  button;     /* MOUSE_* that changed (down/up) */
    uint8_t  buttons;    /* current button mask */
    uint8_t  scancode;   /* raw set-1 scancode (key events) */
    char     ch;         /* translated ASCII, 0 if none */
    uint16_t key;        /* KEY_* for non-ASCII, 0 otherwise */
} input_event_t;

void    input_init(int screen_w, int screen_h);
bool    input_poll(input_event_t *out);      /* false if queue empty */
void    input_push(const input_event_t *e);  /* driver -> queue (ISR-safe) */

/* Called by the mouse driver each packet. dy is already in screen space. */
void    input_update_mouse(int dx, int dy, uint8_t buttons);

int32_t input_mouse_x(void);
int32_t input_mouse_y(void);
