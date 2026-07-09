#pragma once

/* XNU Mach/BSD → x-os Compatibility Layer
 *
 * This header provides the Mach types and functions that XNU's BSD
 * networking and POSIX subsystems expect, mapped to x-os's existing
 * kernel infrastructure.
 */

#include <stdint.h>
#include <stddef.h>
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/vmm.h"
#include "kernel/sched/sched.h"

/* ------------------------------------------------------------------ */
/* Mach basic types                                                    */
/* ------------------------------------------------------------------ */

typedef uint64_t kern_return_t;
#define KERN_SUCCESS     0
#define KERN_FAILURE     1
#define KERN_NO_SPACE    3
#define KERN_INVALID_ARG 4
#define KERN_PROTECTION_FAILURE 2
#define KERN_NOT_FOUND   5
#define KERN_ABORTED     14
#define KERN_OLDSIZE     12
#define KERN_INVALID_ADDRESS 1

typedef int vm_prot_t;
#define VM_PROT_NONE    0x00
#define VM_PROT_READ    0x01
#define VM_PROT_WRITE   0x02
#define VM_PROT_EXECUTE 0x04

typedef uint64_t vm_offset_t;
typedef uint64_t vm_size_t;
typedef uint64_t vm_map_offset_t;
typedef uint64_t vm_map_size_t;

typedef int vm_inherit_t;
#define VM_INHERIT_SHARE    0
#define VM_INHERIT_COPY     1
#define VM_INHERIT_NONE     2
#define VM_INHERIT_DEFAULT  VM_INHERIT_COPY

/* ------------------------------------------------------------------ */
/* Task / Thread types — mapped to x-os proc_t                         */
/* ------------------------------------------------------------------ */

typedef proc_t *task_t;
typedef proc_t *thread_t;

#define current_task()    proc_current()
#define current_thread()  proc_current()

#define TASK_NULL  NULL
#define THREAD_NULL NULL

/* ------------------------------------------------------------------ */
/* Locks — mapped to simple spinlocks                                  */
/* ------------------------------------------------------------------ */

#include "kernel/arch/x86_64/io.h"

typedef struct {
    volatile int locked;
    const char *name;
} lck_mtx_t;

typedef struct {
    const char *name;
} lck_grp_t;

#define LCK_GRP_DECLARE(name, str) static lck_grp_t name = { str }
#define LCK_MTX_DECLARE(name, grp) lck_mtx_t name = { 0, #name }

static inline void lck_mtx_init(lck_mtx_t *l, lck_grp_t *g, void *attr) {
    (void)g; (void)attr;
    l->locked = 0;
    l->name = "mtx";
}

static inline void lck_mtx_lock(lck_mtx_t *l) {
    while (__sync_lock_test_and_set(&l->locked, 1)) {
        __asm__ volatile("pause");
    }
}

static inline void lck_mtx_unlock(lck_mtx_t *l) {
    __sync_lock_release(&l->locked);
}

static inline int lck_mtx_try_lock(lck_mtx_t *l) {
    return __sync_lock_test_and_set(&l->locked, 1) == 0;
}

typedef lck_mtx_t lck_spin_t;
#define lck_spin_init   lck_mtx_init
#define lck_spin_lock   lck_mtx_lock
#define lck_spin_unlock lck_mtx_unlock

/* Simple rwlock — just use a mutex for now */
typedef lck_mtx_t lck_rw_t;
#define lck_rw_init     lck_mtx_init
#define lck_rw_lock_shared(l)    lck_mtx_lock(l)
#define lck_rw_lock_exclusive(l) lck_mtx_lock(l)
#define lck_rw_unlock_shared(l)    lck_mtx_unlock(l)
#define lck_rw_unlock_exclusive(l) lck_mtx_unlock(l)

/* ------------------------------------------------------------------ */
/* Memory allocation — mapped to x-os heap                             */
/* ------------------------------------------------------------------ */

