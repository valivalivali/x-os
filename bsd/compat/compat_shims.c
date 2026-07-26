/*
 * compat_shims.c - X OS compatibility implementations for FreeBSD network stack
 *
 * Provides actual implementations of all the stub functions declared in
 * the compat headers under bsd/compat/sys/.
 */

/* Define this before including anything to get _KERNEL mode */
#undef _KERNEL
#define _KERNEL

/* Forward declare ia32_pause — buf_ring.h (via if_var.h) calls it via
 * cpu_spinwait() macro, creating an implicit declaration that conflicts
 * with our definition later. */
void ia32_pause(void);

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/_stdint.h>

/* X OS kernel headers - declare kprintf/kputs directly to avoid kvprintf conflict */
extern void kputs(const char *s);
extern void kprintf(const char *fmt, ...);
#include "kernel/lib/string.h"
#include "kernel/hal/timers/timer.h"
#include "kernel/memory/vmm.h"
#include "kernel/memory/pmm.h"
/* Avoid including kernel/sched/sched.h — it conflicts with FreeBSD's sys/proc.h.
 * Declare proc_current() and the minimal proc_t fields we need. */
struct xos_proc;
extern struct xos_proc *proc_current(void);
/* Access pml4_virt field at known offset — proc_t layout from sched.h */
static inline void *proc_current_pml4_virt(void) {
    struct xos_proc *p = proc_current();
    if (!p) return NULL;
    return *(void **)((char *)p + 40);
}

/* FreeBSD compat headers - these override the real ones */
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/sx.h>
#include <sys/condvar.h>
#include <sys/callout.h>
#include <sys/taskqueue.h>
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/eventhandler.h>
#include <sys/ucred.h>
#include <sys/prison.h>
#include <sys/vnet.h>
#include <sys/counter.h>
#include <sys/epoch.h>
#include <sys/smr.h>
#include <sys/cpuset.h>
#include <sys/domainset.h>
#include <sys/bus_dma.h>
#include <sys/kobj.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/sleepqueue.h>
#include <sys/turnstile.h>
#include <sys/witness.h>
#include <sys/sdt.h>
#include <sys/kassert.h>
#include <sys/sysctl.h>

/* vm/uma.h provides inline wrappers that call uma_zalloc_arg etc. */
#include <vm/uma.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <netinet/in.h>
#include <netinet/in_pcb.h>
#include <netinet/tcp_var.h>
#include <netinet/cc/cc.h>
#include <net/route.h>
#include <net/route/nhop.h>
#include <net/route/route_ctl.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>
#include <sys/sigio.h>
/* sys/jail.h conflicts with compat prison struct - prison_check_af is static inline in compat prison.h */
#include <sys/resourcevar.h>
#include <sys/priv.h>
#include <sys/caprights.h>
#include <sys/rmlock.h>
#include <sys/hash.h>

/* Forward declarations for types we need but can't include headers for */
struct selinfo;
struct kaiocb;
struct m_snd_tag;
struct ip_moptions;
struct sockopt;
struct unpcb;
struct inpcb;

/* ------------------------------------------------------------------ */
/* Global variables                                                    */
/* ------------------------------------------------------------------ */

int hz = 1000;
int ticks_val = 0;
int tick = 0;
int bootverbose = 0;
int maxphys = 256 * 1024;
volatile unsigned int cold = 1;
struct timeval boottime;
#undef curthread
#undef curproc
struct thread *curthread = NULL;
struct proc *curproc = NULL;
struct proc proc0;
struct thread thread0;

/* Static resource limits — all set to RLIM_INFINITY so socket buffer
 * reservations etc. never fail due to rlimit checks. */
static struct plimit proc0_plimit;
static void init_proc0_plimit(void) {
    for (int i = 0; i < RLIM_NLIMITS; i++) {
        proc0_plimit.pl_rlimit[i].rlim_cur = RLIM_INFINITY;
        proc0_plimit.pl_rlimit[i].rlim_max = RLIM_INFINITY;
    }
    proc0_plimit.pl_refcnt = 1;
}
/* Static per-CPU data area for FreeBSD PCPU_GET/SET macros.
 * FreeBSD accesses per-CPU variables via %gs segment.
 * pc_prvspace is at offset 0x180 in the pcpu struct.
 * pc_curthread is at offset 0. */
static char pcpu_storage[4096] __aligned(4096);
#define PCPU_CURTHREAD_OFF  0     /* pc_curthread */
#define PCPU_PRVSPACE_OFF   0x180 /* pc_prvspace */
/* curvnet is defined as NULL macro in vnet.h - undef before our variable */
#undef curvnet
void *curvnet = NULL;
cpuset_t all_cpus;
domainset_t _domainset_rr;
domainset_t *DOMAINSET_RR_PTR = &_domainset_rr;
struct prison prison0 = { .pr_id = 0 };
int root_mounted = 0;
struct mtx Giant = { .lock_object = { .lo_name = "Giant" }, .mtx_lock = 0 };

/* Lock class instances */
struct lock_class lock_class_mtx_spin = { "mtx_spin", 0 };
struct lock_class lock_class_mtx_sleep = { "mtx_sleep", 0 };
struct lock_class lock_class_sx = { "sx", 0 };
struct lock_class lock_class_rw = { "rw", 0 };
struct lock_class lock_class_rm = { "rm", 0 };
struct lock_class lock_class_rm_sleepable = { "rm_sleepable", 0 };

/* Taskqueue pointers - defined by TASKQUEUE_DEFINE in subr_taskqueue.c */

/* ------------------------------------------------------------------ */
/* MALLOC_DEFINE - actual malloc type storage                          */
/* ------------------------------------------------------------------ */

MALLOC_DEFINE(M_MBUF, "mbuf", "mbuf");
MALLOC_DEFINE(M_DEVBUF, "devbuf", "device driver memory");
MALLOC_DEFINE(M_TEMP, "temp", "misc temporary data buffers");
/* M_SONAME defined in uipc_socket.c */
MALLOC_DEFINE(M_SOOPTS, "soopts", "socket options");
MALLOC_DEFINE(M_IPMOPTS, "ip_moptions", "ip_moptions");
MALLOC_DEFINE(M_IPADDR, "ip_addr", "ip address");
/* M_IFADDR, M_IFMADDR defined in net/if.c */
/* M_CLONE defined in net/if_clone.c */
/* M_PCB defined in uipc_socket.c */
MALLOC_DEFINE(M_RTENTRY, "rtentry", "routing table entry");
/* M_RTABLE defined in net/rtsock.c */
MALLOC_DEFINE(M_NETADDR, "netaddr", "network address");
/* M_IFNET defined in net/if.c */
MALLOC_DEFINE(M_IOV, "iov", "large iov struct");
/* M_TCPLOG defined in tcp_subr.c */
MALLOC_DEFINE(M_TCPLOGDEV, "tcplogdev", "TCP log device");

int mp_ncpus = 1;

/* ------------------------------------------------------------------ */
/* malloc / free - bridge to X OS kmalloc/kfree                       */
/* ------------------------------------------------------------------ */

extern void *kmalloc(size_t size);
extern void kfree(void *ptr);

void *
mallocarray(size_t nmemb, size_t size, struct malloc_type *type, int flags)
{
    return malloc(nmemb * size, type, flags);
}

void *
malloc(unsigned long size, struct malloc_type *type, int flags)
{
    void *p = kmalloc((size_t)size);
    if (p && (flags & M_ZERO))
        __builtin_memset(p, 0, (size_t)size);
    return p;
}

void
free(void *addr, struct malloc_type *type)
{
    if (addr)
        kfree(addr);
}

void *
realloc(void *addr, unsigned long size, struct malloc_type *type, int flags)
{
    void *p = malloc(size, type, flags);
    if (p && addr)
        __builtin_memmove(p, addr, (size_t)size);
    if (addr)
        free(addr, type);
    return p;
}

void *
reallocf(void *addr, unsigned long size, struct malloc_type *type, int flags)
{
    void *p = realloc(addr, size, type, flags);
    if (!p && addr)
        free(addr, type);
    return p;
}

/* ------------------------------------------------------------------ */
/* printf family - bridge to X OS kprintf/kputs                       */
/* ------------------------------------------------------------------ */

int
printf(const char *fmt, ...)
{
    char buf[256];
    __va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    kputs(buf);
    return 0;
}

int
snprintf(char *str, size_t size, const char *fmt, ...)
{
    __va_list ap;
    __builtin_va_start(ap, fmt);
    int ret = vsnprintf(str, size, fmt, ap);
    __builtin_va_end(ap);
    return ret;
}

int
sprintf(char *str, const char *fmt, ...)
{
    __va_list ap;
    __builtin_va_start(ap, fmt);
    int ret = vsnprintf(str, (size_t)-1, fmt, ap);
    __builtin_va_end(ap);
    return ret;
}

int uprintf(const char *fmt, ...) { return 0; }
int vprintf(const char *fmt, __va_list ap) { return 0; }
int
vsnprintf(char *str, size_t size, const char *fmt, __va_list ap)
{
    if (size == 0)
        return 0;
    size_t pos = 0;
    char tmp[32];
    for (; *fmt && pos < size - 1; fmt++) {
        if (*fmt != '%') {
            str[pos++] = *fmt;
            continue;
        }
        fmt++;
        bool islong = false;
        bool islonglong = false;
        while (*fmt == 'l') { if (islong) islonglong = true; islong = true; fmt++; }
        switch (*fmt) {
            case 'c': {
                if (pos < size - 1)
                    str[pos++] = (char)__builtin_va_arg(ap, int);
                break;
            }
            case 's': {
                const char *s = __builtin_va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s && pos < size - 1)
                    str[pos++] = *s++;
                break;
            }
            case 'd': case 'i': {
                int64_t v;
                if (islonglong) v = __builtin_va_arg(ap, int64_t);
                else if (islong) v = __builtin_va_arg(ap, long);
                else v = __builtin_va_arg(ap, int);
                if (v < 0) {
                    if (pos < size - 1) str[pos++] = '-';
                    v = -v;
                }
                int ti = 0;
                uint64_t uv = (uint64_t)v;
                if (uv == 0) tmp[ti++] = '0';
                while (uv) { tmp[ti++] = '0' + (uv % 10); uv /= 10; }
                while (ti > 0 && pos < size - 1) str[pos++] = tmp[--ti];
                break;
            }
            case 'u': {
                uint64_t v;
                if (islonglong) v = __builtin_va_arg(ap, uint64_t);
                else if (islong) v = __builtin_va_arg(ap, unsigned long);
                else v = __builtin_va_arg(ap, unsigned);
                int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                while (v) { tmp[ti++] = '0' + (v % 10); v /= 10; }
                while (ti > 0 && pos < size - 1) str[pos++] = tmp[--ti];
                break;
            }
            case 'x': {
                uint64_t v;
                if (islonglong) v = __builtin_va_arg(ap, uint64_t);
                else if (islong) v = __builtin_va_arg(ap, unsigned long);
                else v = __builtin_va_arg(ap, unsigned);
                const char *hex = "0123456789abcdef";
                int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                while (v) { tmp[ti++] = hex[v & 0xf]; v >>= 4; }
                while (ti > 0 && pos < size - 1) str[pos++] = tmp[--ti];
                break;
            }
            case 'X': {
                uint64_t v;
                if (islonglong) v = __builtin_va_arg(ap, uint64_t);
                else if (islong) v = __builtin_va_arg(ap, unsigned long);
                else v = __builtin_va_arg(ap, unsigned);
                const char *hex = "0123456789ABCDEF";
                int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                while (v) { tmp[ti++] = hex[v & 0xf]; v >>= 4; }
                while (ti > 0 && pos < size - 1) str[pos++] = tmp[--ti];
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)(uintptr_t)__builtin_va_arg(ap, void *);
                if (pos < size - 1) str[pos++] = '0';
                if (pos < size - 1) str[pos++] = 'x';
                const char *hex = "0123456789abcdef";
                int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                while (v) { tmp[ti++] = hex[v & 0xf]; v >>= 4; }
                while (ti > 0 && pos < size - 1) str[pos++] = tmp[--ti];
                break;
            }
            case '%': {
                if (pos < size - 1) str[pos++] = '%';
                break;
            }
            default: {
                if (pos < size - 1) str[pos++] = '%';
                if (pos < size - 1) str[pos++] = *fmt;
                break;
            }
        }
    }
    str[pos] = '\0';
    return (int)pos;
}
int vsprintf(char *buf, const char *fmt, __va_list ap) { buf[0] = '\0'; return 0; }
int
vsnrprintf(char *str, size_t size, int x, const char *fmt, __va_list ap)
{
    if (size > 0) str[0] = '\0';
    return 0;
}

