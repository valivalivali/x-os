/* x-os POSIX Syscall Implementations
 *
 * These are the kernel-side implementations of the new POSIX syscalls
 * (sockets, mmap, signals, select/poll, fcntl, ioctl).
 * They bridge x-os's syscall interface to the XNU BSD networking stack.
 */

#include "kernel/include/syscall.h"
#include "kernel/lib/kprintf.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/vmm.h"
#include "kernel/memory/pmm.h"
#include "kernel/sched/sched.h"
#include "kernel/lib/string.h"

/* Kernel-side termios definitions (adapted from XNU bsd/sys/termios.h) */
#define NCCS 20
struct ktermios {
    unsigned long   c_iflag;
    unsigned long   c_oflag;
    unsigned long   c_cflag;
    unsigned long   c_lflag;
    unsigned char   c_cc[NCCS];
    unsigned long   c_ispeed;
    unsigned long   c_ospeed;
};
struct kwinsize {
    unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};

/* ioctl request codes */
#define KTIOCGETA   0x40487413
#define KTIOCSETA   0x80487414
#define KTIOCGWINSZ 0x40087468
#define KTIOCSWINSZ 0x80087467
#define KTIOCGPGRP  0x40047477
#define KTIOCSPGRP  0x80047476

/* Socket layer functions — implemented in bsd/kern/uipc_socket_xos.c */
extern int socreate(int domain, int type, int protocol);
extern int sobind(int fd, void *nam, int namelen);
extern int solisten(int fd, int backlog);
extern int soaccept(int fd, void *nam, int *namelen);
extern int soconnect(int fd, void *nam, int namelen);
extern int sosend(int fd, const void *buf, size_t len, int flags,
                  void *addr, int addrlen);
extern int soreceive(int fd, void *buf, size_t len, int flags,
                     void *addr, int *addrlen);
extern int soshutdown(int fd, int how);
extern int soclose(int fd);
extern int sosetsockopt(int fd, int level, int name, const void *val, int valsize);
extern int sogetsockopt(int fd, int level, int name, void *val, int *valsize);

/* ------------------------------------------------------------------ */
/* Socket syscalls — forward to BSD networking layer                   */
/* ------------------------------------------------------------------ */

/* The BSD socket layer functions are declared in the XNU headers.
 * We provide thin wrappers that will be connected to the actual
 * BSD implementation once the networking stack is compiled. */

/* For now, these are stubs that return -1 with EAFNOSUPPORT.
 * As we compile and link more of the XNU BSD stack, these will
 * be replaced with real implementations. */

uint64_t sys_socket_impl(uint64_t domain, uint64_t type, uint64_t protocol,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    int fd = socreate((int)domain, (int)type, (int)protocol);
    return (uint64_t)fd;
}

uint64_t sys_bind_impl(uint64_t fd, uint64_t addr, uint64_t addrlen,
                       uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    return (uint64_t)sobind((int)fd, (void*)addr, (int)addrlen);
}

uint64_t sys_listen_impl(uint64_t fd, uint64_t backlog,
                         uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    return (uint64_t)solisten((int)fd, (int)backlog);
}

uint64_t sys_accept_impl(uint64_t fd, uint64_t addr, uint64_t addrlen,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    return (uint64_t)soaccept((int)fd, (void*)addr, (int*)addrlen);
}

uint64_t sys_connect_impl(uint64_t fd, uint64_t addr, uint64_t addrlen,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    return (uint64_t)soconnect((int)fd, (void*)addr, (int)addrlen);
}

uint64_t sys_send_impl(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                       uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    return (uint64_t)sosend((int)fd, (void*)buf, len, (int)flags, NULL, 0);
}

uint64_t sys_recv_impl(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                       uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    return (uint64_t)soreceive((int)fd, (void*)buf, len, (int)flags, NULL, NULL);
}

