/* x-os POSIX Syscall Implementations
 *
 * These are the kernel-side implementations of the new POSIX syscalls
 * (sockets, mmap, signals, select/poll, fcntl, ioctl).
 * They bridge x-os's syscall interface to the kernel subsystems.
 */

#include "kernel/include/syscall.h"
#include "kernel/lib/kprintf.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/vmm.h"
#include "kernel/memory/pmm.h"
#include "kernel/sched/sched.h"
#include "kernel/lib/string.h"
#include "kernel/proc/signal.h"
#include "kernel/arch/x86_64/cpu.h"

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

/* ------------------------------------------------------------------ */
/* Socket syscalls — wired to FreeBSD network stack                    */
/* ------------------------------------------------------------------ */

/* Minimal FreeBSD-compatible type definitions to avoid header conflicts.
 * These match the layout of the structs defined in bsd/compat/sys/proc.h
 * and FreeBSD's sys/socket.h for the fields we actually access. */
struct bsd_thread {
    void *td_proc;
    void *td_ucred;
    int td_critnest;
    int td_flags;
    int td_retval[2];
};
struct bsd_sockaddr {
    unsigned char sa_len;
    unsigned char sa_family;
    char sa_data[14];
};
struct bsd_iovec {
    void *iov_base;
    unsigned long iov_len;
};
struct bsd_msghdr {
    void *msg_name;
    int msg_namelen;
    struct bsd_iovec *msg_iov;
    unsigned long msg_iovlen;
    void *msg_control;
    unsigned long msg_controllen;
    int msg_flags;
};

/* FreeBSD kern_* function declarations (defined in uipc_syscalls.c) */
int kern_socket(struct bsd_thread *td, int domain, int type, int protocol);
int kern_bindat(struct bsd_thread *td, int dirfd, int fd, struct bsd_sockaddr *sa);
int kern_listen(struct bsd_thread *td, int s, int backlog);
int kern_accept4(struct bsd_thread *td, int s, struct bsd_sockaddr *sa, int flags, void **fp);
int kern_connectat(struct bsd_thread *td, int dirfd, int fd, struct bsd_sockaddr *sa);
int kern_shutdown(struct bsd_thread *td, int s, int how);
int kern_setsockopt(struct bsd_thread *td, int s, int level, int name, const void *val, int valseg, int valsize);
int kern_getsockopt(struct bsd_thread *td, int s, int level, int name, void *val, int valseg, int *valsize);
int kern_getsockname(struct bsd_thread *td, int fd, struct bsd_sockaddr *sa);
int kern_getpeername(struct bsd_thread *td, int fd, struct bsd_sockaddr *sa);
int kern_sendit(struct bsd_thread *td, int s, struct bsd_msghdr *mp, int flags, void *control, int segflg);
int kern_recvfrom(struct bsd_thread *td, int s, void *buf, unsigned long len, int flags, struct bsd_sockaddr *from, int *fromlenaddr);
int getsockaddr(struct bsd_sockaddr **namp, const struct bsd_sockaddr *uaddr, unsigned long len);

/* copyin/copyout implemented in compat_shims.c */
int copyin(const void *uaddr, void *kaddr, unsigned long len);
int copyout(const void *kaddr, void *uaddr, unsigned long len);

/* free/M_SONAME from compat shims */
#define M_SONAME 0
extern void free(void *ptr, int type);

/* thread0 is defined in compat_shims.c using the full FreeBSD struct thread.
 * Our bsd_thread has matching layout for td_retval at the same offset. */
extern struct bsd_thread thread0;

#define AT_FDCWD (-100)
#define ACCEPT4_INHERIT 0
#define UIO_USERSPACE 0

static struct bsd_thread *
get_bsd_td(void)
{
    thread0.td_retval[0] = 0;
    thread0.td_retval[1] = 0;
    return &thread0;
}

