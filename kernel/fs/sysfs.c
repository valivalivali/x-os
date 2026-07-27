#include "kernel/fs/sysfs.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/msgbuf.h"
#include "kernel/sched/sched.h"
#include "kernel/memory/pmm.h"
#include "kernel/memory/heap.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/hal/apic/smp.h"
#include "kernel/hal/apic/spinlock.h"
#include "kernel/hal/timers/timer.h"
#include "kernel/hal/input/input.h"
#include "kernel/hal/gpu/virtio_gpu.h"
#include "kernel/boot/bootargs.h"
#include "kernel/include/syscall.h"

/* ---- Text building ------------------------------------------------------
 * A tiny append-only writer.  Every generator fills one of these and the
 * result becomes the file's contents for the lifetime of the descriptor,
 * which gives readers a stable snapshot even while the system moves. */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} sbuf_t;

static void sb_putc(sbuf_t *s, char c) {
    if (s->len + 1 < s->cap) s->buf[s->len++] = c;
}

static void sb_puts(sbuf_t *s, const char *str) {
    if (!str) str = "(null)";
    while (*str) sb_putc(s, *str++);
}

static void sb_putu(sbuf_t *s, uint64_t v) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) sb_putc(s, tmp[--n]);
}

static void sb_puti(sbuf_t *s, int64_t v) {
    if (v < 0) { sb_putc(s, '-'); v = -v; }
    sb_putu(s, (uint64_t)v);
}

static void sb_puthex(sbuf_t *s, uint64_t v) {
    static const char *hx = "0123456789abcdef";
    char tmp[20];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = hx[v & 0xF]; v >>= 4; }
    sb_puts(s, "0x");
    while (n) sb_putc(s, tmp[--n]);
}

/* "name: value\n" — the whole tree uses this one shape so that both humans
 * and one-line shell pipelines can parse it without a spec. */
static void sb_kv_u(sbuf_t *s, const char *k, uint64_t v) {
    sb_puts(s, k); sb_puts(s, ": "); sb_putu(s, v); sb_putc(s, '\n');
}
static void sb_kv_s(sbuf_t *s, const char *k, const char *v) {
    sb_puts(s, k); sb_puts(s, ": "); sb_puts(s, v); sb_putc(s, '\n');
}
static void sb_kv_hex(sbuf_t *s, const char *k, uint64_t v) {
    sb_puts(s, k); sb_puts(s, ": "); sb_puthex(s, v); sb_putc(s, '\n');
}

/* ---- Path helpers -------------------------------------------------------- */

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Match `path` against "/sys/<pfx>" and hand back whatever follows.
 * Returns NULL when the prefix does not match. */
static const char *path_after(const char *path, const char *pfx) {
    size_t n = 0;
    while (pfx[n]) {
        if (path[n] != pfx[n]) return NULL;
        n++;
    }
    return path + n;
}

static uint64_t parse_u64(const char *s, const char **end) {
    uint64_t v = 0;
    int any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); s++; any = 1; }
    if (end) *end = s;
    return any ? v : (uint64_t)-1;
}

/* ---- Descriptors --------------------------------------------------------- */

typedef struct {
    bool     used;
    bool     is_dir;
    uint32_t pid;          /* owner */
    uint32_t offset;
    uint32_t len;
    char     path[128];
    char     data[SYSFS_BUF_MAX];
} sysfs_fd_t;

static sysfs_fd_t g_fds[SYSFS_MAX_FDS];
static spinlock_t sysfs_lock = SPINLOCK_INIT;

/* ---- Generators ---------------------------------------------------------- */

static const char *state_name(proc_state_t st) {
    switch (st) {
        case PROC_DEAD:    return "dead";
        case PROC_READY:   return "ready";
        case PROC_RUNNING: return "running";
        case PROC_BLOCKED: return "blocked";
    }
    return "?";
}

static const char *prio_name(uint8_t p) {
    switch (p) {
        case PRIO_HIGH:   return "high";
        case PRIO_NORMAL: return "normal";
        case PRIO_LOW:    return "low";
    }
    return "?";
}

