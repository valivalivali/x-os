/* X OS compat: SYSINIT/SYSUNINIT macros using linker sets */
#ifndef _SYS_KERNEL_H_
#define _SYS_KERNEL_H_

#include <sys/cdefs.h>

/* SI_SUB_* ordering constants — must match FreeBSD sys/kernel.h enum values */
#define SI_SUB_DUMMY            0x0000000
#define SI_SUB_COPYRIGHT        0x0100000
#define SI_SUB_TUNABLES         0x0200000
#define SI_SUB_SETTINGS         0x0300000
#define SI_SUB_MTX_POOL         0x0400000
#define SI_SUB_LOCK             0x1B00000
#define SI_SUB_EVENTHANDLER     0x1C00000
#define SI_SUB_VNET_PRELINK     0x1E00000
#define SI_SUB_KMEM             0x0800000
#define SI_SUB_KVM              0x0900000
#define SI_SUB_HYPERVISOR       0x0a00000
#define SI_SUB_INTR             0x0b00000
#define SI_SUB_VM               0x1000000
#define SI_SUB_BUF              0x1200000
#define SI_SUB_TIMER            0x1500000
#define SI_SUB_KLD              0x2000000
#define SI_SUB_KHELP            0x2080000
#define SI_SUB_CPU              0x2100000
#define SI_SUB_RACCT            0x2180000
#define SI_SUB_RANDOM           0x21A0000
#define SI_SUB_KICK_SCHEDULER   0x21C0000
#define SI_SUB_KPROF            0x21D0000
#define SI_SUB_MAC              0x2180000
#define SI_SUB_MAC_POLICY       0x21C0000
#define SI_SUB_MAC_LATE         0x21D0000
#define SI_SUB_VNET             0x21E0000
#define SI_SUB_INTRINSIC        0x2200000
#define SI_SUB_DDB_SERVICES     0x2380000
#define SI_SUB_VM_CONF          0x2300000
#define SI_SUB_DOMAIN           0x2400000
#define SI_SUB_MBUF             0x2700000
#define SI_SUB_SOFTINTR         0x2800000
#define SI_SUB_SWI              0x2800000
#define SI_SUB_TASKQ            0x2880000
#define SI_SUB_EPOCH            0x2888000
#define SI_SUB_IF               0x2400000
#define SI_SUB_NET              0x2500000
#define SI_SUB_NET_DOMAIN       0x2580000
#define SI_SUB_PSEUDO           0x7000000
#define SI_SUB_EXEC             0x7400000
#define SI_SUB_PROTO_BEGIN      0x8000000
#define SI_SUB_PROTO_PFIL       0x8100000
#define SI_SUB_PROTO_MC         0x8300000
#define SI_SUB_PROTO_IF         0x8400000
#define SI_SUB_PROTO_DOMAININIT 0x8600000
#define SI_SUB_PROTO_DOMAIN     0x8800000
#define SI_SUB_PROTO_FIREWALL   0x8806000
#define SI_SUB_PROTO_IFATTACHDOMAIN 0x8808000
#define SI_SUB_PROTO_END        0x8ffffff
#define SI_SUB_INIT_IF          0x3000000
#define SI_SUB_KTHREAD_INIT     0xe000000
#define SI_SUB_KTHREAD_PAGE     0xe400000
#define SI_SUB_KTHREAD_VM       0xe800000
#define SI_SUB_KTHREAD_BUF      0xec00000
#define SI_SUB_KTHREAD_IDLE     0xf000000
#define SI_SUB_KTHREAD_LAZY     0x5500000
#define SI_SUB_SMP              0x2900000
#define SI_SUB_PSEUDO_SMP       0x6100000
#define SI_SUB_KLD_LOAD         0x7000000
#define SI_SUB_KLD_DAEMON       0x7100000
#define SI_SUB_RUN_SCHEDULER    0xa000000
#define SI_SUB_SYSCALLS         0xd800000
#define SI_SUB_VNET_DONE        0xdc00000
#define SI_SUB_VFS              0x4000000
#define SI_SUB_ROOT_CONF        0xb000000
#define SI_SUB_INTRINSIC_POST   0xd000000
#define SI_SUB_DRIVERS          0x3100000
#define SI_SUB_CONFIGURE        0x3800000
#define SI_SUB_CLOCKS           0x4800000

/* SI_ORDER_* */
#define SI_ORDER_FIRST      0x0000000
#define SI_ORDER_SECOND     0x0000001
#define SI_ORDER_THIRD      0x0000002
#define SI_ORDER_FOURTH     0x0000003
#define SI_ORDER_FIFTH      0x0000004
#define SI_ORDER_SIXTH      0x0000005
#define SI_ORDER_SEVENTH    0x0000006
#define SI_ORDER_EIGHTH     0x0000007
#define SI_ORDER_NINTH      0x0000008
#define SI_ORDER_TENTH      0x0000009
#define SI_ORDER_MIDDLE     0x0000010
#define SI_ORDER_ANY        0x7fffffff

/* SYSINIT entry struct collected into .sysinit_set linker section */
struct sysinit_entry {
    uint32_t subsystem;
    uint32_t order;
    void    (*func)(void *);
    void     *arg;
};

#define SYSINIT(ident, subsystem, order, func, arg) \
    static const struct sysinit_entry __sysinit_##ident \
    __attribute__((section(".sysinit_set"), used)) = { \
        (uint32_t)(subsystem), (uint32_t)(order), \
        (void (*)(void *))(func), (arg) \
    };

#define SYSUNINIT(ident, subsystem, order, func, arg) \
    static const struct sysinit_entry __sysuninit_##ident \
    __attribute__((section(".sysuninit_set"), used)) = { \
        (uint32_t)(subsystem), (uint32_t)(order), \
        (void (*)(void *))(func), (arg) \
    };

/* tunable_* */
#define TUNABLE_INT(path, var)
#define TUNABLE_UINT(path, var)
#define TUNABLE_LONG(path, var)
#define TUNABLE_ULONG(path, var)
#define TUNABLE_INT_FETCH(path, var) (0)
#define TUNABLE_UINT_FETCH(path, var) (0)
#define TUNABLE_STR_FETCH(path, var, size) (0)

/* FEATURE macro */
#define FEATURE(name, desc)

/* boot / root mount */
extern int root_mounted;

#endif /* _SYS_KERNEL_H_ */
