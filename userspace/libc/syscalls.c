/* newlib syscall stubs — bridges newlib's _read, _write, etc. to x-os syscalls.
 *
 * newlib calls these low-level functions from its reentrant wrappers
 * (_read_r, _write_r, etc.). We implement each one using our syscall layer.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <stdarg.h>
#include <termios.h>

#include "kernel/include/syscall.h"

/* Use newlib's errno macro ((*__errno())) — do NOT #undef it. Stubs used to
 * write a bare global `errno` while zsh reads (*__errno()), so wait() left
 * errno at 0 and zsh printed "wait failed: success" after every command. */

/* -------------------------------------------------------------------------- */
/* Process management */

void _exit(int status) {
    sys_exit(status);
    while (1) {}
}

int _fork(void) {
    return sys_fork();
}

int _execve(const char *name, char *const argv[], char *const env[]) {
    (void)env;  /* environment not yet supported */
    int ret = sys_exec(name, argv);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

pid_t _getpid(void) {
    return sys_getpid();
}

pid_t _wait(int *status) {
    int ret = sys_waitpid(-1, status, 0);
    if (ret < 0) {
        /* Kernel returns -errno; default ECHILD when bare -1. */
        errno = (ret < -1) ? -ret : ECHILD;
        return -1;
    }
    return ret;
}

int _kill(int pid, int sig) {
    int ret = sys_kill(pid, sig);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* I/O — IPC bridge mode for terminal integration */

/* When set, stdin/stdout and zsh's SHTTY (=dup of 0) talk to the Terminal
 * app over IPC. ZLE writes echo to SHTTY, not necessarily fd 1. */
static port_handle_t g_bridge_input_port = 0;
static port_handle_t g_bridge_output_port = 0;

/* Virtual bridge-tty fds: bit N set ⇒ fd N is a duplex bridge tty.
 * 0/1/2 start set; dup/F_DUPFD allocate fds >= 10. */
#define BRIDGE_FD_MAX 64
static uint64_t g_bridge_tty_bits;
static int g_bridge_next_fd = 10;
static struct termios g_bridge_termios;
static int g_bridge_termios_init;

static void bridge_termios_ensure(void) {
    if (g_bridge_termios_init)
        return;
    memset(&g_bridge_termios, 0, sizeof(g_bridge_termios));
    /* Cooked + echo defaults; ZLE switches to raw via tcsetattr. */
    g_bridge_termios.c_iflag = ICRNL | IXON;
    g_bridge_termios.c_oflag = OPOST | ONLCR;
    g_bridge_termios.c_cflag = CS8 | CREAD;
    g_bridge_termios.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    g_bridge_termios.c_cc[VMIN] = 1;
    g_bridge_termios.c_cc[VTIME] = 0;
    g_bridge_termios.c_ispeed = 9600;
    g_bridge_termios.c_ospeed = 9600;
    g_bridge_termios_init = 1;
}

static int bridge_is_tty(int fd) {
    if (fd < 0 || fd >= BRIDGE_FD_MAX || !g_bridge_output_port)
        return 0;
    return (g_bridge_tty_bits & (1ULL << fd)) != 0;
}

static void bridge_mark_tty(int fd) {
    if (fd >= 0 && fd < BRIDGE_FD_MAX)
        g_bridge_tty_bits |= (1ULL << fd);
}

static void bridge_clear_tty(int fd) {
    if (fd >= 0 && fd < BRIDGE_FD_MAX)
        g_bridge_tty_bits &= ~(1ULL << fd);
}

static int bridge_alloc_fd(int minfd) {
    int start = minfd < 10 ? 10 : minfd;
    if (start < g_bridge_next_fd)
        start = g_bridge_next_fd;
    for (int fd = start; fd < BRIDGE_FD_MAX; fd++) {
        if (!bridge_is_tty(fd)) {
            bridge_mark_tty(fd);
            if (fd >= g_bridge_next_fd)
                g_bridge_next_fd = fd + 1;
            return fd;
        }
    }
    errno = EMFILE;
    return -1;
}

static _ssize_t bridge_write(const void *buf, size_t cnt) {
    const char *p = (const char *)buf;
    size_t remaining = cnt;
    while (remaining > 0) {
        size_t chunk = remaining > IPC_MSG_MAX_PAYLOAD ? IPC_MSG_MAX_PAYLOAD : remaining;
        ipc_msg_t msg;
        for (size_t i = 0; i < sizeof(msg); i++)
            ((uint8_t *)&msg)[i] = 0;
        msg.type = IPC_MSG_EVENT;
        msg.sender_pid = syscall0(SYS_PROC_PID);
        msg.cap_count = 0;
        msg.payload_len = (uint32_t)chunk;
        for (size_t i = 0; i < chunk; i++)
            msg.payload[i] = ((const uint8_t *)p)[i];
        /* Bounded retries to prevent deadlock with the terminal's
         * send_shell_byte (which drains the bridge port between retries).
         * If we still can't send after 64 tries, drop this chunk and
         * continue to the next (don't abort all remaining output). */
        int sent = 0;
        for (int tries = 0; tries < 64; tries++) {
            if (sys_port_send(g_bridge_output_port, &msg)) {
                sent = 1;
                break;
            }
            syscall1(12, 1);  /* SYS_NSLEEP, 1ms — throttle retries */
        }
        if (!sent) {
            p += chunk;
            remaining -= chunk;
            continue;  /* drop this chunk, try the next */
        }
        p += chunk;
        remaining -= chunk;
    }
    return (_ssize_t)(cnt - remaining);
}

static _ssize_t bridge_read(void *buf, size_t cnt) {
    char *cbuf = (char *)buf;
    size_t got = 0;
    while (got < cnt) {
        ipc_msg_t msg;
        for (size_t i = 0; i < sizeof(msg); i++)
            ((uint8_t *)&msg)[i] = 0;
        /* Block until a keystroke arrives instead of busy-yielding. */
        if (sys_port_recv(g_bridge_input_port, &msg, 1)) {
            if (msg.payload_len >= 1) {
                char c = (char)msg.payload[0];
                if (c == '\r')
                    c = '\n';
                cbuf[got++] = c;
                break;
            }
        }
    }
    return (_ssize_t)got;
}

void set_shell_bridge(port_handle_t input_port, port_handle_t output_port) {
    g_bridge_input_port = input_port;
    g_bridge_output_port = output_port;
    g_bridge_tty_bits = 0;
    g_bridge_next_fd = 10;
    /* stdin/stdout/stderr are the primary bridge tty. */
    bridge_mark_tty(0);
    bridge_mark_tty(1);
    bridge_mark_tty(2);
    bridge_termios_ensure();
}

/* -------------------------------------------------------------------------- */
/* I/O */

/* Minimal input_event_t matching kernel's input.h */
typedef struct {
    uint32_t type;      /* event_type_t: EV_KEY_DOWN = 4 */
    int32_t  x, y;
    int32_t  dx, dy;
    uint8_t  button;
    uint8_t  buttons;
    uint8_t  scancode;
    char     ch;         /* translated ASCII, 0 if none */
    uint16_t key;        /* KEY_* for non-ASCII */
} xos_input_event_t;

#define XOS_EV_KEY_DOWN 4

_ssize_t _read(int fd, void *buf, size_t cnt) {
    if (bridge_is_tty(fd) && g_bridge_input_port)
        return bridge_read(buf, cnt);
    if (fd == 0) {
        /* stdin: read from keyboard via SYS_INPUT_POLL */
        char *cbuf = (char *)buf;
        size_t got = 0;
        while (got < (cnt > 256 ? 256 : cnt)) {
            xos_input_event_t ev;
            memset(&ev, 0, sizeof(ev));
            uintptr_t ret = syscall1(SYS_INPUT_POLL, (uintptr_t)&ev);
            if (ret == 1 && ev.type == XOS_EV_KEY_DOWN && ev.ch != 0) {
                char c = ev.ch;
                if (c == '\r') c = '\n';
                cbuf[got++] = c;
                break;
            }
            syscall1(12, 1);  /* SYS_NSLEEP, 1ms — throttle poll */
        }
        return (_ssize_t)got;
    }
    int ret = sys_read(fd, buf, cnt);
    if (ret < 0) {
        errno = EBADF;
        return -1;
    }
    return ret;
}

_ssize_t _write(int fd, const void *buf, size_t cnt) {
    /* ZLE echoes via SHTTY (dup of stdin), not only fd 1/2. */
    if (bridge_is_tty(fd) && g_bridge_output_port)
        return bridge_write(buf, cnt);
    if (fd == 1 || fd == 2) {
        /* stdout/stderr: write to serial via SYS_DEBUG_LOG (max 4096 per call) */
        const char *p = (const char *)buf;
        size_t remaining = cnt;
        while (remaining > 0) {
            size_t chunk = remaining > 4096 ? 4096 : remaining;
            syscall2(SYS_DEBUG_LOG, (uintptr_t)p, chunk);
            p += chunk;
            remaining -= chunk;
        }
        return (_ssize_t)cnt;
    }
    int ret = sys_write(fd, buf, cnt);
    if (ret < 0) {
        errno = EBADF;
        return -1;
    }
    return ret;
}

int _open(const char *file, int flags, int mode) {
    (void)mode;
    /* Map POSIX open flags to XFS flags */
    uint32_t xfs_flags = 0;
    int accmode = flags & O_ACCMODE;
    if (accmode == O_RDONLY) xfs_flags = 0;
    else if (accmode == O_WRONLY) xfs_flags = 1;
    else if (accmode == O_RDWR) xfs_flags = 2;
    if (flags & O_CREAT) xfs_flags |= 4;
    if (flags & O_TRUNC) xfs_flags |= 8;

    /* Kernel sys_open resolves relative paths against cwd (path_abs). */
    if (!file) {
        errno = EINVAL;
        return -1;
    }
    int ret = sys_open(file, xfs_flags);
    if (ret < 0) {
        errno = ENOENT;
        return -1;
    }
    return ret;
}

int _close(int fd) {
    /* Never drop the primary stdio bridge slots — zsh may zclose after movefd. */
    if (bridge_is_tty(fd) && fd >= 10)
        bridge_clear_tty(fd);
    if (fd >= 0 && fd <= 2 && g_bridge_output_port)
        return 0;
    sys_close(fd);
    return 0;
}

off_t _lseek(int fd, off_t offset, int whence) {
    int ret = sys_lseek(fd, (int)offset, whence);
    if (ret < 0) {
        errno = EBADF;
        return -1;
    }
    return ret;
}

int _isatty(int fd) {
    if (bridge_is_tty(fd))
        return 1;
    struct termios t;
    return tcgetattr(fd, &t) == 0 ? 1 : 0;
}

int _fstat(int fd, struct stat *st) {
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    if (bridge_is_tty(fd)) {
        st->st_mode = S_IFCHR | 0666;
        st->st_nlink = 1;
        return 0;
    }
    off_t cur = _lseek(fd, 0, SEEK_CUR);
    off_t end = _lseek(fd, 0, SEEK_END);
    if (cur >= 0 && end >= 0) {
        st->st_size = end;
        _lseek(fd, cur, SEEK_SET);
        st->st_mode = S_IFREG;
    }
    return 0;
}

int _stat(const char *file, struct stat *st) {
    /* Kernel fills xfs_dirent_t via SYS_STAT — map into POSIX struct stat. */
    typedef struct {
        char     name[64];
        uint32_t inode_block;
        uint32_t size;
        uint16_t flags;
        uint16_t reserved;
    } xos_dirent_t;
    xos_dirent_t dent;
    int ret;

    if (!st || !file) {
        errno = EINVAL;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    memset(&dent, 0, sizeof(dent));
    ret = sys_stat(file, &dent);
    if (ret < 0) {
        errno = ENOENT;
        return -1;
    }
    st->st_size = dent.size;
    if (dent.flags & 1)
        st->st_mode = S_IFDIR | 0755;
    else
        st->st_mode = S_IFREG | 0755;
    st->st_nlink = 1;
    return 0;
}

int _unlink(const char *name) {
    int ret = sys_unlink(name);
    if (ret < 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int _link(const char *old, const char *new) {
    (void)old; (void)new;
    errno = ENOSYS;
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Memory — sbrk for malloc */

static char *heap_end = NULL;

void *_sbrk(ptrdiff_t incr) {
    if (heap_end == NULL) {
        /* Query initial brk from kernel */
        uint64_t brk = (uint64_t)sys_brk(0);
        heap_end = (char *)brk;
    }

    char *old_heap_end = heap_end;
    char *new_heap_end = heap_end + incr;

    /* Ask kernel to extend brk */
    uint64_t ret = (uint64_t)sys_brk((void *)new_heap_end);
    if ((int64_t)ret < 0) {
        errno = ENOMEM;
        return (void *)-1;
    }

    heap_end = new_heap_end;
    return old_heap_end;
}

/* -------------------------------------------------------------------------- */
/* Pipe */

int _pipe(int pipefd[2]) {
    return sys_pipe(pipefd);
}

/* -------------------------------------------------------------------------- */
/* Filesystem */

int _mkdir(const char *path, int mode) {
    (void)mode;
    return sys_mkdir(path);
}

char *_getcwd(char *buf, size_t size) {
    int ret = sys_getcwd(buf, size);
    if (ret < 0) return NULL;
    return buf;
}

int _chdir(const char *path) {
    return sys_chdir(path);
}

/* -------------------------------------------------------------------------- */
/* Stubs for not-yet-implemented */

int _gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (tv) {
        uint64_t ticks = 0;
        __asm__ volatile("syscall" : "=a"(ticks) : "0"(SYS_GET_TICKS) : "rcx", "r11", "memory");
        tv->tv_sec = ticks / 1000;
        tv->tv_usec = (ticks % 1000) * 1000;
    }
    return 0;
}
int gettimeofday(struct timeval *tv, void *tz) { return _gettimeofday(tv, tz); }

/* -------------------------------------------------------------------------- */
/* Non-underscore aliases — newlib's connector files (sysread.c, etc.) define
 * read(), write(), etc. which call _read_r(). But _read_r() calls _read().
 * When --disable-newlib-supplied-syscalls is used, newlib doesn't provide
 * the _read() stubs, so we provide both _read() and read() here.
 * The connectors in libc.a reference read/write/close/etc. so we need them. */

_READ_WRITE_RETURN_TYPE read(int fd, void *buf, size_t cnt) { return _read(fd, buf, cnt); }
_READ_WRITE_RETURN_TYPE write(int fd, const void *buf, size_t cnt) { return _write(fd, buf, cnt); }
int open(const char *file, int flags, ...) { (void)0; return _open(file, flags, 0); }
int close(int fd) { return _close(fd); }
off_t lseek(int fd, off_t offset, int whence) { return _lseek(fd, offset, whence); }
int fstat(int fd, struct stat *st) { return _fstat(fd, st); }
int stat(const char *file, struct stat *st) { return _stat(file, st); }
int lstat(const char *file, struct stat *st) { return _stat(file, st); }
int unlink(const char *name) { return _unlink(name); }
int fork(void) { return _fork(); }
int execve(const char *name, char *const argv[], char *const env[]) { return _execve(name, argv, env); }
int execv(const char *path, char *const argv[]) { return _execve(path, argv, NULL); }
int rename(const char *oldp, const char *newp) {
    (void)oldp; (void)newp;
    return -1;
}
pid_t wait(int *status) { return _wait(status); }
int kill(int pid, int sig) { return _kill(pid, sig); }
pid_t getpid(void) { return _getpid(); }
void *sbrk(ptrdiff_t incr) { return _sbrk(incr); }
int pipe(int pipefd[2]) { return _pipe(pipefd); }
char *getcwd(char *buf, size_t size) { return _getcwd(buf, size); }
int chdir(const char *path) { return _chdir(path); }
int mkdir(const char *path, mode_t mode) { return _mkdir(path, (int)mode); }

/* -------------------------------------------------------------------------- */
/* Additional POSIX stubs needed by dash */

int dup(int oldfd) {
    if (bridge_is_tty(oldfd))
        return bridge_alloc_fd(10);
    return sys_dup(oldfd);
}

int dup2(int oldfd, int newfd) {
    if (bridge_is_tty(oldfd) && newfd >= 0 && newfd < BRIDGE_FD_MAX) {
        bridge_mark_tty(newfd);
        return newfd;
    }
    return sys_dup2(oldfd, newfd);
}

uid_t geteuid(void) { return 0; }
uid_t getuid(void) { return 0; }
gid_t getegid(void) { return 0; }
gid_t getgid(void) { return 0; }
int setuid(uid_t uid) { (void)uid; return 0; }
int setgid(gid_t gid) { (void)gid; return 0; }
int seteuid(uid_t uid) { (void)uid; return 0; }
int setegid(gid_t gid) { (void)gid; return 0; }

/* Signals — newlib defines these as macros, so undef first */
#undef sigprocmask
#undef sigemptyset
#undef sigfillset
#undef sigaddset
#undef sigdelset
#undef sigismember
#undef sigaction
#undef sigsuspend

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    return sys_sigprocmask(how, set, oldset);
}
int sigemptyset(sigset_t *set) { if (set) memset(set, 0, sizeof(*set)); return 0; }
int sigfillset(sigset_t *set) { if (set) memset(set, 0xff, sizeof(*set)); return 0; }
/* Match newlib macros: bit N = signal N (not N-1). */
int sigaddset(sigset_t *set, int signum) {
    if (!set || signum <= 0) return -1;
    *set |= (sigset_t)(1UL << signum);
    return 0;
}
int sigdelset(sigset_t *set, int signum) {
    if (!set || signum <= 0) return -1;
    *set &= ~(sigset_t)(1UL << signum);
    return 0;
}
int sigismember(const sigset_t *set, int signum) {
    if (!set || signum <= 0) return 0;
    return (*set & (sigset_t)(1UL << signum)) ? 1 : 0;
}
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    return sys_sigaction(signum, act, oldact);
}
int sigsuspend(const sigset_t *mask) {
    /* POSIX sigsuspend is atomic: replace the mask and sleep until a signal
     * is delivered under that mask. Doing sigprocmask() then sigsuspend()
     * separately is wrong — unblocking SIGCHLD in sigprocmask delivers the
     * handler on that return, then sigsuspend() sleeps forever with nothing
     * pending (zsh never returns to the prompt after an external command). */
    sigset_t old;
    if (sys_sigprocmask(SIG_SETMASK, NULL, &old) < 0)
        return -1;
    (void)sys_sigsuspend(mask); /* kernel installs mask, waits, returns -EINTR */
    /* Handler already ran on syscall return; restore previous mask. */
    sys_sigprocmask(SIG_SETMASK, &old, NULL);
    errno = EINTR;
    return -1;
}

/* Process group / terminal */
pid_t getpgrp(void) { return 0; }
pid_t getppid(void) { return sys_getppid(); }
int setpgid(pid_t pid, pid_t pgrp) { (void)pid;(void)pgrp; return 0; }
pid_t setsid(void) { return 0; }
int tcsetpgrp(int fd, pid_t pgrp) { (void)fd;(void)pgrp; return 0; }
pid_t tcgetpgrp(int fd) { (void)fd; return 0; }

/* Misc */
int fnmatch(const char *pattern, const char *name, int flags) { (void)flags; (void)pattern; (void)name; return 0; }
int getopt(int argc, char *const argv[], const char *optstring) { (void)argc;(void)argv;(void)optstring; return -1; }
char *optarg = NULL;
int optind = 1;

/* More stubs */
int getgroups(int size, gid_t list[]) { (void)size;(void)list; return 0; }
long sysconf(int name) { (void)name; return 0; }
clock_t times(struct tms *buf) { if (buf) memset(buf, 0, sizeof(*buf)); return 0; }

/* ctype wrappers that dash's system.c references */
#undef _isalnum
#undef _iscntrl
#undef _islower
#undef _isspace
#undef _isalpha
#undef _isdigit
#undef _isprint
#undef _isupper
#undef _isblank
#undef _isgraph
#undef _ispunct
#undef _isxdigit
#undef _toupper
#undef _tolower

int _isalnum(int c) { return isalnum(c); }
int _iscntrl(int c) { return iscntrl(c); }
int _islower(int c) { return islower(c); }
int _isspace(int c) { return isspace(c); }
int _isalpha(int c) { return isalpha(c); }
int _isdigit(int c) { return isdigit(c); }
int _isprint(int c) { return isprint(c); }
int _isupper(int c) { return isupper(c); }
int _isblank(int c) { return isblank(c); }
int _isgraph(int c) { return isgraph(c); }
int _ispunct(int c) { return ispunct(c); }
int _isxdigit(int c) { return isxdigit(c); }
int _toupper(int c) { return toupper(c); }
int _tolower(int c) { return tolower(c); }

int killpg(pid_t pid, int sig) { return kill(-pid, sig); }
int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);

    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
        if (bridge_is_tty(fd))
            return bridge_alloc_fd(arg);
        /* Kernel has no F_DUPFD — fall back to dup. */
        return dup(fd);
    }
    if (cmd == F_GETFD || cmd == F_GETFL)
        return 0;
    if (cmd == F_SETFD || cmd == F_SETFL)
        return 0;
    (void)fd;
    return 0;
}
pid_t waitpid(pid_t pid, int *status, int options) {
    int ret = sys_waitpid((int)pid, status, options);
    if (ret < 0) {
        /* zsh treats errno!=ECHILD as fatal ("wait failed: success" when errno
         * was left 0). Map kernel -errno; bare -1 ⇒ ECHILD. */
        errno = (ret < -1) ? -ret : ECHILD;
        return -1;
    }
    return ret;
}
pid_t vfork(void) { return _fork(); }
mode_t umask(mode_t mask) { (void)mask; return 0; }