static void gen_kernel(sbuf_t *s) {
    uint64_t hz = timer_ticks_hz();
    uint64_t ticks = timer_ticks();
    sb_kv_s(s, "name", "X OS");
    sb_kv_s(s, "arch", "x86_64");
    sb_kv_u(s, "ticks", ticks);
    sb_kv_u(s, "tick_hz", hz);
    sb_kv_u(s, "uptime_ms", hz ? (ticks * 1000) / hz : 0);
    sb_kv_u(s, "uptime_s", hz ? ticks / hz : 0);
    sb_kv_s(s, "boot_args", bootargs_raw());
    sb_kv_u(s, "cpus", g_cpu_count);
    sb_kv_u(s, "smp", g_smp_enabled ? 1 : 0);
}

static void gen_cpu(sbuf_t *s) {
    sb_kv_s(s, "vendor", g_cpu.vendor);
    sb_kv_s(s, "brand", g_cpu.brand[0] ? g_cpu.brand : "unknown");
    sb_kv_u(s, "family", g_cpu.family);
    sb_kv_u(s, "model", g_cpu.model);
    sb_kv_u(s, "stepping", g_cpu.stepping);
    sb_kv_u(s, "count", g_cpu_count);
    sb_kv_u(s, "tsc_hz", tsc_freq_hz());
    sb_kv_hex(s, "xcr0", g_cpu.xcr0);
    sb_puts(s, "features:");
    if (g_cpu.nx)        sb_puts(s, " nx");
    if (g_cpu.smep)      sb_puts(s, " smep");
    if (g_cpu.smap)      sb_puts(s, " smap");
    if (g_cpu.umip)      sb_puts(s, " umip");
    if (g_cpu.pge)       sb_puts(s, " pge");
    if (g_cpu.pcid)      sb_puts(s, " pcid");
    if (g_cpu.xsave)     sb_puts(s, " xsave");
    if (g_cpu.avx)       sb_puts(s, " avx");
    if (g_cpu.avx2)      sb_puts(s, " avx2");
    if (g_cpu.avx512f)   sb_puts(s, " avx512f");
    if (g_cpu.x2apic)    sb_puts(s, " x2apic");
    if (g_cpu.rdtscp)    sb_puts(s, " rdtscp");
    if (g_cpu.pdpe1gb)   sb_puts(s, " 1gb-pages");
    if (g_cpu.aes)       sb_puts(s, " aes");
    if (g_cpu.rdrand)    sb_puts(s, " rdrand");
    if (g_cpu.hypervisor) sb_puts(s, " hypervisor");
    sb_putc(s, '\n');
}

static void gen_mem(sbuf_t *s) {
    uint64_t total = pmm_total_bytes();
    uint64_t used  = pmm_used_bytes();
    sb_kv_u(s, "phys_total", total);
    sb_kv_u(s, "phys_used", used);
    sb_kv_u(s, "phys_free", total > used ? total - used : 0);
    sb_kv_u(s, "phys_total_mb", total >> 20);
    sb_kv_u(s, "phys_free_mb", (total > used ? total - used : 0) >> 20);
    sb_kv_u(s, "heap_size", heap_size());
    sb_kv_u(s, "heap_used", heap_used());
    sb_kv_u(s, "page_size", PAGE_SIZE);
}

static void gen_input(sbuf_t *s) {
    sb_puts(s, "mouse_x: "); sb_puti(s, input_mouse_x()); sb_putc(s, '\n');
    sb_puts(s, "mouse_y: "); sb_puti(s, input_mouse_y()); sb_putc(s, '\n');
}

static void gen_display(sbuf_t *s) {
    gpu_fb_info_t info;
    if (!virtio_gpu_get_fb_info(&info)) {
        sb_puts(s, "present: 0\n");
        return;
    }
    sb_kv_s(s, "driver", "virtio-gpu");
    sb_kv_u(s, "present", 1);
    sb_kv_u(s, "width", info.width);
    sb_kv_u(s, "height", info.height);
    sb_kv_u(s, "stride", info.stride);
    sb_kv_u(s, "virgl", info.virgl);
    sb_kv_hex(s, "fb_phys", info.backing_phys);
    sb_kv_u(s, "fb_bytes", info.backing_size);
}

