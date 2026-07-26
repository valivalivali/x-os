#pragma once
#include <stdint.h>
#include <stddef.h>
#include "kernel/include/ipc.h"

/* X OS Microkernel — syscall numbers.
 * These are the ONLY ways userspace can ask the kernel for service.
 * There are no open(), read(), write(), socket(), or mmap() syscalls.
 * Those live in ring-3 services that communicate via IPC.
 */

#define SYS_EXIT        0
#define SYS_YIELD       1
#define SYS_PORT_CREATE 2
#define SYS_PORT_SEND   3
#define SYS_PORT_RECV   4
#define SYS_PORT_CLOSE  5
#define SYS_MEM_ALLOC   6
#define SYS_MEM_MAP     7
/* 8-9 reserved (unimplemented) */
#define SYS_PROC_SPAWN  10
#define SYS_PROC_PID    11
#define SYS_NSLEEP      12
#define SYS_DEBUG_LOG   13
#define SYS_GET_TICKS   14
#define SYS_FB_INFO     15
/* 16 reserved (was SYS_SVC_BLOB, removed) */
#define SYS_MOUSE_POS   17
#define SYS_OPEN        18
#define SYS_READ        19
#define SYS_WRITE       20
#define SYS_CLOSE       21
#define SYS_MKDIR       22
#define SYS_READDIR     23
#define SYS_INPUT_POLL  24
#define SYS_NS_REGISTER 25
#define SYS_NS_LOOKUP   26
#define SYS_MEM_SHARE   27
#define SYS_PROC_EXISTS 28
#define SYS_MEM_FREE    29
#define SYS_GPU_FB_INFO 30
#define SYS_GPU_FLUSH        31
#define SYS_GPU_CURSOR_SET   32
#define SYS_GPU_CURSOR_MOVE  33
#define SYS_PROC_KILL        34
#define SYS_TIME             35
#define SYS_GPU_VIRGL_PRESENT 36
#define SYS_GPU_CTX_CREATE   37
#define SYS_GPU_CTX_DESTROY  38
#define SYS_GPU_CTX_ATTACH   39
#define SYS_GPU_RES_CREATE_2D 40
#define SYS_GPU_RES_ATTACH   41
#define SYS_GPU_RES_UNREF    42
#define SYS_GPU_TRANSFER_2D  43
#define SYS_GPU_SUBMIT_3D    44
#define SYS_GPU_SET_SCANOUT  45
#define SYS_GPU_FLUSH_RES    46
#define SYS_GPU_ALLOC_RES_ID 47
#define SYS_GPU_RES_ATTACH_VIRT 48
#define SYS_GPU_RES_CREATE_3D  49
#define SYS_GPU_TRANSFER_3D   64

/* POSIX process management */
#define SYS_FORK        50
#define SYS_EXEC        51
#define SYS_WAITPID     52
#define SYS_GETPID      53
#define SYS_PIPE        54
#define SYS_DUP         55
#define SYS_DUP2        56

/* POSIX file extensions */
#define SYS_LSEEK       57
#define SYS_STAT        58
#define SYS_FSTAT       59
#define SYS_UNLINK      60
#define SYS_GETCWD      61
#define SYS_CHDIR       62
#define SYS_BRK         63

/* POSIX socket syscalls (from XNU BSD) */
#define SYS_SOCKET      65
#define SYS_BIND        66
#define SYS_LISTEN      67
#define SYS_ACCEPT      68
#define SYS_CONNECT     69
#define SYS_SEND        70
#define SYS_RECV        71
#define SYS_SENDTO      72
#define SYS_RECVFROM    73
#define SYS_SHUTDOWN    74
#define SYS_GETSOCKNAME 75
#define SYS_GETPEERNAME 76
#define SYS_SETSOCKOPT  77
#define SYS_GETSOCKOPT  78
#define SYS_SELECT      79
#define SYS_POLL        80
#define SYS_MMAP        81
#define SYS_MUNMAP      82
#define SYS_MPROTECT    83
#define SYS_SIGACTION   84
#define SYS_SIGPROCMASK 85
#define SYS_KILL        86
#define SYS_FCNTL       87
#define SYS_IOCTL       88
#define SYS_NET_SEND    89
#define SYS_NET_RECV    90
#define SYS_PROC_LIST   91
#define SYS_MSGBUF_READ 92
#define SYS_SYSCTL      93
#define SYS_GETPPID     94
#define SYS_SIGRETURN   95
#define SYS_SIGSUSPEND  96
#define SYS_NO_PREEMPT  97  /* set/clear no-preempt flag for current process */
#define SYS_PORT_LIST   98  /* dump IPC port table for introspection */
#define SYS_SYSTIME_NS  99  /* high-res monotonic time in nanoseconds (TSC) */

