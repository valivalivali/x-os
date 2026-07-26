#pragma once
#include <stdint.h>

/* Programmable Interval Timer (PIT, channel 0) -> periodic IRQ0 tick. */
void     timer_init(uint32_t frequency_hz);
uint64_t timer_ticks(void);
uint64_t timer_ticks_hz(void);  /* approx ticks/sec of timer_ticks() */
void     timer_sleep_ms(uint64_t ms);
void     timer_tick_global(void);  /* increment global tick counter */

/* ---- High-resolution timekeeping via TSC ----------------------------------
 * Calibrated against the PIT during boot.  Provides nanosecond-resolution
 * monotonic time independent of the tick counter. */
void     tsc_calibrate(void);       /* call once after PIT is running */
uint64_t tsc_read(void);            /* current TSC value */
uint64_t tsc_freq_hz(void);         /* calibrated TSC frequency */
uint64_t systime_ns(void);          /* nanoseconds since boot (TSC-based) */
uint64_t systime_us(void);          /* microseconds since boot */