/* Real termios implementation — uses SYS_IOCTL to talk to kernel PTY layer */
int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    /* Bridge tty may be fd>=10 (zsh SHTTY); kernel only accepts 0..2. */
    if (bridge_is_tty(fd)) {
#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x40087468
#endif
#ifndef TIOCSWINSZ
#define TIOCSWINSZ 0x80087467
#endif
        if (request == TIOCGWINSZ && arg) {
            struct { unsigned short row, col, xpixel, ypixel; } *ws = arg;
            ws->row = 24;
            ws->col = 80;
            ws->xpixel = 0;
            ws->ypixel = 0;
            return 0;
        }
        if (request == TIOCSWINSZ)
            return 0;
        if (request == TIOCGETA && arg) {
            bridge_termios_ensure();
            *(struct termios *)arg = g_bridge_termios;
            return 0;
        }
        if ((request == TIOCSETA || request == TIOCSETAW || request == TIOCSETAF)
            && arg) {
            bridge_termios_ensure();
            g_bridge_termios = *(const struct termios *)arg;
            return 0;
        }
        /* Ignore TIOCSPGRP / misc tty ioctls on the bridge. */
        return 0;
    }
    return (int)syscall3(SYS_IOCTL, (uintptr_t)fd, (uintptr_t)request, (uintptr_t)arg);
}