void
panic(const char *fmt, ...)
{
    kputs("[panic] FreeBSD net: ");
    kputs(fmt);
    kputs("\n");
    __asm__ volatile("cli; hlt");
    for (;;) ;
}

void vpanic(const char *fmt, __va_list ap) { panic(fmt); }
void tprintf(struct proc *p, int pri, const char *fmt, ...) { (void)p; (void)pri; }

/* kvprintf - provided by kernel/lib/kprintf.c */

void log(int level, const char *fmt, ...) { (void)level; }

/* ------------------------------------------------------------------ */
/* DELAY / pause                                                       */
/* ------------------------------------------------------------------ */

void
DELAY(int usec)
{
    uint64_t target = timer_ticks() + (usec / 1000);
    while ((int64_t)(target - timer_ticks()) > 0)
        __asm__ volatile("pause");
}

void
pause_sbt(const char *wmesg, int sbt, int pr, int flags)
{
    (void)wmesg; (void)sbt; (void)pr; (void)flags;
    DELAY(1000);
}

/* ------------------------------------------------------------------ */
/* Time functions                                                      */
/* ------------------------------------------------------------------ */

void
getnanotime(struct timespec *tsp)
{
    uint64_t t = timer_ticks();
    tsp->tv_sec = t / hz;
    tsp->tv_nsec = ((t % hz) * 1000000000ULL) / hz;
}

void nanotime(struct timespec *tsp) { getnanotime(tsp); }
void nanouptime(struct timespec *tsp) { getnanotime(tsp); }
void getnanouptime(struct timespec *tsp) { getnanotime(tsp); }

void
getmicrotime(struct timeval *tvp)
{
    uint64_t t = timer_ticks();
    tvp->tv_sec = t / hz;
    tvp->tv_usec = ((t % hz) * 1000000ULL) / hz;
}

void microtime(struct timeval *tvp) { getmicrotime(tvp); }
void getmicrouptime(struct timeval *tvp) { getmicrotime(tvp); }
void microuptime(struct timeval *tvp) { getmicrotime(tvp); }

/* ------------------------------------------------------------------ */
/* ratecheck / ppsratecheck                                            */
/* ------------------------------------------------------------------ */

int
ratecheck(struct timeval *lasttime, const struct timeval *mininterval)
{
    return 1;
}

int
ppsratecheck(struct timeval *lasttime, int *curpps, int maxpps)
{
    if (curpps) *curpps = 0;
    return (maxpps < 0);
}

/* ------------------------------------------------------------------ */
/* getenv stubs                                                        */
/* ------------------------------------------------------------------ */

char *getenv(const char *name) { return NULL; }
char *kern_getenv(const char *name) { return NULL; }
void freeenv(char *env) { }
int getenv_int(const char *name, int *data) { return 0; }
int getenv_uint(const char *name, unsigned int *data) { return 0; }
long getenv_long(const char *name, long *data) { return 0; }
u_long getenv_ulong(const char *name, u_long *data) { return 0; }
quad_t getenv_quad(const char *name, quad_t *data) { return 0; }
const char *getenv_string(const char *name, const char *defval) { return defval; }

/* ------------------------------------------------------------------ */
/* UMA allocator - bridge to malloc                                    */
/* ------------------------------------------------------------------ */

struct uma_zone {
    char uz_name[64];
    size_t uz_size;
    uma_ctor uz_ctor;
    uma_dtor uz_dtor;
    uma_init uz_init;
    uma_fini uz_fini;
    int uz_flags;
};

uma_zone_t
uma_zcreate(const char *name, size_t size,
    uma_ctor ctor, uma_dtor dtor, uma_init uminit, uma_fini fini,
    int align, uint32_t flags)
{
    uma_zone_t zone = (uma_zone_t)kmalloc(sizeof(struct uma_zone));
    if (!zone) return NULL;
    __builtin_memset(zone, 0, sizeof(*zone));
    __builtin_strncpy(zone->uz_name, name, sizeof(zone->uz_name) - 1);
    zone->uz_size = size;
    zone->uz_ctor = ctor;
    zone->uz_dtor = dtor;
    zone->uz_init = uminit;
    zone->uz_fini = fini;
    zone->uz_flags = flags;
    return zone;
}

uma_zone_t
uma_zsecond_create(const char *name, uma_ctor ctor, uma_dtor dtor,
    uma_init uminit, uma_fini fini, uma_zone_t master)
{
    return uma_zcreate(name, master->uz_size, ctor, dtor, uminit, fini,
        0, (uint32_t)master->uz_flags);
}

uma_zone_t
uma_zcache_create(const char *name, int size, uma_ctor ctor, uma_dtor dtor,
    uma_init zinit, uma_fini zfini, uma_import zimport,
    uma_release zrelease, void *arg, int flags)
{
    return uma_zcreate(name, (size_t)size, ctor, dtor, zinit, zfini, 0, (uint32_t)flags);
}

void
uma_zdestroy(uma_zone_t zone)
{
    if (zone) kfree(zone);
}

void *
uma_zalloc_arg(uma_zone_t zone, void *arg, int flags)
{
    void *p = kmalloc(zone->uz_size);
    if (p && (flags & M_ZERO))
        __builtin_memset(p, 0, zone->uz_size);
    if (p && zone->uz_init)
        zone->uz_init(p, (int)zone->uz_size, flags);
    if (p && zone->uz_ctor)
        zone->uz_ctor(p, (int)zone->uz_size, arg, flags);
    return p;
}

void *
uma_zalloc_pcpu_arg(uma_zone_t zone, void *arg, int flags)
{
    return uma_zalloc_arg(zone, arg, flags);
}

void *
uma_zalloc_smr(uma_zone_t zone, int flags)
{
    return uma_zalloc_arg(zone, NULL, flags);
}

void *
uma_zalloc_domain(uma_zone_t zone, void *arg, int domain, int flags)
{
    return uma_zalloc_arg(zone, arg, flags);
}

void
uma_zfree_arg(uma_zone_t zone, void *item, void *arg)
{
    if (item && zone->uz_dtor)
        zone->uz_dtor(item, (int)zone->uz_size, arg);
    if (item)
        kfree(item);
}

void
uma_zfree_pcpu_arg(uma_zone_t zone, void *item, void *arg)
{
    uma_zfree_arg(zone, item, arg);
}

void
uma_zfree_smr(uma_zone_t zone, void *item)
{
    uma_zfree_arg(zone, item, NULL);
}

void uma_zwait(uma_zone_t zone) { }
void uma_reclaim(int req) { }
void uma_reclaim_domain(int req, int domain) { }
void uma_zone_reclaim(uma_zone_t zone, int req) { }
void uma_zone_reclaim_domain(uma_zone_t zone, int req, int domain) { }
void uma_set_cache_align_mask(unsigned int mask) { }
void uma_zone_reserve(uma_zone_t zone, int nitems) { }
int uma_zone_reserve_kva(uma_zone_t zone, int nitems) { return 0; }
int uma_zone_set_max(uma_zone_t zone, int nitems) { return 0; }
void uma_zone_set_maxcache(uma_zone_t zone, int nitems) { }
int uma_zone_get_max(uma_zone_t zone) { return 0; }
void uma_zone_set_warning(uma_zone_t zone, const char *warning) { }
void uma_zone_set_maxaction(uma_zone_t zone, uma_maxaction_t a) { }
int uma_zone_get_cur(uma_zone_t zone) { return 0; }
void uma_zone_set_init(uma_zone_t zone, uma_init uminit) { zone->uz_init = uminit; }
void uma_zone_set_fini(uma_zone_t zone, uma_fini fini) { zone->uz_fini = fini; }
void uma_zone_set_zinit(uma_zone_t zone, uma_init zinit) { }
void uma_zone_set_zfini(uma_zone_t zone, uma_fini zfini) { }
void uma_zone_set_allocf(uma_zone_t zone, uma_alloc allocf) { }
void uma_zone_set_freef(uma_zone_t zone, uma_free freef) { }
void uma_zone_set_smr(uma_zone_t zone, smr_t smr) { }
static struct smr_shared g_smr_shared = { .s_name = "global_smr" };
static struct smr g_smr = { .c_shared = &g_smr_shared };
smr_t uma_zone_get_smr(uma_zone_t zone) { return &g_smr; }
void uma_prealloc(uma_zone_t zone, int itemcnt) { }
int uma_zone_exhausted(uma_zone_t zone) { return 0; }
void uma_reclaim_wakeup(void) { }
void uma_reclaim_worker(void *arg) { }
unsigned long uma_limit(void) { return 0; }
unsigned long uma_size(void) { return 0; }

/* ------------------------------------------------------------------ */
/* ucred - already defined in compat sys/ucred.h                       */
/* ------------------------------------------------------------------ */

struct ucred *
crget(void)
{
    struct ucred *cr = (struct ucred *)kmalloc(sizeof(struct ucred));
    if (cr) __builtin_memset(cr, 0, sizeof(*cr));
    cr->cr_ref = 1;
    return cr;
}

struct ucred *
crhold(struct ucred *cr)
{
    if (cr) cr->cr_ref++;
    return cr;
}

void
crfree(struct ucred *cr)
{
    if (!cr) return;
    if (--cr->cr_ref == 0)
        kfree(cr);
}

int
crcmp(struct ucred *cr1, struct ucred *cr2)
{
    return (cr1 == cr2) ? 0 : 1;
}

struct ucred *
crcop(struct ucred *cr)
{
    struct ucred *newcr = crget();
    if (newcr && cr)
        __builtin_memmove(newcr, cr, sizeof(*newcr));
    newcr->cr_ref = 1;
    return newcr;
}

/* ------------------------------------------------------------------ */
/* critical_enter / critical_exit                                      */
/* ------------------------------------------------------------------ */

void critical_enter(void) {}
void critical_exit(void) {}

/* ------------------------------------------------------------------ */
/* proc stubs                                                          */
/* ------------------------------------------------------------------ */

struct proc *pfind(pid_t pid) { return NULL; }
struct proc *zpfind(pid_t pid) { return NULL; }

/* ------------------------------------------------------------------ */
/* mtx_pool stubs                                                      */
/* ------------------------------------------------------------------ */

struct mtx_pool *
mtx_pool_create(const char *name, int count, const char *type, int flags)
{
    return NULL;
}

void mtx_pool_destroy(struct mtx_pool **poolp) { }
struct mtx *mtx_pool_acquire(struct mtx_pool *pool) { return NULL; }
void mtx_pool_release(struct mtx_pool *pool, struct mtx *m) { }
struct mtx *mtx_pool_find(struct mtx_pool *pool, void *thing) { return NULL; }
void mtx_pool_lock(struct mtx_pool *pool, void *thing) { }
void mtx_pool_unlock(struct mtx_pool *pool, void *thing) { }
void mtx_pool_lock_spin(struct mtx_pool *pool, void *thing) { }
void mtx_pool_unlock_spin(struct mtx_pool *pool, void *thing) { }

/* eventhandler functions - provided by kern/subr_eventhandler.c */

/* ------------------------------------------------------------------ */
/* UNR (unit number allocator) stubs                                   */
/* ------------------------------------------------------------------ */

/* sysctl handler stubs */
int sysctl_handle_int(struct sysctl_oid *oidp, void *arg1, intptr_t arg2, struct sysctl_req *req) { return 0; }
int sysctl_handle_long(struct sysctl_oid *oidp, void *arg1, intptr_t arg2, struct sysctl_req *req) { return 0; }
int sysctl_handle_string(struct sysctl_oid *oidp, void *arg1, intptr_t arg2, struct sysctl_req *req) { return 0; }
int sysctl_handle_opaque(struct sysctl_oid *oidp, void *arg1, intptr_t arg2, struct sysctl_req *req) { return 0; }

struct unrhdr { int low; int high; };
static int unr_counter = 0;

struct unrhdr *
new_unrhdr(int low, int high, void *opaque)
{
    struct unrhdr *uh = (struct unrhdr *)kmalloc(sizeof(struct unrhdr));
    if (uh) { uh->low = low; uh->high = high; }
    return uh;
}

void delete_unrhdr(struct unrhdr *uh) { if (uh) kfree(uh); }

int
alloc_unr(struct unrhdr *uh)
{
    if (unr_counter >= uh->high) return -1;
    return uh->low + unr_counter++;
}

void free_unr(struct unrhdr *uh, int item) { }
int alloc_unr_specific(struct unrhdr *uh, int item) { return item; }

