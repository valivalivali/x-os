#include "kernel/hal/timers/timer.h"
#include "kernel/arch/x86_64/io.h"
#include "kernel/interrupts/idt.h"
#include "kernel/hal/input/mouse.h"

#define PIT_FREQ   1193182u
#define PIT_CH0    0x40
#define PIT_CMD    0x43

static volatile uint64_t local_ticks = 0;
static uint32_t hz = 1000;

static void timer_tick(void) {
    local_ticks++;
    mouse_poll();
    /* Update FreeBSD time counters for ARP expiry and IP fragment timeouts */
    extern volatile long time_uptime;
    extern volatile long time_second;
    extern int ticks_val;
    extern int ticks;
    extern void callout_process(unsigned long long);
    extern unsigned long long tick_sbt;
    ticks_val++;
    ticks++;
    if ((local_ticks % hz) == 0) {
        time_uptime++;
        time_second++;
    }
    /* Process expired FreeBSD callouts (TCP retransmit, keepalive, etc.) */
    callout_process((unsigned long long)ticks * tick_sbt);
    /* Poll virtio-net for received packets and feed into FreeBSD stack */
    extern void vioif_rx_poll(void);
    vioif_rx_poll();
}

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

uint64_t timer_ticks(void) { return local_ticks; }

void timer_sleep_ms(uint64_t ms) {
    uint64_t target = local_ticks + (ms * hz) / 1000u;
    while (local_ticks < target) __asm__ volatile("sti; hlt");
}
