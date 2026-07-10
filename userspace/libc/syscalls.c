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
#include <stdarg.h>
#include <termios.h>

#include "kernel/include/syscall.h"

#undef errno
extern int errno;

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
    return sys_waitpid(-1, status, 0);
}

int _kill(int pid, int sig) {
    (void)sig;  /* signals not yet implemented */
    (void)pid;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* I/O — IPC bridge mode for terminal integration */

/* When set, _read receives from g_bridge_input_port and _write sends to
 * g_bridge_output_port. Used by zsh when running inside the terminal. */
static port_handle_t g_bridge_input_port = 0;   /* port to recv keyboard chars from */
static port_handle_t g_bridge_output_port = 0;  /* port to send output text to */

void set_shell_bridge(port_handle_t input_port, port_handle_t output_port) {
    g_bridge_input_port = input_port;
    g_bridge_output_port = output_port;
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
    if (fd == 0 && g_bridge_input_port) {
        /* IPC bridge mode: receive keyboard chars from terminal via IPC */
        char *cbuf = (char *)buf;
        size_t got = 0;
        while (got < cnt) {
            ipc_msg_t msg;
            /* Manual zeroing — avoid newlib memset which may use SSE */
            for (size_t i = 0; i < sizeof(msg); i++) ((uint8_t *)&msg)[i] = 0;
            if (sys_port_recv(g_bridge_input_port, &msg, 0)) {
                if (msg.payload_len >= 1) {
                    char c = (char)msg.payload[0];
                    if (c == '\r') c = '\n';
                    cbuf[got++] = c;
                    break;
                }
            }
            syscall0(SYS_YIELD);
        }
        return (_ssize_t)got;
    }
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
            syscall0(SYS_YIELD);
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
    if ((fd == 1 || fd == 2) && g_bridge_output_port) {
        /* IPC bridge mode: send output text to terminal via IPC */
        const char *p = (const char *)buf;
        size_t remaining = cnt;
        while (remaining > 0) {
            size_t chunk = remaining > IPC_MSG_MAX_PAYLOAD ? IPC_MSG_MAX_PAYLOAD : remaining;
            ipc_msg_t msg;
            /* Manual zeroing — avoid newlib memset which may use SSE */
            for (size_t i = 0; i < sizeof(msg); i++) ((uint8_t *)&msg)[i] = 0;
            msg.type = IPC_MSG_EVENT;
            msg.sender_pid = syscall0(SYS_PROC_PID);
            msg.cap_count = 0;
            msg.payload_len = (uint32_t)chunk;
            /* Manual copy — avoid newlib memcpy which may use SSE */
            for (size_t i = 0; i < chunk; i++) msg.payload[i] = ((const uint8_t *)p)[i];
            sys_port_send(g_bridge_output_port, &msg);
            p += chunk;
            remaining -= chunk;
        }
        return (_ssize_t)cnt;
    }
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

    int ret = sys_open(file, xfs_flags);
    if (ret < 0) {
        errno = ENOENT;
        return -1;
    }
    return ret;
}

int _close(int fd) {
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
    struct termios t;
    return tcgetattr(fd, &t) == 0 ? 1 : 0;
}

int _fstat(int fd, struct stat *st) {
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
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
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG;
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

int dup2(int oldfd, int newfd) { return sys_dup2(oldfd, newfd); }
int dup(int oldfd) { return sys_dup(oldfd); }

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

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) { (void)how;(void)set;(void)oldset; return 0; }
int sigemptyset(sigset_t *set) { if (set) memset(set, 0, sizeof(*set)); return 0; }
int sigfillset(sigset_t *set) { if (set) memset(set, 0xff, sizeof(*set)); return 0; }
int sigaddset(sigset_t *set, int signum) { (void)signum; if (set) ((unsigned char*)set)[signum/8] |= (1<<(signum%8)); return 0; }
int sigdelset(sigset_t *set, int signum) { (void)signum; if (set) ((unsigned char*)set)[signum/8] &= ~(1<<(signum%8)); return 0; }
int sigismember(const sigset_t *set, int signum) { (void)signum; return set && (((const unsigned char*)set)[signum/8] & (1<<(signum%8))); }
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) { (void)signum;(void)act;(void)oldact; return 0; }
int sigsuspend(const sigset_t *mask) { (void)mask; return 0; }

/* Process group / terminal */
pid_t getpgrp(void) { return 0; }
pid_t getppid(void) { return 0; }
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
int fcntl(int fd, int cmd, ...) { (void)fd;(void)cmd; return 0; }
pid_t waitpid(pid_t pid, int *status, int options) { return sys_waitpid((int)pid, status, options); }
pid_t vfork(void) { return _fork(); }
mode_t umask(mode_t mask) { (void)mask; return 0; }

/* Real termios implementation — uses SYS_IOCTL to talk to kernel PTY layer */
int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
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
    return ioctl(fd, TIOCGETA, t);
}

int tcsetattr(int fd, int act, const struct termios *t) {
    if (!t) return -1;
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
int access(const char *path, int mode) { (void)path; (void)mode; return 0; }
unsigned int sleep(unsigned int seconds) { (void)seconds; return 0; }
char *getlogin(void) { return "root"; }
char *ttyname(int fd) { (void)fd; return "/dev/tty"; }
char *getenv(const char *name) { (void)name; return NULL; }
int setenv(const char *name, const char *value, int overwrite) { (void)name; (void)value; (void)overwrite; return 0; }
int unsetenv(const char *name) { (void)name; return 0; }
int putenv(char *string) { (void)string; return 0; }

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
            uint64_t end = 0;
            __asm__ volatile("syscall" : "=a"(end) : "0"(SYS_GET_TICKS) : "rcx", "r11", "memory");
            end += ms;
            for (;;) {
                uint64_t now = 0;
                __asm__ volatile("syscall" : "=a"(now) : "0"(SYS_GET_TICKS) : "rcx", "r11", "memory");
                if (now >= end) break;
                syscall0(SYS_YIELD);
            }
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
