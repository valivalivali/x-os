/* X OS compat: core system declarations for FreeBSD network stack */
#ifndef _SYS_SYSTM_H_
#define _SYS_SYSTM_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/_stdint.h>
#include <sys/uio.h>
#include <sys/kassert.h>
#include <sys/witness.h>
#include <sys/bitset.h>
#include <machine/atomic.h>

/* __pcpu - per-CPU data pointer, needed by machine/pcpu.h macros */
struct pcpu;
extern struct pcpu *__pcpu;

/* Sleep functions - matching real sys/systm.h */
struct lock_object;
struct mtx;
extern sbintime_t tick_sbt;
int _sleep(const void *chan, struct lock_object *lock, int pri,
    const char *wmesg, sbintime_t sbt, sbintime_t pr, int flags);
int msleep_spin_sbt(const void *chan, struct mtx *mtx, const char *wmesg,
    sbintime_t sbt, sbintime_t pr, int flags);
int tsleep(const void *chan, int pri, const char *wmesg, int timo);
int msleep_sbt(const void *chan, struct mtx *mtx, int pri, const char *wmesg,
    sbintime_t bt, sbintime_t pr, int flags);

#define msleep(chan, mtx, pri, wmesg, timo) \
    _sleep((chan), &(mtx)->lock_object, (pri), (wmesg), tick_sbt * (timo), 0, C_HARDCLOCK)
#define msleep_spin(chan, mtx, wmesg, timo) \
    msleep_spin_sbt((chan), (mtx), (wmesg), tick_sbt * (timo), 0, C_HARDCLOCK)

/* __assert_unreachable - stub (real one panics or uses __unreachable) */
#define __assert_unreachable() do { } while (0)

/* DEBUG_POISON_POINTER - stub (only real in INVARIANTS builds) */
#define DEBUG_POISON_POINTER(x) do { } while (0)

/* Override SAN_INTERCEPTOR so bcopy/bzero/bcmp work */
#define SAN_INTERCEPTOR_PREFIX __sanitizer
#define SAN_INTERCEPTOR(func) func

/* Override bcopy/bzero/bcmp to use standard C functions */
#undef bcopy
#undef bzero
#undef bcmp
#define bcopy(from, to, len)   __builtin_memmove((to), (from), (len))
#define bzero(buf, len)        __builtin_memset((buf), 0, (len))
#define bcmp(b1, b2, len)      __builtin_memcmp((b1), (b2), (len))
#define ovbcopy(f, t, l)       bcopy((f), (t), (l))

/* __diagused / __unused_ok */
#define __diagused __attribute__((unused))
#define __read_mostly __attribute__((section(".data.read_mostly")))
#define __read_frequently __attribute__((section(".data.read_frequently")))
#define __exclusive_cache_line __attribute__((aligned(64)))
#define __exclusive_cache_line_no_smap __attribute__((aligned(64)))

/* Limits */
#ifndef INT32_MAX
#define INT32_MAX 0x7fffffff
#endif
#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffff
#endif
#ifndef INT_MAX
#define INT_MAX 2147483647
#endif

/* Callout flags */
#define CALLOUT_RETURNUNLOCKED  0x0002
#define C_DIRECT_EXEC           0x0004
#define C_HARDCLOCK             0x0008

/* Epoch tracker - include sys/epoch.h for full definition */
#include <sys/epoch.h>

/* critical_enter / exit - declared here to avoid implicit declaration from seqc.h */
void critical_enter(void);
void critical_exit(void);

/* zpcpu_offset - stub out per-CPU offset to 0 */
#define zpcpu_offset() 0

/* Include the real sys/pcpu.h for zpcpu_get and PCPU macros */
#include <sys/pcpu.h>

/* Include sys/callout.h for callout function declarations */
#include <sys/callout.h>

/* Include sys/ucred.h for struct ucred definition */
#include <sys/ucred.h>

/* Forward declarations */
struct malloc_type;
struct thread;
struct proc;