speed_t cfgetispeed(const struct termios *t) { return t ? t->c_ispeed : 0; }
speed_t cfgetospeed(const struct termios *t) { return t ? t->c_ospeed : 0; }
int cfsetispeed(struct termios *t, speed_t s) { if (t) t->c_ispeed = s; return 0; }
int cfsetospeed(struct termios *t, speed_t s) { if (t) t->c_ospeed = s; return 0; }
int cfsetspeed(struct termios *t, speed_t s) {
    if (t) { t->c_ispeed = s; t->c_ospeed = s; }
    return 0;
}

int tcgetattr(int fd, struct termios *t) {
    if (!t) return -1;
    if (bridge_is_tty(fd)) {
        bridge_termios_ensure();
        *t = g_bridge_termios;
        return 0;
    }
    return ioctl(fd, TIOCGETA, t);
}

int tcsetattr(int fd, int act, const struct termios *t) {
    if (!t) return -1;
    (void)act;
    if (bridge_is_tty(fd)) {
        bridge_termios_ensure();
        g_bridge_termios = *t;
        return 0;
    }
    unsigned long req;
    switch (act & ~TCSASOFT) {
        case TCSANOW:   req = TIOCSETA; break;
        case TCSADRAIN: req = TIOCSETA; break;  /* kernel drains for us */
        case TCSAFLUSH: req = TIOCSETA; break;  /* kernel flushes for us */
        default: return -1;
    }
    return ioctl(fd, req, (void *)(uintptr_t)t);
}

