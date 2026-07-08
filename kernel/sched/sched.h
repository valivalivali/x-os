#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Microkernel scheduler — process table, thread states, context switch.
 *
 * Phase 1 (current): cooperative tasking within kernel, no ring-3 yet.
 * Phase 2: preemptive scheduler with per-process page tables.
 */

#define SCHED_MAX_PROCS 32
#define SCHED_STACK_SIZE (64 * 1024)

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
} proc_t;

void sched_init(void);
void sched_yield(void);
proc_t *proc_current(void);
proc_t *proc_by_pid(uint64_t pid);
void proc_set_current(proc_t *p);
void sched_adopt_current(proc_t *p);  /* set p as RUNNING, remove from ready */
proc_t *proc_create(uint64_t entry, uint64_t pml4_phys, uint64_t *pml4_virt,
                    uint8_t *kstack);
void proc_exit(proc_t *p);
void proc_kill(uint64_t pid);
void proc_sleep(uint64_t ms);

/* Assembly: context switch from current to next process */
void context_switch(proc_t *from, proc_t *to);
