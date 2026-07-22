/* X OS compat: lock/mutex/rwlock/sx primitives for FreeBSD network stack */
#ifndef _SYS_LOCK_H_
#define _SYS_LOCK_H_

#include <sys/cdefs.h>
#include <sys/_stdint.h>

/* Forward declarations */
struct thread;
struct proc;

/* curthread/curproc - override pcpu.h macros with our static globals */
/* These need to be available early since rmlock.h uses curthread */
#undef curthread
#undef curproc
extern struct thread *curthread;
extern struct proc *curproc;
extern struct proc proc0;
extern struct thread thread0;

/* Lock classes */
#define LC_SLEEPLOCK    0
#define LC_SPINLOCK     1
#define LC_SLOCK        2

/* Lock flags */
#define LO_INITIALIZED  0x0001
#define LO_WITNESS      0x0002
#define LO_QUIET        0x0004
#define LO_RECURSABLE   0x0008
#define LO_SLEEPABLE    0x0010
#define LO_UPGRADABLE   0x0020
#define LO_DUPOK        0x0040
#define LO_CLASSFLAGS   0x00ff
#define LO_ALLFLAGS     0x00ff

/* Warn flags for WITNESS_WARN */
#define WARN_GIANTOK    0x01
#define WARN_SLEEPOK    0x04
#define WARN_KERNELOK   0x02
#define WARN_PANIC      0x08

/* Lock init names */
#define MTX_DEF         0x00000000
#define MTX_SPIN        0x01000000
#define MTX_DUPOK       0x04000000
#define MTX_NOPROFILE   0x08000000
#define MTX_QUIET       0x10000000
#define MTX_RECURSE     0x20000000
#define MTX_NOWITNESS   0x40000000

#define RW_DUPOK        0x00000010
#define RW_NOPROFILE    0x00000020
#define RW_QUIET        0x00000040
#define RW_RECURSE      0x00000080
#define RW_SLEEPABLE    0x00000100

#define SX_DUPOK        0x00000010
#define SX_NOPROFILE    0x00000020
#define SX_QUIET        0x00000040
#define SX_RECURSE      0x00000080
#define SX_SLEEPABLE    0x00000100
#define SX_NOADAPTIVE   0x00000200

/* Lock class names - just stubs */
struct lock_class {
    const char *lc_name;
    int lc_flags;
};
extern struct lock_class lock_class_mtx_spin;
extern struct lock_class lock_class_mtx_sleep;
extern struct lock_class lock_class_sx;
extern struct lock_class lock_class_rw;
extern struct lock_class lock_class_rm;
extern struct lock_class lock_class_rm_sleepable;

/* Lock assert flags */
#define LA_LOCKED       0x01
#define LA_NOTLOCKED    0x02
#define LA_SLOCKED      0x04
#define LA_XLOCKED      0x08
#define LA_RECURSED     0x10
#define LA_NOTRECURSED  0x20

#endif /* _SYS_LOCK_H_ */