int tcdrain(int fd) { (void)fd; return 0; }
int tcflow(int fd, int action) { (void)fd; (void)action; return 0; }
int tcflush(int fd, int which) { (void)fd; (void)which; return 0; }
int tcsendbreak(int fd, int dur) { (void)fd; (void)dur; return 0; }

void cfmakeraw(struct termios *t) {
    if (!t) return;
    t->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(CSIZE | PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

int isatty(int fd) {
    if (bridge_is_tty(fd))
        return 1;
    struct termios t;
    return tcgetattr(fd, &t) == 0 ? 1 : 0;
}

/* zsh extras */
int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    (void)clk_id;
    if (tp) {
        uint64_t ticks = 0;
        __asm__ volatile("syscall" : "=a"(ticks) : "0"(SYS_GET_TICKS) : "rcx", "r11", "memory");
        tp->tv_sec = ticks / 1000;
        tp->tv_nsec = (ticks % 1000) * 1000000;
    }
    return 0;
}
int setreuid(uid_t ruid, uid_t euid) { (void)ruid; (void)euid; return 0; }
int setregid(gid_t rgid, gid_t egid) { (void)rgid; (void)egid; return 0; }
int nice(int inc) { (void)inc; return 0; }

/* More zsh stubs */
int link(const char *oldpath, const char *newpath) { (void)oldpath; (void)newpath; return -1; }
int symlink(const char *target, const char *linkpath) { (void)target; (void)linkpath; errno = ENOSYS; return -1; }
ssize_t readlink(const char *path, char *buf, size_t bufsiz) { (void)path; (void)buf; (void)bufsiz; errno = EINVAL; return -1; }
int rmdir(const char *path) { (void)path; errno = ENOSYS; return -1; }
int mkfifo(const char *path, mode_t mode) { (void)path; (void)mode; errno = ENOSYS; return -1; }
int chown(const char *path, uid_t owner, gid_t group) { (void)path; (void)owner; (void)group; return 0; }
int chmod(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }
unsigned int alarm(unsigned int seconds) { (void)seconds; return 0; }
int access(const char *path, int mode) {
    (void)mode;
    struct stat st;
    if (!path || _stat(path, &st) < 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int nanosleep(const struct timespec *rqtp, struct timespec *rmtp);

unsigned int sleep(unsigned int seconds) {
    struct timespec ts;
    ts.tv_sec = seconds;
    ts.tv_nsec = 0;
    nanosleep(&ts, NULL);
    return 0;
}

char *getlogin(void) { return "root"; }
char *ttyname(int fd) { (void)fd; return "/dev/tty"; }

/* Minimal environ for zsh PATH / HOME (newlib environ starts empty). */
extern char **environ;
#define XOS_ENV_MAX 64
static char *xos_env_storage[XOS_ENV_MAX + 1];
static int xos_env_ready;

static void xos_env_init(void) {
    if (xos_env_ready)
        return;
    xos_env_storage[0] = NULL;
    environ = xos_env_storage;
    xos_env_ready = 1;
}

static char *xos_env_get(const char *name) {
    size_t nlen;
    if (!name || !*name)
        return NULL;
    xos_env_init();
    nlen = strlen(name);
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, name, nlen) == 0 && (*e)[nlen] == '=')
            return *e + nlen + 1;
    }
    return NULL;
}

