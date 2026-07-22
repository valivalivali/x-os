/* X OS compat: malloc declarations for FreeBSD network stack */
#ifndef _SYS_MALLOC_H_
#define _SYS_MALLOC_H_

#include <sys/cdefs.h>
#include <sys/_domainset.h>

/* MALLOC_DEFINE / MALLOC_DECLARE - create malloc type stubs */
struct malloc_type {
    int mt_magic;
};

#define MALLOC_DEFINE(type, shortdesc, longdesc) \
    static struct malloc_type type##_storage = { 0 }; \
    struct malloc_type *type = &type##_storage

#define MALLOC_DECLARE(type) \
    extern struct malloc_type *type

/* M_NOWAIT / M_WAITOK / M_ZERO */
#define M_NOWAIT       0x0001
#define M_WAITOK       0x0002
#define M_ZERO         0x0100
#define M_NOVM         0x0004
#define M_USE_RESERVE  0x0008
#define M_NODUMP       0x0040
#define M_BESTFIT      0x0010
#define M_EXEC         0x4000
#define M_NORECLAIM    0x0080
#define M_NEVERFREED   0x10000

/* malloc / free */
void *malloc(unsigned long size, struct malloc_type *type, int flags);
void free(void *addr, struct malloc_type *type);
void *realloc(void *addr, unsigned long size, struct malloc_type *type, int flags);
void *reallocf(void *addr, unsigned long size, struct malloc_type *type, int flags);

/* MALLOC / FREE macros */
#define MALLOC(space, cast, size, type, flags) \
    ((space) = (cast)malloc((size), (type), (flags)))
#define FREE(addr, type) free((addr), (type))

/* zfree */
#define zfree(p, type) do { free((p), (type)); (p) = NULL; } while (0)

/* malloc_domainset */
static inline void *malloc_domainset(unsigned long size, struct malloc_type *type,
    domainset_t *ds, int flags) { (void)ds; return malloc(size, type, flags); }

/* M_* malloc type declarations used by network code */
MALLOC_DECLARE(M_MBUF);
MALLOC_DECLARE(M_DEVBUF);
MALLOC_DECLARE(M_TEMP);
MALLOC_DECLARE(M_SONAME);
MALLOC_DECLARE(M_SOOPTS);
MALLOC_DECLARE(M_IPMOPTS);
MALLOC_DECLARE(M_IPADDR);
MALLOC_DECLARE(M_IFADDR);
MALLOC_DECLARE(M_IFMADDR);
MALLOC_DECLARE(M_CLONE);
MALLOC_DECLARE(M_PCB);
MALLOC_DECLARE(M_RTENTRY);
MALLOC_DECLARE(M_RTABLE);
MALLOC_DECLARE(M_NETADDR);
MALLOC_DECLARE(M_IFNET);
MALLOC_DECLARE(M_IOV);
MALLOC_DECLARE(M_TCPLOG);
MALLOC_DECLARE(M_TCPLOGDEV);

/* mallocarray - from real sys/malloc.h */
void *mallocarray(size_t nmemb, size_t size, struct malloc_type *type, int flags);

/* mp_ncpus - from sys/smp.h */
extern int mp_ncpus;

/* uma_zone_t - provided by vm/uma.h in the real FreeBSD source */
#include <vm/uma.h>

#endif /* _SYS_MALLOC_H_ */
