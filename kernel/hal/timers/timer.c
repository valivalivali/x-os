#include "kernel/hal/timers/timer.h"
#include "kernel/arch/x86_64/io.h"
#include "kernel/interrupts/idt.h"
#include "kernel/hal/input/mouse.h"
#include "kernel/hal/input/keyboard.h"
#include "kernel/hal/input/input.h"
#include "kernel/sched/sched.h"
#include "kernel/hal/apic/smp.h"
#include "kernel/lib/kprintf.h"

#define PIT_FREQ   1193182u
#define PIT_CH0    0x40
#define PIT_CMD    0x43
#define PS2_DATA   0x60
#define PS2_STATUS 0x64

static volatile uint64_t local_ticks = 0;
static uint32_t hz = 1000;

/* Global tick counter — incremented by BOTH the BSP PIT handler and
 * AP LAPIC timer handlers.  This ensures timer_ticks() advances even
 * if the PIT stops firing on the BSP (a race that can occur during
 * SMP startup when APs enable their LAPICs before the BSP). */
static volatile uint64_t g_global_ticks = 0;

void timer_tick_global(void) {
    __atomic_add_fetch(&g_global_ticks, 1, __ATOMIC_RELAXED);
}

/* Poll PS/2 keyboard from the timer tick.  In LAPIC/SMP mode, IRQ1
 * may not be reliably delivered (LINT0=ExtINT pass-through only goes
 * to the BSP, which may be busy in userspace).  Polling the status
 * port from the timer ensures keyboard input works on all CPUs.
 * Uses ps2_lock to prevent racing with mouse_poll on PS/2 I/O ports. */
static void keyboard_poll(void) {
    uint64_t rflags = spinlock_acquire_irqsave(&ps2_lock);
    for (int i = 0; i < 8; i++) {
        uint8_t st = inb(PS2_STATUS);
        if (!(st & 0x01)) break;       /* output buffer empty */
        if (st & 0x20) break;          /* mouse data, not keyboard */
        uint8_t data = inb(PS2_DATA);
        keyboard_handle_byte(data);
    }
    spinlock_release_irqrestore(&ps2_lock, rflags);
}

static void timer_tick(void) {
    local_ticks++;
    timer_tick_global();
    mouse_poll();
    keyboard_poll();
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
    /* Deferred preemption: set need_resched flag instead of calling
     * sched_yield_try().  The flag is checked at safe points (syscall
     * return, interrupt return to userspace, idle loop).  This follows
     * the XNU AST / Linux TIF_NEED_RESCHED pattern and eliminates
     * sched_lock contention from 8 CPUs × 1000Hz timer ticks.
     * Also wake up expired sleepers so NSLEEP works without requiring
     * another process to voluntarily yield. */
    extern void sched_check_canaries(void);
    extern void sched_wake_sleepers(void);
    sched_check_canaries();
    sched_wake_sleepers();
    /* Preserve the request across no_preempt sections.  Clearing it here
     * would lose a timer preemption in exactly the same way as a lost AST or
     * TIF_NEED_RESCHED notification. */
    this_cpu()->need_resched = 1;
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

uint64_t timer_ticks(void) { return g_global_ticks; }

/* Approximate tick rate of g_global_ticks.  With 1 BSP at 1000Hz and
 * N APs at 250Hz, this is 1000 + N*250.  Used by proc_sleep to convert
 * milliseconds to the correct number of global ticks. */
uint64_t timer_ticks_hz(void) {
    extern uint32_t g_cpu_count;
    return 1000 + (uint64_t)(g_cpu_count > 1 ? (g_cpu_count - 1) * 250 : 0);
}

void timer_sleep_ms(uint64_t ms) {
    uint64_t target = local_ticks + (ms * hz) / 1000u;
    while (local_ticks < target) __asm__ volatile("sti; hlt");
}