char *getenv(const char *name) {
    return xos_env_get(name);
}

int setenv(const char *name, const char *value, int overwrite) {
    size_t nlen, vlen, i;
    char *entry;
    if (!name || !*name || strchr(name, '=') || !value) {
        errno = EINVAL;
        return -1;
    }
    xos_env_init();
    nlen = strlen(name);
    vlen = strlen(value);
    for (i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=') {
            if (!overwrite)
                return 0;
            entry = malloc(nlen + vlen + 2);
            if (!entry) {
                errno = ENOMEM;
                return -1;
            }
            memcpy(entry, name, nlen);
            entry[nlen] = '=';
            memcpy(entry + nlen + 1, value, vlen + 1);
            free(environ[i]);
            environ[i] = entry;
            return 0;
        }
    }
    if (i >= XOS_ENV_MAX) {
        errno = ENOMEM;
        return -1;
    }
    entry = malloc(nlen + vlen + 2);
    if (!entry) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(entry, name, nlen);
    entry[nlen] = '=';
    memcpy(entry + nlen + 1, value, vlen + 1);
    environ[i] = entry;
    environ[i + 1] = NULL;
    return 0;
}

int unsetenv(const char *name) {
    size_t nlen, i, j;
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    xos_env_init();
    nlen = strlen(name);
    for (i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=') {
            free(environ[i]);
            for (j = i; environ[j]; j++)
                environ[j] = environ[j + 1];
            return 0;
        }
    }
    return 0;
}

