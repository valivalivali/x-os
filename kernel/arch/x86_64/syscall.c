#include "kernel/include/syscall.h"
#include "kernel/arch/x86_64/rtc.h"
#include "kernel/sched/sched.h"
#include "kernel/proc/proc.h"
#include "kernel/proc/signal.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/vmm.h"
#include "kernel/memory/pmm.h"
#include "kernel/ipc/ipc.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/msgbuf.h"
#include "kernel/lib/string.h"
#include "kernel/hal/timers/timer.h"
#include "kernel/arch/x86_64/serial.h"
#include "kernel/hal/input/input.h"
#include "kernel/hal/apic/smp.h"
#include "kernel/hal/block/block_dev.h"
#include "kernel/fs/xfs.h"
#include "kernel/hal/gpu/virtio_gpu.h"
#include "kernel/ipc/pipe.h"
#include "boot/handoff/handoff.h"
#include <stdint.h>

/* Assembly entry point, defined in syscall_entry.S */
extern void syscall_entry(void);

static void proc_brk_reset(uint64_t pid);

/* MSR definitions */
#define MSR_EFER        0xC0000080
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_CSTAR       0xC0000083
#define MSR_SFMASK      0xC0000084

#define EFER_SCE        (1ULL << 0)

/* Syscall arguments packed by syscall_entry.S */
typedef struct {
    uint64_t num;
    uint64_t a1, a2, a3, a4, a5, a6;
} syscall_args_t;

static uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

/* -------------------------------------------------------------------------- */

static uint64_t sys_yield_impl(void) {
    sched_yield();
    return 0;
}

static uint64_t sys_exit_impl(uint64_t code) {
    proc_t *p = proc_current();
    if (p && p->pid != 0) {
        proc_brk_reset(p->pid);
        p->exit_code = (int)code;
        proc_exit(p);
        sched_yield();  /* switches to another READY task; never returns here */
        /* If nothing else is runnable, park. Page tables are already gone. */
        for (;;) __asm__ volatile("cli; hlt");
    }
    return 0;
}

static uint64_t sys_nsleep(uint64_t ms) {
    proc_sleep(ms);
    return 0;
}

/* Set/clear no-preempt flag for the current process.
 * When set, timer interrupts skip sched_yield, letting the process
 * run uninterrupted during critical initialization (e.g., GPU setup). */
static uint64_t sys_no_preempt_impl(uint64_t enable, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    proc_t *p = proc_current();
    if (!p) return (uint64_t)-1;
    p->no_preempt = enable ? 1 : 0;
    return 0;
}

static uint64_t sys_proc_pid_impl(void) {
    proc_t *p = proc_current();
    return p ? p->pid : 0;
}

static uint64_t sys_proc_spawn(uint64_t uelf, uint64_t len,
                               uint64_t a3, uint64_t a4,
                               uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (!uelf || len == 0 || len > 8 * 1024 * 1024) {
        return 0; /* invalid args */
    }
    /* Copy ELF from userspace to kernel heap.
     * Kernel higher-half mappings are present, so userspace lower-half
     * addresses are directly readable. */
    uint8_t *kbuf = kmalloc(len);
    if (!kbuf) return 0;
    memcpy(kbuf, (const uint8_t *)uelf, len);

    proc_t *child = proc_spawn_ring3(kbuf, len);
    uint64_t pid = child ? child->pid : 0;
    /* Optional process name (a3) — used by init for composer/dock/zsh/… */
    if (child && a3) {
        const char *name = (const char *)a3;
        size_t i = 0;
        while (name[i] && i < sizeof(child->name) - 1) {
            child->name[i] = name[i];
            i++;
        }
        child->name[i] = '\0';
    }

    kfree(kbuf);
    return pid;
}

static uint64_t sys_debug_log(uint64_t umsg, uint64_t len,
                              uint64_t a3, uint64_t a4,
                              uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!umsg || len == 0 || len > 4096) return 0;
    const char *s = (const char *)umsg;
    /* Use kwrite (console spinlock-protected) so concurrent sys_debug_log
     * calls from multiple CPUs don't interleave characters on the serial
     * console.  Without this, SMP output is garbled. */
    size_t n = 0;
    while (n < len && s[n] != '\0') n++;
    kwrite(s, n);
    return 0;
}

static uint64_t sys_get_ticks(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return timer_ticks();
}

static uint64_t sys_fb_info(uint64_t uinfo, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!uinfo) return (uint64_t)-1;
    fb_info_t *info = (fb_info_t *)uinfo;
    const handoff_t *h = handoff_get();
    if (h->fb.width && h->fb.height) {
        info->phys_base = (uint64_t)h->fb.addr - h->hhdm_offset;
        info->width     = (uint32_t)h->fb.width;
        info->height    = (uint32_t)h->fb.height;
        info->pitch     = (uint32_t)h->fb.pitch;
        info->bpp       = h->fb.bpp;
    } else if (virtio_gpu_present()) {
        gpu_fb_info_t g;
        if (virtio_gpu_get_fb_info(&g)) {
            info->phys_base = g.backing_phys;
            info->width     = g.width;
            info->height    = g.height;
            info->pitch     = g.stride;
            info->bpp       = 32;
        } else {
            return (uint64_t)-1;
        }
    } else {
        return (uint64_t)-1;
    }
    return 0;
}

static uint64_t sys_mem_map(uint64_t vaddr, uint64_t paddr, uint64_t flags,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (vaddr >= 0xffff800000000000ULL) return (uint64_t)-1;
    proc_t *p = proc_current();
    if (!p || !p->pml4_virt) return (uint64_t)-1;
    uint64_t f = (flags & (VMM_RW | VMM_WT | VMM_CD)) | VMM_U | VMM_P;
    if (!vmm_map_page(p->pml4_virt, vaddr, paddr, f)) return (uint64_t)-1;
    return 0;
}