static void gen_proc_one(sbuf_t *s, proc_t *p) {
    sb_kv_u(s, "pid", p->pid);
    sb_kv_s(s, "name", p->name[0] ? p->name : "(kernel)");
    sb_kv_s(s, "state", state_name(p->state));
    sb_kv_s(s, "prio", prio_name(p->priority));
    sb_kv_u(s, "ppid", p->parent_pid);
    sb_kv_u(s, "tgid", p->tgid);
    sb_kv_u(s, "ring", p->ring3 ? 3 : 0);
    sb_kv_u(s, "no_preempt", p->no_preempt);
    sb_kv_u(s, "sleep_until", p->sleep_until);
    sb_kv_hex(s, "pml4", p->pml4_phys);
    sb_kv_hex(s, "rip", p->rip);
}

/* One line per process — this is what `ps` reads. */
static void gen_proc_table(sbuf_t *s) {
    sb_puts(s, "PID\tPPID\tSTATE\tPRIO\tRING\tNAME\n");
    for (int i = 0; i < SCHED_MAX_PROCS; i++) {
        proc_t *p = sched_proc_slot(i);
        if (!p || (p->state == PROC_DEAD && p->reaped)) continue;
        if (p->pid == 0) continue;   /* per-CPU idle tasks */
        sb_putu(s, p->pid);        sb_putc(s, '\t');
        sb_putu(s, p->parent_pid); sb_putc(s, '\t');
        sb_puts(s, state_name(p->state)); sb_putc(s, '\t');
        sb_puts(s, prio_name(p->priority)); sb_putc(s, '\t');
        sb_putu(s, p->ring3 ? 3 : 0); sb_putc(s, '\t');
        sb_puts(s, p->name[0] ? p->name : "(kernel)");
        sb_putc(s, '\n');
    }
}

static void gen_log(sbuf_t *s) {
    size_t n = msgbuf_copy(s->buf, s->cap - 1);
    s->len = n;
    s->buf[n] = '\0';
}

/* /sys itself and its subdirectories. */
static const char *const dir_root[]    = { "kernel", "cpu", "mem", "proc",
                                           "input", "display", NULL };
static const char *const dir_kernel[]  = { "info", "uptime", "log", NULL };
static const char *const dir_cpu[]     = { "info", NULL };
static const char *const dir_mem[]     = { "info", NULL };
static const char *const dir_input[]   = { "mouse", NULL };
static const char *const dir_display[] = { "info", NULL };
static const char *const dir_procent[] = { "status", "name", "state",
                                           "prio", "kill", NULL };

/* Resolve a path to its directory listing, or NULL if it is not a directory.
 * `pid_out` is set for /sys/proc/<pid>, which is generated per process. */
static const char *const *dir_for(const char *path, uint64_t *pid_out) {
    if (pid_out) *pid_out = 0;
    if (str_eq(path, "/sys") || str_eq(path, "/sys/")) return dir_root;
    if (str_eq(path, "/sys/kernel"))  return dir_kernel;
    if (str_eq(path, "/sys/cpu"))     return dir_cpu;
    if (str_eq(path, "/sys/mem"))     return dir_mem;
    if (str_eq(path, "/sys/input"))   return dir_input;
    if (str_eq(path, "/sys/display")) return dir_display;
    if (str_eq(path, "/sys/proc"))    return dir_root; /* replaced below */

    const char *rest = path_after(path, "/sys/proc/");
    if (rest && *rest) {
        const char *end;
        uint64_t pid = parse_u64(rest, &end);
        if (pid != (uint64_t)-1 && *end == '\0' && proc_by_pid(pid)) {
            if (pid_out) *pid_out = pid;
            return dir_procent;
        }
    }
    return NULL;
}

/* Fill a descriptor's snapshot for a regular file.  Returns false if the
 * path does not name one. */
