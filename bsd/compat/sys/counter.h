/* X OS compat: counter(9) stubs */
#ifndef _SYS_COUNTER_H_
#define _SYS_COUNTER_H_

#include <sys/cdefs.h>
#include <sys/_stdint.h>

typedef void *counter_u64_t;

#define COUNTER_ALLOC 0x01

/* These are implemented in subr_counter.c */
counter_u64_t counter_u64_alloc(int flags);
void counter_u64_free(counter_u64_t c);
uint64_t counter_u64_fetch(counter_u64_t c);
void counter_u64_zero(counter_u64_t c);

static inline void counter_u64_add(counter_u64_t c, int64_t inc) {
    (void)c; (void)inc;
}

static inline void counter_u64_inc(counter_u64_t c) {
    (void)c;
}

static inline void counter_u64_dec(counter_u64_t c) {
    (void)c;
}

#define counter_enter() do { } while (0)
#define counter_exit() do { } while (0)
#define counter_u64_add_protected(c, inc) counter_u64_add(c, inc)

/* counter_rate - from sys/counter.h */
struct counter_rate;
struct counter_rate *counter_rate_alloc(int flags, int period);
void counter_rate_free(struct counter_rate *cr);
int64_t counter_ratecheck(struct counter_rate *cr, int64_t limit);

#endif /* _SYS_COUNTER_H_ */