#define kalloc(size)        kmalloc(size)
#define kfree(ptr, ...)     kfree(ptr)
#define kalloc_data(size, ...)  kmalloc(size)
#define kfree_data(ptr, ...)    kfree(ptr)

/* Zone allocator — just use heap */
typedef void *zone_t;
#define zone_create(name, size, type, flags) ((zone_t)1)
#define zalloc(zone)    kmalloc(4096)
#define zfree(zone, ptr) kfree(ptr, 4096)
#define zalloc_n(zone, count) kmalloc(count * 4096)

/* ------------------------------------------------------------------ */
/* Panic / assert                                                      */
/* ------------------------------------------------------------------ */

#define panic(fmt, ...)  do { kprintf("PANIC: " fmt "\n", ##__VA_ARGS__); __asm__ volatile("cli; hlt"); } while(0)
#define assert(x) do { if (!(x)) panic("assert failed: %s", #x); } while(0)

/* ------------------------------------------------------------------ */
/* Misc Mach functions                                                 */
/* ------------------------------------------------------------------ */

static inline kern_return_t task_resume(task_t t) { (void)t; return KERN_SUCCESS; }
static inline kern_return_t task_suspend(task_t t) { (void)t; return KERN_SUCCESS; }
static inline void thread_resume(thread_t t) { (void)t; }

#define mach_msg_timeout_none 0

/* ------------------------------------------------------------------ */
/* errno — XNU BSD uses these                                          */
/* ------------------------------------------------------------------ */

#define EPERM           1
#define ENOENT          2
#define ESRCH           3
#define EINTR           4
#define EIO             5
#define ENXIO           6
#define E2BIG           7
#define ENOEXEC         8
#define EBADF           9
#define ECHILD          10
#define EDEADLK         11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define ENOTBLK         15
#define EBUSY           16
#define EEXIST          17
#define EXDEV           18
#define ENODEV          19
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define ENFILE          23
#define EMFILE          24
#define ENOTTY          25
#define ETXTBSY         26
#define EFBIG           27
#define ENOSPC          28
#define ESPIPE          29
#define EROFS           30
#define EMLINK          31
#define EPIPE           32
#define EDOM            33
#define ERANGE          34
#define EAGAIN          35
#define EWOULDBLOCK     EAGAIN
#define EINPROGRESS     36
#define EALREADY        37
#define ENOTSOCK        38
#define EDESTADDRREQ    39
#define EMSGSIZE        40
#define EPROTOTYPE      41
#define ENOPROTOOPT     42
#define EPROTONOSUPPORT 43
#define ESOCKTNOSUPPORT 44
#define EOPNOTSUPP      45
#define ENOTSUP         EOPNOTSUPP
#define EPFNOSUPPORT    46
#define EAFNOSUPPORT    47
#define EADDRINUSE      48
#define EADDRNOTAVAIL   49
#define ENETDOWN        50
#define ENETUNREACH     51
#define ENETRESET       52
#define ECONNABORTED    53
#define ECONNRESET      54
#define ENOBUFS         55
#define EISCONN         56
#define ENOTCONN        57
#define ESHUTDOWN       58
#define ETOOMANYREFS    59
#define ETIMEDOUT       60
#define ECONNREFUSED    61
#define ELOOP           62
#define ENAMETOOLONG    63
#define EHOSTDOWN       64
#define EHOSTUNREACH    65
#define ENOTEMPTY       66
#define EPROCLIM        67
#define EUSERS          68
#define EDQUOT          69
#define ESTALE          70
#define EREMOTE         71
#define EBADRPC         72
#define ERPCMISMATCH    73
#define EPROGUNAVAIL    74
#define EPROGMISMATCH   75
#define EPROCUNAVAIL    76
#define ENOLCK          77
#define ENOSYS          78
#define EFTYPE          79
#define EAUTH           80
#define ENEEDAUTH       81
#define EPWROFF         82
#define EDEVERR         83
#define EOVERFLOW       84
#define EBADEXEC        85
#define EBADARCH        86
#define ESHLIBVERS      87
#define EBADMACHO       88
#define ECANCELED       89
#define EIDRM           90
#define ENOMS           91
#define ENOATTR         93
#define EBADMSG         94
#define EMULTIHOP       95
#define ENODATA         96
#define ENOLINK         97
#define ENOSR           98
#define ENOSTR          99
#define EPROTO          100
#define ETIME           101
#define ELAST           102

