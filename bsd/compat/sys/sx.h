/* X OS compat: sx (shared/exclusive) lock primitives */
#ifndef _SYS_SX_H_
#define _SYS_SX_H_

#include <sys/cdefs.h>
#include <sys/_stdint.h>

/* Use the real struct sx from sys/_sx.h */
#include <sys/_sx.h>

#define sx_init(s, name) do { \
    (s)->lock_object.lo_name = (name); \
    (s)->lock_object.lo_flags = 0; \
    (s)->lock_object.lo_data = 0; \
    (s)->lock_object.lo_witness = NULL; \
    (s)->sx_lock = 0; \
} while (0)

#define sx_init_flags(s, name, opts) sx_init(s, name)
#define sx_destroy(s) do { } while (0)

/* Simple exclusive-only lock with interrupt disable */
#define sx_slock(s)    do { \
    unsigned long __save; \
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(__save)); \
    while (__sync_lock_test_and_set(&(s)->sx_lock, 1)) \
        __asm__ volatile("pause"); \
    (s)->lock_object.lo_data = (unsigned int)__save; \
} while (0)
#define sx_xlock(s)    do { \
    unsigned long __save; \
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(__save)); \
    while (__sync_lock_test_and_set(&(s)->sx_lock, 1)) \
        __asm__ volatile("pause"); \
    (s)->lock_object.lo_data = (unsigned int)__save; \
} while (0)
#define sx_sunlock(s)  do { \
    __sync_lock_release(&(s)->sx_lock); \
    __asm__ volatile("pushq %0; popfq" : : "r"((unsigned long)(s)->lock_object.lo_data)); \
} while (0)
#define sx_xunlock(s)  do { \
    __sync_lock_release(&(s)->sx_lock); \
    __asm__ volatile("pushq %0; popfq" : : "r"((unsigned long)(s)->lock_object.lo_data)); \
} while (0)

#define sx_tryslock(s)  ({ \
    unsigned long __save; \
    int __ret; \
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(__save)); \
    if (__sync_lock_test_and_set(&(s)->sx_lock, 1) == 0) { \
        (s)->lock_object.lo_data = (unsigned int)__save; \
        __ret = 1; \
    } else { \
        __asm__ volatile("pushq %0; popfq" : : "r"(__save)); \
        __ret = 0; \
    } \
    __ret; \
})
#define sx_tryxlock(s)  sx_tryslock(s)
#define sx_tryupgrade(s) (0)
#define sx_downgrade(s) do { } while (0)

#define sx_xlocked(s) ((s)->sx_lock != 0)
#define sx_assert(s, what) do { } while (0)

#define SA_SLOCKED    0x01
#define SA_XLOCKED    0x02
#define SA_UNLOCKED   0x04
#define SA_RECURSED   0x08

#define sx_sleep(chan, lock, pri, wmesg, timo) (0)
#define sx_xholder(s) ((void *)0)

#define SX_DUPOK      0x00000010
#define SX_NOPROFILE  0x10000000
#define SX_QUIET      0x20000000
#define SX_RECURSE    0x40000000
#define SX_NOWITNESS  0x80000000

/* SX_SYSINIT */
#define SX_SYSINIT(name, sxa, desc) SX_SYSINIT_FLAGS(name, sxa, desc, 0)
#define SX_SYSINIT_FLAGS(name, sxa, desc, flags) \
    static void name##_sys_init(void *data) { \
        struct sx *s = (struct sx *)data; \
        sx_init(s, desc); \
    } \
    SYSINIT(name##_sx, SI_SUB_LOCK, SI_ORDER_MIDDLE, name##_sys_init, sxa)

#endif /* _SYS_SX_H_ */