uint64_t sys_sendto_impl(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                         uint64_t addr, uint64_t addrlen) {
    return (uint64_t)sosend((int)fd, (void*)buf, len, (int)flags, (void*)addr, (int)addrlen);
}

uint64_t sys_recvfrom_impl(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                           uint64_t addr, uint64_t addrlen) {
    return (uint64_t)soreceive((int)fd, (void*)buf, len, (int)flags, (void*)addr, (int*)addrlen);
}

uint64_t sys_shutdown_impl(uint64_t fd, uint64_t how,
                           uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    return (uint64_t)soshutdown((int)fd, (int)how);
}

uint64_t sys_getsockname_impl(uint64_t fd, uint64_t addr, uint64_t addrlen,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)fd; (void)addr; (void)addrlen; (void)a4; (void)a5; (void)a6;
    return 0;  /* TODO: implement */
}

uint64_t sys_getpeername_impl(uint64_t fd, uint64_t addr, uint64_t addrlen,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)fd; (void)addr; (void)addrlen; (void)a4; (void)a5; (void)a6;
    return 0;  /* TODO: implement */
}

uint64_t sys_setsockopt_impl(uint64_t fd, uint64_t level, uint64_t optname,
                             uint64_t optval, uint64_t optlen, uint64_t a6) {
    (void)a6;
    return (uint64_t)sosetsockopt((int)fd, (int)level, (int)optname, (void*)optval, (int)optlen);
}

uint64_t sys_getsockopt_impl(uint64_t fd, uint64_t level, uint64_t optname,
                             uint64_t optval, uint64_t optlen, uint64_t a6) {
    (void)a6;
    return (uint64_t)sogetsockopt((int)fd, (int)level, (int)optname, (void*)optval, (int*)optlen);
}

uint64_t sys_select_impl(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                         uint64_t exceptfds, uint64_t timeout, uint64_t a6) {
    (void)nfds; (void)readfds; (void)writefds; (void)exceptfds; (void)timeout; (void)a6;
    return 0;
}

uint64_t sys_poll_impl(uint64_t fds, uint64_t nfds, uint64_t timeout,
                       uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)fds; (void)nfds; (void)timeout; (void)a4; (void)a5; (void)a6;
    return 0;
}

uint64_t sys_mmap_impl(uint64_t addr, uint64_t length, uint64_t prot,
                       uint64_t flags, uint64_t fd, uint64_t offset) {
    (void)fd; (void)offset; (void)flags;
    uint64_t vmm_flags = 0;
    if (prot & 0x01) vmm_flags |= VMM_U;
    if (prot & 0x02) vmm_flags |= VMM_RW | VMM_U;
    if (prot & 0x04) vmm_flags |= VMM_U;

    uint64_t vaddr = addr;
    if (vaddr == 0) {
        static uint64_t anon_base = 0x0000600000000000ULL;
        vaddr = anon_base;
        anon_base += (length + 0xFFF) & ~0xFFFULL;
    }

    uint64_t npages = (length + 0xFFF) / 0x1000;
    proc_t *p = proc_current();
    if (!p || !p->pml4_virt) return (uint64_t)-1;
    for (uint64_t i = 0; i < npages; i++) {
        uint64_t va = vaddr + i * 0x1000;
        uint64_t phys = pmm_alloc_frame();
        if (!phys) {
            kprintf("[syscall] mmap: out of memory\n");
            return (uint64_t)-1;
        }
        if (!vmm_map_page(p->pml4_virt, va, phys, vmm_flags | VMM_P)) {
            kprintf("[syscall] mmap: failed to map page at %p\n", (void*)va);
            return (uint64_t)-1;
        }
    }
    return vaddr;
}

uint64_t sys_munmap_impl(uint64_t addr, uint64_t length,
                         uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)addr; (void)length; (void)a3; (void)a4; (void)a5; (void)a6;
    return 0;
}

