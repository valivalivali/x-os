/* X OS compat: domainset primitives */
#ifndef _SYS_DOMAINSET_H_
#define _SYS_DOMAINSET_H_

#include <sys/cdefs.h>
#include <sys/_stdint.h>

/* Use the real domainset_t from sys/_domainset.h */
#include <sys/_domainset.h>

#ifndef MAXMEMDOM
#define MAXMEMDOM 1
#endif

/* DOMAINSET_RR_PTR - defined in compat_shims.c */
extern domainset_t *DOMAINSET_RR_PTR;

#define DOMAINSET_POLICY_ROUNDROBIN  2
#define DOMAINSET_POLICY_FIRST_touch 3
#define DOMAINSET_POLICY_PREFER      4
#define DOMAINSET_POLICY_INTERLEAVE  5

#define ds_policy  __bits.__policy
#define ds_prefer  __bits.__prefer

static inline void
domainset_zero(domainset_t *ds)
{
    __builtin_memset(ds, 0, sizeof(*ds));
}

static domainset_t _domainset_prefer[MAXMEMDOM];
#define DOMAINSET_PREF(domain)  (&_domainset_prefer[(domain)])

#endif /* _SYS_DOMAINSET_H_ */
