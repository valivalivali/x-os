#ifndef XOS_TERMINAL_H
#define XOS_TERMINAL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *xos_terminal_init(
    uint32_t width,
    uint32_t height,
    uint32_t ctx_id,
    uint8_t *vb_mem, size_t vb_mem_size,
    uint8_t *ib_mem, size_t ib_mem_size,
    uint8_t *tex_mem, size_t tex_mem_size);

void xos_terminal_destroy(void *state);

void xos_terminal_mouse_event(void *state, int32_t x, int32_t y,
                              uint32_t button, uint32_t action);

void xos_terminal_key_event(void *state, uint32_t key, uint32_t pressed);

void xos_terminal_text_event(void *state, uint32_t ch);

/* Append shell stdout/stderr bytes (\n / \b handled). Marks needs_present. */
void xos_terminal_feed_output(void *state, const uint8_t *data, size_t len);

/* Switch UI into bridged stream mode (drop connecting banner). */
void xos_terminal_set_bridged(void *state);

uint32_t xos_terminal_run_frame(void *state);

uint32_t xos_terminal_needs_present(void *state);
void xos_terminal_ack_present(void *state);

/* 1 while egui::Window is open; 0 after user hits the window close (X). */
uint32_t xos_terminal_is_open(void *state);

uint32_t xos_terminal_render_target_id(void *state);
uint32_t xos_terminal_context_id(void *state);

#ifdef __cplusplus
}
#endif

#endif /* XOS_TERMINAL_H */
