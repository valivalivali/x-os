#include "kernel/hal/timers/timer.h"
#include "kernel/arch/x86_64/io.h"
#include "kernel/interrupts/idt.h"

#define PIT_FREQ   1193182u
#define PIT_CH0    0x40
#define PIT_CMD    0x43

static volatile uint64_t ticks = 0;
static uint32_t hz = 1000;

static void timer_tick(void) { ticks++; }

void timer_init(uint32_t frequency_hz) {
    if (frequency_hz == 0) frequency_hz = 1000;
    hz = frequency_hz;
    uint32_t divisor = PIT_FREQ / frequency_hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    outb(PIT_CMD, 0x36);                       /* ch0, lo/hi, mode 3 */
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));

    irq_install(0, timer_tick);
}

uint64_t timer_ticks(void) { return ticks; }

void timer_sleep_ms(uint64_t ms) {
    uint64_t target = ticks + (ms * hz) / 1000u;
    while (ticks < target) __asm__ volatile("sti; hlt");
}
