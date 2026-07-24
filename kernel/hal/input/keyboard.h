#pragma once
#include <stdint.h>

/* PS/2 keyboard: installs IRQ1, translates set-1 scancodes into input events. */
void keyboard_init(void);

/* Process a single scancode byte (shared by ISR and timer poll). */
void keyboard_handle_byte(uint8_t sc);

/* Number of scancodes processed (diagnostic). */
uint32_t keyboard_get_count(void);