uint64_t sys_socket_impl(uint64_t domain, uint64_t type, uint64_t protocol,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct bsd_thread *td = get_bsd_td();
    int error = kern_socket(td, (int)domain, (int)type, (int)protocol);
    kprintf("[sock] socket(%d, %d, %d) = error=%d fd=%d\n",
            (int)domain, (int)type, (int)protocol, error, td->td_retval[0]);
    if (error) return (uint64_t)-1;
    return (uint64_t)td->td_retval[0];
}

uint64_t sys_bind_impl(uint64_t fd, uint64_t addr, uint64_t addrlen,
                       uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct bsd_thread *td = get_bsd_td();
    struct bsd_sockaddr *sa = NULL;
    int error = getsockaddr(&sa, (const struct bsd_sockaddr *)addr, (unsigned long)addrlen);
    if (error) return (uint64_t)-1;
    error = kern_bindat(td, AT_FDCWD, (int)fd, sa);
    free(sa, M_SONAME);
    return error ? (uint64_t)-1 : 0;
}

uint64_t sys_listen_impl(uint64_t fd, uint64_t backlog,
                         uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct bsd_thread *td = get_bsd_td();
    int error = kern_listen(td, (int)fd, (int)backlog);
    return error ? (uint64_t)-1 : 0;
}

uint64_t sys_accept_impl(uint64_t fd, uint64_t addr, uint64_t addrlen,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct bsd_thread *td = get_bsd_td();
    struct bsd_sockaddr sa;
    __builtin_memset(&sa, 0, sizeof(sa));
    int error = kern_accept4(td, (int)fd, &sa, ACCEPT4_INHERIT, NULL);
    if (error) return (uint64_t)-1;
    if (addr && addrlen) {
        int ulen;
        copyin((void *)addrlen, &ulen, sizeof(ulen));
        int sa_len = sa.sa_len;
        if (sa_len > ulen) sa_len = ulen;
        if (sa_len > 0) copyout(&sa, (void *)addr, sa_len);
        copyout(&sa_len, (void *)addrlen, sizeof(int));
    }
    return (uint64_t)td->td_retval[0];
}

uint64_t sys_connect_impl(uint64_t fd, uint64_t addr, uint64_t addrlen,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct bsd_thread *td = get_bsd_td();
    struct bsd_sockaddr *sa = NULL;
    int error = getsockaddr(&sa, (const struct bsd_sockaddr *)addr, (unsigned long)addrlen);
    if (error) return (uint64_t)-1;
    error = kern_connectat(td, AT_FDCWD, (int)fd, sa);
    kprintf("[sock] connect(fd=%d) = error=%d\n", (int)fd, error);
    free(sa, M_SONAME);
    return error ? (uint64_t)-1 : 0;
}

uint64_t sys_send_impl(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                       uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    struct bsd_thread *td = get_bsd_td();
    struct bsd_msghdr msg;
    struct bsd_iovec aiov;
    __builtin_memset(&msg, 0, sizeof(msg));
    aiov.iov_base = (void *)buf;
    aiov.iov_len = len;
    msg.msg_iov = &aiov;
    msg.msg_iovlen = 1;
    int error = kern_sendit(td, (int)fd, &msg, (int)flags, NULL, UIO_USERSPACE);
    if (error) return (uint64_t)-1;
    return (uint64_t)td->td_retval[0];
}

uint64_t sys_recv_impl(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                       uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    struct bsd_thread *td = get_bsd_td();
    int error = kern_recvfrom(td, (int)fd, (void *)buf, (unsigned long)len, (int)flags, NULL, NULL);
    if (error) return (uint64_t)-1;
    return (uint64_t)td->td_retval[0];
}

