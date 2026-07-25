#pragma once
#include <stdint.h>

/* Programmable Interval Timer (PIT, channel 0) -> periodic IRQ0 tick. */
void     timer_init(uint32_t frequency_hz);
uint64_t timer_ticks(void);
uint64_t timer_ticks_hz(void);  /* approx ticks/sec of timer_ticks() */
void     timer_sleep_ms(uint64_t ms);
void     timer_tick_global(void);  /* increment global tick counter */
