#pragma once
#include <stdint.h>

/* 8042 PS/2 controller. */
void    ps2_init(void);          /* configure controller, enable both ports */
void    ps2_wait_write(void);
void    ps2_wait_read(void);
void    ps2_command(uint8_t cmd);
void    ps2_write_data(uint8_t data);
uint8_t ps2_read_data(void);
void    ps2_mouse_write(uint8_t data);   /* send a byte to the mouse + eat ACK */
