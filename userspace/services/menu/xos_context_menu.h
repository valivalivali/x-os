#ifndef XOS_CONTEXT_MENU_H
#define XOS_CONTEXT_MENU_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the egui context menu. Returns opaque state pointer. */
void *xos_context_menu_init(uint32_t screen_w, uint32_t screen_h);

/* Destroy the context menu state. */
void xos_context_menu_destroy(void *state);

/* Feed a mouse event to the context menu. */
void xos_context_menu_mouse_event(void *state, int32_t x, int32_t y, uint32_t button, uint32_t action);

/* Run one frame. Returns 1 if an item was clicked, 0 otherwise. */
uint32_t xos_context_menu_run_frame(void *state, uint32_t *pixels, uint32_t width, uint32_t height);

/* Get the last selected action (see MenuAction enum in lib.rs). */
uint32_t xos_context_menu_get_action(void *state);

/* Check if the menu is currently open. Returns 1 or 0. */
uint32_t xos_context_menu_is_open(void *state);

#ifdef __cplusplus
}
#endif

#endif /* XOS_CONTEXT_MENU_H */