uint64_t sys_sendto_impl(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                         uint64_t addr, uint64_t addrlen) {
    struct bsd_thread *td = get_bsd_td();
    struct bsd_msghdr msg;
    struct bsd_iovec aiov;
    struct bsd_sockaddr *sa = NULL;
    __builtin_memset(&msg, 0, sizeof(msg));
    aiov.iov_base = (void *)buf;
    aiov.iov_len = len;
    msg.msg_iov = &aiov;
    msg.msg_iovlen = 1;
    if (addr) {
        int error = getsockaddr(&sa, (const struct bsd_sockaddr *)addr, (unsigned long)addrlen);
        if (error) return (uint64_t)-1;
        msg.msg_name = sa;
        msg.msg_namelen = (int)addrlen;
    }
    int error = kern_sendit(td, (int)fd, &msg, (int)flags, NULL, UIO_USERSPACE);
    if (sa) free(sa, M_SONAME);
    if (error) return (uint64_t)-1;
    return (uint64_t)td->td_retval[0];
}

uint64_t sys_recvfrom_impl(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                           uint64_t addr, uint64_t addrlen) {
    struct bsd_thread *td = get_bsd_td();
    /* kern_recvfrom expects user-space pointers for from and fromlenaddr
     * (it uses copyin/copyout on them). Pass them directly. */
    int error = kern_recvfrom(td, (int)fd, (void *)buf, (unsigned long)len, (int)flags,
                              addr ? (struct bsd_sockaddr *)addr : NULL,
                              addrlen ? (int *)addrlen : NULL);
    if (error) return (uint64_t)-1;
    return (uint64_t)td->td_retval[0];
}

uint64_t sys_shutdown_impl(uint64_t fd, uint64_t how,
                           uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct bsd_thread *td = get_bsd_td();
    int error = kern_shutdown(td, (int)fd, (int)how);
    return error ? (uint64_t)-1 : 0;
}

uint64_t sys_getsockname_impl(uint64_t fd, uint64_t addr, uint64_t addrlen,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct bsd_thread *td = get_bsd_td();
    struct bsd_sockaddr sa;
    __builtin_memset(&sa, 0, sizeof(sa));
    int error = kern_getsockname(td, (int)fd, &sa);
    if (error) return (uint64_t)-1;
    if (addr && addrlen) {
        int ulen;
        copyin((void *)addrlen, &ulen, sizeof(ulen));
        int sa_len = sa.sa_len;
        if (sa_len > ulen) sa_len = ulen;
        if (sa_len > 0) copyout(&sa, (void *)addr, sa_len);
        copyout(&sa_len, (void *)addrlen, sizeof(int));
    }
    return 0;
}

uint64_t sys_getpeername_impl(uint64_t fd, uint64_t addr, uint64_t addrlen,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct bsd_thread *td = get_bsd_td();
    struct bsd_sockaddr sa;
    __builtin_memset(&sa, 0, sizeof(sa));
    int error = kern_getpeername(td, (int)fd, &sa);
    if (error) return (uint64_t)-1;
    if (addr && addrlen) {
        int ulen;
        copyin((void *)addrlen, &ulen, sizeof(ulen));
        int sa_len = sa.sa_len;
        if (sa_len > ulen) sa_len = ulen;
        if (sa_len > 0) copyout(&sa, (void *)addr, sa_len);
        copyout(&sa_len, (void *)addrlen, sizeof(int));
    }
    return 0;
}

uint64_t sys_setsockopt_impl(uint64_t fd, uint64_t level, uint64_t optname,
                             uint64_t optval, uint64_t optlen, uint64_t a6) {
    (void)a6;
    struct bsd_thread *td = get_bsd_td();
    int error = kern_setsockopt(td, (int)fd, (int)level, (int)optname,
                                (const void *)optval, UIO_USERSPACE, (int)optlen);
    return error ? (uint64_t)-1 : 0;
}

uint64_t sys_getsockopt_impl(uint64_t fd, uint64_t level, uint64_t optname,
                             uint64_t optval, uint64_t optlen, uint64_t a6) {
    (void)a6;
    struct bsd_thread *td = get_bsd_td();
    int valsize;
    copyin((void *)optlen, &valsize, sizeof(valsize));
    int error = kern_getsockopt(td, (int)fd, (int)level, (int)optname,
                                (void *)optval, UIO_USERSPACE, &valsize);
    if (error) return (uint64_t)-1;
    copyout(&valsize, (void *)optlen, sizeof(int));
    return 0;
}

