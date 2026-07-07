#include <stdint.h>
#include "boot/handoff/handoff.h"
#include "kernel/arch/x86_64/serial.h"
#include "kernel/arch/x86_64/io.h"
#include "kernel/lib/kprintf.h"
#include "kernel/memory/pmm.h"
#include "kernel/memory/heap.h"
#include "kernel/arch/x86_64/gdt.h"
#include "kernel/arch/x86_64/rtc.h"
#include "kernel/interrupts/idt.h"
#include "kernel/hal/timers/timer.h"
#include "kernel/hal/input/ps2.h"
#include "kernel/hal/input/input.h"
#include "kernel/hal/input/keyboard.h"
#include "kernel/hal/input/mouse.h"
#include "kernel/sched/sched.h"
#include "kernel/ipc/ipc.h"
#include "kernel/include/syscall.h"
#include "kernel/proc/proc.h"
#include "kernel/hal/block/block_dev.h"
#include "kernel/hal/gpu/virtio_gpu.h"
#include "kernel/fs/xfs.h"

extern const uint8_t *init_elf_data;
extern size_t init_elf_len;
extern uint64_t g_kernel_rsp0;

/* Ensure SSE/SSE2 is usable (Limine enables it, but make it explicit so the
 * compiler may freely emit SSE for math/animation code). */
static void enable_sse(void) {
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2); /* clear EM  */
    cr0 |=  (1ULL << 1); /* set   MP  */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10); /* OSFXSR | OSXMMEXCPT */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

void kmain(void) {
    enable_sse();
    serial_init();
    kputs("\n=== X OS ===\n");
    boot_puts("kmain reached\n");
    const handoff_t *h = handoff_get();
    boot_log("framebuffer %lux%lu pitch=%lu bpp=%u addr=%p\n",
            h->fb.width, h->fb.height, h->fb.pitch, h->fb.bpp, (void *)h->fb.addr);
    boot_log("hhdm=%p memmap entries=%lu\n",
            (void *)h->hhdm_offset, h->memmap_count);

    pmm_init();
    heap_init();
    gdt_init();
    idt_init();
    timer_init(1000);
    syscall_init();
    sched_init();
    ipc_init();
    __asm__ volatile("sti");
    boot_puts("gdt/idt/timer up, interrupts enabled\n");
    boot_puts("scheduler + ipc up\n");

    rtc_time_t now;
    rtc_read(&now);
    boot_log("rtc %u:%u:%u\n", now.hour, now.min, now.sec);

    ps2_init();

    virtio_gpu_init();

    /* Use virtio-gpu dimensions if Limine framebuffer is absent. */
    uint32_t screen_w = h->fb.width;
    uint32_t screen_h = h->fb.height;
    if (screen_w == 0 || screen_h == 0) {
        gpu_fb_info_t ginfo;
        if (virtio_gpu_get_fb_info(&ginfo)) {
            screen_w = ginfo.width;
            screen_h = ginfo.height;
        }
    }
    input_init(screen_w, screen_h);
    keyboard_init();
    mouse_init();
    boot_puts("ps2 keyboard + mouse online\n");

    block_dev_t *bdev = nvme_probe();
    if (!bdev) {
        boot_puts("nvme not found, falling back to ramdisk\n");
        bdev = ramdisk_create();
    }
    if (bdev) {
        xfs_format(bdev);
        xfs_mount(bdev);
    } else {
        boot_puts("no block device available\n");
    }

    /* Spawn init (PID 1) as the first ring-3 userspace process.
     * The init.elf is embedded into the kernel as a byte array. */
    boot_puts("spawning ring-3 init\n");
    proc_t *init = proc_spawn_ring3(init_elf_data, init_elf_len);
    if (init) {
        gdt_set_rsp0((uint64_t)(init->kstack + SCHED_STACK_SIZE));
        boot_log("init spawned pid=%lu, entering ring-3\n", init->pid);

        /* Set up PID 0 (idle) stack frame so context_switch can resume us
         * after init calls SYS_YIELD.
         * context_switch pops: rbx, rbp, r12, r13, r14, r15, then ret.
         * We push 6 dummy callee-saved regs and the resume address. */
        proc_t *idle = proc_current();
        uint64_t resume = (uint64_t)&&init_resume;
        __asm__ volatile (
            "pushq $0\n"           /* rbx */
            "pushq $0\n"           /* rbp */
            "pushq $0\n"           /* r12 */
            "pushq $0\n"           /* r13 */
            "pushq $0\n"           /* r14 */
            "pushq $0\n"           /* r15 */
            "movq %1, 48(%%rsp)\n" /* resume address at original rsp */
            "movq %%rsp, %0"
            : "=m"(idle->rsp)
            : "r"(resume)
            : "memory"
        );
        idle->rip = resume;
        g_kernel_rsp0 = (uint64_t)(init->kstack + SCHED_STACK_SIZE);
        sched_adopt_current(init);
        enter_userspace(
            init->pml4_phys, init->rip, init->sleep_until
        );
    init_resume:
        boot_puts("resumed from ring-3 init yield\n");
        goto idle;
    }
    boot_puts("init spawn FAILED\n");

idle:
    /* Kernel idle loop — all work is now in ring-3 processes. */
    for (;;) {
        __asm__ volatile("hlt");
    }
}