/* ------------------------------------------------------------------ */
/* Network stack initialization — SYSINIT runner                       */
/* ------------------------------------------------------------------ */

/* Linker-set symbols for .sysinit_set section */
extern const struct sysinit_entry __sysinit_start[];
extern const struct sysinit_entry __sysinit_end[];

/* Simple insertion sort by (subsystem, order) — we have <100 entries */
static void
sysinit_sort(struct sysinit_entry *entries, int n)
{
    for (int i = 1; i < n; i++) {
        struct sysinit_entry tmp = entries[i];
        int j = i - 1;
        while (j >= 0 &&
               (entries[j].subsystem > tmp.subsystem ||
                (entries[j].subsystem == tmp.subsystem &&
                 entries[j].order > tmp.order))) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = tmp;
    }
}

/* UMA ctor for mbuf zone — initializes mbuf fields on allocation */
static int
mbuf_ctor(void *mem, int size, void *arg, int flags)
{
    struct mbuf *m = (struct mbuf *)mem;
    struct mb_args *args = (struct mb_args *)arg;

    m->m_next = NULL;
    m->m_nextpkt = NULL;
    m->m_type = args->type;
    m->m_flags = (short)args->flags;
    m->m_len = 0;
    if (args->flags & M_PKTHDR) {
        m->m_data = m->m_pktdat;
        __builtin_memset(&m->m_pkthdr, 0, sizeof(m->m_pkthdr));
    } else {
        m->m_data = m->m_dat;
    }
    return (0);
}

/* UMA init for mbuf zone — zero the whole mbuf before ctor */
static int
mbuf_init(void *mem, int size, int flags)
{
    __builtin_memset(mem, 0, (size_t)size);
    return (0);
}

/* UMA ctor for packet (mbuf+cluster) zone */
static int
pack_ctor(void *mem, int size, void *arg, int flags)
{
    struct mbuf *m = (struct mbuf *)mem;
    struct mb_args *args = (struct mb_args *)arg;

    m->m_next = NULL;
    m->m_nextpkt = NULL;
    m->m_type = args->type;
    m->m_flags = (short)(args->flags | M_EXT);
    m->m_len = 0;
    /* Cluster data follows the mbuf header */
    m->m_ext.ext_buf = (char *)m + MSIZE;
    m->m_ext.ext_size = MCLBYTES;
    m->m_ext.ext_free = NULL;
    m->m_ext.ext_arg1 = NULL;
    m->m_ext.ext_arg2 = NULL;
    m->m_ext.ext_type = EXT_PACKET;
    m->m_ext.ext_flags = EXT_FLAG_EMBREF;
    m->m_ext.ext_count = 1;
    m->m_data = m->m_ext.ext_buf;
    if (args->flags & M_PKTHDR)
        __builtin_memset(&m->m_pkthdr, 0, sizeof(m->m_pkthdr));
    return (0);
}

void
bsd_net_init(void)
{
    kputs("[net] FreeBSD network stack initializing...\n");

    /* Initialize thread0 and proc0 so socket syscalls can access
     * td->td_proc->p_fibnum and td->td_ucred without NULL derefs. */
    init_proc0_plimit();
    __builtin_memset(&proc0, 0, sizeof(proc0));
    __builtin_memset(&thread0, 0, sizeof(thread0));
    thread0.td_proc = &proc0;
    thread0.td_ucred = crget();
    thread0.td_limit = &proc0_plimit;
    proc0.p_fibnum = 0;

    /* Set up per-CPU data area for FreeBSD PCPU_GET/SET macros.
     * FreeBSD code accesses per-CPU variables via __pcpu pointer
     * (overridden in compat/sys/systm.h to not use %gs: segment).
     * This allows X OS to use GS base for cpu_data_t. */
    __builtin_memset(pcpu_storage, 0, sizeof(pcpu_storage));
    /* pc_prvspace (offset 0x180) = pointer to self */
    *(void **)(pcpu_storage + PCPU_PRVSPACE_OFF) = (void *)pcpu_storage;
    /* pc_curthread (offset 0) = thread0 */
    *(void **)(pcpu_storage + PCPU_CURTHREAD_OFF) = (void *)&thread0;
    /* pc_cpuid (offset 60) = 0 */
    *(unsigned int *)(pcpu_storage + 60) = 0;
    /* pc_zpcpu_offset (offset 176) = 0 */
    *(unsigned long *)(pcpu_storage + 176) = 0;
    /* Set __pcpu global — PCPU_GET macros use this pointer */
    __pcpu = (struct pcpu *)pcpu_storage;
    kputs("[net] per-CPU data area initialized (__pcpu pointer set)\n");

    /* Initialize CPU set and cpuhead so CPU_FOREACH iterates CPU 0.
     * Without this, netisr_register skips per-CPU workstream init
     * (nw_qlimit stays 0, dropping all queued packets), and
     * netisr_select_cpuid divides by nws_count==0, crashing. */
    __builtin_memset(&all_cpus, 0, sizeof(all_cpus));
    all_cpus.__bits[0] = 1;  /* CPU 0 present */
    STAILQ_INIT(&cpuhead);
    STAILQ_INSERT_HEAD(&cpuhead, (struct pcpu *)pcpu_storage, pc_allcpu);
    kputs("[net] CPU 0 marked present, cpuhead initialized\n");

    /* Create UMA zones for mbufs before any SYSINIT runs */
    zone_mbuf = uma_zcreate("mbuf", MSIZE,
        mbuf_ctor, NULL, mbuf_init, NULL,
        0, 0);
    zone_clust = uma_zcreate("mbuf_cluster", MCLBYTES,
        NULL, NULL, NULL, NULL, 0, 0);
    zone_pack = uma_zcreate("mbuf_packet", MSIZE + MCLBYTES,
        pack_ctor, NULL, mbuf_init, NULL,
        0, 0);
    pcpu_zone_8 = uma_zcreate("pcpu_8", 8, NULL, NULL, NULL, NULL, 0, 0);

    kputs("[net] UMA zones created (mbuf, cluster, packet)\n");

    /* Collect and sort SYSINIT entries */
    int n = (int)(__sysinit_end - __sysinit_start);
    if (n <= 0) {
        kputs("[net] WARNING: no SYSINIT entries found!\n");
        return;
    }

    /* Copy to a mutable array for sorting */
    struct sysinit_entry *entries =
        (struct sysinit_entry *)kmalloc(sizeof(struct sysinit_entry) * n);
    if (!entries) {
        kputs("[net] PANIC: cannot allocate SYSINIT sort array\n");
        return;
    }
    for (int i = 0; i < n; i++)
        entries[i] = __sysinit_start[i];

    sysinit_sort(entries, n);

    kprintf("[net] Running %d SYSINIT entries...\n", n);

    /* Call each SYSINIT in order */
    for (int i = 0; i < n; i++) {
        if (entries[i].func)
            entries[i].func(entries[i].arg);
    }

    kfree(entries);

    kputs("[net] SYSINIT complete\n");

    /* Mark boot complete — some FreeBSD code checks `cold` */
    cold = 0;

    kputs("[net] FreeBSD network stack initialized\n");
}

/* ------------------------------------------------------------------ */
/* Missing linker stubs                                                */
/* ------------------------------------------------------------------ */

/* atomic_store_rel_int and atomic_cmpset_int are static inline in machine/atomic.h */
/* They are provided in atomic_stubs.c to avoid redefinition conflicts */

/* Counter inline functions */
void counter_u64_zero_inline(counter_u64_t c) { (void)c; }
uint64_t counter_u64_fetch_inline(counter_u64_t c) { (void)c; return 0; }
void COUNTER_ARRAY_ALLOC(counter_u64_t *arr, int n, int flags) { (void)arr; (void)n; (void)flags; }

/* Callout system — simple singly-linked list processed from timer tick.
 * Single-CPU safe: list mutations disable interrupts to prevent racing
 * with the timer IRQ that calls callout_process(). */
static struct callout *callout_list_head = NULL;

void callout_init(struct callout *c, int mpsafe) {
    c->c_lock = NULL;
    c->c_flags = mpsafe ? 0 : 0;
    c->c_iflags = 0;
    c->c_func = NULL;
    c->c_arg = NULL;
    c->c_time = 0;
    c->c_precision = 0;
    c->c_cpu = 0;
    c->c_links.sle.sle_next = NULL;
}

void _callout_init_lock(struct callout *c, struct lock_object *lo, int flags) {
    c->c_lock = lo;
    c->c_flags = (short)flags;
    c->c_iflags = 0;
    c->c_func = NULL;
    c->c_arg = NULL;
    c->c_time = 0;
    c->c_precision = 0;
    c->c_cpu = 0;
    c->c_links.sle.sle_next = NULL;
}

void callout_when(sbintime_t sbt, sbintime_t precision, int flags,
    sbintime_t *sbt_res, sbintime_t *prec_res) {
    if (!(flags & C_ABSOLUTE)) {
        sbintime_t now = (sbintime_t)ticks * tick_sbt;
        sbt += now;
    }
    if (sbt_res) *sbt_res = sbt;
    if (prec_res) *prec_res = precision;
}

int callout_reset_sbt_on(struct callout *c, sbintime_t sbt, sbintime_t pr,
    void (*func)(void *), void *arg, int cpu, int flags) {
    unsigned long save_rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(save_rflags));

    /* If not absolute, convert relative to absolute */
    if (!(flags & C_ABSOLUTE)) {
        sbintime_t now = (sbintime_t)ticks * tick_sbt;
        sbt += now;
    }

    /* Remove from list if already pending */
    if (c->c_iflags & CALLOUT_PENDING) {
        struct callout **pp = &callout_list_head;
        while (*pp && *pp != c) pp = &(*pp)->c_links.sle.sle_next;
        if (*pp) *pp = c->c_links.sle.sle_next;
    }

    c->c_time = sbt;
    c->c_precision = pr;
    c->c_func = func;
    c->c_arg = arg;
    c->c_cpu = cpu;
    c->c_iflags |= (CALLOUT_PENDING | CALLOUT_ACTIVE);

    /* Prepend to list */
    c->c_links.sle.sle_next = callout_list_head;
    callout_list_head = c;

    __asm__ volatile("pushq %0; popfq" : : "r"(save_rflags));
    return 0;
}

int _callout_stop_safe(struct callout *c, int flags) {
    unsigned long save_rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(save_rflags));

    int was_pending = (c->c_iflags & CALLOUT_PENDING) != 0;
    if (was_pending) {
        struct callout **pp = &callout_list_head;
        while (*pp && *pp != c) pp = &(*pp)->c_links.sle.sle_next;
        if (*pp) *pp = c->c_links.sle.sle_next;
        c->c_links.sle.sle_next = NULL;
        c->c_iflags &= ~CALLOUT_PENDING;
    }

    __asm__ volatile("pushq %0; popfq" : : "r"(save_rflags));
    return was_pending;
}

int callout_schedule(struct callout *c, int to_ticks) {
    return callout_reset_sbt_on(c, tick_sbt * to_ticks, 0,
        c->c_func, c->c_arg, c->c_cpu, C_HARDCLOCK);
}

int callout_schedule_on(struct callout *c, int to_ticks, int cpu) {
    return callout_reset_sbt_on(c, tick_sbt * to_ticks, 0,
        c->c_func, c->c_arg, cpu, C_HARDCLOCK);
}

/* Process expired callouts — called from timer tick with current sbintime.
 * Runs in interrupt context (interrupts already disabled). */
void callout_process(sbintime_t now) {
    struct callout **pp = &callout_list_head;
    while (*pp) {
        struct callout *c = *pp;
        if (c->c_time <= now) {
            /* Expired — remove from list */
            *pp = c->c_links.sle.sle_next;
            c->c_links.sle.sle_next = NULL;
            c->c_iflags &= ~CALLOUT_PENDING;
            c->c_iflags |= CALLOUT_ACTIVE;

            /* Acquire callout lock if needed.
             * We're in interrupt context (IF=0), so just grab the
             * spinlock without RFLAGS save. Set lo_data to RFLAGS
             * with IF=0 so the callback's rw_wunlock keeps
             * interrupts disabled. */
            if (c->c_lock) {
                struct rwlock *rw = (struct rwlock *)c->c_lock;
                while (__sync_lock_test_and_set(&rw->rw_lock, 1))
                    __asm__ volatile("pause");
                unsigned long rflags;
                __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
                rflags &= ~0x200UL;
                rw->lock_object.lo_data = (unsigned int)rflags;
            }

            if (c->c_func)
                c->c_func(c->c_arg);

            /* If not RETURNUNLOCKED, release lock here.
             * Don't restore RFLAGS — we're in interrupt context. */
            if (c->c_lock && !(c->c_flags & CALLOUT_RETURNUNLOCKED)) {
                struct rwlock *rw = (struct rwlock *)c->c_lock;
                __sync_lock_release(&rw->rw_lock);
            }

            /* Restart scan — list may have been modified by callback */
            pp = &callout_list_head;
        } else {
            pp = &c->c_links.sle.sle_next;
        }
    }
}

