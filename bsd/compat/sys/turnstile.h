/* X OS compat: turnstile stubs */
#ifndef _SYS_TURNSTILE_H_
#define _SYS_TURNSTILE_H_

#include <sys/cdefs.h>

struct turnstile;

static inline struct turnstile *turnstile_trylookup(void *lock) { return NULL; }
static inline void turnstile_lock(void *lock) { (void)lock; }
static inline void turnstile_unlock(void *lock) { (void)lock; }
static inline void turnstile_chain_lock(void *lock) { (void)lock; }
static inline void turnstile_chain_unlock(void *lock) { (void)lock; }

#endif /* _SYS_TURNSTILE_H_ */
