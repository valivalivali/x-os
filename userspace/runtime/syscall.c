#include "kernel/include/syscall.h"

/* Userspace syscall wrappers — assembly stubs that invoke `syscall` instruction.
 * These are linked into every userspace ELF (services and apps).
 */

uintptr_t syscall0(uint64_t num) {
    uintptr_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory"
    );
    return ret;
}

uintptr_t syscall1(uint64_t num, uintptr_t a1) {
    uintptr_t ret;
    __asm__ volatile (
        "movq %2, %%rdi\n"
        "syscall"
        : "=a"(ret)
        : "a"(num), "r"(a1)
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory"
    );
    return ret;
}

uintptr_t syscall2(uint64_t num, uintptr_t a1, uintptr_t a2) {
    uintptr_t ret;
    __asm__ volatile (
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "syscall"
        : "=a"(ret)
        : "a"(num), "r"(a1), "r"(a2)
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory"
    );
    return ret;
}

uintptr_t syscall3(uint64_t num, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    uintptr_t ret;
    __asm__ volatile (
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "syscall"
        : "=a"(ret)
        : "a"(num), "r"(a1), "r"(a2), "r"(a3)
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory"
    );
    return ret;
}

uintptr_t syscall4(uint64_t num, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                   uintptr_t a4) {
    uintptr_t ret;
    __asm__ volatile (
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "movq %5, %%r10\n"
        "syscall"
        : "=a"(ret)
        : "a"(num), "r"(a1), "r"(a2), "r"(a3), "r"(a4)
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory"
    );
    return ret;
}

/* Port / IPC wrappers */

port_handle_t sys_port_create(void) {
    return (port_handle_t)syscall0(SYS_PORT_CREATE);
}

int sys_port_send(port_handle_t h, const ipc_msg_t *msg) {
    return (int)syscall2(SYS_PORT_SEND, h, (uintptr_t)msg);
}

int sys_port_recv(port_handle_t h, ipc_msg_t *out, int block) {
    return (int)syscall3(SYS_PORT_RECV, h, (uintptr_t)out, (uintptr_t)block);
}

void sys_port_close(port_handle_t h) {
    syscall1(SYS_PORT_CLOSE, h);
}

/* Nameserver wrappers */

void sys_ns_register(uint32_t id, port_handle_t port) {
    syscall2(SYS_NS_REGISTER, id, port);
}

port_handle_t sys_ns_lookup(uint32_t id) {
    return (port_handle_t)syscall1(SYS_NS_LOOKUP, id);
}

/* Memory wrappers */

int sys_mem_alloc(uint64_t vaddr, uint64_t flags) {
    return (int)syscall2(SYS_MEM_ALLOC, vaddr, flags);
}

int sys_mem_share(uint64_t vaddr, uint64_t target_pid, uint64_t target_vaddr,
                  uint64_t flags) {
    return (int)syscall4(SYS_MEM_SHARE, vaddr, target_pid, target_vaddr, flags);
}

int sys_proc_exists(uint64_t pid) {
    return (int)syscall1(SYS_PROC_EXISTS, pid);
}

/* Input wrappers */

int sys_input_poll(input_event_t *out) {
    return (int)syscall1(SYS_INPUT_POLL, (uintptr_t)out);
}

/* Filesystem wrappers */

int sys_open(const char *path, uint32_t flags) {
    return (int)syscall2(SYS_OPEN, (uintptr_t)path, flags);
}

int sys_read(int fd, void *buf, size_t count) {
    return (int)syscall3(SYS_READ, fd, (uintptr_t)buf, count);
}

int sys_write(int fd, const void *buf, size_t count) {
    return (int)syscall3(SYS_WRITE, fd, (uintptr_t)buf, count);
}

void sys_close(int fd) {
    syscall1(SYS_CLOSE, fd);
}

int sys_mkdir(const char *path) {
    return (int)syscall1(SYS_MKDIR, (uintptr_t)path);
}

int sys_readdir(int fd, void *entries, int max) {
    return (int)syscall3(SYS_READDIR, fd, (uintptr_t)entries, max);
}

int sys_gpu_fb_info(gpu_fb_info_t *info) {
    return (int)syscall1(SYS_GPU_FB_INFO, (uintptr_t)info);
}

int sys_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return (int)syscall4(SYS_GPU_FLUSH, x, y, w, h);
}

int sys_gpu_cursor_set(int32_t x, int32_t y, uint32_t hot_x, uint32_t hot_y) {
    return (int)syscall4(SYS_GPU_CURSOR_SET, (uint64_t)(int64_t)x, (uint64_t)(int64_t)y, hot_x, hot_y);
}

int sys_gpu_cursor_move(int32_t x, int32_t y) {
    return (int)syscall2(SYS_GPU_CURSOR_MOVE, (uint64_t)(int64_t)x, (uint64_t)(int64_t)y);
}