int putenv(char *string) {
    char *eq;
    char namebuf[128];
    size_t nlen;
    if (!string || !(eq = strchr(string, '='))) {
        errno = EINVAL;
        return -1;
    }
    nlen = (size_t)(eq - string);
    if (nlen == 0 || nlen >= sizeof(namebuf)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(namebuf, string, nlen);
    namebuf[nlen] = '\0';
    return setenv(namebuf, eq + 1, 1);
}

/* Group/password stubs */
struct group { char *gr_name; char *gr_passwd; gid_t gr_gid; char **gr_mem; };
struct passwd { char *pw_name; char *pw_passwd; uid_t pw_uid; gid_t pw_gid; char *pw_gecos; char *pw_dir; char *pw_shell; };
struct group *getgrgid(gid_t gid) { (void)gid; return NULL; }
struct group *getgrnam(const char *name) { (void)name; return NULL; }
struct passwd *getpwuid(uid_t uid) { (void)uid; return NULL; }
struct passwd *getpwnam(const char *name) { (void)name; return NULL; }

/* zsh internal resource info stubs */
void set_resinfo(void *p) { (void)p; }
void free_resinfo(void *p) { (void)p; }

/* Missing newlib functions */
int nanosleep(const struct timespec *rqtp, struct timespec *rmtp) {
    (void)rmtp;
    if (rqtp) {
        uint64_t ms = (uint64_t)rqtp->tv_sec * 1000 + rqtp->tv_nsec / 1000000;
        if (ms > 0) {
            syscall1(12, ms);  /* SYS_NSLEEP — blocks in kernel */
        }
    }
    return 0;
}

int gethostname(char *name, size_t len) __attribute__((weak));
int gethostname(char *name, size_t len) {
    if (name && len >= 5) {
        /* Manual copy — avoid SSE */
        name[0]='x'; name[1]='-'; name[2]='o'; name[3]='s'; name[4]='\0';
    }
    return 0;
}

int uname(struct utsname *buf) __attribute__((weak));
int uname(struct utsname *buf) {
    if (!buf) return -1;
    /* Manual string copies — avoid SSE/memcpy */
    int i;
    const char *s;
    for (i = 0, s = "X-OS"; s[i] && i < 255; i++) buf->sysname[i] = s[i];
    buf->sysname[i] = '\0';
    for (i = 0, s = "x-os"; s[i] && i < 255; i++) buf->nodename[i] = s[i];
    buf->nodename[i] = '\0';
    for (i = 0, s = "1.0"; s[i] && i < 255; i++) buf->release[i] = s[i];
    buf->release[i] = '\0';
    for (i = 0, s = "X-OS 1.0"; s[i] && i < 255; i++) buf->version[i] = s[i];
    buf->version[i] = '\0';
    for (i = 0, s = "x86_64"; s[i] && i < 255; i++) buf->machine[i] = s[i];
    buf->machine[i] = '\0';
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Directory operations — opendir/readdir/closedir using XFS sys_readdir */

#define XFS_NAME_MAX 128

typedef struct {
    char     name[XFS_NAME_MAX];
    uint32_t inode_block;
    uint32_t size;
    uint16_t flags;
    uint16_t reserved;
} xfs_dirent_raw_t;

DIR *opendir(const char *path) __attribute__((weak));
DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    DIR *d = (DIR *)malloc(sizeof(DIR));
    if (!d) { close(fd); return NULL; }
    d->dd_fd = fd;
    d->dd_loc = 0;
    d->dd_size = 0;
    d->dd_bufsize = sizeof(xfs_dirent_raw_t) * 64;
    d->dd_buf = (char *)malloc(d->dd_bufsize);
    if (!d->dd_buf) { free(d); close(fd); return NULL; }
    d->dd_flags = 0;
    return d;
}

struct dirent *readdir(DIR *d) __attribute__((weak));
struct dirent *readdir(DIR *d) {
    if (!d || d->dd_fd < 0) return NULL;
    /* We read entries in bulk; dd_loc tracks position in the buffer */
    if (d->dd_loc >= d->dd_size) {
        int n = sys_readdir(d->dd_fd, d->dd_buf, 64);
        if (n <= 0) return NULL;
        d->dd_size = n * sizeof(xfs_dirent_raw_t);
        d->dd_loc = 0;
    }
    xfs_dirent_raw_t *raw = (xfs_dirent_raw_t *)(d->dd_buf + d->dd_loc);
    d->dd_loc += sizeof(xfs_dirent_raw_t);
    /* Copy to static result (POSIX allows this) */
    static struct dirent result;
    result.d_fileno = raw->inode_block;
    result.d_reclen = sizeof(struct dirent);
    result.d_type = (raw->flags & 1) ? DT_DIR : DT_REG;
    result.d_namlen = 0;
    while (result.d_namlen < 255 && raw->name[result.d_namlen]) {
        result.d_name[result.d_namlen] = raw->name[result.d_namlen];
        result.d_namlen++;
    }
    result.d_name[result.d_namlen] = '\0';
    return &result;
}

int closedir(DIR *d) __attribute__((weak));
int closedir(DIR *d) {
    if (!d) return -1;
    if (d->dd_fd >= 0) close(d->dd_fd);
    if (d->dd_buf) free(d->dd_buf);
    free(d);
    return 0;
}

void rewinddir(DIR *d) __attribute__((weak));
void rewinddir(DIR *d) {
    if (!d) return;
    d->dd_loc = 0;
    d->dd_size = 0;
    lseek(d->dd_fd, 0, SEEK_SET);
}

/* Minimal sockaddr / sockaddr_in definitions (no newlib sys/socket.h) */
struct xos_sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};

