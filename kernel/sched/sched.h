#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "kernel/hal/apic/spinlock.h"

/* Microkernel scheduler — process table, thread states, context switch.
 *
 * Phase 1 (current): cooperative tasking within kernel, no ring-3 yet.
 * Phase 2: preemptive scheduler with per-process page tables.
 */

#define SCHED_MAX_PROCS 256
#define SCHED_STACK_SIZE (64 * 1024)

/* Priority levels — lower number = higher priority.
 * pick_next_ready scans PRIO_HIGH first, then NORMAL, then LOW. */
#define SCHED_NPRIO 3
typedef enum {
    PRIO_HIGH = 0,      /* interactive: composer, terminal, shell */
    PRIO_NORMAL = 1,    /* default for services and user apps */
    PRIO_LOW = 2,       /* background / batch */
} proc_prio_t;

typedef enum {
    PROC_DEAD = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
} proc_state_t;

typedef struct proc {
    uint64_t pid;
    proc_state_t state;
    uint64_t rsp;           /* kernel stack pointer for context switch */
    uint64_t rip;           /* instruction pointer */
    uint64_t pml4_phys;     /* page table root PHYSICAL address (0 = kernel) */
    uint64_t *pml4_virt;    /* page table root VIRTUAL address for cleanup */
    uint8_t *kstack;        /* kernel stack base */
    uint64_t sleep_until;   /* timer tick to wake at */
    struct proc *next;      /* ready queue */
    bool ring3;             /* true if this is a ring-3 userspace process */
    uint64_t parent_pid;    /* PID of parent (0 = init/kernel) */
    int exit_code;          /* exit code when state == PROC_DEAD but not reaped */
    bool reaped;            /* true after wait() has collected exit_code */
    /* Callee-saved GPRs to restore for fork children (SysV AMD64).
     * Offsets are hardcoded in context.S — keep these fields before name[]. */
    uint64_t fork_rbx, fork_rbp, fork_r12, fork_r13, fork_r14, fork_r15;
    uint64_t fork_rflags;
    /* Argument registers (SysV ABI: preserved across syscall).
     * enter_userspace_fork must restore these or the child sees
     * kernel register values instead of the user's. */
    uint64_t fork_rdi, fork_rsi, fork_rdx, fork_r8, fork_r9, fork_r10;
    char name[16];          /* short comm for ps (must stay after fork_*) */
    /* Signal state (after fork_* / name — safe for context.S offsets). */
#define XOS_NSIG 32
    uint64_t sig_blocked;                 /* current blocked mask */
    uint64_t sig_pending;                 /* pending bitmask (bit N = signal N) */
    uint64_t sig_handler[XOS_NSIG];       /* 0=DFL, 1=IGN, else userspace addr */
    uint64_t sig_mask[XOS_NSIG];          /* sa_mask per action */
    int      sig_flags[XOS_NSIG];         /* sa_flags */
    uint64_t saved_rflags;                /* rflags saved across context_switch */
    volatile int exiting;                 /* 1 = proc_exit in progress or done */
    uint64_t saved_ret;                   /* return address saved in proc_t (not on stack) */
    /* Callee-saved registers saved in proc_t (not on kstack) so they
     * can't be corrupted by IRETQ frames pushed onto the kstack top
     * while the process is in the ready queue. */
    uint64_t ctx_rbx, ctx_rbp, ctx_r12, ctx_r13, ctx_r14, ctx_r15;
    uint64_t canary;  /* detect proc_t corruption */
    volatile uint8_t no_preempt;  /* if 1, timer interrupts skip sched_yield */
    volatile int switching;  /* 1 = context_switch in progress, don't pick */
    /* Extended CPU state (x87/SSE/AVX) — swapped on every context switch.
     * MUST stay last (with wait_chan): context.S hardcodes byte offsets into
     * this struct up to and including `switching` at 956. */
    void *xstate;
    /* Non-NULL when the process is BLOCKED waiting for an event rather than
     * for a deadline.  Woken by sched_wake_chan() on the same address. */
    const void *wait_chan;
    /* Scheduling priority (PRIO_HIGH / PRIO_NORMAL / PRIO_LOW).
     * Safe to add here — context.S only hardcodes offsets up to `switching`
     * at 956; this field is well past that. */
    uint8_t priority;
    /* Thread support: tgid = thread group leader's pid (= pid for leader).
     * tid = unique thread ID (= pid for the slot).  For a single-threaded
     * process, tgid == tid == pid.  All fields below are past context.S
     * hardcoded offsets. */
    uint64_t tgid;              /* thread group ID (POSIX PID) */
    uint64_t tid;               /* thread ID (Linux TID) */
    struct proc *thread_next;   /* linked list of threads in this group */
    volatile int pml4_refcount; /* refcount for shared address space (threads) */
    uint64_t clear_tid_addr;    /* userspace address to zero on thread exit */
} proc_t;

/* Ensure p has an extended-state area allocated (idempotent). */
void proc_ensure_xstate(proc_t *p);

/* ---- Event waiting ------------------------------------------------------
 * Sleep/wake on an arbitrary address, so services can block for work
 * instead of polling.  `unlock` (may be NULL) is a spinlock the caller
 * holds; it is released only after the current process has been marked
 * blocked, which is what makes the wakeup impossible to lose.  Interrupts
 * are NOT re-enabled on return — the caller still owns that state.
 *
 * timeout_ms is a backstop, not the primary mechanism: 0 means "use the
 * default safety timeout" so that a missed wakeup anywhere in the system
 * degrades into a latency blip rather than a hung desktop. */
void sched_block_on(const void *chan, spinlock_t *unlock, uint64_t timeout_ms);

/* Wake every process blocked on `chan`. Safe to call with other locks held
 * as long as they are always taken before sched_lock. */
void sched_wake_chan(const void *chan);

void sched_init(void);
void sched_early_init(void); /* Early init before smp_init */
void sched_yield(void);
bool sched_yield_try(void);  /* non-blocking yield for timer handlers */
void sched_check_resched(void);  /* deferred preemption check (syscall/irq return) */
void sched_wake_sleepers(void);  /* wake expired sleepers (timer handler) */
void sched_idle_loop(void) __attribute__((noreturn));  /* per-CPU idle loop */
void sched_dbg_dump(void);       /* dump proc table + ready queues */
void sched_init_ap(void);  /* Per-CPU scheduler init for APs */
proc_t *proc_current(void);
proc_t *sched_proc_slot(int idx);  /* raw process-table access for /sys/proc */
proc_t *proc_by_pid(uint64_t pid);
void proc_set_current(proc_t *p);
void sched_adopt_current(proc_t *p);  /* set p as RUNNING, remove from ready */
proc_t *proc_create(uint64_t entry, uint64_t pml4_phys, uint64_t *pml4_virt,
                    uint8_t *kstack);
void proc_make_ready(proc_t *p);  /* enqueue + notify after full init */
void proc_exit(proc_t *p);
void proc_kill(uint64_t pid);
void proc_sleep(uint64_t ms);
void sched_notify_new_proc(void); /* Send resched IPI to all APs */
void sched_check_canaries(void);  /* Check all proc kstack canaries */
void sched_release_lock(void);    /* Release sched_lock (for ring3_trampoline) */
void proc_set_priority(proc_t *p, uint8_t prio);  /* set scheduling priority */

/* Assembly: context switch from current to next process */
void context_switch(proc_t *from, proc_t *to);

/* Called from context.S when to->saved_ret is corrupted */
void context_switch_panic(uint64_t bad_ret, proc_t *to);