/* Epoch preempt */
void _epoch_enter_preempt(epoch_t epoch, epoch_tracker_t et) { (void)epoch; (void)et; }
void _epoch_exit_preempt(epoch_t epoch, epoch_tracker_t et) { (void)epoch; (void)et; }

/* UMA zones - declared as uma_zone_t in vm/uma.h */
uma_zone_t pcpu_zone_8 = NULL;
uma_zone_t zone_mbuf = NULL;
uma_zone_t zone_clust = NULL;
uma_zone_t zone_pack = NULL;
uma_zone_t zone_jumbop = NULL;
uma_zone_t zone_jumbo9 = NULL;
uma_zone_t zone_jumbo16 = NULL;
uma_zone_t zone_extpgs = NULL;

/* Network functions */
/* m_freem — walk the mbuf chain freeing each one */
void m_freem(struct mbuf *m) {
    struct mbuf *n;
    while (m) {
        n = m->m_next;
        if (m->m_flags & M_PKTHDR && m->m_pkthdr.csum_flags & CSUM_SND_TAG)
            m_snd_tag_rele(m->m_pkthdr.snd_tag);
        if (m->m_flags & M_EXTPG)
            mb_free_extpg(m);
        else if (m->m_flags & M_EXT)
            mb_free_ext(m);
        else if ((m->m_flags & M_NOFREE) == 0)
            uma_zfree(zone_mbuf, m);
        m = n;
    }
}
void m_freemp(struct mbuf *m) { m_freem(m); }
void m_free_raw(struct mbuf *m) { m_freem(m); }

void nhop_free(struct nhop_object *nh) { (void)nh; }
u_int rt_tables_get_gen(uint32_t table, sa_family_t family) { (void)table; (void)family; return 0; }
char *inet_ntoa_r(struct in_addr addr, char *buf) {
    if (buf) {
        unsigned char *a = (unsigned char *)&addr.s_addr;
        snprintf(buf, INET_ADDRSTRLEN, "%d.%d.%d.%d",
            a[0], a[1], a[2], a[3]);
    }
    return buf;
}

/* Sysctl */
int SYSCTL_OUT(struct sysctl_req *req, const void *p, size_t l) { (void)req; (void)p; (void)l; return 0; }

/* Time */
int ticks = 0;
sbintime_t tick_sbt = (sbintime_t)(1ULL << 32) / 1000; /* hz=1000: ~4294967 */
/* time_uptime is declared as volatile time_t in sys/time.h */

/* Audit stubs */
#define AUDIT_ARG_STUB(name) void name() {}
AUDIT_ARG_STUB(AUDIT_ARG_SOCKET)
AUDIT_ARG_STUB(AUDIT_ARG_FD)
AUDIT_ARG_STUB(AUDIT_ARG_SOCKADDR)

/* Capability rights stubs - declared as const cap_rights_t in sys/caprights.h */
const cap_rights_t cap_accept_rights = {};
const cap_rights_t cap_bind_rights = {};
const cap_rights_t cap_listen_rights = {};

/* File descriptor stubs — real implementations above */
/* copyin/copyout — copy between user and kernel virtual addresses.
 * During syscalls, the user's PML4 is still active (kernel higher-half
 * mappings are copied to every user PML4), so user memory is directly
 * accessible via its virtual address. */
int copyin(const void *uaddr, void *kaddr, size_t len) {
    __builtin_memcpy(kaddr, uaddr, len);
    return 0;
}
int copyout(const void *kaddr, void *uaddr, size_t len) {
    __builtin_memcpy(uaddr, kaddr, len);
    return 0;
}
pid_t fgetown(struct sigio **sigiop) { (void)sigiop; return 0; }
void filecaps_free(void *fc) { (void)fc; }
int fsetown(pid_t pgid, struct sigio **sigiop) { (void)pgid; (void)sigiop; return 0; }

/* Knote */
void knote(struct knlist *list, long hint, int lockflags) { (void)list; (void)hint; (void)lockflags; }
void knlist_init(struct knlist *knl, void *lock, void (*kl_lock)(void *),
    void (*kl_unlock)(void *), void (*kl_assert_lock)(void *, int)) {
    (void)knl; (void)lock; (void)kl_lock; (void)kl_unlock; (void)kl_assert_lock;
}

/* Misc */
int min(int a, int b) { return a < b ? a : b; }
int imax(int a, int b) { return a > b ? a : b; }
uint32_t arc4random(void) {
    static uint32_t state = 0x6d5a56a3u;
    state = state * 1103515245u + 12345u;
    return (state >> 16) ^ (state & 0xffff);
}
void wakeup(void *ident) { (void)ident; }
int maxfiles = 1024;
int khelp_destroy_osd(struct osd *hosd) { (void)hosd; return 0; }
int khelp_init_osd(uint32_t classes, struct osd *hosd) { (void)classes; (void)hosd; return 0; }
int chgsbsize(struct uidinfo *uip, u_int *hiwat, u_int to, rlim_t max) { (void)uip; (void)max; *hiwat = to; return 1; }
/* prison_check_af - provided as static inline by compat sys/prison.h */

/* ------------------------------------------------------------------
 * File descriptor table — minimal implementation for socket syscalls
 * ------------------------------------------------------------------ */
#include <sys/file.h>

#define MAX_FDS 256
static struct file *fd_table[MAX_FDS];
static int next_fd = 0;

/* Forward declarations for socket functions in uipc_socket.c */
struct uio;
int soreceive(struct socket *so, struct sockaddr **psa, struct uio *uio,
    struct mbuf **mp0, struct mbuf **controlp, int *flagsp);
int sosend(struct socket *so, struct sockaddr *addr, struct uio *uio,
    struct mbuf *top, struct mbuf *control, int flags, struct thread *td);
int soclose(struct socket *so);
int soshutdown(struct socket *so, enum shutdown_how how);
int sopoll_generic(struct socket *so, int events, struct thread *td);

/* soo_* file operation wrappers */
static int
soo_read(struct file *fp, struct uio *uio, struct ucred *cred, int flags, struct thread *td)
{
    struct socket *so = (struct socket *)fp->f_data;
    int error;
    error = soreceive(so, NULL, uio, NULL, NULL, NULL);
    return (error);
}

static int
soo_write(struct file *fp, struct uio *uio, struct ucred *cred, int flags, struct thread *td)
{
    struct socket *so = (struct socket *)fp->f_data;
    int error;
    error = sosend(so, NULL, uio, NULL, NULL, 0, td);
    return (error);
}

static int
soo_ioctl(struct file *fp, u_long cmd, void *data, struct ucred *cred, struct thread *td)
{
    struct socket *so = (struct socket *)fp->f_data;
    (void)so; (void)cmd; (void)data; (void)cred; (void)td;
    return (0);
}

static int
soo_poll(struct file *fp, int events, struct ucred *cred, struct thread *td)
{
    struct socket *so = (struct socket *)fp->f_data;
    (void)cred;
    return (sopoll_generic(so, events, td));
}

static int
soo_close(struct file *fp, struct thread *td)
{
    struct socket *so = (struct socket *)fp->f_data;
    int error;
    if (so)
        error = soclose(so);
    else
        error = 0;
    fp->f_data = NULL;
    return (error);
}

static int
soo_stat(struct file *fp, struct stat *sb, struct ucred *cred)
{
    (void)fp; (void)sb; (void)cred;
    return (0);
}

static int
soo_kqfilter(struct file *fp, struct knote *kn)
{
    (void)fp; (void)kn;
    return (0);
}

static int
soo_fill_kinfo(struct file *fp, struct kinfo_file *kif, struct filedesc *fdp)
{
    (void)fp; (void)kif; (void)fdp;
    return (0);
}

const struct fileops socketops = {
    .fo_read = soo_read,
    .fo_write = soo_write,
    .fo_truncate = NULL,
    .fo_ioctl = soo_ioctl,
    .fo_poll = soo_poll,
    .fo_kqfilter = soo_kqfilter,
    .fo_stat = soo_stat,
    .fo_close = soo_close,
    .fo_fdclose = NULL,
    .fo_chmod = NULL,
    .fo_chown = NULL,
    .fo_sendfile = NULL,
    .fo_seek = NULL,
    .fo_fill_kinfo = soo_fill_kinfo,
    .fo_mmap = NULL,
    .fo_aio_queue = NULL,
    .fo_add_seals = NULL,
    .fo_get_seals = NULL,
    .fo_fallocate = NULL,
    .fo_fspacectl = NULL,
    .fo_cmp = NULL,
    .fo_fork = NULL,
    .fo_flags = 0,
};

/* Accessor for X OS select/poll — call fo_poll on a file without needing
 * the full struct file definition in kernel/bsd/syscalls.c. */
int xos_fop_poll(struct file *fp, int events) {
    if (!fp || !fp->f_ops || !fp->f_ops->fo_poll) return 0;
    struct thread td;
    memset(&td, 0, sizeof(td));
    return fp->f_ops->fo_poll(fp, events, NULL, &td);
}

void xos_fdrop(struct file *fp) {
    if (fp)
        __atomic_sub_fetch(&fp->f_count, 1, __ATOMIC_RELAXED);
}

/* falloc — allocate a file descriptor and struct file */
int
falloc_caps(struct thread *td, void **fpp, int *fdp, int flags, const void *fcapp)
{
    (void)fcapp;
    int fd;
    for (fd = next_fd; fd < MAX_FDS; fd++) {
        if (fd_table[fd] == NULL)
            break;
    }
    if (fd >= MAX_FDS)
        return (EMFILE);
    struct file *fp = (struct file *)kmalloc(sizeof(struct file));
    if (!fp)
        return (ENOMEM);
    __builtin_memset(fp, 0, sizeof(*fp));
    fp->f_count = 2;  /* 1 for fd table, 1 extra for caller (fdrop'd by kern_socket) */
    fd_table[fd] = fp;
    next_fd = fd + 1;
    *fdp = fd;
    *(struct file **)fpp = fp;
    (void)flags; (void)td;
    return (0);
}

/* fget_unlocked — look up fd, return struct file with ref */
int
fget_unlocked(struct thread *td, int fd, const cap_rights_t *rightsp, struct file **fpp)
{
    (void)td; (void)rightsp;
    if (fd < 0 || fd >= MAX_FDS) {
        *fpp = NULL;
        return (EBADF);
    }
    struct file *fp = fd_table[fd];
    if (fp == NULL) {
        *fpp = NULL;
        return (EBADF);
    }
    __atomic_add_fetch(&fp->f_count, 1, __ATOMIC_RELAXED);
    *fpp = fp;
    return (0);
}

/* fget — same as fget_unlocked but older API */
int
fget(struct thread *td, int fd, const cap_rights_t *rightsp, struct file **fpp)
{
    return (fget_unlocked(td, fd, rightsp, fpp));
}

/* fget_cap — same with caps and filecaps */
int
fget_cap(struct thread *td, int fd, const cap_rights_t *rightsp,
    uint8_t *flagsp, struct file **fpp, struct filecaps *havecapsp)
{
    (void)flagsp; (void)havecapsp;
    return (fget_unlocked(td, fd, rightsp, fpp));
}

/* _fdrop — called by fdrop macro when refcount reaches zero */
int
_fdrop(struct file *fp, struct thread *td)
{
    if (!fp)
        return (0);
    if (fp->f_ops && fp->f_ops->fo_close)
        fp->f_ops->fo_close(fp, td);
    /* Remove from fd table if present */
    for (int i = 0; i < MAX_FDS; i++) {
        if (fd_table[i] == fp) {
            fd_table[i] = NULL;
            break;
        }
    }
    kfree(fp);
    return (0);
}

/* fdclose — remove fd from table (no refcount change) */
void
fdclose(struct thread *td, int fd)
{
    (void)td;
    if (fd >= 0 && fd < MAX_FDS)
        fd_table[fd] = NULL;
}

/* finit — initialize file structure */
void
finit(struct file *fp, u_int flags, short type, void *vp, const struct fileops *ops)
{
    fp->f_flag = flags;
    fp->f_type = type;
    fp->f_data = vp;
    fp->f_ops = ops;
}

