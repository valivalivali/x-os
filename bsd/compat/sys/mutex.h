/* X OS compat: mutex primitives for FreeBSD network stack */
#ifndef _SYS_MUTEX_H_
#define _SYS_MUTEX_H_

#include <sys/cdefs.h>
#include <sys/_stdint.h>

/* Use the real struct mtx from sys/_mutex.h */
#include <sys/_mutex.h>

struct mtx_pool;

/* Mutex init/destroy - use mtx_lock field as our spinlock */
#define mtx_init(m, name, type, opts) do { \
    (m)->lock_object.lo_name = (name); \
    (m)->lock_object.lo_flags = (opts); \
    (m)->lock_object.lo_data = 0; \
    (m)->lock_object.lo_witness = NULL; \
    (m)->mtx_lock = 0; \
} while (0)

#define mtx_destroy(m) do { } while (0)

/* Mutex lock/unlock - spinlock with interrupt disable to prevent
 * timer IRQ (vioif_rx_poll) from deadlocking on held locks. */
#define mtx_lock(m)    do { \
    unsigned long __save; \
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(__save)); \
    while (__sync_lock_test_and_set(&(m)->mtx_lock, 1)) \
        __asm__ volatile("pause"); \
    (m)->lock_object.lo_data = (unsigned int)__save; \
} while (0)

#define mtx_unlock(m)  do { \
    __sync_lock_release(&(m)->mtx_lock); \
    __asm__ volatile("pushq %0; popfq" : : "r"((unsigned long)(m)->lock_object.lo_data)); \
} while (0)

#define mtx_lock_spin(m)   mtx_lock(m)
#define mtx_unlock_spin(m) mtx_unlock(m)

/* Mutex try */
#define mtx_trylock(m) ({ \
    unsigned long __save; \
    int __ret; \
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(__save)); \
    if (__sync_lock_test_and_set(&(m)->mtx_lock, 1) == 0) { \
        (m)->lock_object.lo_data = (unsigned int)__save; \
        __ret = 1; \
    } else { \
        __asm__ volatile("pushq %0; popfq" : : "r"(__save)); \
        __ret = 0; \
    } \
    __ret; \
})

/* Mutex owned */
#define mtx_owned(m) ((m)->mtx_lock != 0)

/* Mutex assert */
#define mtx_assert(m, what) do { } while (0)
#define MA_OWNED       0x01
#define MA_NOTOWNED    0x02
#define MA_RECURSED    0x04
#define MA_NOTRECURSED 0x08

/* MTX flags */
#define MTX_DEF         0x00000000
#define MTX_SPIN        0x00000001
#define MTX_DUPOK       0x00000010
#define MTX_NOPROFILE   0x00000020
#define MTX_NEW         0x00000040
#define MTX_QUIET       0x00000080
#define MTX_RECURSE     0x00000100
#define MTX_NOWITNESS   0x00000200

/* MTX_SYSINIT */
#define MTX_SYSINIT(name, mtx_ptr, desc, opts) \
    static void name##_sys_init(void *data) { \
        struct mtx *m = (struct mtx *)data; \
        mtx_init(m, desc, NULL, opts); \
    } \
    SYSINIT(name##_mtx, SI_SUB_LOCK, SI_ORDER_MIDDLE, name##_sys_init, mtx_ptr)

/* mtx_pool */
struct mtx_pool *mtx_pool_create(const char *name, int count, const char *type, int flags);
void mtx_pool_destroy(struct mtx_pool **poolp);
struct mtx *mtx_pool_acquire(struct mtx_pool *pool);
void mtx_pool_release(struct mtx_pool *pool, struct mtx *m);
struct mtx *mtx_pool_find(struct mtx_pool *pool, void *thing);
void mtx_pool_lock(struct mtx_pool *pool, void *thing);
void mtx_pool_unlock(struct mtx_pool *pool, void *thing);
void mtx_pool_lock_spin(struct mtx_pool *pool, void *thing);
void mtx_pool_unlock_spin(struct mtx_pool *pool, void *thing);

#endif /* _SYS_MUTEX_H_ */