static uint64_t sys_mouse_pos(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    int32_t x = input_mouse_x();
    int32_t y = input_mouse_y();
    uint64_t ret = ((uint64_t)(uint32_t)y << 32) | (uint64_t)(uint32_t)x;
    return ret;
}

static uint64_t sys_port_create_impl(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    proc_t *p = proc_current();
    return port_create(p ? p->pid : 0);
}

static uint64_t sys_port_send_impl(uint64_t handle, uint64_t umsg,
                                   uint64_t a3, uint64_t a4,
                                   uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (!umsg || umsg < 0x1000) return 0;
    ipc_msg_t msg;
    memcpy(&msg, (const void *)umsg, sizeof(msg));
    /* a3 != 0: blocking send (sleeps until port has space). */
    if (a3)
        return port_send_blocking(handle, &msg) ? 1 : 0;
    return port_send(handle, &msg) ? 1 : 0;
}

static uint64_t sys_port_recv_impl(uint64_t handle, uint64_t umsg,
                                   uint64_t block,
                                   uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (!umsg || umsg < 0x1000) return 0;
    ipc_msg_t msg;
    bool ok = port_recv(handle, &msg, block != 0);
    if (!ok) return 0;
    memcpy((void *)umsg, &msg, sizeof(msg));
    return 1;
}

static uint64_t sys_port_close_impl(uint64_t handle, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    port_close(handle);
    return 0;
}

static uint64_t sys_ns_register_impl(uint64_t id, uint64_t port, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    ns_register((uint32_t)id, (port_handle_t)port);
    return 0;
}

static uint64_t sys_ns_lookup_impl(uint64_t id, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return (uint64_t)ns_lookup((uint32_t)id);
}

static uint64_t sys_mem_alloc_impl(uint64_t vaddr, uint64_t flags,
                                   uint64_t a3, uint64_t a4,
                                   uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (vaddr >= 0xffff800000000000ULL) return (uint64_t)-1;
    uint64_t frame = pmm_alloc_frame();
    if (!frame) { kprintf("[k] mem_alloc: pmm OOM vaddr=%lx\n", vaddr); return (uint64_t)-1; }
    proc_t *p = proc_current();
    if (!p || !p->pml4_virt) {
        pmm_free_frame(frame);
        return (uint64_t)-1;
    }
    uint64_t f = (flags & (VMM_RW | VMM_WT | VMM_CD)) | VMM_U | VMM_P;
    if (!vmm_map_page(p->pml4_virt, vaddr, frame, f)) {
        kprintf("[k] mem_alloc: vmm_map_page fail vaddr=%lx\n", vaddr);
        pmm_free_frame(frame);
        return (uint64_t)-1;
    }
    return 0;
}

static uint64_t sys_input_poll_impl(uint64_t uevent, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!uevent || uevent < 0x1000) return 0;
    input_event_t e;
    if (!input_poll(&e)) return 0;
    memcpy((void *)uevent, &e, sizeof(e));
    return 1;
}

/* Share a physical page from caller's address space into target process.
 * a1 = caller vaddr (must be page-aligned)
 * a2 = target pid
 * a3 = target vaddr (must be page-aligned)
 * a4 = flags (VMM_RW etc)
 */
static uint64_t sys_mem_share_impl(uint64_t vaddr, uint64_t target_pid,
                                   uint64_t target_vaddr, uint64_t flags,
                                   uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    if (vaddr & 0xFFF) return (uint64_t)-1;
    if (target_vaddr & 0xFFF) return (uint64_t)-1;
    if (target_vaddr >= 0xffff800000000000ULL) return (uint64_t)-1;

    proc_t *caller = proc_current();
    if (!caller || !caller->pml4_virt) return (uint64_t)-1;

    uint64_t paddr = vmm_virt_to_phys(caller->pml4_virt, vaddr);
    if (!paddr) return (uint64_t)-1;

    proc_t *target = proc_by_pid(target_pid);
    if (!target || !target->pml4_virt) return (uint64_t)-1;

    /* The frame now has two owners.  Without this the first process to
     * unmap it would return it to the free pool while the other still has
     * it mapped. */
    pmm_ref_frame(paddr);

    /* Tag both mappings SHARED so fork copy-on-write leaves them alone —
     * the whole point of the page is that writes are mutually visible. */
    uint64_t f = (flags & (VMM_RW | VMM_WT | VMM_CD)) | VMM_U | VMM_P | VMM_SHARED;
    if (!vmm_map_page(target->pml4_virt, target_vaddr, paddr, f)) {
        pmm_unref_frame(paddr);
        return (uint64_t)-1;
    }
    uint64_t *src_pte = vmm_pte_lookup(caller->pml4_virt, vaddr);
    if (src_pte) *src_pte |= VMM_SHARED;
    return 0;
}

static uint64_t sys_proc_exists_impl(uint64_t pid, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    proc_t *p = proc_by_pid(pid);
    return p ? 1 : 0;
}

static uint64_t sys_proc_kill_impl(uint64_t pid, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    proc_kill(pid);
    return 0;
}

static uint64_t sys_mem_free_impl(uint64_t vaddr, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    proc_t *p = proc_current();
    if (!p || !p->pml4_virt) return (uint64_t)-1;
    uint64_t paddr = vmm_virt_to_phys(p->pml4_virt, vaddr);
    if (!paddr) return (uint64_t)-1;
    vmm_unmap_page(p->pml4_virt, vaddr);
    pmm_free_frame(paddr);
    return 0;
}