#define SYSCALL_MAX          99

/* Process table entry for SYS_PROC_LIST (adv_cmds ps). */
typedef struct {
    uint64_t pid;
    uint64_t ppid;
    uint32_t state; /* PROC_* from sched.h */
    char     name[16];
} proc_info_t;

/* Port table entry for SYS_PORT_LIST (introspection tool). */
typedef struct {
    uint32_t handle;
    uint32_t used;      /* 1 = in use */
    uint64_t owner_pid;
    uint32_t count;     /* messages queued */
    uint32_t depth;     /* capacity */
} port_info_t;

/* Page flags for SYS_MEM_MAP (match kernel VMM_* constants) */
#define VMM_P   (1ULL << 0)
#define VMM_RW  (1ULL << 1)
#define VMM_U   (1ULL << 2)
#define VMM_WT  (1ULL << 3)   /* write-through */
#define VMM_CD  (1ULL << 4)   /* cache disable */

/* Framebuffer info returned by SYS_FB_INFO */
typedef struct {
    uint64_t phys_base;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint16_t bpp;
} fb_info_t;

/* GPU framebuffer info returned by SYS_GPU_FB_INFO */
typedef struct {
    uint64_t backing_phys;
    uint64_t backing_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t cursor_phys;
    uint32_t cursor_w;
    uint32_t cursor_h;
    uint32_t virgl;       /* 1 if virgl 3D is available */
} gpu_fb_info_t;

/* Mouse state returned by SYS_MOUSE_POS.
 * Pack into single uint64_t: low 32 bits = x, high 32 bits = y. */
typedef struct {
    int32_t x;
    int32_t y;
    uint8_t buttons;
} mouse_state_t;

#include "kernel/hal/input/input.h"

/* Userspace syscall wrapper.
 * Implemented in userspace/runtime/syscall.c */
uintptr_t syscall0(uint64_t num);
uintptr_t syscall1(uint64_t num, uintptr_t a1);
uintptr_t syscall2(uint64_t num, uintptr_t a1, uintptr_t a2);
uintptr_t syscall3(uint64_t num, uintptr_t a1, uintptr_t a2, uintptr_t a3);
uintptr_t syscall4(uint64_t num, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4);

/* Port / IPC wrappers */
port_handle_t sys_port_create(void);
int sys_port_send(port_handle_t h, const ipc_msg_t *msg);
int sys_port_recv(port_handle_t h, ipc_msg_t *out, int block);
void sys_port_close(port_handle_t h);

/* Nameserver wrappers */
void sys_ns_register(uint32_t id, port_handle_t port);
port_handle_t sys_ns_lookup(uint32_t id);

/* Memory wrappers */
int sys_mem_alloc(uint64_t vaddr, uint64_t flags);
int sys_mem_share(uint64_t vaddr, uint64_t target_pid, uint64_t target_vaddr,
                  uint64_t flags);
int sys_proc_exists(uint64_t pid);
int sys_proc_list(proc_info_t *buf, int max);
int sys_port_list(port_info_t *buf, int max);
int sys_msgbuf_read(char *buf, size_t size);
/* name: NUL-terminated OID (e.g. "kern.ostype"); out/out_len for string values. */
int sys_sysctl(const char *name, char *out, size_t out_len);

/* Input wrappers */
int sys_input_poll(input_event_t *out);

/* Time wrapper */
int sys_time(uint8_t *hour, uint8_t *min, uint8_t *sec);
uint64_t sys_systime_ns(void);  /* high-res monotonic time in ns (TSC) */

/* GPU wrappers */
int sys_gpu_fb_info(gpu_fb_info_t *info);
int sys_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int sys_gpu_cursor_set(int32_t x, int32_t y, uint32_t hot_x, uint32_t hot_y);
int sys_gpu_cursor_move(int32_t x, int32_t y);

/* GPU virgl wrappers */
int sys_gpu_virgl_present(void);
int sys_gpu_ctx_create(uint32_t ctx_id);
int sys_gpu_ctx_destroy(uint32_t ctx_id);
int sys_gpu_ctx_attach(uint32_t ctx_id, uint32_t resource_id);
int sys_gpu_res_create_2d(uint32_t resource_id, uint32_t format,
                          uint32_t width, uint32_t height);