static bool render_file(const char *path, sbuf_t *s) {
    if (str_eq(path, "/sys/kernel/info"))   { gen_kernel(s);  return true; }
    if (str_eq(path, "/sys/kernel/log"))    { gen_log(s);     return true; }
    if (str_eq(path, "/sys/cpu/info"))      { gen_cpu(s);     return true; }
    if (str_eq(path, "/sys/mem/info"))      { gen_mem(s);     return true; }
    if (str_eq(path, "/sys/input/mouse"))   { gen_input(s);   return true; }
    if (str_eq(path, "/sys/display/info"))  { gen_display(s); return true; }
    if (str_eq(path, "/sys/kernel/uptime")) {
        uint64_t hz = timer_ticks_hz();
        sb_putu(s, hz ? timer_ticks() / hz : 0);
        sb_putc(s, '\n');
        return true;
    }
    if (str_eq(path, "/sys/proc/self")) {
        proc_t *p = proc_current();
        if (p) sb_putu(s, p->pid);
        sb_putc(s, '\n');
        return true;
    }

    const char *rest = path_after(path, "/sys/proc/");
    if (rest && *rest) {
        const char *end;
        uint64_t pid = parse_u64(rest, &end);
        if (pid == (uint64_t)-1) return false;
        if (*end == '\0') {
            /* Bare /sys/proc/<pid> with no trailing component: the process
             * table row.  Handy for `cat /sys/proc` too (see below). */
            return false;
        }
        if (*end != '/') return false;
        proc_t *p = proc_by_pid(pid);
        if (!p) return false;
        const char *leaf = end + 1;
        if (str_eq(leaf, "status")) { gen_proc_one(s, p); return true; }
        if (str_eq(leaf, "name"))   { sb_puts(s, p->name[0] ? p->name : "(kernel)"); sb_putc(s, '\n'); return true; }
        if (str_eq(leaf, "state"))  { sb_puts(s, state_name(p->state)); sb_putc(s, '\n'); return true; }
        if (str_eq(leaf, "prio"))   { sb_puts(s, prio_name(p->priority)); sb_putc(s, '\n'); return true; }
        if (str_eq(leaf, "kill"))   { sb_puts(s, "0\n"); return true; }
        return false;
    }

    /* Reading the directory itself as a file gives the whole table, so
     * `cat /sys/proc` is a working `ps`. */
    if (str_eq(path, "/sys/proc")) { gen_proc_table(s); return true; }
    return false;
}

/* ---- Writes: change the running system in place -------------------------- */

static void trim(char *s) {
    size_t n = 0;
    while (s[n]) n++;
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = '\0';
}

static int apply_write(const char *path, const char *val) {
    if (str_eq(path, "/sys/kernel/log")) {
        kprintf("%s\n", val);
        return 0;
    }
    if (str_eq(path, "/sys/input/mouse")) {
        /* "x y" — warp the pointer.  Absolute, so an agent can drive the UI
         * without synthesising relative deltas. */
        const char *p = val;
        const char *end;
        uint64_t x = parse_u64(p, &end);
        if (x == (uint64_t)-1) return -1;
        while (*end == ' ' || *end == '\t') end++;
        uint64_t y = parse_u64(end, NULL);
        if (y == (uint64_t)-1) return -1;
        input_warp_mouse((int32_t)x, (int32_t)y);
        return 0;
    }

    const char *rest = path_after(path, "/sys/proc/");
    if (rest && *rest) {
        const char *end;
        uint64_t pid = parse_u64(rest, &end);
        if (pid == (uint64_t)-1 || *end != '/') return -1;
        proc_t *p = proc_by_pid(pid);
        if (!p) return -1;
        const char *leaf = end + 1;
        if (str_eq(leaf, "kill")) {
            proc_kill(pid);
            return 0;
        }
        if (str_eq(leaf, "prio")) {
            uint8_t prio;
            if (str_eq(val, "high"))        prio = PRIO_HIGH;
            else if (str_eq(val, "normal")) prio = PRIO_NORMAL;
            else if (str_eq(val, "low"))    prio = PRIO_LOW;
            else {
                uint64_t n = parse_u64(val, NULL);
                if (n > PRIO_LOW) return -1;
                prio = (uint8_t)n;
            }
            proc_set_priority(p, prio);
            return 0;
        }
    }
    return -1;
}

/* ---- Filesystem entry points --------------------------------------------- */

bool sysfs_owns(const char *path) {
    if (!path) return false;
    if (path[0] != '/' || path[1] != 's' || path[2] != 'y' || path[3] != 's')
        return false;
    return path[4] == '\0' || path[4] == '/';
}

bool sysfs_owns_fd(int fd) {
    return fd >= SYSFS_FD_BASE && fd < SYSFS_FD_BASE + SYSFS_MAX_FDS;
}

