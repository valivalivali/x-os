/* X OS compat: cpuset primitives */
#ifndef _SYS_CPUSET_H_
#define _SYS_CPUSET_H_

#include <sys/cdefs.h>
#include <sys/_stdint.h>

/* Use the real cpuset_t from sys/_cpuset.h */
#include <sys/_cpuset.h>

/* all_cpus - defined in compat_shims.c */
extern cpuset_t all_cpus;

#define CPUSET_FSET    __bitset_fset(CPU_SETSIZE)
#define CPU_CLR(n, p)          __BIT_CLR(CPU_SETSIZE, (n), (p))
#define CPU_SET(n, p)          __BIT_SET(CPU_SETSIZE, (n), (p))
#define CPU_ISSET(n, p)        __BIT_ISSET(CPU_SETSIZE, (n), (p))
#define CPU_CLR_ATOMIC(n, p)   __BIT_CLR_ATOMIC(CPU_SETSIZE, (n), (p))
#define CPU_SET_ATOMIC(n, p)   __BIT_SET_ATOMIC(CPU_SETSIZE, (n), (p))
#define CPU_SET_ATOMIC_ACQ(n, p) __BIT_SET_ATOMIC_ACQ(CPU_SETSIZE, (n), (p))
#define CPU_SET_ATOMIC_REL(n, p) __BIT_SET_ATOMIC_REL(CPU_SETSIZE, (n), (p))
#define CPU_AND(n, p)          __BIT_AND(CPU_SETSIZE, (n), (p))
#define CPU_OR(n, p)           __BIT_OR(CPU_SETSIZE, (n), (p))
#define CPU_COPY(from, to)     __BIT_COPY(CPU_SETSIZE, (from), (to))
#define CPU_ISZERO(p)          __BIT_ISZERO(CPU_SETSIZE, (p))
#define CPU_ISFULLSET(p)       __BIT_ISFULLSET(CPU_SETSIZE, (p))
#define CPU_SUBSET(p, c)       __BIT_SUBSET(CPU_SETSIZE, (p), (c))
#define CPU_OVERLAP(p, c)      __BIT_OVERLAP(CPU_SETSIZE, (p), (c))
#define CPU_CMP(p, c)          __BIT_CMP(CPU_SETSIZE, (p), (c))
#define CPU_EMPTY(p)           __BIT_EMPTY(CPU_SETSIZE, (p))
#define CPU_FILL(p)            __BIT_FILL(CPU_SETSIZE, (p))
#define CPU_SETOF(n, p)        __BIT_SETOF(CPU_SETSIZE, (n), (p))
#define CPU_FFS(p)             __BIT_FFS(CPU_SETSIZE, (p))

#endif /* _SYS_CPUSET_H_ */