/* ------------------------------------------------------------------ */
/* BSD proc accessors — mapped to x-os proc_t                          */
/* ------------------------------------------------------------------ */

/* In XNU, proc_t is a complex struct. In x-os, we use a simpler proc_t.
 * The BSD networking code mostly needs: p_pid, p_fd, p_stats, etc.
 * We provide minimal shims here. */

/* filedesc — x-os doesn't have a full fd table, but we stub it */
struct filedesc;
typedef struct filedesc filedesc_t;

/* ucred */
struct ucred {
    uint32_t cr_uid;
    uint32_t cr_gid;
};
typedef struct ucred ucred_t;
typedef struct ucred *posix_cred_t;

/* ------------------------------------------------------------------ */
/* Time/tick functions                                                 */
/* ------------------------------------------------------------------ */

#include "kernel/hal/timers/timer.h"

#define ticks timer_ticks()
#define hz 1000  /* x-os timer runs at 1000 Hz */

static inline uint64_t mach_absolute_time(void) {
    return timer_ticks();
}

static inline void mach_timebase_info(uint32_t *numer, uint32_t *denom) {
    *numer = 1;
    *denom = 1;  /* 1 tick = 1 ms = 1,000,000 ns */
}

/* ------------------------------------------------------------------ */
/* Sleep / wakeup — simple stubs using x-os scheduler                  */
/* ------------------------------------------------------------------ */

static inline void msleep(void *chan, lck_mtx_t *mtx, int pri, const char *wmesg, uint64_t timeout) {
    (void)chan; (void)pri; (void)wmesg;
    if (mtx) lck_mtx_unlock(mtx);
    if (timeout) {
        proc_sleep(timeout / 1000000);  /* ns → ms */
    } else {
        extern void sched_yield(void);
        sched_yield();
    }
    if (mtx) lck_mtx_lock(mtx);
}

static inline void wakeup(void *chan) {
    (void)chan;
    /* In x-os, all blocked processes are woken on each scheduler tick
     * if their sleep_until has passed. For channel-based wakeup,
     * we'd need to extend the scheduler. For now, this is a no-op. */
}

static inline void wakeup_one(void *chan) {
    (void)chan;
}

/* Mbuf — defined in uipc_mbuf_xos.h, not here */

/* ------------------------------------------------------------------ */
/* Socket types — minimal definitions                                  */
/* ------------------------------------------------------------------ */

struct socket;
struct sockaddr;
struct mbuf;
struct proc;
struct file;

/* ------------------------------------------------------------------ */
/* Sysctl — stubs                                                      */
/* ------------------------------------------------------------------ */

#define SYSCTL_DECL(name) extern int name##_sysctl_enabled
#define SYSCTL_INT(parent, nbr, name, access, ptr, val, descr)

/* ------------------------------------------------------------------ */
/* Log levels                                                          */
/* ------------------------------------------------------------------ */

#define LOG_ERR    3
#define LOG_INFO   6
#define LOG_DEBUG  7

/* ------------------------------------------------------------------ */
/* Misc macros                                                         */
/* ------------------------------------------------------------------ */

#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))

#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define powerof2(x) (((x) & ((x) - 1)) == 0)

#define NULL ((void*)0)

#define __unused __attribute__((unused))
#define __dead2 __attribute__((noreturn))
#define __private_extern__

#define CAST_DOWN(t, v) ((t)(uintptr_t)(v))
#define CAST_AWAY(t, v) ((t)(v))