/* ------------------------------------------------------------------ */
/* Additional missing linker stubs                                     */
/* ------------------------------------------------------------------ */

/* atomic_store_rel_int and atomic_cmpset_int are static inline in machine/atomic.h */

/* Time */
volatile time_t time_uptime = 0;

/* Synchronization — real sleep that yields CPU so timer IRQ can fire.
 * The timer tick calls vioif_rx_poll() and callout_process() which are
 * essential for TCP handshake completion (SYN-ACK processing) and TCP
 * timers (retransmit, keepalive, etc). Without yielding, msleep returns
 * immediately and kern_connectat busy-loops forever on SS_ISCONNECTING. */
int msleep_sbt(const void *chan, struct mtx *mtx, int pri, const char *wmesg,
    sbintime_t bt, sbintime_t pr, int flags) {
    (void)chan; (void)pri; (void)wmesg; (void)bt; (void)pr; (void)flags;
    if (mtx) {
        __sync_lock_release(&mtx->mtx_lock);
        __asm__ volatile("pushq %0; popfq" : : "r"((unsigned long)mtx->lock_object.lo_data));
    }
    __asm__ volatile("sti; hlt");
    if (mtx) {
        unsigned long save;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(save));
        while (__sync_lock_test_and_set(&mtx->mtx_lock, 1))
            __asm__ volatile("pause");
        mtx->lock_object.lo_data = (unsigned int)save;
    }
    return 0;
}
int mtx_sleep(const void *chan, struct mtx *mtx, int pri, const char *wmesg, int timo) {
    (void)chan; (void)pri; (void)wmesg; (void)timo;
    if (mtx) {
        __sync_lock_release(&mtx->mtx_lock);
        __asm__ volatile("pushq %0; popfq" : : "r"((unsigned long)mtx->lock_object.lo_data));
    }
    __asm__ volatile("sti; hlt");
    if (mtx) {
        unsigned long save;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(save));
        while (__sync_lock_test_and_set(&mtx->mtx_lock, 1))
            __asm__ volatile("pause");
        mtx->lock_object.lo_data = (unsigned int)save;
    }
    return 0;
}
int tsleep(const void *chan, int pri, const char *wmesg, int timo) {
    (void)chan; (void)pri; (void)wmesg; (void)timo;
    unsigned long save;
    __asm__ volatile("pushfq; popq %0; sti; hlt" : "=r"(save));
    __asm__ volatile("pushq %0; popfq" : : "r"(save));
    return 0;
}

/* rwlock functions not in compat header */
int rw_try_upgrade(struct rwlock *rw) { (void)rw; return 0; }
void rw_unlock(struct rwlock *rw) { (void)rw; }

/* Privilege checks */
int priv_check(struct thread *td, int priv) { (void)td; (void)priv; return 0; }
int priv_check_cred(struct ucred *cred, int priv) { (void)cred; (void)priv; return 0; }

/* Jail functions - already provided as static inline by compat sys/prison.h */

/* Socket AIO */
int soaio_queue_generic(struct socket *so, struct kaiocb *job) { (void)so; (void)job; return 0; }
void sowakeup_aio(struct socket *so, sb_which which) { (void)so; (void)which; }

/* Select — real implementation using X OS scheduler block/wake.
 *
 * selrecord stores the current select wait channel in the selinfo.
 * selwakeup wakes any process blocked on that channel.
 * The wait channel is a per-syscall stack address set by sys_poll/sys_select. */

static void *g_select_chan = NULL;  /* current select wait channel */

/* Set the current select wait channel (called by sys_poll/sys_select). */
void xos_select_set_chan(void *chan) { g_select_chan = chan; }
void xos_select_clear_chan(void) { g_select_chan = NULL; }

void selrecord(struct thread *td, struct selinfo *sip) {
    (void)td;
    /* Store the current select wait channel in the selinfo's si_mtx field
     * (abusing it since we don't use real mutexes here). */
    if (g_select_chan)
        sip->si_mtx = (struct mtx *)g_select_chan;
}

void selwakeuppri(struct selinfo *sip, int pri) {
    (void)pri;
    if (sip && sip->si_mtx) {
        extern void sched_wake_chan(const void *chan);
        sched_wake_chan((const void *)sip->si_mtx);
        sip->si_mtx = NULL;
    }
}

/* Signal */
void pgsigio(struct sigio **sigiop, int sig, int checkctty) { (void)sigiop; (void)sig; (void)checkctty; }

/* mbuf functions */
void m_snd_tag_destroy(struct m_snd_tag *mst) { (void)mst; }
void mb_free_ext(struct mbuf *m) {
    /* Decrement refcount if embedded */
    if (m->m_ext.ext_flags & EXT_FLAG_EMBREF) {
        if (__atomic_sub_fetch(&m->m_ext.ext_count, 1, __ATOMIC_ACQ_REL) > 0)
            return; /* still referenced */
    } else if (m->m_ext.ext_cnt) {
        if (__atomic_sub_fetch(m->m_ext.ext_cnt, 1, __ATOMIC_ACQ_REL) > 0)
            return; /* still referenced */
    }
    /* Call custom free if present */
    if (m->m_ext.ext_free) {
        m->m_ext.ext_free(m);
        return;
    }
    /* Free based on type */
    switch (m->m_ext.ext_type) {
    case EXT_PACKET:
        /* mbuf+cluster from zone_pack — one allocation */
        uma_zfree(zone_pack, m);
        break;
    case EXT_CLUSTER:
        /* Separate cluster + mbuf */
        if (m->m_ext.ext_buf)
            uma_zfree(zone_clust, m->m_ext.ext_buf);
        uma_zfree(zone_mbuf, m);
        break;
    case EXT_JUMBOP:
        if (m->m_ext.ext_buf)
            uma_zfree(zone_jumbop, m->m_ext.ext_buf);
        uma_zfree(zone_mbuf, m);
        break;
    default:
        /* Unknown ext type — just free the mbuf */
        uma_zfree(zone_mbuf, m);
        break;
    }
}
void mb_free_extpg(struct mbuf *m) { (void)m; }
int mb_unmapped_to_ext(struct mbuf *m, struct mbuf **mres) { if (mres) *mres = m; return 0; }
int mb_unmapped_compress(struct mbuf *m) { (void)m; return 0; }

/* File descriptor — fdrop/fdclose/finit implemented above */
int kern_close(struct thread *td, int fd) {
    struct file *fp;
    int error = fget_unlocked(td, fd, &cap_no_rights, &fp);
    if (error)
        return (error);
    fdclose(td, fd);
    /* fdrop will decrement the ref from fget_unlocked */
    fdrop(fp, td);
    return (0);
}

/* Capability rights */
const cap_rights_t cap_connect_rights = {};
const cap_rights_t cap_send_connect_rights = {};

/* UNIX domain socket */
void unp_copy_peercred(struct thread *td, void *client_unp, void *server_unp) { (void)td; (void)client_unp; (void)server_unp; }

/* FIB / routing — minimal implementation for single-interface virtio-net.
 * Returns a static nhop_object pointing to the virtio interface with the
 * gateway as the next-hop for non-local destinations. */

/* Forward declarations from if_virtio.c */
struct ifnet *vioif_get_ifp(void);
uint32_t vioif_get_gateway(void);

/* Static nhop_object for the default route via virtio-net gateway. */
static struct nhop_object g_default_nh;	/* gateway route for non-local */
static struct nhop_object g_local_nh;		/* direct route for local subnet */
static int g_nh_init = 0;

#define VIOIF_NETMASK 0x0a000200u	/* 10.0.2.0 in host order */
#define VIOIF_IP_ADDR 0x0a00020fu	/* 10.0.2.15 in host order */

static void
ensure_default_nh(void)
{
	if (g_nh_init)
		return;
	struct ifnet *ifp = vioif_get_ifp();
	if (ifp == NULL)
		return;

	/* Common setup for both nhops */
	struct ifaddr *ifa = NULL;
	CK_STAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
		if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET)
			break;
	}

	/* Gateway route (for non-local destinations) */
	memset(&g_default_nh, 0, sizeof(g_default_nh));
	g_default_nh.nh_flags = NHF_GATEWAY;
	g_default_nh.nh_mtu = ifp->if_mtu;
	g_default_nh.nh_ifp = ifp;
	g_default_nh.nh_aifp = ifp;
	g_default_nh.nh_ifa = ifa;
	g_default_nh.gw4_sa.sin_len = sizeof(struct sockaddr_in);
	g_default_nh.gw4_sa.sin_family = AF_INET;
	g_default_nh.gw4_sa.sin_addr.s_addr = vioif_get_gateway();
	static uint64_t s_pksent_gw = 0;
	g_default_nh.nh_pksent = &s_pksent_gw;

	/* Direct route (for local subnet destinations — no gateway, ARP directly) */
	memset(&g_local_nh, 0, sizeof(g_local_nh));
	g_local_nh.nh_mtu = ifp->if_mtu;
	g_local_nh.nh_ifp = ifp;
	g_local_nh.nh_aifp = ifp;
	g_local_nh.nh_ifa = ifa;
	static uint64_t s_pksent_local = 0;
	g_local_nh.nh_pksent = &s_pksent_local;

	g_nh_init = 1;
}

struct nhop_object *
fib4_lookup(uint32_t fibnum, struct in_addr dst, uint32_t scopeid,
    uint32_t flags, uint32_t flowid)
{
	(void)fibnum; (void)scopeid; (void)flags; (void)flowid;
	ensure_default_nh();
	if (!g_nh_init)
		return (NULL);
	/* If destination is on local subnet (10.0.2.0/24), return direct route.
	 * Otherwise return gateway route. */
	if ((ntohl(dst.s_addr) & 0xFFFFFF00) == VIOIF_NETMASK)
		return (&g_local_nh);
	return (&g_default_nh);
}
uint32_t fib4_calc_software_hash(struct in_addr src, struct in_addr dst, unsigned short src_port, unsigned short dst_port, char proto, uint32_t *flowid) { (void)src; (void)dst; (void)src_port; (void)dst_port; (void)proto; if (flowid) *flowid = 0; return 0; }

/* VNET variables */
VNET_DEFINE(u_int, fib_hash_outbound) = 0;
VNET_DEFINE(u_int, rt_add_addr_allfibs) = 1;

/* Multicast */
int imo_multi_filter(const void *imo, const void *ifp, const void *ip, const void *group) { (void)imo; (void)ifp; (void)ip; (void)group; return 0; }
int in_mcast_loop(struct ip_moptions *imo) { (void)imo; return 0; }
int inp_getmoptions(struct inpcb *inp, void *sopt) { (void)inp; (void)sopt; return 0; }
int inp_setmoptions(struct inpcb *inp, void *sopt) { (void)inp; (void)sopt; return 0; }

/* Hash */
uint32_t jenkins_hash32(const uint32_t *k, size_t len, uint32_t initval) { (void)k; (void)len; return initval; }
void *hashalloc(struct hashalloc_args *args) {
	struct hashalloc_args *ha = args;
	size_t nelem = ha->size;
	/* For power-of-2 type, round up to next power of 2 */
	if (ha->type == HASH_TYPE_POWER2) {
		size_t p = 1;
		while (p < nelem) p <<= 1;
		nelem = p;
	}
	/* Each bucket is a list head — pointer-sized (8 bytes on 64-bit) */
	size_t bucketsize = ha->hdrsize ? ha->hdrsize : sizeof(void *);
	size_t totalsize = nelem * bucketsize;
	void *tbl = malloc(totalsize, ha->mtype, ha->mflags | M_ZERO);
	if (tbl == NULL) {
		ha->error = ENOMEM;
		return NULL;
	}
	ha->size = nelem;
	ha->error = 0;
	return tbl;
}
void hashfree(void *p, struct hashalloc_args *args) {
	struct hashalloc_args *ha = args;
	if (p) free(p, ha->mtype);
}

/* Random */
void arc4rand(void *ptr, u_int len, int reseed) {
    (void)reseed;
    if (ptr) {
        uint8_t *p = (uint8_t *)ptr;
        for (u_int i = 0; i < len; i++)
            p[i] = (uint8_t)(arc4random() >> 8);
    }
}
uint32_t arc4random_uniform(uint32_t upper_bound) {
    if (upper_bound == 0) return 0;
    return arc4random() % upper_bound;
}

/* SMP */
u_int mp_maxid = 0;

/* RM lock functions */

