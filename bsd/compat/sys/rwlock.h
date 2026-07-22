/* X OS compat: rwlock primitives */
#ifndef _SYS_RWLOCK_H_
#define _SYS_RWLOCK_H_

#include <sys/cdefs.h>
#include <sys/_stdint.h>

/* Use the real struct rwlock from sys/_rwlock.h */
#include <sys/_rwlock.h>

#define rw_init(rw, name) do { \
    (rw)->lock_object.lo_name = (name); \
    (rw)->lock_object.lo_flags = 0; \
    (rw)->lock_object.lo_data = 0; \
    (rw)->lock_object.lo_witness = NULL; \
    (rw)->rw_lock = 0; \
} while (0)

#define rw_init_flags(rw, name, opts) rw_init(rw, name)
#define rw_destroy(rw) do { } while (0)

/* Exclusive lock with interrupt disable to prevent timer IRQ deadlock. */
#define rw_wlock(rw)   ({ \
    unsigned long __save; \
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(__save)); \
    while (__sync_lock_test_and_set(&(rw)->rw_lock, 1)) \
        __asm__ volatile("pause"); \
    (rw)->lock_object.lo_data = (unsigned int)__save; \
})
#define rw_rlock(rw)   rw_wlock(rw)
#define rw_wunlock(rw) ({ \
    __sync_lock_release(&(rw)->rw_lock); \
    __asm__ volatile("pushq %0; popfq" : : "r"((unsigned long)(rw)->lock_object.lo_data)); \
})
#define rw_runlock(rw) rw_wunlock(rw)

#define rw_trywlock(rw) ({ \
    unsigned long __save; \
    int __ret; \
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(__save)); \
    if (__sync_lock_test_and_set(&(rw)->rw_lock, 1) == 0) { \
        (rw)->lock_object.lo_data = (unsigned int)__save; \
        __ret = 1; \
    } else { \
        __asm__ volatile("pushq %0; popfq" : : "r"(__save)); \
        __ret = 0; \
    } \
    __ret; \
})
#define rw_tryrlock(rw) rw_trywlock(rw)
#define rw_tryupgrade(rw) (0)
#define rw_downgrade(rw) do { } while (0)

#define rw_wowned(rw) ((rw)->rw_lock != 0)
#define rw_assert(rw, what) do { } while (0)

#define RA_WLOCKED    0x01
#define RA_RLOCKED    0x02
#define RA_UNLOCKED   0x04
#define RA_RECURSED   0x08

#define RW_DUPOK      0x00000010
#define RW_NOPROFILE  0x10000000
#define RW_QUIET      0x20000000
#define RW_RECURSE    0x40000000
#define RW_NOWITNESS  0x80000000

/* RW_SYSINIT */
#define RW_SYSINIT_FLAGS(name, rw, desc, flags) \
    static void name##_sys_init(void *data) { \
        struct rwlock *r = (struct rwlock *)data; \
        rw_init(r, desc); \
    } \
    SYSINIT(name##_rw, SI_SUB_LOCK, SI_ORDER_MIDDLE, name##_sys_init, rw)
#define RW_SYSINIT(name, rw, desc) RW_SYSINIT_FLAGS(name, rw, desc, 0)

#endif /* _SYS_RWLOCK_H_ */
