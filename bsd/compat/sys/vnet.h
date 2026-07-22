/* X OS compat: vnet (virtual network stack) stubs */
#ifndef _SYS_VNET_H_
#define _SYS_VNET_H_

#include <sys/cdefs.h>

/* VNET macros - we have a single network stack, no virtualization */
#define VNET_DEFINE(t, v)       t v
#define VNET_DEFINE_STATIC(t, v) static t v
#define _VNET_PTR(b, n)         (&(n))
#define VNET(n)                 (n)
#define VNET_PTR(n)             (&(n))

#define VNET_SYSINIT(ident, subsystem, order, func, arg) \
    SYSINIT(ident, subsystem, order, func, arg)

#define VNET_SYSUNINIT(ident, subsystem, order, func, arg) \
    SYSUNINIT(ident, subsystem, order, func, arg)

/* curvnet - just NULL */
extern void *curvnet;
#define CURVNET_SET(v) do { (void)(v); } while (0)
#define CURVNET_RESTORE() do { } while (0)

/* vnet_alloc / vnet_destroy */
static inline void *vnet_alloc(void) { return (void *)1; }
static inline void vnet_destroy(void *vnet) { (void)vnet; }

#endif /* _SYS_VNET_H_ */
