/* X OS compat: proc/thread primitives for FreeBSD network stack */
#ifndef _SYS_PROC_H_
#define _SYS_PROC_H_

#include <sys/cdefs.h>
#include <sys/_stdint.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/queue.h>

/* struct plimit is defined in sys/resourcevar.h */
#include <sys/resourcevar.h>

/* Forward declarations */
struct sigio;
struct vmspace;

/* Include sys/sigio.h for struct sigio definition */
#include <sys/sigio.h>

/* Minimal struct proc for FreeBSD network stack */
struct proc {
    int p_pid;
    int p_flag;
    int p_osrel;
    int p_fibnum;
    struct ucred *p_ucred;
    struct vmspace *p_vmspace;
    struct mtx p_mtx;
};

/* Minimal struct thread */
struct thread {
    struct proc *td_proc;
    struct ucred *td_ucred;
    int td_critnest;
    int td_flags;
    int td_retval[2];
    int td_priority;
    int td_user_pri;
    int td_pinned;
    int td_tid;
    struct plimit *td_limit;
    struct rusage td_ru;
    struct mtx td_lock;
};

/* proc lock/unlock */
#define PROC_LOCK(p)    mtx_lock(&(p)->p_mtx)
#define PROC_UNLOCK(p)  mtx_unlock(&(p)->p_mtx)
#define PROC_LOCK_ASSERT(p, type) (void)(p)
#define thread_lock(td) mtx_lock(&(td)->td_lock)
#define thread_unlock(td) mtx_unlock(&(td)->td_lock)
#define THREAD_CAN_SLEEP()     1
#define CRITICAL_ASSERT(td)    (void)(td)

/* p_* flags */
#define P_SYSTEM    0x00000400
#define P_WEXIT     0x02000000
#define P_INEXEC    0x04000000

/* td_* flags */
#define TDF_ASTPENDING  0x00000001
#define TDF_NEEDRESCHED 0x00000002
#define TDF_UNUSED23    0x00000004
#define TDF_BORROWING   0x00000008

/* proc_* functions */
struct proc *pfind(pid_t pid);
struct proc *zpfind(pid_t pid);

/* curthread / curproc - declared in sys/lock.h */

/* sched_pin / sched_unpin - defined in sys/sched.h */

/* critical_enter / exit - declared in sys/systm.h, defined in compat_shims.c */

/* p_candebug */
static inline int p_candebug(struct proc *p, struct proc *t) { return 0; }

/* prison_check is declared in sys/jail.h */

/* pargs / p_ucred stubs */
struct ucred;
struct ucred *crcop(struct ucred *cr);
void crfree(struct ucred *cr);
struct ucred *crhold(struct ucred *cr);

#endif /* _SYS_PROC_H_ */