/* Routing */
VNET_DEFINE(uint32_t, _rt_numfibs) = 1;
void rib_foreach_table_walk_del(int family, rib_filter_f_t *filter_f, void *arg) { (void)family; (void)filter_f; (void)arg; }
void nhop_set_pxtype_flag(struct nhop_object *nh, int nh_flag) { (void)nh; (void)nh_flag; }
void nhop_set_broadcast(struct nhop_object *nh, bool is_broadcast) { (void)nh; (void)is_broadcast; }
void nhop_set_type(struct nhop_object *nh, enum nhop_type nh_type) { (void)nh; (void)nh_type; }
enum nhop_type nhop_get_type(const struct nhop_object *nh) { (void)nh; return 0; }
int nhop_get_rtflags(const struct nhop_object *nh) { (void)nh; return 0; }

/* Multicast */
int in_joingroup(struct ifnet *ifp, const struct in_addr *gina, void *imf, void **pinm) { (void)ifp; (void)gina; (void)imf; if (pinm) *pinm = NULL; return 0; }
int in_leavegroup(void *inm, void *imf) { (void)inm; (void)imf; return 0; }

/* Interface */
int ifa_add_loopback_route(struct ifaddr *ifa, struct sockaddr *sa) { (void)ifa; (void)sa; return 0; }

/* Jail - implemented here because compat prison.h no longer has static inline */
int prison_check_ip4(const struct ucred *cr, const struct in_addr *ia) { (void)cr; (void)ia; return 0; }
int prison_local_ip4(const struct ucred *pr, struct in_addr *ia, int saddr) { (void)pr; (void)ia; (void)saddr; return 0; }
int prison_remote_ip4(struct ucred *cred, struct in_addr *ia) { (void)cred; (void)ia; return 0; }
int prison_check_af(struct ucred *cred, int af) { (void)cred; (void)af; return 0; }
bool jailed_without_vnet(struct ucred *cred) { (void)cred; return false; }

/* Stats */
int stats_v1_voi_update(void *sb, int32_t voi_id, int32_t type, const void *voi, uint32_t voi_dsz) { (void)sb; (void)voi_id; (void)type; (void)voi; (void)voi_dsz; return 0; }

/* flsll - find last set bit in long long */
int flsll(long long mask) {
    if (mask == 0) return 0;
    return 64 - __builtin_clzll(mask);
}

/* More missing stubs */
void epoch_call(epoch_t epoch, epoch_callback_t cb, epoch_context_t ctx) { (void)epoch; (void)cb; (void)ctx; }
int prison_if(struct ucred *cred, const struct sockaddr *sa) { (void)cred; (void)sa; return 0; }
int nmbclusters = 2048;

/* Interface routing */
int ifa_del_loopback_route(struct ifaddr *ifa, struct sockaddr *sa) { (void)ifa; (void)sa; return 0; }
int ifa_switch_loopback_route(struct ifaddr *ifa, struct sockaddr *sa) { (void)ifa; (void)sa; return 0; }
int rib_handle_ifaddr_info(uint32_t fibnum, int cmd, struct rt_addrinfo *info) { (void)fibnum; (void)cmd; (void)info; return 0; }

/* IGMP */
void igmp_domifattach(struct ifnet *ifp) { (void)ifp; }
void igmp_domifdetach(struct ifnet *ifp) { (void)ifp; }
void igmp_ifdetach(struct ifnet *ifp) { (void)ifp; }

/* Multicast */
struct mtx in_multi_list_mtx;
struct sx in_multi_sx;
void inm_disconnect(void *inm) { (void)inm; }
void inm_release_list_deferred(void *head) { (void)head; }
void inm_release_wait(void *arg) { (void)arg; }

/* Stats */
int stats_v1_tpl_alloc(const char *name, uint32_t flags) { (void)name; (void)flags; return 0; }
int stats_vss_hlpr_init(int voi_dtype, uint32_t nvss, void *vss) { (void)voi_dtype; (void)nvss; (void)vss; return 0; }

/* max */
int max(int a, int b) { return a > b ? a : b; }

/* More stubs */
int imin(int a, int b) { return a < b ? a : b; }
size_t strlcpy(char *dst, const char *src, size_t siz) {
    if (siz == 0) return 0;
    size_t i = 0;
    for (; i < siz - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
    return i;
}
void binuptime(struct bintime *bt) { if (bt) __builtin_memset(bt, 0, sizeof(*bt)); }
void getbinuptime(struct bintime *bt) { if (bt) __builtin_memset(bt, 0, sizeof(*bt)); }
int in_epoch(epoch_t epoch) { (void)epoch; return 0; }
int cr_cansee(struct ucred *u1, struct ucred *u2) { (void)u1; (void)u2; return 0; }
int vm_ndomains = 1;

/* Hhook */
int hhook_head_register(int32_t hhook_type, int32_t hhook_id, void *hhk, int flags) {
    (void)hhook_type; (void)hhook_id; (void)hhk; (void)flags; return 0;
}

/* IGMP */
int igmp_input(struct mbuf **mp, int *offp, int IPPROTO) { (void)mp; (void)offp; (void)IPPROTO; return 0; }

/* CC */
struct rwlock cc_list_lock;

/* UMA */
unsigned int uma_get_cache_align_mask(void) { return 15; }

/* mbuf */
void mb_free_notready(struct mbuf *m, int count) { (void)m; (void)count; }

/* TCP log */
int tcp_log_dev_add_log(void *entry) { (void)entry; return 0; }

/* Encap */
int encap4_input(struct mbuf **mp, int *offp, int proto) { (void)mp; (void)offp; (void)proto; return 0; }

/* RSS */
uint32_t rss_proto_software_hash_v4(struct in_addr src, struct in_addr dst,
    uint16_t src_port, uint16_t dst_port, int proto, uint32_t *hashval) {
    (void)src; (void)dst; (void)src_port; (void)dst_port; (void)proto;
    if (hashval) *hashval = 0; return 0;
}

/* SipHash */
void SipHash_InitX(void *ctx, int cRounds, int dRounds) { (void)ctx; (void)cRounds; (void)dRounds; }
void SipHash_SetKey(void *ctx, const uint8_t *key) { (void)ctx; (void)key; }
void SipHash_Update(void *ctx, const void *src, size_t len) { (void)ctx; (void)src; (void)len; }
void SipHash_Final(uint8_t *digest, void *ctx) { (void)ctx; if (digest) __builtin_memset(digest, 0, 8); }

/* Stats */
struct statsblobv1 *stats_v1_blob_alloc(uint32_t tpl_id, uint32_t flags) { (void)tpl_id; (void)flags; return NULL; }
int stats_v1_tpl_add_voistats(uint32_t tpl_id, int32_t voi_id, const char *voi_name,
    int voi_dtype, uint32_t nvss, void *vss, uint32_t flags) {
    (void)tpl_id; (void)voi_id; (void)voi_name; (void)voi_dtype; (void)nvss; (void)vss; (void)flags; return 0;
}
void stats_vss_hlpr_cleanup(uint32_t nvss, void *vss) { (void)nvss; (void)vss; }
int stats_vss_numeric_hlpr(int voi_dtype, void *vss, void *sb, void *voi) {
    (void)voi_dtype; (void)vss; (void)sb; (void)voi; return 0;
}
int stats_vss_tdgst_hlpr(int voi_dtype, void *vss, void *sb, void *voi) {
    (void)voi_dtype; (void)vss; (void)sb; (void)voi; return 0;
}

/* More stubs - round 3 */
void bintime(struct bintime *bt) { if (bt) __builtin_memset(bt, 0, sizeof(*bt)); }
void getboottime(struct timeval *boottime) { if (boottime) __builtin_memset(boottime, 0, sizeof(*boottime)); }
void getboottimebin(struct bintime *boottimebin) { if (boottimebin) __builtin_memset(boottimebin, 0, sizeof(*boottimebin)); }
struct mbuf *ip_tryforward(struct mbuf *m) { (void)m; return NULL; }

/* rwlock try */
int rw_try_rlock(struct rwlock *rw) { (void)rw; return 1; }
int rw_try_wlock(struct rwlock *rw) { (void)rw; return 1; }

/* SMR */
smr_seq_t smr_advance(smr_t smr) { (void)smr; return 0; }
bool smr_poll(smr_t smr, smr_seq_t goal, bool wait) { (void)smr; (void)goal; (void)wait; return true; }

/* Prison */
bool prison_flag(struct ucred *cr, unsigned flag) { (void)cr; (void)flag; return false; }
bool prison_equal_ip4(struct prison *pr1, struct prison *pr2) { (void)pr1; (void)pr2; return true; }
int prison_get_ip4(struct ucred *cred, struct in_addr *ia) { (void)cred; if (ia) ia->s_addr = 0; return 0; }
int prison_check_ip4_locked(const struct prison *pr, const struct in_addr *ia) { (void)pr; (void)ia; return 0; }
bool prison_saddrsel_ip4(struct ucred *cred, struct in_addr *ia) { (void)cred; (void)ia; return true; }

/* RM lock */
void rm_sysinit(const void *arg) { (void)arg; }
void rm_init_flags(struct rmlock *rm, const char *name, int opts) { (void)rm; (void)name; (void)opts; }

/* CC — minimal NewReno congestion control algorithm.
 * The FreeBSD TCP stack requires a registered CC algorithm; without one,
 * tcp_newtcpcb dereferences NULL when setting up a new TCP connection. */

static int newreno_cb_init(struct cc_var *ccv, void *ptr) {
    (void)ccv; (void)ptr; return 0;
}
static void newreno_cb_destroy(struct cc_var *ccv) { (void)ccv; }
static void newreno_conn_init(struct cc_var *ccv) {
    struct tcpcb *tp = ccv->tp;
    uint32_t mss = tp->t_maxseg;
    tp->snd_cwnd = mss * 10; /* RFC 6928 initial window */
}
static void newreno_ack_received(struct cc_var *ccv, ccsignal_t type) {
    struct tcpcb *tp = ccv->tp;
    uint32_t mss = tp->t_maxseg;
    if (type == CC_ACK) {
        if (tp->snd_cwnd < tp->snd_ssthresh) {
            /* Slow start */
            tp->snd_cwnd += min(ccv->bytes_this_ack, mss);
        } else {
            /* Congestion avoidance */
            uint32_t incr = mss * mss;
            if (incr < tp->snd_cwnd)
                incr = (uint32_t)((uint64_t)incr / tp->snd_cwnd);
            else
                incr = 1;
            tp->snd_cwnd += incr;
        }
    }
}
static void newreno_cong_signal(struct cc_var *ccv, ccsignal_t type) {
    struct tcpcb *tp = ccv->tp;
    uint32_t mss = tp->t_maxseg;
    uint32_t cwnd = tp->snd_cwnd;
    switch (type) {
    case CC_RTO:
        tp->snd_ssthresh = max(cwnd / 2, 2 * mss);
        tp->snd_cwnd = mss;
        break;
    case CC_NDUPACK:
        tp->snd_ssthresh = max(cwnd / 2, 2 * mss);
        tp->snd_cwnd = tp->snd_ssthresh + 3 * mss;
        break;
    case CC_ECN:
        tp->snd_ssthresh = max(cwnd / 2, 2 * mss);
        tp->snd_cwnd = tp->snd_ssthresh;
        break;
    default:
        break;
    }
}
static void newreno_post_recovery(struct cc_var *ccv) {
    struct tcpcb *tp = ccv->tp;
    tp->snd_cwnd = tp->snd_ssthresh;
}
static void newreno_after_idle(struct cc_var *ccv) {
    struct tcpcb *tp = ccv->tp;
    uint32_t mss = tp->t_maxseg;
    tp->snd_ssthresh = max(tp->snd_cwnd / 2, 2 * mss);
    tp->snd_cwnd = mss * 10;
}

static struct cc_algo newreno_cc_algo = {
    .name = "newreno",
    .cb_init = newreno_cb_init,
    .cb_destroy = newreno_cb_destroy,
    .conn_init = newreno_conn_init,
    .ack_received = newreno_ack_received,
    .cong_signal = newreno_cong_signal,
    .post_recovery = newreno_post_recovery,
    .after_idle = newreno_after_idle,
};

VNET_DEFINE(struct cc_algo *, default_cc_ptr) = &newreno_cc_algo;
void cc_refer(struct cc_algo *algo) { (void)algo; }
void cc_detach(struct tcpcb *tp) { (void)tp; }

/* Cred */
int cr_bsd_visible(struct ucred *u1, struct ucred *u2) { (void)u1; (void)u2; return 0; }
bool cr_xids_subset(struct ucred *active, struct ucred *obj) { (void)active; (void)obj; return true; }

/* Stats hist */
int stats_vss_hist_hlpr(int voi_dtype, void *vss, void *sb, void *voi) {
    (void)voi_dtype; (void)vss; (void)sb; (void)voi; return 0;
}

/* WITNESS_WARN and KASSERT are macros, __assert_unreachable is a macro */
/* __BIT_ISSET and __pcpu are macros - should not be undefined symbols */

/* Sleep — _sleep is called by msleep() macro, used extensively in TCP connect,
 * accept, and receive paths. Must yield CPU so timer IRQ can process packets. */
int _sleep(const void *chan, struct lock_object *lock, int pri, const char *wmesg, sbintime_t sbt, sbintime_t pr, int flags) {
    (void)chan; (void)pri; (void)wmesg; (void)sbt; (void)pr; (void)flags;
    if (lock) {
        volatile uintptr_t *sl = (volatile uintptr_t *)((char *)lock + sizeof(struct lock_object));
        __sync_lock_release(sl);
        __asm__ volatile("pushq %0; popfq" : : "r"((unsigned long)lock->lo_data));
    }
    __asm__ volatile("sti; hlt");
    if (lock) {
        volatile uintptr_t *sl = (volatile uintptr_t *)((char *)lock + sizeof(struct lock_object));
        unsigned long save;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(save));
        while (__sync_lock_test_and_set(sl, 1))
            __asm__ volatile("pause");
        lock->lo_data = (unsigned int)save;
    }
    return 0;
}
int msleep_spin_sbt(const void *chan, struct mtx *mtx, const char *wmesg, sbintime_t sbt, sbintime_t pr, int flags) {
    (void)chan; (void)wmesg; (void)sbt; (void)pr; (void)flags;
    if (mtx) {
        __sync_lock_release(&mtx->mtx_lock);
        __asm__ volatile("pushq %0; popfq" : : "r"((unsigned long)mtx->lock_object.lo_data));
    }
    __asm__ volatile("sti; hlt");
    if (mtx) {
        unsigned long save;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(save));
        while (__sync_lock_test_and_set(&mtx->mtx_lock, 1))
            __asm__ volatile("pause");
        mtx->lock_object.lo_data = (unsigned int)save;
    }
    return 0;
}