/* Global variables */
extern int hz;
extern int ticks;
extern int tick;
extern int bootverbose;
extern int maxphys;
extern volatile unsigned int cold;

/* printf family - we provide implementations in compat_shims.c */
int printf(const char *fmt, ...);
int snprintf(char *str, size_t size, const char *fmt, ...);
int sprintf(char *str, const char *fmt, ...);
int uprintf(const char *fmt, ...);
int vprintf(const char *fmt, __va_list ap);
int vsnprintf(char *str, size_t size, const char *fmt, __va_list ap);
int vsnrprintf(char *str, size_t size, int, const char *fmt, __va_list ap);
int vsprintf(char *buf, const char *fmt, __va_list ap);

void panic(const char *fmt, ...);
void vpanic(const char *fmt, __va_list ap);

/* tprintf */
struct proc;
void tprintf(struct proc *p, int pri, const char *fmt, ...);

/* kvprintf - matches FreeBSD sys/systm.h signature */
int kvprintf(const char *fmt, void (*func)(int, void*), void *arg, int radix, __va_list ap);

/* log */
void log(int level, const char *fmt, ...);

/* pause / DELAY */
void DELAY(int usec);
void pause_sbt(const char *wmesg, int sbt, int pr, int flags);
#define pause(wmesg, timo) pause_sbt(wmesg, timo, 0, 0)
#define pause_sig(wmesg, timo) pause_sbt(wmesg, timo, 0, 0)

/* cpu_tick_f */
typedef uint64_t (*cpu_tick_f)(void);
extern cpu_tick_f cpu_ticks;

/* getnanotime / nanotime / nanouptime */
struct timespec;
void getnanotime(struct timespec *tsp);
void nanotime(struct timespec *tsp);
void nanouptime(struct timespec *tsp);
void getnanouptime(struct timespec *tsp);

/* getmicrotime / microtime */
struct timeval;
void getmicrotime(struct timeval *tvp);
void microtime(struct timeval *tvp);
void getmicrouptime(struct timeval *tvp);
void microuptime(struct timeval *tvp);

/* boot time */
extern struct timeval boottime;

/* TSENTER / TSEXIT (trace macros) */
#define TSENTER()
#define TSEXIT()

/* Giant lock - defined in compat_shims.c */
extern struct mtx Giant;

/* load and store with byte order - provided by sys/endian.h */

/* ratecheck / ppsratecheck */
struct timeval;
int ratecheck(struct timeval *lasttime, const struct timeval *mininterval);
int ppsratecheck(struct timeval *lasttime, int *curpps, int maxpps);

/* getenv / kern_getenv */
char *getenv(const char *name);
char *kern_getenv(const char *name);
void freeenv(char *env);
int getenv_int(const char *name, int *data);
int getenv_uint(const char *name, unsigned int *data);
long getenv_long(const char *name, long *data);
u_long getenv_ulong(const char *name, u_long *data);
quad_t getenv_quad(const char *name, quad_t *data);
const char *getenv_string(const char *name, const char *defval);

/* NDFLAGS / debug */
extern long nbuf;
extern volatile int dumpdev;

/* __FreeBSD_version */
#ifndef __FreeBSD_version
#define __FreeBSD_version 1600019
#endif

/* MAXCPU / MAXMEMDOM */
#ifndef MAXCPU
#define MAXCPU 1
#endif
#ifndef MAXMEMDOM
#define MAXMEMDOM 1
#endif

/* MAXCOMLEN / MAXLOGNAME */
#define MAXCOMLEN 19
#define MAXLOGNAME 33

/* resource_unrhd / unr stuff */
struct unrhdr;
struct unrhdr *new_unrhdr(int low, int high, void *opaque);
void delete_unrhdr(struct unrhdr *uh);
int alloc_unr(struct unrhdr *uh);
void free_unr(struct unrhdr *uh, int item);
int alloc_unr_specific(struct unrhdr *uh, int item);

#endif /* _SYS_SYSTM_H_ */