/* ---- select/poll implementation ------------------------------------------
 *
 * Real select/poll using the BSD socket layer's sopoll_generic and the
 * X OS scheduler's block_on/wake_chan for sleeping.
 *
 * Only socket FDs (managed by the BSD fd_table) are supported.  Pipe and
 * XFS FDs are checked separately (pipes: data_bytes > 0 means readable). */

/* Forward declarations for BSD socket layer (avoid pulling in BSD headers
 * which conflict with kernel headers). */
struct file;
struct thread;
typedef unsigned long cap_rights_t;
extern const cap_rights_t cap_no_rights;
extern int fget_unlocked(struct thread *td, int fd, const cap_rights_t *rightsp,
                         struct file **fpp);
extern void xos_select_set_chan(void *chan);
extern void xos_select_clear_chan(void);
extern int xos_fop_poll(struct file *fp, int events);
extern void xos_fdrop(struct file *fp);

/* Poll a single FD. Returns revents (0 if not ready). */
static int xos_poll_fd(int fd, int events) {
    /* Pipe FDs (>= 64): check if readable/writable */
    if (fd >= 64) {
        extern int pipe_readable(int fd);
        extern int pipe_writable(int fd);
        int revents = 0;
        if (events & 0x001 && pipe_readable(fd)) revents |= 0x001; /* POLLIN */
        if (events & 0x004 && pipe_writable(fd)) revents |= 0x004; /* POLLOUT */
        return revents;
    }

    /* Socket FDs: use BSD fo_poll */
    struct file *fp = NULL;
    if (fget_unlocked(NULL, fd, &cap_no_rights, &fp) != 0 || !fp)
        return 0;  /* not a socket FD, or bad FD */

    int revents = xos_fop_poll(fp, events);
    xos_fdrop(fp);
    return revents;
}

/* pollfd structure (POSIX) */
struct xos_pollfd {
    int fd;
    short events;
    short revents;
};

uint64_t sys_poll_impl(uint64_t fds, uint64_t nfds, uint64_t timeout,
                       uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (!fds || nfds == 0 || nfds > 256) return 0;

    /* Copy pollfd array from userspace */
    struct xos_pollfd pfds[256];
    cpu_user_access_begin();
    memcpy(pfds, (void *)fds, nfds * sizeof(struct xos_pollfd));
    cpu_user_access_end();

    /* Convert timeout (ms) to ticks for sched_block_on */
    uint64_t deadline_ticks = 0;
    if (timeout >= 0) {
        extern uint64_t timer_ticks(void);
        extern uint64_t timer_ticks_hz(void);
        deadline_ticks = timer_ticks() + (timeout * timer_ticks_hz()) / 1000;
    }

    /* Wait channel for this poll call (stack address, unique per call). */
    int wait_chan;

    for (;;) {
        int ready_count = 0;

        /* Set the select channel so selrecord can find it. */
        xos_select_set_chan(&wait_chan);

        /* Poll all FDs */
        for (uint64_t i = 0; i < nfds; i++) {
            pfds[i].revents = 0;
            if (pfds[i].fd < 0) continue;
            int revents = xos_poll_fd(pfds[i].fd, pfds[i].events);
            pfds[i].revents = (short)revents;
            if (revents) ready_count++;
        }

        xos_select_clear_chan();

        /* If any FD is ready, copy results back and return. */
        if (ready_count > 0) {
            cpu_user_access_begin();
            memcpy((void *)fds, pfds, nfds * sizeof(struct xos_pollfd));
            cpu_user_access_end();
            return (uint64_t)ready_count;
        }

        /* Check timeout */
        if (timeout == 0) return 0;  /* immediate, nothing ready */
        if (timeout > 0) {
            extern uint64_t timer_ticks(void);
            if (timer_ticks() >= deadline_ticks) return 0;
        }

        /* Block briefly (10ms backstop) and retry.  The selwakeup
         * infrastructure will wake us if a socket becomes ready. */
        sched_block_on(&wait_chan, NULL, 10);
    }
}

