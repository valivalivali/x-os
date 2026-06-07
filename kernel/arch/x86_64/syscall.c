#include "kernel/include/syscall.h"
#include "kernel/sched/sched.h"
#include "kernel/proc/proc.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/vmm.h"
#include "kernel/memory/pmm.h"
#include "kernel/ipc/ipc.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/hal/timers/timer.h"
#include "kernel/arch/x86_64/serial.h"
#include "kernel/hal/input/input.h"
#include "kernel/hal/block/block_dev.h"
#include "kernel/fs/xfs.h"
#include "kernel/hal/gpu/virtio_gpu.h"
#include "boot/handoff/handoff.h"
#include <stdint.h>

/* Assembly entry point, defined in syscall_entry.S */
extern void syscall_entry(void);

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

static uint64_t sys_yield(void) {
    sched_yield();
    return 0;
}

static uint64_t sys_exit(uint64_t code) {
    (void)code;
    proc_t *p = proc_current();
    if (p && p->pid != 0) {
        proc_exit(p);
        sched_yield();  /* should never return */
    }
    return 0;
}

static uint64_t sys_nsleep(uint64_t ms) {
    proc_sleep(ms);
    return 0;
}

static uint64_t sys_getpid(void) {
    proc_t *p = proc_current();
    return p ? p->pid : 0;
}

static uint64_t sys_proc_spawn(uint64_t uelf, uint64_t len,
                               uint64_t a3, uint64_t a4,
                               uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!uelf || len == 0 || len > 1024 * 1024) {
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

    kfree(kbuf);
    return pid;
}

static uint64_t sys_debug_log(uint64_t umsg, uint64_t len,
                              uint64_t a3, uint64_t a4,
                              uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!umsg || len == 0 || len > 4096) return 0;
    const char *s = (const char *)umsg;
    /* Prevent unbounded kernel loops on bad userspace pointers.
     * We trust the pointer since kernel higher-half is mapped. */
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\0') break;
        serial_putc(c);
    }
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
    info->phys_base = (uint64_t)h->fb.addr - h->hhdm_offset;
    info->width     = (uint32_t)h->fb.width;
    info->height    = (uint32_t)h->fb.height;
    info->pitch     = (uint32_t)h->fb.pitch;
    info->bpp       = h->fb.bpp;
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

static uint64_t sys_svc_blob(uint64_t index, uint64_t ubuf,
                             uint64_t maxlen, uint64_t a4,
                             uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    extern const uint8_t *composer_elf_data;
    extern size_t composer_elf_len;
    extern const uint8_t *window_elf_data;
    extern size_t window_elf_len;
    const uint8_t *data = NULL;
    size_t len = 0;
    if (index == 0) { data = composer_elf_data; len = composer_elf_len; }
    else if (index == 1) { data = window_elf_data; len = window_elf_len; }
    else return 0;
    if (!ubuf) return len;
    size_t n = maxlen < len ? maxlen : len;
    memcpy((void *)ubuf, data, n);
    return n;
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
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!umsg) return 0;
    ipc_msg_t msg;
    memcpy(&msg, (const void *)umsg, sizeof(msg));
    return port_send(handle, &msg) ? 1 : 0;
}

static uint64_t sys_port_recv_impl(uint64_t handle, uint64_t umsg,
                                   uint64_t block,
                                   uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (!umsg) return 0;
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
    if (!frame) return (uint64_t)-1;
    proc_t *p = proc_current();
    if (!p || !p->pml4_virt) {
        pmm_free_frame(frame);
        return (uint64_t)-1;
    }
    uint64_t f = (flags & (VMM_RW | VMM_WT | VMM_CD)) | VMM_U | VMM_P;
    if (!vmm_map_page(p->pml4_virt, vaddr, frame, f)) {
        pmm_free_frame(frame);
        return (uint64_t)-1;
    }
    return 0;
}

static uint64_t sys_input_poll_impl(uint64_t uevent, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!uevent) return 0;
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

    uint64_t f = (flags & (VMM_RW | VMM_WT | VMM_CD)) | VMM_U | VMM_P;
    if (!vmm_map_page(target->pml4_virt, target_vaddr, paddr, f))
        return (uint64_t)-1;
    return 0;
}

static uint64_t sys_proc_exists_impl(uint64_t pid, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    proc_t *p = proc_by_pid(pid);
    return p ? 1 : 0;
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

static uint64_t sys_open_impl(uint64_t path, uint64_t flags,
                         uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!path) return (uint64_t)-1;
    return (uint64_t)xfs_open((const char *)path, (uint32_t)flags);
}

static uint64_t sys_read_impl(uint64_t fd, uint64_t buf, uint64_t count,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (!buf) return (uint64_t)-1;
    return (uint64_t)xfs_read((int)fd, (void *)buf, (size_t)count);
}

static uint64_t sys_write_impl(uint64_t fd, uint64_t buf, uint64_t count,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (!buf) return (uint64_t)-1;
    return (uint64_t)xfs_write((int)fd, (const void *)buf, (size_t)count);
}

static uint64_t sys_close_impl(uint64_t fd, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    xfs_close((int)fd);
    return 0;
}

static uint64_t sys_mkdir_impl(uint64_t path, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!path) return (uint64_t)-1;
    return (uint64_t)xfs_mkdir((const char *)path);
}

static uint64_t sys_readdir_impl(uint64_t fd, uint64_t entries, uint64_t max,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (!entries) return (uint64_t)-1;
    return (uint64_t)xfs_readdir((int)fd, (xfs_dirent_t *)entries, (int)max);
}

static uint64_t (*syscall_table[])(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = {
    [SYS_EXIT]        = (void *)sys_exit,
    [SYS_YIELD]       = (void *)sys_yield,
    [SYS_PORT_CREATE] = (void *)sys_port_create_impl,
    [SYS_PORT_SEND]   = (void *)sys_port_send_impl,
    [SYS_PORT_RECV]   = (void *)sys_port_recv_impl,
    [SYS_PORT_CLOSE]  = (void *)sys_port_close_impl,
    [SYS_MEM_ALLOC]   = (void *)sys_mem_alloc_impl,
    [SYS_MEM_MAP]     = (void *)sys_mem_map,
    [SYS_PROC_SPAWN]  = (void *)sys_proc_spawn,
    [SYS_PROC_PID]    = (void *)sys_getpid,
    [SYS_NSLEEP]      = (void *)sys_nsleep,
    [SYS_DEBUG_LOG]   = (void *)sys_debug_log,
    [SYS_GET_TICKS]   = (void *)sys_get_ticks,
    [SYS_FB_INFO]     = (void *)sys_fb_info,
    [SYS_SVC_BLOB]    = (void *)sys_svc_blob,
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
