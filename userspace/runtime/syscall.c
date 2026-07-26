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
    register uintptr_t r10 __asm__("r10") = a4;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
        : "rcx", "r11", "r8", "r9", "memory"
    );
    return ret;
}

uintptr_t syscall5(uint64_t num, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                   uintptr_t a4, uintptr_t a5) {
    uintptr_t ret;
    register uintptr_t r10 __asm__("r10") = a4;
    register uintptr_t r8 __asm__("r8") = a5;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "r9", "memory"
    );
    return ret;
}

uintptr_t syscall6(uint64_t num, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                   uintptr_t a4, uintptr_t a5, uintptr_t a6) {
    uintptr_t ret;
    register uintptr_t r10 __asm__("r10") = a4;
    register uintptr_t r8 __asm__("r8") = a5;
    register uintptr_t r9 __asm__("r9") = a6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
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

int sys_proc_list(proc_info_t *buf, int max) {
    return (int)syscall2(SYS_PROC_LIST, (uintptr_t)buf, (uintptr_t)max);
}

int sys_port_list(port_info_t *buf, int max) {
    return (int)syscall2(SYS_PORT_LIST, (uintptr_t)buf, (uintptr_t)max);
}

int sys_msgbuf_read(char *buf, size_t size) {
    return (int)syscall2(SYS_MSGBUF_READ, (uintptr_t)buf, (uintptr_t)size);
}

int sys_sysctl(const char *name, char *out, size_t out_len) {
    return (int)syscall3(SYS_SYSCTL, (uintptr_t)name, (uintptr_t)out, (uintptr_t)out_len);
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

/* GPU virgl wrappers */

int sys_gpu_virgl_present(void) {
    return (int)syscall0(SYS_GPU_VIRGL_PRESENT);
}

int sys_gpu_ctx_create(uint32_t ctx_id) {
    return (int)syscall1(SYS_GPU_CTX_CREATE, ctx_id);
}

int sys_gpu_ctx_destroy(uint32_t ctx_id) {
    return (int)syscall1(SYS_GPU_CTX_DESTROY, ctx_id);
}

int sys_gpu_ctx_attach(uint32_t ctx_id, uint32_t resource_id) {
    return (int)syscall2(SYS_GPU_CTX_ATTACH, ctx_id, resource_id);
}

int sys_gpu_res_create_2d(uint32_t resource_id, uint32_t format,
                          uint32_t width, uint32_t height) {
    return (int)syscall4(SYS_GPU_RES_CREATE_2D, resource_id, format, width, height);
}

int sys_gpu_res_attach(uint32_t resource_id, uint64_t phys, uint64_t size) {
    return (int)syscall3(SYS_GPU_RES_ATTACH, resource_id, phys, size);
}

int sys_gpu_res_unref(uint32_t resource_id) {
    return (int)syscall1(SYS_GPU_RES_UNREF, resource_id);
}

int sys_gpu_transfer_2d(uint32_t resource_id, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, uint64_t offset) {
    return (int)syscall6(SYS_GPU_TRANSFER_2D, resource_id, x, y, w, h, offset);
}

int sys_gpu_submit_3d(uint32_t ctx_id, void *cmds, uint32_t size) {
    return (int)syscall3(SYS_GPU_SUBMIT_3D, ctx_id, (uintptr_t)cmds, size);
}

int sys_gpu_set_scanout(uint32_t scanout_id, uint32_t resource_id,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return (int)syscall6(SYS_GPU_SET_SCANOUT, scanout_id, resource_id, x, y, w, h);
}

int sys_gpu_flush_res(uint32_t resource_id, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h) {
    return (int)syscall5(SYS_GPU_FLUSH_RES, resource_id, x, y, w, h);
}

uint32_t sys_gpu_alloc_res_id(void) {
    return (uint32_t)syscall0(SYS_GPU_ALLOC_RES_ID);
}

int sys_gpu_res_attach_virt(uint32_t resource_id, uint64_t vaddr, uint32_t npages,
                            uint64_t buf_size) {
    return (int)syscall4(SYS_GPU_RES_ATTACH_VIRT, resource_id, vaddr, npages, buf_size);
}

int sys_gpu_res_create_3d(uint32_t resource_id, uint32_t target, uint32_t format,
                          uint32_t bind, uint32_t width, uint32_t height,
                          uint32_t depth, uint32_t array_size,
                          uint32_t last_level, uint32_t nr_samples,
                          uint32_t flags) {
    (void)depth; (void)array_size; (void)last_level; (void)nr_samples; (void)flags;
    return (int)syscall6(SYS_GPU_RES_CREATE_3D, resource_id, target, format,
                         bind, width, height);
}

int sys_gpu_transfer_3d(uint32_t resource_id, uint32_t x, uint32_t y,
                        uint32_t z, uint32_t w, uint32_t h, uint32_t d,
                        uint64_t offset, uint32_t level, uint32_t stride,
                        uint32_t layer_stride) {
    (void)d; (void)offset; (void)level; (void)stride; (void)layer_stride;
    return (int)syscall6(SYS_GPU_TRANSFER_3D, resource_id, x, y, z, w, h);
}

/* Time wrapper — reads RTC hour/min/sec into 3-byte buffer */
int sys_time(uint8_t *hour, uint8_t *min, uint8_t *sec) {
    uint8_t buf[3];
    int ret = (int)syscall1(SYS_TIME, (uintptr_t)buf);
    if (ret == 0) { *hour = buf[0]; *min = buf[1]; *sec = buf[2]; }
    return ret;
}

uint64_t sys_systime_ns(void) {
    return syscall0(SYS_SYSTIME_NS);
}

uint64_t sys_clone(uint64_t flags, uint64_t child_stack, uint64_t ptid,
                   uint64_t ctid, uint64_t tls) {
    return syscall5(SYS_CLONE, flags, child_stack, ptid, ctid, tls);
}

int sys_futex(uint32_t *uaddr, int op, uint32_t val, uint64_t timeout_ns) {
    return (int)syscall4(SYS_FUTEX, (uintptr_t)uaddr, (uintptr_t)op,
                         (uintptr_t)val, timeout_ns);
}

uint64_t sys_gettid(void) {
    return syscall0(SYS_GETTID);
}

int sys_set_tid_address(uint32_t *tid_addr) {
    return (int)syscall1(SYS_SET_TID_ADDRESS, (uintptr_t)tid_addr);
}

/* POSIX process wrappers */

void sys_exit(int code) {
    syscall1(SYS_EXIT, (uintptr_t)code);
    while (1) {}
}

void sys_yield(void) {
    syscall0(SYS_YIELD);
}

int sys_fork(void) {
    return (int)syscall0(SYS_FORK);
}

int sys_exec(const char *path, char *const argv[]) {
    return (int)syscall2(SYS_EXEC, (uintptr_t)path, (uintptr_t)argv);
}

int sys_waitpid(int pid, int *status, int options) {
    return (int)syscall3(SYS_WAITPID, (uintptr_t)pid, (uintptr_t)status,
                         (uintptr_t)options);
}

int sys_getpid(void) {
    return (int)syscall0(SYS_GETPID);
}

int sys_getppid(void) {
    return (int)syscall0(SYS_GETPPID);
}

int sys_pipe(int pipefd[2]) {
    return (int)syscall1(SYS_PIPE, (uintptr_t)pipefd);
}

int sys_dup(int oldfd) {
    return (int)syscall1(SYS_DUP, (uintptr_t)oldfd);
}

int sys_dup2(int oldfd, int newfd) {
    return (int)syscall2(SYS_DUP2, (uintptr_t)oldfd, (uintptr_t)newfd);
}

/* POSIX file extension wrappers */

int sys_lseek(int fd, int offset, int whence) {
    return (int)syscall3(SYS_LSEEK, (uintptr_t)fd, (uintptr_t)offset, (uintptr_t)whence);
}

int sys_stat(const char *path, void *statbuf) {
    return (int)syscall2(SYS_STAT, (uintptr_t)path, (uintptr_t)statbuf);
}

int sys_fstat(int fd, void *statbuf) {
    return (int)syscall2(SYS_FSTAT, (uintptr_t)fd, (uintptr_t)statbuf);
}

int sys_unlink(const char *path) {
    return (int)syscall1(SYS_UNLINK, (uintptr_t)path);
}

int sys_getcwd(char *buf, size_t size) {
    return (int)syscall2(SYS_GETCWD, (uintptr_t)buf, (uintptr_t)size);
}

int sys_chdir(const char *path) {
    return (int)syscall1(SYS_CHDIR, (uintptr_t)path);
}

uint64_t sys_brk(void *addr) {
    return syscall1(SYS_BRK, (uintptr_t)addr);
}

/* POSIX socket wrappers */
int sys_socket(int domain, int type, int protocol) {
    return (int)syscall3(SYS_SOCKET, domain, type, protocol);
}
int sys_bind(int fd, const void *addr, int addrlen) {
    return (int)syscall3(SYS_BIND, fd, (uintptr_t)addr, addrlen);
}
int sys_listen(int fd, int backlog) {
    return (int)syscall2(SYS_LISTEN, fd, backlog);
}
int sys_accept(int fd, void *addr, int *addrlen) {
    return (int)syscall3(SYS_ACCEPT, fd, (uintptr_t)addr, (uintptr_t)addrlen);
}
int sys_connect(int fd, const void *addr, int addrlen) {
    return (int)syscall3(SYS_CONNECT, fd, (uintptr_t)addr, addrlen);
}
int sys_send(int fd, const void *buf, size_t len, int flags) {
    return (int)syscall4(SYS_SEND, fd, (uintptr_t)buf, len, flags);
}
int sys_recv(int fd, void *buf, size_t len, int flags) {
    return (int)syscall4(SYS_RECV, fd, (uintptr_t)buf, len, flags);
}
int sys_sendto(int fd, const void *buf, size_t len, int flags,
               const void *addr, int addrlen) {
    return (int)syscall6(SYS_SENDTO, fd, (uintptr_t)buf, len, flags, (uintptr_t)addr, addrlen);
}
int sys_recvfrom(int fd, void *buf, size_t len, int flags,
                 void *addr, int *addrlen) {
    return (int)syscall6(SYS_RECVFROM, fd, (uintptr_t)buf, len, flags, (uintptr_t)addr, (uintptr_t)addrlen);
}
int sys_shutdown(int fd, int how) {
    return (int)syscall2(SYS_SHUTDOWN, fd, how);
}
int sys_getsockname(int fd, void *addr, int *addrlen) {
    return (int)syscall3(SYS_GETSOCKNAME, fd, (uintptr_t)addr, (uintptr_t)addrlen);
}
int sys_getpeername(int fd, void *addr, int *addrlen) {
    return (int)syscall3(SYS_GETPEERNAME, fd, (uintptr_t)addr, (uintptr_t)addrlen);
}
int sys_setsockopt(int fd, int level, int optname, const void *optval, int optlen) {
    return (int)syscall5(SYS_SETSOCKOPT, fd, level, optname, (uintptr_t)optval, optlen);
}
int sys_getsockopt(int fd, int level, int optname, void *optval, int *optlen) {
    return (int)syscall5(SYS_GETSOCKOPT, fd, level, optname, (uintptr_t)optval, (uintptr_t)optlen);
}
int sys_select(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout) {
    return (int)syscall5(SYS_SELECT, nfds, (uintptr_t)readfds, (uintptr_t)writefds, (uintptr_t)exceptfds, (uintptr_t)timeout);
}
int sys_poll(void *fds, int nfds, int timeout) {
    return (int)syscall3(SYS_POLL, (uintptr_t)fds, nfds, timeout);
}
void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, int offset) {
    return (void *)syscall6(SYS_MMAP, (uintptr_t)addr, length, prot, flags, fd, offset);
}
int sys_munmap(void *addr, size_t length) {
    return (int)syscall2(SYS_MUNMAP, (uintptr_t)addr, length);
}
int sys_mprotect(void *addr, size_t length, int prot) {
    return (int)syscall3(SYS_MPROTECT, (uintptr_t)addr, length, prot);
}
int sys_sigaction(int signum, const void *act, void *oldact) {
    return (int)syscall3(SYS_SIGACTION, signum, (uintptr_t)act, (uintptr_t)oldact);
}
int sys_sigprocmask(int how, const void *set, void *oldset) {
    return (int)syscall3(SYS_SIGPROCMASK, how, (uintptr_t)set, (uintptr_t)oldset);
}
int sys_sigsuspend(const void *mask) {
    return (int)syscall1(SYS_SIGSUSPEND, (uintptr_t)mask);
}
int sys_kill(int pid, int sig) {
    return (int)syscall2(SYS_KILL, pid, sig);
}
int sys_sigreturn(void) {
    return (int)syscall0(SYS_SIGRETURN);
}
int sys_fcntl(int fd, int cmd, int arg) {
    return (int)syscall3(SYS_FCNTL, fd, cmd, arg);
}
int sys_ioctl(int fd, int cmd, void *arg) {
    return (int)syscall3(SYS_IOCTL, fd, cmd, (uintptr_t)arg);
}
int sys_net_send(const void *buf, int len) {
    return (int)syscall2(SYS_NET_SEND, (uintptr_t)buf, len);
}
int sys_net_recv(void *buf, int maxlen) {
    return (int)syscall2(SYS_NET_RECV, (uintptr_t)buf, maxlen);
}