/* Capability rights */
const cap_rights_t cap_send_rights = {};
const cap_rights_t cap_recv_rights = {};
const cap_rights_t cap_no_rights = {};
const cap_rights_t cap_setsockopt_rights = {};
const cap_rights_t cap_getsockopt_rights = {};
const cap_rights_t cap_getsockname_rights = {};
const cap_rights_t cap_getpeername_rights = {};
const cap_rights_t cap_shutdown_rights = {};

/* File descriptor — fget implemented above */
int copyiniov(const struct iovec *iovp, u_int iovcnt, struct iovec **iov, int flags) { (void)iovp; (void)iovcnt; (void)flags; if (iov) *iov = NULL; return 0; }

/* mbuf */
struct mbuf *m_get2(int len, int how, short type, int flags) {
    struct mbuf *m;
    (void)flags;
    if (len <= MLEN) {
        m = m_gethdr(how, type);
    } else if (len <= MCLBYTES) {
        m = m_gethdr(how, type);
        if (m) {
            m_clget(m, how);
            if ((m->m_flags & M_EXT) == 0) {
                m_free(m);
                return NULL;
            }
        }
    } else {
        /* Large allocation — use m_gethdr with cluster */
        m = m_gethdr(how, type);
        if (m) {
            m_clget(m, how);
            if ((m->m_flags & M_EXT) == 0) {
                m_free(m);
                return NULL;
            }
        }
    }
    return m;
}

/* Socket AIO */
void soaio_rcv(void *context, int pending) { (void)context; (void)pending; }
void soaio_snd(void *context, int pending) { (void)context; (void)pending; }

/* Wakeup */
void wakeup_one(const void *chan) { (void)chan; }

/* String */
size_t strnlen(const char *s, size_t maxlen) {
    size_t i = 0;
    for (; i < maxlen && s[i]; i++);
    return i;
}

/* Sbuf */
struct sbuf *sbuf_new(struct sbuf *s, char *buf, int length, int flags) { (void)s; (void)buf; (void)length; (void)flags; return NULL; }
void sbuf_clear(struct sbuf *s) { (void)s; }
int sbuf_bcat(struct sbuf *s, const void *buf, size_t len) { (void)s; (void)buf; (void)len; return 0; }
int sbuf_cat(struct sbuf *s, const char *str) { (void)s; (void)str; return 0; }
int sbuf_printf(struct sbuf *s, const char *fmt, ...) { (void)s; (void)fmt; return 0; }
int sbuf_finish(struct sbuf *s) { (void)s; return 0; }
char *sbuf_data(struct sbuf *s) { (void)s; return NULL; }
ssize_t sbuf_len(struct sbuf *s) { (void)s; return 0; }

/* BPF */
void bpfattach(struct ifnet *ifp, u_int dlt, u_int hdrlen) { (void)ifp; (void)dlt; (void)hdrlen; }
void bpfdetach(struct ifnet *ifp) { (void)ifp; }
void bpf_mtap(void *arg, struct mbuf *m) { (void)arg; (void)m; }
void bpf_mtap2(void *arg, void *data, u_int len, struct mbuf *m) { (void)arg; (void)data; (void)len; (void)m; }

/* Netlink */
void nl_modify_ifp_generic(struct ifnet *ifp) { (void)ifp; }

/* KVA - minimal struct to satisfy linker, real definition in machine/pmap.h */
struct kva_layout_s { char _dummy; };
struct kva_layout_s kva_layout = { 0 };

/* ------------------------------------------------------------------ */
/* Bulk stubs for remaining undefined symbols                          */
/* ------------------------------------------------------------------ */

/* __pcpu */
struct pcpu *__pcpu = NULL;

/* pcpu_storage and PCPU_*_OFF defined earlier near thread0 */

/* Atomic and tick_sbt already defined earlier in this file */

/* RM lock functions */
void _rm_wlock(struct rmlock *rm) { (void)rm; }
void _rm_wunlock(struct rmlock *rm) { (void)rm; }
int _rm_rlock(struct rmlock *rm, struct rm_priotracker *tracker, int trylock) { (void)rm; (void)tracker; (void)trylock; return 1; }
void _rm_runlock(struct rmlock *rm, struct rm_priotracker *tracker) { (void)rm; (void)tracker; }
void rm_destroy(struct rmlock *rm) { (void)rm; }
void rm_init(struct rmlock *rm, const char *name) { (void)rm; (void)name; }

/* tick_sbt already defined earlier */
void wakeup_any(const void *chan) { (void)chan; }

/* Time */
volatile time_t time_second = 0;

/* String functions */
char *strcat(char *s, const char *append) {
    char *p = s;
    while (*p) p++;
    while ((*p++ = *append++));
    return s;
}
int ffs(int mask) { return __builtin_ffs(mask); }

/* Math */
long lmax(long a, long b) { return a > b ? a : b; }
long lmin(long a, long b) { return a < b ? a : b; }
long ulmin(long a, long b) { return a < b ? a : b; }

/* UIO */
int uiomove(void *cp, int n, struct uio *uio) {
    if (n < 0) return EINVAL;
    if (n > uio->uio_resid) return EFAULT;

    struct iovec *iov = uio->uio_iov;
    int remaining = n;

    while (remaining > 0 && uio->uio_iovcnt > 0) {
        int cnt = iov->iov_len;
        if (cnt > remaining) cnt = remaining;
        if (cnt == 0) { iov++; uio->uio_iovcnt--; continue; }

        if (uio->uio_segflg == UIO_SYSSPACE) {
            if (uio->uio_rw == UIO_WRITE)
                __builtin_memcpy(cp, iov->iov_base, cnt);
            else
                __builtin_memcpy(iov->iov_base, cp, cnt);
        } else if (uio->uio_segflg == UIO_NOCOPY) {
            /* no copy needed */
        } else {
            /* UIO_USERSPACE — use copyin/copyout */
            if (uio->uio_rw == UIO_WRITE)
                copyin(iov->iov_base, cp, cnt);
            else
                copyout(cp, iov->iov_base, cnt);
        }

        iov->iov_base = (char *)iov->iov_base + cnt;
        iov->iov_len -= cnt;
        uio->uio_resid -= cnt;
        uio->uio_offset += cnt;
        cp = (char *)cp + cnt;
        remaining -= cnt;

        if (iov->iov_len == 0) { iov++; uio->uio_iovcnt--; }
    }

    uio->uio_iov = iov;
    return 0;
}
/* uiomove_fromphys - real signature uses struct vm_page *ma[] and vm_offset_t */
struct vm_page;
int uiomove_fromphys(struct vm_page *ma[], vm_offset_t offset, int n, struct uio *uio) { (void)ma; (void)offset; (void)n; (void)uio; return 0; }

/* Select — selrecord is implemented above near selwakeuppri */
void seldrain(struct selinfo *sip) { (void)sip; }

/* Scheduler */
void sched_add(struct thread *td, int flags) { (void)td; (void)flags; }
void sched_prio(struct thread *td, u_char prio) { (void)td; (void)prio; }

/* Kthread */
int kthread_add(void (*func)(void *), void *arg, void *proc, void **tdp, int flags, int pages, const char *tag, const char *arg0) {
    (void)func; (void)arg; (void)proc; (void)tdp; (void)flags; (void)pages; (void)tag; (void)arg0; return 0;
}
void kthread_exit(void) { for(;;); }
int kproc_kthread_add(void (*func)(void *), void *arg, void *proc, void **tdp, int flags, int pages, const char *tag, const char *arg0) {
    (void)func; (void)arg; (void)proc; (void)tdp; (void)flags; (void)pages; (void)tag; (void)arg0; return 0;
}

/* SWI */
int swi_add(void **eventp, const char *name, void (*handler)(void *), void *arg, int pri, int flags, void **cookiep) {
    (void)eventp; (void)name; (void)handler; (void)arg; (void)pri; (void)flags; if (cookiep) *cookiep = NULL; return 0;
}
void swi_sched(void *cookie, int flags) { (void)cookie; (void)flags; }
int swi_remove(void *cookie) { (void)cookie; return 0; }

/* Interrupt */
int intr_event_bind(void *ie, int cpu) { (void)ie; (void)cpu; return 0; }
int intr_event_bind_ithread_cpuset(void *ie, void *cs) { (void)ie; (void)cs; return 0; }

/* PCPU */
struct pcpu *pcpu_find(u_int cpuid) { (void)cpuid; return NULL; }

/* Copy */
int copyin_nofault(const void *uaddr, void *kaddr, size_t len) { (void)uaddr; (void)kaddr; (void)len; return 0; }
int copyinstr(const void *uaddr, void *kaddr, size_t len, size_t *done) { (void)uaddr; (void)kaddr; (void)len; if (done) *done = 0; return 0; }

/* CPUSET */
int cpuset_setthread(int cpu, void *cs) { (void)cpu; (void)cs; return 0; }

/* Epoch */
epoch_t epoch_alloc(const char *name, int flags) { (void)name; (void)flags; return NULL; }
void epoch_drain_callbacks(epoch_t epoch) { (void)epoch; }
void epoch_wait_preempt(epoch_t epoch) { (void)epoch; }

/* CC */
MALLOC_DEFINE(M_CC_MEM, "cc_mem", "congestion control");
void cc_attach(struct tcpcb *tp, struct cc_algo *algo) { (void)tp; (void)algo; }
void cc_release(struct cc_algo *algo) { (void)algo; }
struct cc_head cc_list = STAILQ_HEAD_INITIALIZER(cc_list);

/* Devctl */
void devctl_notify(const char *system, const char *subsystem, const char *type, void *data) { (void)system; (void)subsystem; (void)type; (void)data; }

/* Device */
void make_dev_args_init_impl(void *args) { (void)args; }
int make_dev_s(void *args, void **dev, int uid, int gid, int mode, const char *fmt, ...) { (void)args; (void)uid; (void)gid; (void)mode; (void)fmt; if (dev) *dev = NULL; return 0; }

/* Bus */
int bus_get_domain(void *dev, void *domain) { (void)dev; (void)domain; return 0; }

/* Network interface */
void if_dead(struct ifnet *ifp) { (void)ifp; }

/* Multicast */
void in_leavegroup_locked(void *inm, void *imf) { (void)inm; (void)imf; }
void inp_freemoptions(void *imo) { (void)imo; }
void ip_mfilter_free(void *imf) { (void)imf; }