/* fd_set helpers (bitmap of FDs) */
#define XOS_NFDBITS (8 * sizeof(uint64_t))
#define XOS_FD_SETSIZE 256

static inline void fd_set_bit(uint64_t *set, int fd) {
    set[fd / XOS_NFDBITS] |= (1ULL << (fd % XOS_NFDBITS));
}

static inline int fd_test_bit(const uint64_t *set, int fd) {
    return (set[fd / XOS_NFDBITS] >> (fd % XOS_NFDBITS)) & 1;
}

uint64_t sys_select_impl(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                         uint64_t exceptfds, uint64_t timeout, uint64_t a6) {
    (void)exceptfds; (void)a6;
    if (nfds == 0 || nfds > XOS_FD_SETSIZE) return 0;

    int set_words = (XOS_FD_SETSIZE + XOS_NFDBITS - 1) / XOS_NFDBITS;
    uint64_t rset[4] = {0}, wset[4] = {0};

    /* Copy fd_sets from userspace */
    cpu_user_access_begin();
    if (readfds) memcpy(rset, (void *)readfds, set_words * sizeof(uint64_t));
    if (writefds) memcpy(wset, (void *)writefds, set_words * sizeof(uint64_t));
    cpu_user_access_end();

    /* Parse timeout: struct timeval { tv_sec, tv_usec } */
    int timeout_ms = -1;
    if (timeout) {
        cpu_user_access_begin();
        uint64_t tv_sec = *(uint64_t *)timeout;
        uint64_t tv_usec = *(uint64_t *)(timeout + 8);
        cpu_user_access_end();
        timeout_ms = (int)(tv_sec * 1000 + tv_usec / 1000);
    }

    int wait_chan;

    for (;;) {
        int ready_count = 0;
        uint64_t out_rset[4] = {0}, out_wset[4] = {0};

        xos_select_set_chan(&wait_chan);

        for (int fd = 0; fd < (int)nfds; fd++) {
            int events = 0;
            if (readfds && fd_test_bit(rset, fd)) events |= 0x001;  /* POLLIN */
            if (writefds && fd_test_bit(wset, fd)) events |= 0x004; /* POLLOUT */
            if (!events) continue;

            int revents = xos_poll_fd(fd, events);
            if (revents & 0x001) { fd_set_bit(out_rset, fd); ready_count++; }
            if (revents & 0x004) { fd_set_bit(out_wset, fd); ready_count++; }
        }

        xos_select_clear_chan();

        if (ready_count > 0) {
            cpu_user_access_begin();
            if (readfds) memcpy((void *)readfds, out_rset, set_words * sizeof(uint64_t));
            if (writefds) memcpy((void *)writefds, out_wset, set_words * sizeof(uint64_t));
            cpu_user_access_end();
            return (uint64_t)ready_count;
        }

        if (timeout_ms == 0) return 0;
        if (timeout_ms > 0) {
            extern uint64_t timer_ticks(void);
            extern uint64_t timer_ticks_hz(void);
            uint64_t deadline = timer_ticks() + (timeout_ms * timer_ticks_hz()) / 1000;
            if (timer_ticks() >= deadline) return 0;
        }

        sched_block_on(&wait_chan, NULL, 10);
    }
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

/* Apple/BSD layout used by prebuilt zsh objects (NOT newlib's flags-first):
 *   sa_handler @0, sa_mask @8, sa_flags @16. */
struct xos_sigaction {
    uint64_t sa_handler;
    uint64_t sa_mask;
    int      sa_flags;
};

#define XOS_SIG_BLOCK   1
#define XOS_SIG_UNBLOCK 2
#define XOS_SIG_SETMASK 0

uint64_t sys_sigaction_impl(uint64_t signum, uint64_t act, uint64_t oldact,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    proc_t *p = proc_current();
    if (!p) return (uint64_t)-1;
    if (signum == 0 || signum >= (uint64_t)XOS_NSIG)
        return (uint64_t)-1;
    /* SIGKILL / SIGSTOP cannot be caught or ignored. */
    if (signum == 9 || signum == 17)
        return (uint64_t)-1;

    if (oldact) {
        struct xos_sigaction *oa = (struct xos_sigaction *)oldact;
        oa->sa_handler = p->sig_handler[signum];
        oa->sa_mask = p->sig_mask[signum];
        oa->sa_flags = p->sig_flags[signum];
    }
    if (act) {
        const struct xos_sigaction *na = (const struct xos_sigaction *)act;
        p->sig_handler[signum] = na->sa_handler;
        p->sig_mask[signum] = na->sa_mask;
        p->sig_flags[signum] = na->sa_flags;
        if (signum == 20 /* SIGCHLD */)
            kprintf("[sig] pid=%lu SIGCHLD handler=%p flags=%d\n",
                    p->pid, (void *)na->sa_handler, na->sa_flags);
    }
    return 0;
}

uint64_t sys_sigprocmask_impl(uint64_t how, uint64_t set, uint64_t oldset,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    proc_t *p = proc_current();
    if (!p) return (uint64_t)-1;

    if (oldset)
        *(uint64_t *)oldset = p->sig_blocked;

    if (set) {
        uint64_t s = *(const uint64_t *)set;
        switch ((int)how) {
        case XOS_SIG_BLOCK:
            p->sig_blocked |= s;
            break;
        case XOS_SIG_UNBLOCK:
            p->sig_blocked &= ~s;
            break;
        case XOS_SIG_SETMASK:
            p->sig_blocked = s;
            break;
        default:
            return (uint64_t)-1;
        }
    }
    return 0;
}

uint64_t sys_kill_impl(uint64_t pid, uint64_t sig,
                       uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    extern int proc_send_signal(uint64_t pid, int sig);
    if ((int64_t)pid < 0)
        pid = (uint64_t)(-(int64_t)pid); /* killpg-style: treat as pid */

    proc_t *t = proc_by_pid(pid);
    if (!t)
        return (uint64_t)-(int64_t)3; /* -ESRCH */

    /* kill(pid, 0) — existence check (zsh waitforpid). */
    if ((int)sig == 0)
        return 0;

    int ret = proc_send_signal(pid, (int)sig);
    return ret < 0 ? (uint64_t)-(int64_t)1 : 0;
}

/* Atomic sigsuspend: optionally install `maskptr`, then sleep until a signal
 * is deliverable. Returns -EINTR so signal_on_syscall_return runs the handler.
 * Must install the mask in-kernel (not via a prior sigprocmask) so a pending
 * SIGCHLD is not consumed before we enter the wait. */
uint64_t sys_sigsuspend_impl(uint64_t maskptr, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    proc_t *p = proc_current();
    if (!p) return (uint64_t)-1;

    if (maskptr)
        p->sig_blocked = *(const uint64_t *)maskptr;

    if (p->sig_pending & ~p->sig_blocked) {
        kprintf("[sig] sigsuspend wake(immediate) pid=%lu pending=%lx blocked=%lx\n",
                p->pid, (unsigned long)p->sig_pending,
                (unsigned long)p->sig_blocked);
        return (uint64_t)-(int64_t)4; /* -EINTR */
    }
    kprintf("[sig] sigsuspend sleep pid=%lu pending=%lx blocked=%lx\n",
            p->pid, (unsigned long)p->sig_pending,
            (unsigned long)p->sig_blocked);
    for (;;) {
        if (p->sig_pending & ~p->sig_blocked) {
            kprintf("[sig] sigsuspend wake pid=%lu pending=%lx blocked=%lx\n",
                    p->pid, (unsigned long)p->sig_pending,
                    (unsigned long)p->sig_blocked);
            return (uint64_t)-(int64_t)4; /* -EINTR */
        }
        sched_yield();
    }
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
/* Raw network packet syscalls — forward to virtio-net driver          */
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