int sysfs_open(const char *path, uint32_t flags) {
    (void)flags;
    if (!sysfs_owns(path)) return -1;

    uint64_t pid_dummy;
    const char *const *dir = dir_for(path, &pid_dummy);
    bool is_dir = (dir != NULL);

    uint64_t rf = spinlock_acquire_irqsave(&sysfs_lock);
    int slot = -1;
    for (int i = 0; i < SYSFS_MAX_FDS; i++) {
        if (!g_fds[i].used) { slot = i; break; }
    }
    if (slot < 0) { spinlock_release_irqrestore(&sysfs_lock, rf); return -1; }
    sysfs_fd_t *f = &g_fds[slot];
    f->used = true;
    f->is_dir = is_dir;
    f->pid = (uint32_t)proc_current()->pid;
    f->offset = 0;
    f->len = 0;
    strncpy(f->path, path, sizeof(f->path) - 1);
    f->path[sizeof(f->path) - 1] = '\0';
    spinlock_release_irqrestore(&sysfs_lock, rf);

    /* Snapshot outside the lock: generators touch the scheduler and the GPU
     * and must not run with sysfs_lock held. */
    sbuf_t s = { f->data, sizeof(f->data), 0 };
    if (!is_dir && !render_file(path, &s)) {
        rf = spinlock_acquire_irqsave(&sysfs_lock);
        f->used = false;
        spinlock_release_irqrestore(&sysfs_lock, rf);
        return -1;
    }
    if (is_dir) {
        /* Directories also read as text, so `cat /sys` is self-describing. */
        if (str_eq(path, "/sys/proc")) gen_proc_table(&s);
        else for (const char *const *e = dir; *e; e++) { sb_puts(&s, *e); sb_putc(&s, '\n'); }
    }
    f->data[s.len < sizeof(f->data) ? s.len : sizeof(f->data) - 1] = '\0';
    f->len = (uint32_t)s.len;
    return SYSFS_FD_BASE + slot;
}

void sysfs_close(int fd) {
    if (!sysfs_owns_fd(fd)) return;
    uint64_t rf = spinlock_acquire_irqsave(&sysfs_lock);
    g_fds[fd - SYSFS_FD_BASE].used = false;
    spinlock_release_irqrestore(&sysfs_lock, rf);
}

int sysfs_read(int fd, void *buf, size_t count) {
    if (!sysfs_owns_fd(fd) || !buf) return -1;
    uint64_t rf = spinlock_acquire_irqsave(&sysfs_lock);
    sysfs_fd_t *f = &g_fds[fd - SYSFS_FD_BASE];
    if (!f->used) { spinlock_release_irqrestore(&sysfs_lock, rf); return -1; }
    size_t remain = f->len > f->offset ? f->len - f->offset : 0;
    if (count > remain) count = remain;
    memcpy(buf, f->data + f->offset, count);
    f->offset += (uint32_t)count;
    spinlock_release_irqrestore(&sysfs_lock, rf);
    return (int)count;
}

int sysfs_write(int fd, const void *buf, size_t count) {
    if (!sysfs_owns_fd(fd) || !buf) return -1;
    char val[256];
    size_t n = count < sizeof(val) - 1 ? count : sizeof(val) - 1;

    uint64_t rf = spinlock_acquire_irqsave(&sysfs_lock);
    sysfs_fd_t *f = &g_fds[fd - SYSFS_FD_BASE];
    if (!f->used || f->is_dir) { spinlock_release_irqrestore(&sysfs_lock, rf); return -1; }
    char path[128];
    strncpy(path, f->path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    spinlock_release_irqrestore(&sysfs_lock, rf);

    memcpy(val, buf, n);
    val[n] = '\0';
    trim(val);
    if (apply_write(path, val) != 0) return -1;
    return (int)count;
}

int sysfs_lseek(int fd, int offset, int whence) {
    if (!sysfs_owns_fd(fd)) return -1;
    uint64_t rf = spinlock_acquire_irqsave(&sysfs_lock);
    sysfs_fd_t *f = &g_fds[fd - SYSFS_FD_BASE];
    if (!f->used) { spinlock_release_irqrestore(&sysfs_lock, rf); return -1; }
    int base = (whence == 1) ? (int)f->offset : (whence == 2) ? (int)f->len : 0;
    int no = base + offset;
    if (no < 0) { spinlock_release_irqrestore(&sysfs_lock, rf); return -1; }
    f->offset = (uint32_t)no;
    spinlock_release_irqrestore(&sysfs_lock, rf);
    return no;
}

static void fill_dirent(xfs_dirent_t *d, const char *name, bool dir, uint32_t size) {
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name, XFS_NAME_MAX - 1);
    d->inode_block = 0xFFFFFFFFu;   /* synthetic: no backing block */
    d->size = size;
    d->flags = dir ? XFS_DENT_DIR : XFS_DENT_FILE;
}

