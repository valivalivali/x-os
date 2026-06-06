#pragma once
#include <stdint.h>

typedef struct { uint8_t hour, min, sec; } rtc_time_t;

void rtc_read(rtc_time_t *t);   /* current wall-clock time from the CMOS RTC */