struct xos_sockaddr_in {
    unsigned short sin_family;
    unsigned short sin_port;
    unsigned int sin_addr;
    unsigned char sin_zero[8];
};

int socket(int domain, int type, int protocol) {
    int fd = sys_socket(domain, type, protocol);
    if (fd < 0) {
        errno = -fd;
        return -1;
    }
    return fd;
}

int connect(int fd, const void *addr, int addrlen) {
    int ret = sys_connect(fd, addr, addrlen);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

int bind(int fd, const void *addr, int addrlen) {
    int ret = sys_bind(fd, addr, addrlen);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

int sendto(int fd, const void *buf, size_t len, int flags,
           const void *addr, int addrlen) {
    int ret = sys_sendto(fd, buf, len, flags, addr, addrlen);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int recvfrom(int fd, void *buf, size_t len, int flags,
             void *addr, int *addrlen) {
    int ret = sys_recvfrom(fd, buf, len, flags, addr, addrlen);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int send(int fd, const void *buf, size_t len, int flags) {
    int ret = sys_send(fd, buf, len, flags);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int recv(int fd, void *buf, size_t len, int flags) {
    int ret = sys_recv(fd, buf, len, flags);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int setsockopt(int fd, int level, int optname, const void *optval, int optlen) {
    int ret = sys_setsockopt(fd, level, optname, optval, optlen);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

int getsockopt(int fd, int level, int optname, void *optval, int *optlen) {
    int ret = sys_getsockopt(fd, level, optname, optval, optlen);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

int shutdown(int fd, int how) {
    int ret = sys_shutdown(fd, how);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

int getsockname(int fd, void *addr, int *addrlen) {
    int ret = sys_getsockname(fd, addr, addrlen);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

int getpeername(int fd, void *addr, int *addrlen) {
    int ret = sys_getpeername(fd, addr, addrlen);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

int listen(int fd, int backlog) {
    int ret = sys_listen(fd, backlog);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

int accept(int fd, void *addr, int *addrlen) {
    int ret = sys_accept(fd, addr, addrlen);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}
