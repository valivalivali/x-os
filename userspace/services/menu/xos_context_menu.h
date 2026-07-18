#ifndef XOS_CONTEXT_MENU_H
#define XOS_CONTEXT_MENU_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the egui context menu with GPU rendering backend.
 * Returns opaque state pointer, or NULL on failure.
 * screen_w/screen_h: full screen dimensions (for egui layout)
 * surf_w/surf_h: GPU render target dimensions (actual surface size)
 * ctx_id: unique virgl context ID (must be >= 2)
 * vb_mem/ib_mem/tex_mem: shared memory buffers for GPU data uploads */
void *xos_context_menu_init(uint32_t screen_w, uint32_t screen_h,
                            uint32_t surf_w, uint32_t surf_h,
                            uint32_t ctx_id,
                            void *vb_mem, size_t vb_mem_size,
                            void *ib_mem, size_t ib_mem_size,
                            void *tex_mem, size_t tex_mem_size);

/* Destroy the context menu state. */
void xos_context_menu_destroy(void *state);

/* Feed a mouse event to the context menu.
 * action: 0=move, 1=down, 2=up; button: 0=none, 1=left, 2=right. */
void xos_context_menu_mouse_event(void *state, int32_t x, int32_t y, uint32_t button, uint32_t action);

/* Wall-clock for egui (ms since boot / SYS_GET_TICKS). Call before run_frame. */
void xos_context_menu_set_time_ms(void *state, uint64_t time_ms);

/* Trigger the context menu to open at position (0,0) relative to surface. */
void xos_context_menu_trigger(void *state);

/* Run one egui frame (hover + hit-test + paint into GPU texture).
 * Returns 1 if an item was chosen. */
uint32_t xos_context_menu_run_frame(void *state);

/* bit0: present OVERLAY; bit1: refresh L1 under menu (fly-out change). */
uint32_t xos_context_menu_needs_present(void *state);

/* Clear needs_present after a successful send_dirty. */
void xos_context_menu_ack_present(void *state);

/* Unused (kept for ABI). */
uint32_t xos_context_menu_pending_close(void *state);

/* Get the render target resource ID (for compositor GPU compositing). */
uint32_t xos_context_menu_render_target_id(void *state);

/* Get the virgl context ID. */
uint32_t xos_context_menu_context_id(void *state);

/* Get the last selected action (see MenuAction enum in lib.rs). */
uint32_t xos_context_menu_get_action(void *state);

/* Check if the menu is currently open. Returns 1 or 0. */
uint32_t xos_context_menu_is_open(void *state);

/* Opaque painted bbox — crop the WM overlay to this (RT may be larger). */
void xos_context_menu_content_size(void *state, uint32_t *out_w, uint32_t *out_h);

#ifdef __cplusplus
}
#endif

#endif /* XOS_CONTEXT_MENU_H */
