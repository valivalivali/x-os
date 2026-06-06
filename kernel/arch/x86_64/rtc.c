#include "kernel/arch/x86_64/rtc.h"
#include "kernel/arch/x86_64/io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v & 0x0F) + ((v >> 4) * 10)); }

void rtc_read(rtc_time_t *t) {
    while (update_in_progress()) { }

    uint8_t sec  = cmos_read(0x00);
    uint8_t min  = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t regB = cmos_read(0x0B);

    int pm = (!(regB & 0x02)) && (hour & 0x80);  /* 12h mode, PM flag */
    hour &= 0x7F;

    if (!(regB & 0x04)) {                         /* values are BCD */
        sec  = bcd2bin(sec);
        min  = bcd2bin(min);
        hour = bcd2bin(hour);
    }
    if (pm && hour != 12) hour = (uint8_t)(hour + 12);

    t->sec = sec;
    t->min = min;
    t->hour = hour;
}