static uint64_t sys_gpu_fb_info_impl(uint64_t uinfo, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!uinfo) return (uint64_t)-1;
    gpu_fb_info_t *info = (gpu_fb_info_t *)uinfo;
    return virtio_gpu_get_fb_info(info) ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_flush_impl(uint64_t x, uint64_t y, uint64_t w,
                                   uint64_t h, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    return virtio_gpu_flush((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h) ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_cursor_set_impl(uint64_t x, uint64_t y, uint64_t hot_x,
                                        uint64_t hot_y, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    return virtio_gpu_cursor_set((int32_t)x, (int32_t)y, (uint32_t)hot_x, (uint32_t)hot_y) ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_cursor_move_impl(uint64_t x, uint64_t y, uint64_t a3,
                                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    return virtio_gpu_cursor_move((int32_t)x, (int32_t)y) ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_virgl_present_impl(uint64_t a1, uint64_t a2, uint64_t a3,
                                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return virtio_gpu_virgl_present() ? 1 : 0;
}

static uint64_t sys_gpu_ctx_create_impl(uint64_t ctx_id, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return virtio_gpu_ctx_create((uint32_t)ctx_id) ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_ctx_destroy_impl(uint64_t ctx_id, uint64_t a2, uint64_t a3,
                                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return virtio_gpu_ctx_destroy((uint32_t)ctx_id) ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_ctx_attach_impl(uint64_t ctx_id, uint64_t resource_id,
                                        uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    return virtio_gpu_ctx_attach_resource((uint32_t)ctx_id, (uint32_t)resource_id) ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_res_create_2d_impl(uint64_t resource_id, uint64_t format,
                                           uint64_t width, uint64_t height,
                                           uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    return virtio_gpu_resource_create_2d_for((uint32_t)resource_id, (uint32_t)format,
                                             (uint32_t)width, (uint32_t)height) ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_res_attach_impl(uint64_t resource_id, uint64_t phys,
                                        uint64_t size, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    return virtio_gpu_resource_attach_backing_for((uint32_t)resource_id, phys, size) ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_res_unref_impl(uint64_t resource_id, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return virtio_gpu_resource_unref_for((uint32_t)resource_id) ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_transfer_2d_impl(uint64_t resource_id, uint64_t x, uint64_t y,
                                         uint64_t w, uint64_t h, uint64_t offset) {
    return virtio_gpu_transfer_to_host_2d_for((uint32_t)resource_id, (uint32_t)x,
                                              (uint32_t)y, (uint32_t)w, (uint32_t)h,
                                              offset) ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_submit_3d_impl(uint64_t ctx_id, uint64_t cmds, uint64_t size,
                                       uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (!cmds) return (uint64_t)-1;
    return virtio_gpu_submit_3d((uint32_t)ctx_id, (void *)cmds, (uint32_t)size) ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_set_scanout_impl(uint64_t scanout_id, uint64_t resource_id,
                                         uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    return virtio_gpu_set_scanout_for((uint32_t)scanout_id, (uint32_t)resource_id,
                                      (uint32_t)x, (uint32_t)y, (uint32_t)w,
                                      (uint32_t)h) ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_flush_res_impl(uint64_t resource_id, uint64_t x, uint64_t y,
                                       uint64_t w, uint64_t h, uint64_t a6) {
    (void)a6;
    return virtio_gpu_flush_for((uint32_t)resource_id, (uint32_t)x, (uint32_t)y,
                                (uint32_t)w, (uint32_t)h) ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_alloc_res_id_impl(uint64_t a1, uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return (uint64_t)virtio_gpu_alloc_resource_id();
}

static uint64_t sys_gpu_res_attach_virt_impl(uint64_t resource_id, uint64_t vaddr,
                                             uint64_t npages, uint64_t buf_size,
                                             uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    if (npages == 0 || npages > 4096) return (uint64_t)-1;
    if (vaddr & 0xFFF) return (uint64_t)-1;
    if (buf_size == 0 || buf_size > npages * PAGE_SIZE) return (uint64_t)-1;

    proc_t *p = proc_current();
    if (!p || !p->pml4_virt) return (uint64_t)-1;

    /* Allocate array to hold physical addresses */
    uint64_t *phys_pages = (uint64_t *)kmalloc(npages * sizeof(uint64_t));
    if (!phys_pages) return (uint64_t)-1;

    /* Walk page tables to get physical address of each page */
    int ok = 1;
    for (uint32_t i = 0; i < npages; i++) {
        uint64_t va = vaddr + (uint64_t)i * PAGE_SIZE;
        phys_pages[i] = vmm_virt_to_phys(p->pml4_virt, va);
        if (!phys_pages[i]) { ok = 0; break; }
    }

    uint64_t ret = (uint64_t)-1;
    if (ok) {
        ret = virtio_gpu_resource_attach_backing_sg((uint32_t)resource_id,
                                                    phys_pages, (uint32_t)npages,
                                                    buf_size)
              ? 0 : (uint64_t)-1;
    }

    kfree(phys_pages);
    return ret;
}

static uint64_t sys_gpu_res_create_3d_impl(uint64_t resource_id, uint64_t target,
                                           uint64_t format, uint64_t bind,
                                           uint64_t width, uint64_t height) {
    return virtio_gpu_resource_create_3d_for((uint32_t)resource_id, (uint32_t)target,
                                             (uint32_t)format, (uint32_t)bind,
                                             (uint32_t)width, (uint32_t)height,
                                             1, 1, 0, 0, 0) ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_transfer_3d_impl(uint64_t resource_id, uint64_t x,
                                         uint64_t y, uint64_t z, uint64_t w,
                                         uint64_t h, uint64_t a6) {
    (void)a6;
    /* Stride must be the RESOURCE row pitch, not the transfer-box width.
     * Guest backing is a full linear image; sub-rect transfers start at
     * offset = y*stride + x*4. Using w*4 as stride scrambled partial uploads. */
    uint32_t res_w = (uint32_t)w;
    uint32_t res_h = (uint32_t)h;
    (void)virtio_gpu_res_dims((uint32_t)resource_id, &res_w, &res_h);
    if (res_w == 0) res_w = (uint32_t)w;
    if (res_h == 0) res_h = (uint32_t)h;

    uint32_t stride = res_w * 4;
    uint32_t layer_stride = stride * res_h;
    uint64_t offset = (uint64_t)y * (uint64_t)stride + (uint64_t)x * 4;
    return virtio_gpu_transfer_to_host_3d_for((uint32_t)resource_id,
                                              (uint32_t)x, (uint32_t)y,
                                              (uint32_t)z, (uint32_t)w,
                                              (uint32_t)h, 1,
                                              offset, 0, stride, layer_stride)
               ? 0
               : (uint64_t)-1;
}

/* Current working directory + absolute path resolution (relative paths). */
#define XOS_PATH_MAX 256
static char g_cwd[XOS_PATH_MAX] = "/";

static int path_abs(const char *in, char *out, size_t outsz) {
    char joined[XOS_PATH_MAX];
    char comps[32][XFS_NAME_MAX];
    int ncomp = 0;

    if (!in || !in[0] || !out || outsz < 2)
        return -1;

    if (in[0] == '/') {
        if (strlen(in) >= sizeof(joined))
            return -1;
        strcpy(joined, in);
    } else {
        size_t cl = strlen(g_cwd);
        size_t il = strlen(in);
        if (cl + 1 + il + 1 > sizeof(joined))
            return -1;
        strcpy(joined, g_cwd);
        if (cl == 0 || g_cwd[cl - 1] != '/') {
            joined[cl] = '/';
            strcpy(joined + cl + 1, in);
        } else {
            strcpy(joined + cl, in);
        }
    }

    const char *p = joined;
    if (*p == '/')
        p++;
    while (*p) {
        char comp[XFS_NAME_MAX];
        size_t i = 0;
        while (p[i] && p[i] != '/' && i < XFS_NAME_MAX - 1) {
            comp[i] = p[i];
            i++;
        }
        comp[i] = '\0';
        p += i;
        if (*p == '/')
            p++;
        if (comp[0] == '\0' || (comp[0] == '.' && comp[1] == '\0'))
            continue;
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
            if (ncomp > 0)
                ncomp--;
            continue;
        }
        if (ncomp >= 32)
            return -1;
        strcpy(comps[ncomp++], comp);
    }

    if (ncomp == 0) {
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }

    size_t pos = 0;
    for (int i = 0; i < ncomp; i++) {
        size_t cl = strlen(comps[i]);
        if (pos + 1 + cl + 1 > outsz)
            return -1;
        out[pos++] = '/';
        memcpy(out + pos, comps[i], cl);
        pos += cl;
    }
    out[pos] = '\0';
    return 0;
}

static uint64_t sys_open_impl(uint64_t path, uint64_t flags,
                         uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!path) return (uint64_t)-1;
    char abs[XOS_PATH_MAX];
    if (path_abs((const char *)path, abs, sizeof(abs)) != 0)
        return (uint64_t)-1;
    return (uint64_t)xfs_open(abs, (uint32_t)flags);
}

static uint64_t sys_read_impl(uint64_t fd, uint64_t buf, uint64_t count,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (!buf) return (uint64_t)-1;
    if (fd >= 64) return (uint64_t)pipe_read((int)fd, (void *)buf, (size_t)count);
    return (uint64_t)xfs_read((int)fd, (void *)buf, (size_t)count);
}

static uint64_t sys_write_impl(uint64_t fd, uint64_t buf, uint64_t count,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (!buf) return (uint64_t)-1;
    if (fd >= 64) return (uint64_t)pipe_write((int)fd, (const void *)buf, (size_t)count);
    return (uint64_t)xfs_write((int)fd, (const void *)buf, (size_t)count);
}

static uint64_t sys_close_impl(uint64_t fd, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (fd >= 64) { pipe_close((int)fd); return 0; }
    xfs_close((int)fd);
    return 0;
}

static uint64_t sys_mkdir_impl(uint64_t path, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!path) return (uint64_t)-1;
    char abs[XOS_PATH_MAX];
    if (path_abs((const char *)path, abs, sizeof(abs)) != 0)
        return (uint64_t)-1;
    return (uint64_t)xfs_mkdir(abs);
}

static uint64_t sys_readdir_impl(uint64_t fd, uint64_t entries, uint64_t max,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (!entries) return (uint64_t)-1;
    return (uint64_t)xfs_readdir((int)fd, (xfs_dirent_t *)entries, (int)max);
}

static uint64_t sys_time_impl(uint64_t utime, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!utime) return (uint64_t)-1;
    if (utime >= 0xffff800000000000ULL) return (uint64_t)-1;
    rtc_time_t t;
    rtc_read(&t);
    uint8_t *p = (uint8_t *)utime;
    p[0] = t.hour; p[1] = t.min; p[2] = t.sec;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* POSIX process management syscalls */

static uint64_t sys_fork_impl(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return proc_fork();
}

static uint64_t sys_exec_impl(uint64_t path, uint64_t argv, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!path) return (uint64_t)-1;
    /* New image → new heap. Stale brk from a prior cmds on this PID would
     * make malloc write into unmapped VA and freeze the cooperative sched. */
    proc_t *p = proc_current();
    if (p)
        proc_brk_reset(p->pid);
    return (uint64_t)proc_exec((const char *)path, (char *const *)argv);
}

static uint64_t sys_waitpid_impl(uint64_t pid, uint64_t status, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    int *st = status ? (int *)status : NULL;
    return (uint64_t)proc_waitpid((int)pid, st, (int)a3);
}

static uint64_t sys_getpid_impl(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    proc_t *p = proc_current();
    return p ? p->tgid : 0;  /* POSIX getpid returns thread group ID */
}

static uint64_t sys_getppid_impl(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    proc_t *p = proc_current();
    return p ? p->parent_pid : 0;
}

static uint64_t sys_pipe_impl(uint64_t upipefd, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!upipefd) return (uint64_t)-1;
    int pipefd[2];
    int ret = pipe_create(pipefd);
    if (ret < 0) return (uint64_t)-1;
    /* Copy pipefd to userspace */
    int *dst = (int *)upipefd;
    dst[0] = pipefd[0];
    dst[1] = pipefd[1];
    return 0;
}

static uint64_t sys_dup_impl(uint64_t oldfd, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* For file fds, dup just returns the same fd (no fd table per process yet) */
    if (oldfd < 0 || oldfd >= XFS_MAX_FDS) return (uint64_t)-1;
    return oldfd;
}

static uint64_t sys_dup2_impl(uint64_t oldfd, uint64_t newfd, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (oldfd < 0 || newfd < 0) return (uint64_t)-1;
    /* Simple: just return newfd (no real fd table management yet) */
    return newfd;
}

/* POSIX file extension syscalls */

static uint64_t sys_lseek_impl(uint64_t fd, uint64_t offset, uint64_t whence,
                               uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    return (uint64_t)xfs_lseek((int)fd, (int)offset, (int)whence);
}

static uint64_t sys_stat_impl(uint64_t path, uint64_t statbuf, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!path || !statbuf) return (uint64_t)-1;
    char abs[XOS_PATH_MAX];
    if (path_abs((const char *)path, abs, sizeof(abs)) != 0)
        return (uint64_t)-1;
    return (uint64_t)xfs_stat(abs, (xfs_dirent_t *)statbuf);
}

static uint64_t sys_fstat_impl(uint64_t fd, uint64_t statbuf, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!statbuf) return (uint64_t)-1;
    return (uint64_t)xfs_fstat((int)fd, (xfs_dirent_t *)statbuf);
}

static uint64_t sys_unlink_impl(uint64_t path, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!path) return (uint64_t)-1;
    char abs[XOS_PATH_MAX];
    if (path_abs((const char *)path, abs, sizeof(abs)) != 0)
        return (uint64_t)-1;
    return (uint64_t)xfs_unlink(abs);
}

static uint64_t sys_getcwd_impl(uint64_t buf, uint64_t size, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!buf || size == 0) return (uint64_t)-1;
    size_t len = strlen(g_cwd);
    if (len + 1 > size) return (uint64_t)-1;
    memcpy((void *)buf, g_cwd, len + 1);
    /* Return 0 on success — returning `buf` truncates to a negative int in
     * userspace when the pointer sits in high canonical VA. */
    return 0;
}

static uint64_t sys_chdir_impl(uint64_t path, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!path) return (uint64_t)-1;
    char abs[XOS_PATH_MAX];
    if (path_abs((const char *)path, abs, sizeof(abs)) != 0)
        return (uint64_t)-1;
    /* Verify path exists and is a directory */
    xfs_dirent_t st;
    if (xfs_stat(abs, &st) != 0) return (uint64_t)-1;
    if (!(st.flags & 1)) return (uint64_t)-1; /* not a directory */
    size_t len = strlen(abs);
    if (len >= sizeof(g_cwd)) return (uint64_t)-1;
    memcpy(g_cwd, abs, len + 1);
    return 0;
}

static uint64_t sys_proc_list_impl(uint64_t ubuf, uint64_t max,
                                   uint64_t a3, uint64_t a4,
                                   uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!ubuf || max == 0 || max > SCHED_MAX_PROCS)
        return (uint64_t)-1;
    proc_info_t *out = (proc_info_t *)ubuf;
    int n = 0;
    for (uint64_t i = 1; i < SCHED_MAX_PROCS && n < (int)max; i++) {
        proc_t *p = proc_by_pid(i);
        if (!p)
            continue;
        if (p->state == PROC_DEAD && p->reaped)
            continue;
        out[n].pid = p->pid;
        out[n].ppid = p->parent_pid;
        out[n].state = (uint32_t)p->state;
        memcpy(out[n].name, p->name, sizeof(out[n].name));
        n++;
    }
    return (uint64_t)n;
}

static uint64_t sys_port_list_impl(uint64_t ubuf, uint64_t max,
                                   uint64_t a3, uint64_t a4,
                                   uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!ubuf || max == 0 || max > IPC_MAX_PORTS)
        return (uint64_t)-1;
    return (uint64_t)ipc_port_list((void *)ubuf, (int)max);
}

static uint64_t sys_systime_ns_impl(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return systime_ns();
}

static uint64_t sys_clone_impl(uint64_t flags, uint64_t child_stack,
                               uint64_t ptid, uint64_t ctid, uint64_t tls,
                               uint64_t a6) {
    (void)a6;
    return proc_clone(flags, child_stack, ptid, ctid, tls);
}

extern int sys_futex_impl(uint32_t *uaddr, int op, uint32_t val,
                          uint64_t timeout_ns);

static uint64_t sys_futex_impl_wrapper(uint64_t uaddr, uint64_t op,
                                       uint64_t val, uint64_t timeout_ns,
                                       uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    return (uint64_t)sys_futex_impl((uint32_t *)uaddr, (int)op,
                                    (uint32_t)val, timeout_ns);
}

static uint64_t sys_gettid_impl(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    proc_t *p = proc_current();
    return p ? p->tid : 0;
}

static uint64_t sys_set_tid_address_impl(uint64_t tid_addr, uint64_t a2,
                                         uint64_t a3, uint64_t a4,
                                         uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    proc_t *p = proc_current();
    if (p) p->clear_tid_addr = tid_addr;
    return p ? p->tid : 0;
}

static uint64_t sys_msgbuf_read_impl(uint64_t ubuf, uint64_t size,
                                     uint64_t a3, uint64_t a4,
                                     uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!ubuf || size == 0)
        return (uint64_t)-1;
    if (size > MSGBUF_SIZE)
        size = MSGBUF_SIZE;
    return (uint64_t)msgbuf_copy((char *)ubuf, (size_t)size);
}

/* Minimal sysctl MIB inspired by XNU kern_mib / system_cmds. */
static uint64_t sys_sysctl_impl(uint64_t uname, uint64_t uout, uint64_t out_len,
                                uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (!uname || !uout || out_len == 0)
        return (uint64_t)-1;
    const char *name = (const char *)uname;
    char *out = (char *)uout;

    struct { const char *key; const char *val; } keys[] = {
        { "kern.ostype", "XOS" },
        { "kern.osrelease", "1.0.0" },
        { "kern.osversion", "XOS1" },
        { "kern.hostname", "x-os" },
        { "kern.version", "X OS 1.0; microkernel + userspace" },
        { "hw.ncpu", "1" },
        { "hw.memsize", "536870912" },
        { "hw.machine", "x86_64" },
        { NULL, NULL }
    };

    if (strcmp(name, "kern.msgbuf") == 0) {
        size_t n = msgbuf_copy(out, (size_t)out_len - 1);
        out[n] = '\0';
        return (uint64_t)n;
    }

    for (int i = 0; keys[i].key; i++) {
        if (strcmp(name, keys[i].key) == 0) {
            size_t n = strlen(keys[i].val);
            if (n + 1 > out_len)
                n = (size_t)out_len - 1;
            memcpy(out, keys[i].val, n);
            out[n] = '\0';
            return (uint64_t)n;
        }
    }
    return (uint64_t)-1;
}

/* Per-process brk (program break) for sbrk/malloc support.
 * The heap starts at USER_HEAP_BASE and grows upward.
 * We track the current break per-process using a simple static array. */
#define USER_HEAP_BASE  0x0000020000000000ULL
#define USER_HEAP_MAX   0x0000030000000000ULL

static uint64_t g_proc_brk[32];  /* per-pid brk, indexed by pid */

static void proc_brk_reset(uint64_t pid) {
    if (pid < 32)
        g_proc_brk[pid] = 0;
}

static uint64_t sys_brk_impl(uint64_t addr, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    proc_t *p = proc_current();
    if (!p || !p->ring3) return (uint64_t)-1;

    uint32_t pid_idx = (uint32_t)(p->pid);
    if (pid_idx >= 32) return (uint64_t)-1;

    /* Initialize brk on first call */
    if (g_proc_brk[pid_idx] == 0) {
        g_proc_brk[pid_idx] = USER_HEAP_BASE;
    }

    if (addr == 0) {
        /* Query current brk */
        return g_proc_brk[pid_idx];
    }

    if (addr < USER_HEAP_BASE || addr >= USER_HEAP_MAX) {
        return (uint64_t)-1;  /* invalid address */
    }

    /* Grow or shrink: map/unmap pages as needed */
    uint64_t old_brk = g_proc_brk[pid_idx];
    uint64_t old_page = (old_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t new_page = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (new_page > old_page) {
        /* Grow — map new pages */
        for (uint64_t va = old_page; va < new_page; va += PAGE_SIZE) {
            uint64_t page = pmm_alloc_frame();
            if (!page) {
                /* Out of memory — return current brk unchanged */
                return old_brk;
            }
            vmm_map_page(p->pml4_virt, va, page, VMM_U | VMM_RW);
        }
    } else if (new_page < old_page) {
        /* Shrink — unmap pages (don't free physical for simplicity) */
        for (uint64_t va = new_page; va < old_page; va += PAGE_SIZE) {
            vmm_unmap_page(p->pml4_virt, va);
        }
    }

    g_proc_brk[pid_idx] = addr;
    return addr;
}

/* Forward declarations for BSD POSIX syscall implementations */
uint64_t sys_socket_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_bind_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_listen_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_accept_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_connect_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_send_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_recv_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_sendto_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_recvfrom_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_shutdown_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_getsockname_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_getpeername_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_setsockopt_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_getsockopt_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_select_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_poll_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_mmap_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_munmap_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_mprotect_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_sigaction_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_sigprocmask_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_sigsuspend_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_kill_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
/* sys_sigreturn_impl declared in kernel/proc/signal.h */
uint64_t sys_fcntl_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_ioctl_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_net_send_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t sys_net_recv_impl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static uint64_t (*syscall_table[])(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = {
    [SYS_EXIT]        = (void *)sys_exit_impl,
    [SYS_YIELD]       = (void *)sys_yield_impl,
    [SYS_PORT_CREATE] = (void *)sys_port_create_impl,
    [SYS_PORT_SEND]   = (void *)sys_port_send_impl,
    [SYS_PORT_RECV]   = (void *)sys_port_recv_impl,
    [SYS_PORT_CLOSE]  = (void *)sys_port_close_impl,
    [SYS_MEM_ALLOC]   = (void *)sys_mem_alloc_impl,
    [SYS_MEM_MAP]     = (void *)sys_mem_map,
    [SYS_PROC_SPAWN]  = (void *)sys_proc_spawn,
    [SYS_PROC_PID]    = (void *)sys_proc_pid_impl,
    [SYS_NSLEEP]      = (void *)sys_nsleep,
    [SYS_DEBUG_LOG]   = (void *)sys_debug_log,
    [SYS_GET_TICKS]   = (void *)sys_get_ticks,
    [SYS_FB_INFO]     = (void *)sys_fb_info,
    [SYS_MOUSE_POS]   = (void *)sys_mouse_pos,
    [SYS_OPEN]        = (void *)sys_open_impl,
    [SYS_READ]        = (void *)sys_read_impl,
    [SYS_WRITE]       = (void *)sys_write_impl,
    [SYS_CLOSE]       = (void *)sys_close_impl,
    [SYS_MKDIR]       = (void *)sys_mkdir_impl,
    [SYS_READDIR]     = (void *)sys_readdir_impl,
    [SYS_INPUT_POLL]  = (void *)sys_input_poll_impl,
    [SYS_NS_REGISTER] = (void *)sys_ns_register_impl,
    [SYS_NS_LOOKUP]   = (void *)sys_ns_lookup_impl,
    [SYS_MEM_SHARE]   = (void *)sys_mem_share_impl,
    [SYS_PROC_EXISTS] = (void *)sys_proc_exists_impl,
    [SYS_MEM_FREE]    = (void *)sys_mem_free_impl,
    [SYS_GPU_FB_INFO]     = (void *)sys_gpu_fb_info_impl,
    [SYS_GPU_FLUSH]       = (void *)sys_gpu_flush_impl,
    [SYS_GPU_CURSOR_SET]  = (void *)sys_gpu_cursor_set_impl,
    [SYS_GPU_CURSOR_MOVE] = (void *)sys_gpu_cursor_move_impl,
    [SYS_PROC_KILL]       = (void *)sys_proc_kill_impl,
    [SYS_TIME]            = (void *)sys_time_impl,
    [SYS_GPU_VIRGL_PRESENT] = (void *)sys_gpu_virgl_present_impl,
    [SYS_GPU_CTX_CREATE]  = (void *)sys_gpu_ctx_create_impl,
    [SYS_GPU_CTX_DESTROY] = (void *)sys_gpu_ctx_destroy_impl,
    [SYS_GPU_CTX_ATTACH]  = (void *)sys_gpu_ctx_attach_impl,
    [SYS_GPU_RES_CREATE_2D] = (void *)sys_gpu_res_create_2d_impl,
    [SYS_GPU_RES_ATTACH]  = (void *)sys_gpu_res_attach_impl,
    [SYS_GPU_RES_UNREF]   = (void *)sys_gpu_res_unref_impl,
    [SYS_GPU_TRANSFER_2D] = (void *)sys_gpu_transfer_2d_impl,
    [SYS_GPU_SUBMIT_3D]   = (void *)sys_gpu_submit_3d_impl,
    [SYS_GPU_SET_SCANOUT] = (void *)sys_gpu_set_scanout_impl,
    [SYS_GPU_FLUSH_RES]   = (void *)sys_gpu_flush_res_impl,
    [SYS_GPU_ALLOC_RES_ID] = (void *)sys_gpu_alloc_res_id_impl,
    [SYS_GPU_RES_ATTACH_VIRT] = (void *)sys_gpu_res_attach_virt_impl,
    [SYS_GPU_RES_CREATE_3D]  = (void *)sys_gpu_res_create_3d_impl,
    [SYS_GPU_TRANSFER_3D]   = (void *)sys_gpu_transfer_3d_impl,
    [SYS_FORK]        = (void *)sys_fork_impl,
    [SYS_EXEC]        = (void *)sys_exec_impl,
    [SYS_WAITPID]     = (void *)sys_waitpid_impl,
    [SYS_GETPID]      = (void *)sys_getpid_impl,
    [SYS_GETPPID]     = (void *)sys_getppid_impl,
    [SYS_PIPE]        = (void *)sys_pipe_impl,
    [SYS_DUP]         = (void *)sys_dup_impl,
    [SYS_DUP2]        = (void *)sys_dup2_impl,
    [SYS_LSEEK]       = (void *)sys_lseek_impl,
    [SYS_STAT]        = (void *)sys_stat_impl,
    [SYS_FSTAT]       = (void *)sys_fstat_impl,
    [SYS_UNLINK]      = (void *)sys_unlink_impl,
    [SYS_GETCWD]      = (void *)sys_getcwd_impl,
    [SYS_CHDIR]       = (void *)sys_chdir_impl,
    [SYS_BRK]         = (void *)sys_brk_impl,
    [SYS_SOCKET]      = (void *)sys_socket_impl,
    [SYS_BIND]        = (void *)sys_bind_impl,
    [SYS_LISTEN]      = (void *)sys_listen_impl,
    [SYS_ACCEPT]      = (void *)sys_accept_impl,
    [SYS_CONNECT]     = (void *)sys_connect_impl,
    [SYS_SEND]        = (void *)sys_send_impl,
    [SYS_RECV]        = (void *)sys_recv_impl,
    [SYS_SENDTO]      = (void *)sys_sendto_impl,
    [SYS_RECVFROM]    = (void *)sys_recvfrom_impl,
    [SYS_SHUTDOWN]    = (void *)sys_shutdown_impl,
    [SYS_GETSOCKNAME] = (void *)sys_getsockname_impl,
    [SYS_GETPEERNAME] = (void *)sys_getpeername_impl,
    [SYS_SETSOCKOPT]  = (void *)sys_setsockopt_impl,
    [SYS_GETSOCKOPT]  = (void *)sys_getsockopt_impl,
    [SYS_SELECT]      = (void *)sys_select_impl,
    [SYS_POLL]        = (void *)sys_poll_impl,
    [SYS_MMAP]        = (void *)sys_mmap_impl,
    [SYS_MUNMAP]      = (void *)sys_munmap_impl,
    [SYS_MPROTECT]    = (void *)sys_mprotect_impl,
    [SYS_SIGACTION]   = (void *)sys_sigaction_impl,
    [SYS_SIGPROCMASK] = (void *)sys_sigprocmask_impl,
    [SYS_SIGSUSPEND]  = (void *)sys_sigsuspend_impl,
    [SYS_KILL]        = (void *)sys_kill_impl,
    [SYS_FCNTL]       = (void *)sys_fcntl_impl,
    [SYS_IOCTL]       = (void *)sys_ioctl_impl,
    [SYS_NET_SEND]    = (void *)sys_net_send_impl,
    [SYS_NET_RECV]    = (void *)sys_net_recv_impl,
    [SYS_PROC_LIST]   = (void *)sys_proc_list_impl,
    [SYS_PORT_LIST]   = (void *)sys_port_list_impl,
    [SYS_MSGBUF_READ] = (void *)sys_msgbuf_read_impl,
    [SYS_SYSCTL]      = (void *)sys_sysctl_impl,
    [SYS_SIGRETURN]   = (void *)sys_sigreturn_impl,
    [SYS_NO_PREEMPT]  = (void *)sys_no_preempt_impl,
    [SYS_SYSTIME_NS]  = (void *)sys_systime_ns_impl,
    [SYS_CLONE]       = (void *)sys_clone_impl,
    [SYS_FUTEX]       = (void *)sys_futex_impl_wrapper,
    [SYS_GETTID]      = (void *)sys_gettid_impl,
    [SYS_SET_TID_ADDRESS] = (void *)sys_set_tid_address_impl,
};

#define NUM_SYSCALLS (sizeof(syscall_table) / sizeof(syscall_table[0]))

/* Called from syscall_entry.S with a pointer to the saved arg frame. */
uint64_t syscall_dispatch(syscall_args_t *args) {
    if (args->num >= NUM_SYSCALLS || !syscall_table[args->num]) {
        kprintf("[syscall] unimplemented num=%lu\n", args->num);
        return (uint64_t)-1;
    }

    uint64_t (*fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    fn = syscall_table[args->num];
    return fn(args->a1, args->a2, args->a3, args->a4, args->a5, args->a6);
}

/* Called from syscall_entry.S when the ret frame is corrupted.
 * rdi = current rsp (points into the 112-byte dispatch frame or
 *       the 64-byte spill frame, depending on when the check fired)
 * rsi = the bad value that failed the check */
void syscall_ret_panic(uint64_t rsp, uint64_t bad_val) {
    proc_t *cur = proc_current();
    uint64_t krsp0 = this_cpu()->rsp0;
    kprintf("\n*** SYSCALL RET FRAME CORRUPTED ***\n");
    kprintf("    rsp=%lx bad_val=%lx krsp0=%lx\n", rsp, bad_val, krsp0);
    if (cur) {
        kprintf("    pid=%lu state=%d ring3=%d kstack=%lx-%lx\n",
                cur->pid, cur->state, cur->ring3,
                (uint64_t)cur->kstack, (uint64_t)cur->kstack + SCHED_STACK_SIZE);
        kprintf("    cur->rsp=%lx saved_ret=%lx rip=%lx\n",
                cur->rsp, cur->saved_ret, cur->rip);
        kprintf("    canary=%lx switching=%d\n", cur->canary, cur->switching);
    }
    /* Dump kstack top (where ret frame should be) */
    if (krsp0) {
        uint64_t *ktop = (uint64_t *)(krsp0 - 24);
        kprintf("    ret frame: rflags=%lx rip=%lx user_rsp=%lx\n",
                ktop[0], ktop[1], ktop[2]);
        kprintf("    kstack top dump:\n");
        uint64_t *p = (uint64_t *)(krsp0 - 80);
        for (int i = 0; i < 10; i++)
            kprintf("      ktop-%d: %lx\n", (10-i)*8, p[i]);
    }
    for (;;) __asm__ volatile("cli; hlt");
}

/* -------------------------------------------------------------------------- */

void syscall_init(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    /* STAR[47:32] = syscall CS (kernel code, 0x08), ss = +8 = 0x10
     * STAR[63:48] = sysret  CS (user code,  0x1B), ss = +8 = 0x23 */
    wrmsr(MSR_STAR, ((uint64_t)0x1B << 48) | ((uint64_t)0x08 << 32));

    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_CSTAR, 0);  /* compatibility mode; unused */

    /* Clear IF (bit 9) and DF (bit 10) in RFLAGS during syscall handler.
     * AC (bit 18) is also often cleared. */
    wrmsr(MSR_SFMASK, 0x200 | 0x400 | 0x40000);

    kputs("[syscall] syscall/sysret enabled (STAR=0x001B00000008)\n");
}

/* Per-CPU syscall MSR init for APs — same as syscall_init but without
 * the log message (BSP already logged it). */
void syscall_init_ap(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);
    wrmsr(MSR_STAR, ((uint64_t)0x1B << 48) | ((uint64_t)0x08 << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_CSTAR, 0);
    wrmsr(MSR_SFMASK, 0x200 | 0x400 | 0x40000);
}