int sysfs_readdir(int fd, xfs_dirent_t *entries, int max_entries) {
    if (!sysfs_owns_fd(fd) || !entries || max_entries <= 0) return -1;
    uint64_t rf = spinlock_acquire_irqsave(&sysfs_lock);
    sysfs_fd_t *f = &g_fds[fd - SYSFS_FD_BASE];
    if (!f->used || !f->is_dir) { spinlock_release_irqrestore(&sysfs_lock, rf); return -1; }
    char path[128];
    strncpy(path, f->path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    spinlock_release_irqrestore(&sysfs_lock, rf);

    int n = 0;
    if (str_eq(path, "/sys/proc")) {
        /* Live: one directory per running process. */
        for (int i = 0; i < SCHED_MAX_PROCS && n < max_entries; i++) {
            proc_t *p = sched_proc_slot(i);
            if (!p || p->pid == 0) continue;
            if (p->state == PROC_DEAD && p->reaped) continue;
            char nm[16];
            int k = 0;
            uint64_t v = p->pid;
            char tmp[20];
            int t = 0;
            if (!v) tmp[t++] = '0';
            while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
            while (t) nm[k++] = tmp[--t];
            nm[k] = '\0';
            fill_dirent(&entries[n++], nm, true, 0);
        }
        return n;
    }

    uint64_t pid;
    const char *const *dir = dir_for(path, &pid);
    if (!dir) return -1;
    for (const char *const *e = dir; *e && n < max_entries; e++) {
        bool is_sub = str_eq(path, "/sys");   /* /sys children are all dirs */
        fill_dirent(&entries[n++], *e, is_sub, 0);
    }
    return n;
}

int sysfs_stat(const char *path, xfs_dirent_t *out) {
    if (!sysfs_owns(path) || !out) return -1;
    uint64_t pid;
    if (dir_for(path, &pid)) {
        const char *nm = path;
        for (const char *p = path; *p; p++) if (*p == '/') nm = p + 1;
        fill_dirent(out, *nm ? nm : "sys", true, 0);
        return 0;
    }
    /* Regular file: render once to learn its length. */
    int fd = sysfs_open(path, 0);
    if (fd < 0) return -1;
    uint64_t rf = spinlock_acquire_irqsave(&sysfs_lock);
    uint32_t len = g_fds[fd - SYSFS_FD_BASE].len;
    spinlock_release_irqrestore(&sysfs_lock, rf);
    sysfs_close(fd);
    const char *nm = path;
    for (const char *p = path; *p; p++) if (*p == '/') nm = p + 1;
    fill_dirent(out, nm, false, len);
    return 0;
}

int sysfs_fstat(int fd, xfs_dirent_t *out) {
    if (!sysfs_owns_fd(fd) || !out) return -1;
    uint64_t rf = spinlock_acquire_irqsave(&sysfs_lock);
    sysfs_fd_t *f = &g_fds[fd - SYSFS_FD_BASE];
    if (!f->used) { spinlock_release_irqrestore(&sysfs_lock, rf); return -1; }
    const char *nm = f->path;
    for (const char *p = f->path; *p; p++) if (*p == '/') nm = p + 1;
    bool dir = f->is_dir;
    uint32_t len = f->len;
    char name[XFS_NAME_MAX];
    strncpy(name, *nm ? nm : "sys", XFS_NAME_MAX - 1);
    name[XFS_NAME_MAX - 1] = '\0';
    spinlock_release_irqrestore(&sysfs_lock, rf);
    fill_dirent(out, name, dir, len);
    return 0;
}