uint64_t sys_mprotect_impl(uint64_t addr, uint64_t length, uint64_t prot,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)addr; (void)length; (void)prot; (void)a4; (void)a5; (void)a6;
    return 0;
}

uint64_t sys_sigaction_impl(uint64_t signum, uint64_t act, uint64_t oldact,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)signum; (void)act; (void)oldact; (void)a4; (void)a5; (void)a6;
    return 0;
}

uint64_t sys_sigprocmask_impl(uint64_t how, uint64_t set, uint64_t oldset,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)how; (void)set; (void)oldset; (void)a4; (void)a5; (void)a6;
    return 0;
}

uint64_t sys_kill_impl(uint64_t pid, uint64_t sig,
                       uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)sig; (void)a3; (void)a4; (void)a5; (void)a6;
    if (sig == 9 || sig == 15) {
        extern void proc_kill(uint64_t pid);
        proc_kill(pid);
    }
    return 0;
}

uint64_t sys_fcntl_impl(uint64_t fd, uint64_t cmd, uint64_t arg,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)fd; (void)cmd; (void)arg; (void)a4; (void)a5; (void)a6;
    return 0;
}

uint64_t sys_ioctl_impl(uint64_t fd, uint64_t cmd, uint64_t arg,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;

    /* Only handle terminal ioctls for stdin/stdout/stderr (fds 0-2) */
    if (fd > 2) return (uint64_t)-1;

    /* Default terminal settings — canonical mode with echo, like a real tty */
    static struct ktermios default_termios = {
        .c_iflag = 0x00000002 | 0x00000100 | 0x00002000 | 0x00000200 | 0x00000800,
        .c_oflag = 0x00000001 | 0x00000002,
        .c_cflag = 0x00000800 | 0x00000300,
        .c_lflag = 0x00000008 | 0x00000100 | 0x00000080 | 0x00000400 | 0x00000002 | 0x00000001 | 0x00000040,
        .c_cc = { 0x04, 0xff, 0xff, 0x7f, 0x17, 0x15, 0x12, 0xff,
                  0x03, 0x1c, 0x1a, 0x19, 0x11, 0x13, 0x16, 0x0f,
                  0x01, 0x00, 0x14, 0xff },
        .c_ispeed = 9600,
        .c_ospeed = 9600,
    };

    static struct ktermios proc_termios;
    static int termios_initialized = 0;
    if (!termios_initialized) {
        proc_termios = default_termios;
        termios_initialized = 1;
    }

    switch (cmd) {
    case KTIOCGETA:
        if (arg) {
            memcpy((void *)arg, &proc_termios, sizeof(struct ktermios));
            return 0;
        }
        return (uint64_t)-1;

    case KTIOCSETA:
        if (arg) {
            memcpy(&proc_termios, (void *)arg, sizeof(struct ktermios));
            return 0;
        }
        return (uint64_t)-1;

    case KTIOCGWINSZ:
        if (arg) {
            struct kwinsize *ws = (struct kwinsize *)arg;
            ws->ws_row = 25;
            ws->ws_col = 80;
            ws->ws_xpixel = 0;
            ws->ws_ypixel = 0;
            return 0;
        }
        return (uint64_t)-1;

    case KTIOCSWINSZ:
        return 0;

    case KTIOCGPGRP:
        if (arg) {
            *(int *)arg = 0;
            return 0;
        }
        return (uint64_t)-1;

    case KTIOCSPGRP:
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Raw network packet syscalls                                         */
/* ------------------------------------------------------------------ */

extern int virtio_net_send(const void *data, int len);
extern int virtio_net_recv(void *buf, int maxlen);

uint64_t sys_net_send_impl(uint64_t buf, uint64_t len,
                           uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    return (uint64_t)virtio_net_send((void*)buf, (int)len);
}

uint64_t sys_net_recv_impl(uint64_t buf, uint64_t maxlen,
                           uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    return (uint64_t)virtio_net_recv((void*)buf, (int)maxlen);
}