/* Inet */
char *inet_ntop(int af, const void *src, char *dst, socklen_t size) { (void)af; (void)src; if (dst && size > 0) dst[0] = 0; return dst; }

/* Signal */
void kern_psignal(struct proc *p, int sig) { (void)p; (void)sig; }
void tdsignal(struct thread *td, int sig) { (void)td; (void)sig; }

/* Knote */
void knlist_add(struct knlist *knl, struct knote *kn, int islocked) { (void)knl; (void)kn; (void)islocked; }
void knlist_destroy(struct knlist *knl) { (void)knl; }
int knlist_empty(struct knlist *knl) { (void)knl; return 1; }
void knlist_remove(struct knlist *knl, struct knote *kn, int islocked) { (void)knl; (void)kn; (void)islocked; }
int knote_triv_copy(struct knote *kn, struct proc *p) { (void)kn; (void)p; return 0; }

/* Sbuf */
int sbuf_delete(struct sbuf *s) { (void)s; return 0; }
int sbuf_error(struct sbuf *s) { (void)s; return 0; }

/* SX */
int sx_try_xlock(struct sx *sx) {
    unsigned long save;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(save));
    if (__sync_lock_test_and_set(&sx->sx_lock, 1) == 0) {
        sx->lock_object.lo_data = (unsigned int)save;
        return 1;
    }
    __asm__ volatile("pushq %0; popfq" : : "r"(save));
    return 0;
}
int sx_xlock_sig(struct sx *sx) {
    unsigned long save;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(save));
    while (__sync_lock_test_and_set(&sx->sx_lock, 1))
        __asm__ volatile("pause");
    sx->lock_object.lo_data = (unsigned int)save;
    return 0;
}

/* Sysctl */
int SYSCTL_IN(struct sysctl_req *req, void *p, size_t l) { (void)req; (void)p; (void)l; return 0; }

/* Prison */
int prison_check(struct ucred *cred1, struct ucred *cred2) { (void)cred1; (void)cred2; return 0; }

/* Routing */
struct nhop_object *nhop_alloc(uint32_t fibnum, int family) { (void)fibnum; (void)family; return NULL; }
void *match_nhop_gw(struct nhop_object *nh, void *gw) { (void)nh; (void)gw; return NULL; }
void nhop_set_expire(struct nhop_object *nh, uint32_t expire) { (void)nh; (void)expire; }
bool nhop_set_gw(struct nhop_object *nh, const struct sockaddr *sa, bool is_gw) { (void)nh; (void)sa; (void)is_gw; return true; }
void nhop_set_origin(struct nhop_object *nh, uint8_t origin) { (void)nh; (void)origin; }
void nhop_set_redirect(struct nhop_object *nh, bool is_redirect) { (void)nh; (void)is_redirect; }
void nhop_set_src(struct nhop_object *nh, struct ifaddr *ifa) { (void)nh; (void)ifa; }
void nhop_set_transmit_ifp(struct nhop_object *nh, struct ifnet *ifp) { (void)nh; (void)ifp; }
uint32_t nhop_get_expire(const struct nhop_object *nh) { (void)nh; return 0; }
uint32_t nhop_get_idx(const struct nhop_object *nh) { (void)nh; return 0; }
uint32_t nhop_get_metric(const struct nhop_object *nh) { (void)nh; return 0; }
struct nhop_object *nhop_get_nhop(struct nhop_object *nh, int *pidx) { (void)nh; if (pidx) *pidx = 0; return NULL; }
const struct weightened_nhop *nhgrp_get_nhops(const struct nhgrp_object *nhg, uint32_t *pnum_nhops) { (void)nhg; if (pnum_nhops) *pnum_nhops = 0; return NULL; }

void nhops_init(void) {}
int nhops_init_rib(struct rib_head *rh) { (void)rh; return 0; }
void nhops_destroy_rib(struct rib_head *rh) { (void)rh; }
void nhops_update_ifmtu(struct rib_head *rh, struct ifnet *ifp) { (void)rh; (void)ifp; }

int rib_action(uint32_t fibnum, int action, struct rt_addrinfo *info, struct rib_cmd_info *rc) { (void)fibnum; (void)action; (void)info; (void)rc; return 0; }
int rib_add_route_px(uint32_t fibnum, struct sockaddr *dst, int plen, struct route_nhop_data *rnd, int op_flags, struct rib_cmd_info *rc) { (void)fibnum; (void)dst; (void)plen; (void)rnd; (void)op_flags; (void)rc; return 0; }
void rib_decompose_notification(const struct rib_cmd_info *rc, route_notification_t *cb, void *cbdata) { (void)rc; (void)cb; (void)cbdata; }
void rib_destroy_subscriptions(struct rib_head *rh) { (void)rh; }
void rib_init_subscriptions(struct rib_head *rh) { (void)rh; }
struct nhop_object *rib_lookup(uint32_t fibnum, const struct sockaddr *dst, uint32_t flags, uint32_t flowid) { (void)fibnum; (void)dst; (void)flags; (void)flowid; return NULL; }
const char *rib_print_family(int family) { (void)family; return "?"; }

sa_family_t rt_get_family(const struct rtentry *rt) { (void)rt; return 0; }
void rt_get_inet_prefix_pmask(struct rtentry *rt, void *prefix, void *pmask) { (void)rt; (void)prefix; (void)pmask; }
struct nhop_object *rt_get_raw_nhop(const struct rtentry *rt) { (void)rt; return NULL; }
bool rt_is_exportable(const struct rtentry *rt, struct ucred *cred) { (void)rt; (void)cred; return false; }
bool rt_is_host(const struct rtentry *rt) { (void)rt; return false; }
void *rt_tables_get_rnh(uint32_t table, int family) { (void)table; (void)family; return NULL; }
struct rtbridge *rtsock_callback_p = NULL;
struct rtbridge *netlink_callback_p = NULL;

void tmproutes_init(struct rib_head *rh) { (void)rh; }
void tmproutes_destroy(struct rib_head *rh) { (void)rh; }

/* mbuf functions - declared in sys/mbuf.h, provide implementations here */
int m_clget(struct mbuf *m, int how) {
    (void)how;
    if (!m) return 0;
    caddr_t cl = (caddr_t)uma_zalloc_arg(zone_clust, NULL, how);
    if (!cl) return 0;
    m->m_ext.ext_buf = cl;
    m->m_ext.ext_free = NULL;
    m->m_ext.ext_arg1 = NULL;
    m->m_ext.ext_arg2 = NULL;
    m->m_ext.ext_size = MCLBYTES;
    m->m_ext.ext_type = EXT_CLUSTER;
    m->m_ext.ext_flags = EXT_FLAG_EMBREF;
    m->m_ext.ext_count = 1;
    m->m_flags |= M_EXT;
    m->m_data = m->m_ext.ext_buf;
    return 1;
}
void m_extadd(struct mbuf *m, char *buf, u_int size, m_ext_free_t *free_fn,
    void *arg1, void *arg2, int flags, int type) {
    if (!m) return;
    m->m_ext.ext_buf = buf;
    m->m_ext.ext_free = free_fn;
    m->m_ext.ext_arg1 = arg1;
    m->m_ext.ext_arg2 = arg2;
    m->m_ext.ext_size = size;
    m->m_ext.ext_type = type;
    m->m_ext.ext_flags = flags;
    m->m_flags |= M_EXT;
    m->m_data = m->m_ext.ext_buf;
}
void m_rcvif_serialize(struct mbuf *m) { (void)m; }
struct ifnet *m_rcvif_restore(struct mbuf *m) { (void)m; return NULL; }
struct mbuf *mb_alloc_ext_pgs(int how, m_ext_free_t *free_fn, int flags) { (void)how; (void)free_fn; (void)flags; return NULL; }

/* VM */
void *PHYS_TO_VM_PAGE(uint64_t pa) { (void)pa; return NULL; }
int vm_fault_quick_hold_pages(void *map, void *addr, int count, int prot, void *m, int *countp) { (void)map; (void)addr; (void)count; (void)prot; (void)m; if (countp) *countp = 0; return 0; }
void vm_page_unhold_pages(void *m, int count) { (void)m; (void)count; }
void *vm_page_alloc_noobj(int flags) { (void)flags; return NULL; }
void vm_page_free(void *m) { (void)m; }
void vm_page_unwire_noq(void *m) { (void)m; }
void vm_wait(void) {}

/* NVList */
MALLOC_DEFINE(M_NVLIST, "nvlist", "nvlist");
void *nvlist_create(int flags) { (void)flags; return NULL; }
void nvlist_destroy(void *nvl) { (void)nvl; }
int nvlist_error(const void *nvl) { (void)nvl; return 0; }
int nvlist_pack(const void *nvl, void **bufp, size_t *sizep, int flags) { (void)nvl; (void)bufp; (void)sizep; (void)flags; return 0; }
void *nvlist_unpack(const void *buf, size_t size, int flags) { (void)buf; (void)size; (void)flags; return NULL; }
int nvlist_add_bool(void *nvl, const char *name, int value) { (void)nvl; (void)name; (void)value; return 0; }
int nvlist_exists_bool(const void *nvl, const char *name) { (void)nvl; (void)name; return 0; }
int nvlist_get_bool(const void *nvl, const char *name) { (void)nvl; (void)name; return 0; }

/* Misc */
void ia32_pause(void) { __asm__ volatile("pause"); }
void funsetown(struct sigio **sigiop) { (void)sigiop; }
void getcredhostuuid(struct ucred *cred, char *hostuuid) { (void)cred; if (hostuuid) hostuuid[0] = 0; }
int getjailname(struct ucred *cred, char *name, size_t len) { (void)cred; if (name && len > 0) name[0] = 0; return 0; }
/* cpuhead - declared extern in sys/pcpu.h, provide definition here */
struct cpuhead cpuhead;
uintptr_t dpcpu_off[1] = { 0 };
void ck_pr_store_ptr(void *target, void *v) { *(void **)target = v; }
void memcpy_data(void *dst, const void *src, size_t len) { __builtin_memcpy(dst, src, len); }
int sendfile_wait_generic(struct socket *so, off_t need, int *space) { (void)so; (void)need; if (space) *space = 0; return 0; }
int mc_get(struct mchain *mc, u_int size, int how, short type, int flags) {
    STAILQ_INIT(&mc->mc_q);
    mc->mc_len = 0;
    mc->mc_mlen = 0;
    
    struct mbuf *m;
    u_int remaining = size;
    bool first = true;
    
    while (remaining > 0) {
        if (first && (flags & M_PKTHDR)) {
            m = m_gethdr(how, type);
        } else {
            m = m_get(how, type);
        }
        if (!m)
            goto fail;
        
        u_int space;
        if (m->m_flags & M_EXT)
            space = MCLBYTES;
        else if (m->m_flags & M_PKTHDR)
            space = MHLEN;
        else
            space = MLEN;
        
        if (space >= remaining) {
            /* Last mbuf in chain */
            STAILQ_INSERT_TAIL(&mc->mc_q, m, m_stailq);
            mc->mc_mlen += MSIZE;
            break;
        }
        
        /* Need a cluster for more space */
        if (!(m->m_flags & M_EXT) && remaining > (m->m_flags & M_PKTHDR ? MHLEN : MLEN)) {
            m_clget(m, how);
            if (m->m_flags & M_EXT)
                space = MCLBYTES;
        }
        
        STAILQ_INSERT_TAIL(&mc->mc_q, m, m_stailq);
        mc->mc_mlen += MSIZE;
        remaining -= space;
        first = false;
    }
    
    return 0;
fail:
    mc_freem(mc);
    return ENOBUFS;
}
void vlog(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void uuid_ether_add(const uint8_t *addr) { (void)addr; }
void uuid_ether_del(const uint8_t *addr) { (void)addr; }

/* SHA1 */
void sha1_init(void *ctx) { (void)ctx; }
void sha1_loop(void *ctx, const void *data, size_t len) { (void)ctx; (void)data; (void)len; }
void sha1_result(void *ctx, void *digest) { (void)ctx; if (digest) __builtin_memset(digest, 0, 20); }

/* Stats */
void stats_v1_blob_destroy(void *sb) { (void)sb; }
int stats_v1_blob_snapshot(void *sb, void *buf, size_t len, int flags) { (void)sb; (void)buf; (void)len; (void)flags; return 0; }

/* asprintf */
int asprintf(char **str, const char *fmt, ...) {
    (void)fmt;
    if (str) *str = NULL;
    return 0;
}
