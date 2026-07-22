#ifndef XOS_LVGL_DRV_H
#define XOS_LVGL_DRV_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

/* X OS LVGL display + input driver
 *
 * Creates an LVGL display backed by a compositor surface.
 * LVGL renders into a pixel buffer; the flush callback copies dirty
 * pixels to the compositor surface buffer and sends a WM_SURFACE_DIRTY
 * message so the compositor re-composites.
 *
 * Input (mouse + keyboard) is received via IPC from the compositor
 * and forwarded to LVGL indev drivers.
 */

typedef struct {
    uint32_t *surface_buf;   /* compositor-provided shared buffer (CPU mode) */
    uint32_t  surface_idx;   /* compositor surface index */
    int32_t   width;
    int32_t   height;
    lv_display_t *disp;
    lv_indev_t   *mouse;
    lv_indev_t   *keyboard;
    /* Internal draw buffer (LVGL renders here, flush copies to surface) */
    void     *draw_buf1;
    void     *draw_buf2;
    bool      closed;        /* set when WM_WINDOW_CLOSE received */
    /* GPU mode fields */
    int      gpu_mode;       /* 1 = GPU-backed surface */
    uint32_t gpu_res_id;     /* virtio-gpu resource ID for render target */
    uint32_t *gpu_backing;   /* GPU backing buffer (page-aligned, full-screen) */
    uint64_t gpu_backing_vaddr; /* GPU backing buffer virtual address */
    uint32_t gpu_backing_size;  /* GPU backing buffer size in bytes */
} xos_lvgl_ctx_t;

/* Key event hook — called before LVGL processes the key.
 * Allows apps (e.g. terminal) to intercept raw key events. */
typedef void (*xos_key_hook_t)(uint8_t scancode, char ch,
                               uint16_t key, uint32_t action);
void xos_lvgl_set_key_hook(xos_key_hook_t hook);

/* Initialize LVGL + create display of given size at given position.
 * Returns 0 on success, -1 on failure. */
int xos_lvgl_init(xos_lvgl_ctx_t *ctx,
                  int32_t x, int32_t y, int32_t w, int32_t h,
                  uint32_t wm_flags, const char *title);

/* Initialize LVGL with a GPU-backed compositor surface.
 * Creates a VirGL texture resource and uploads LVGL's rendered output
 * via GPU transfer. The compositor composites this surface as a
 * textured quad. Returns 0 on success, -1 on failure. */
int xos_lvgl_gpu_init(xos_lvgl_ctx_t *ctx,
                      int32_t x, int32_t y, int32_t w, int32_t h,
                      uint32_t wm_flags, const char *title);

/* Pump: call lv_tick_inc + lv_timer_handler + process IPC input.
 * Call this in your main loop. */
void xos_lvgl_pump(xos_lvgl_ctx_t *ctx);

/* Feed a mouse event from compositor IPC into LVGL.
 * action: 0=move, 1=down, 2=up, 3=wheel */
void xos_lvgl_mouse_event(xos_lvgl_ctx_t *ctx,
                          int32_t x, int32_t y,
                          uint32_t button, uint32_t action);

/* Feed a keyboard event from compositor IPC into LVGL.
 * action: 0=down, 1=up */
void xos_lvgl_key_event(xos_lvgl_ctx_t *ctx,
                        uint8_t scancode, char ch, uint16_t key,
                        uint32_t action);

#endif /* XOS_LVGL_DRV_H */