int sys_gpu_res_attach(uint32_t resource_id, uint64_t phys, uint64_t size);
int sys_gpu_res_unref(uint32_t resource_id);
int sys_gpu_transfer_2d(uint32_t resource_id, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, uint64_t offset);
int sys_gpu_submit_3d(uint32_t ctx_id, void *cmds, uint32_t size);
int sys_gpu_set_scanout(uint32_t scanout_id, uint32_t resource_id,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int sys_gpu_flush_res(uint32_t resource_id, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h);
uint32_t sys_gpu_alloc_res_id(void);
int sys_gpu_res_attach_virt(uint32_t resource_id, uint64_t vaddr, uint32_t npages,
                            uint64_t buf_size);
int sys_gpu_res_create_3d(uint32_t resource_id, uint32_t target, uint32_t format,
                          uint32_t bind, uint32_t width, uint32_t height,
                          uint32_t depth, uint32_t array_size,
                          uint32_t last_level, uint32_t nr_samples,
                          uint32_t flags);
int sys_gpu_transfer_3d(uint32_t resource_id, uint32_t x, uint32_t y,
                        uint32_t z, uint32_t w, uint32_t h, uint32_t d,
                        uint64_t offset, uint32_t level, uint32_t stride,
                        uint32_t layer_stride);

/* Filesystem wrappers */
int sys_open(const char *path, uint32_t flags);
int sys_read(int fd, void *buf, size_t count);
int sys_write(int fd, const void *buf, size_t count);
void sys_close(int fd);
int sys_mkdir(const char *path);
int sys_readdir(int fd, void *entries, int max);

/* Process control wrappers */
void sys_exit(int code);
void sys_yield(void);

/* POSIX process wrappers */
int sys_fork(void);
int sys_exec(const char *path, char *const argv[]);
int sys_waitpid(int pid, int *status, int options);
int sys_getpid(void);
int sys_getppid(void);
int sys_pipe(int pipefd[2]);
int sys_dup(int oldfd);
int sys_dup2(int oldfd, int newfd);

/* POSIX file extension wrappers */
int sys_lseek(int fd, int offset, int whence);
int sys_stat(const char *path, void *statbuf);
int sys_fstat(int fd, void *statbuf);
int sys_unlink(const char *path);
int sys_getcwd(char *buf, size_t size);
int sys_chdir(const char *path);
uint64_t sys_brk(void *addr);

/* POSIX socket wrappers */
int sys_socket(int domain, int type, int protocol);
int sys_bind(int fd, const void *addr, int addrlen);
int sys_listen(int fd, int backlog);
int sys_accept(int fd, void *addr, int *addrlen);
int sys_connect(int fd, const void *addr, int addrlen);
int sys_send(int fd, const void *buf, size_t len, int flags);
int sys_recv(int fd, void *buf, size_t len, int flags);
int sys_sendto(int fd, const void *buf, size_t len, int flags,
               const void *addr, int addrlen);
int sys_recvfrom(int fd, void *buf, size_t len, int flags,
                 void *addr, int *addrlen);
int sys_shutdown(int fd, int how);
int sys_getsockname(int fd, void *addr, int *addrlen);
int sys_getpeername(int fd, void *addr, int *addrlen);
int sys_setsockopt(int fd, int level, int optname, const void *optval, int optlen);
int sys_getsockopt(int fd, int level, int optname, void *optval, int *optlen);
int sys_select(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout);
int sys_poll(void *fds, int nfds, int timeout);
void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, int offset);
int sys_munmap(void *addr, size_t length);
int sys_mprotect(void *addr, size_t length, int prot);
int sys_sigaction(int signum, const void *act, void *oldact);
int sys_sigprocmask(int how, const void *set, void *oldset);
int sys_sigsuspend(const void *mask);
int sys_kill(int pid, int sig);
int sys_sigreturn(void);
int sys_fcntl(int fd, int cmd, int arg);
int sys_ioctl(int fd, int cmd, void *arg);
int sys_net_send(const void *buf, int len);
int sys_net_recv(void *buf, int maxlen);

/* Kernel entry point for syscalls.
 * Implemented in kernel/arch/x86_64/syscall.c */
void syscall_handler(void);
void syscall_init(void);
void syscall_init_ap(void); /* Per-CPU syscall MSR init for APs */
