#include "kernel/sched/sched.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/vmm.h"
#include "kernel/memory/pmm.h"
#include "kernel/hal/timers/timer.h"
#include "kernel/arch/x86_64/gdt.h"

static proc_t procs[SCHED_MAX_PROCS];
static proc_t *current = NULL;
static proc_t *ready_head = NULL;

static void ready_dequeue(proc_t *p) {
    proc_t **pp = &ready_head;
    while (*pp) {
        if (*pp == p) {
            *pp = p->next;
            p->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void ready_enqueue(proc_t *p) {
    /* Avoid duplicate links (ready-list cycles freeze the scheduler). */
    for (proc_t *q = ready_head; q; q = q->next) {
        if (q == p)
            return;
    }
    p->next = NULL;
    proc_t **tail = &ready_head;
    while (*tail)
        tail = &(*tail)->next;
    *tail = p;
}

void sched_init(void) {
    memset(procs, 0, sizeof(procs));
    /* Free slots are DEAD+reaped so proc_create can claim them. Zombies are
     * DEAD+!reaped until waitpid collects them. */
    for (int i = 0; i < SCHED_MAX_PROCS; i++)
        procs[i].reaped = true;
    /* PID 0 is the idle/kernel task. It already has a stack (the boot stack). */
    procs[0].pid = 0;
    procs[0].state = PROC_RUNNING;
    procs[0].pml4_phys = (uint64_t)vmm_get_cr3();
    procs[0].pml4_virt = vmm_kernel_pml4();
    current = &procs[0];
    ready_head = NULL;
}

proc_t *proc_current(void) {
    return current;
}

proc_t *proc_by_pid(uint64_t pid) {
    for (int i = 0; i < SCHED_MAX_PROCS; i++) {
        /* Include unreaped zombies so waitpid can collect them. */
        if (procs[i].pid == pid &&
            (procs[i].state != PROC_DEAD || !procs[i].reaped))
            return &procs[i];
    }
    return NULL;
}

void proc_set_current(proc_t *p) {
    current = p;
}

void sched_adopt_current(proc_t *p) {
    p->state = PROC_RUNNING;
    current = p;
    ready_dequeue(p);
}

proc_t *proc_create(uint64_t entry, uint64_t pml4_phys, uint64_t *pml4_virt,
                    uint8_t *kstack) {
    for (int i = 1; i < SCHED_MAX_PROCS; i++) {
        /* Only reuse slots that waitpid (or an explicit reap) has collected.
         * Stealing an unreaped zombie would make the parent's waitpid hang. */
        if (procs[i].state == PROC_DEAD && procs[i].reaped) {
            procs[i].pid = (uint64_t)(i);
            procs[i].state = PROC_READY;
            procs[i].rip = entry;
            procs[i].pml4_phys = pml4_phys;
            procs[i].pml4_virt = pml4_virt;
            procs[i].kstack = kstack;
            /* Stack grows down; set rsp to top of stack.
             * On x86_64, stack must be 16-byte aligned before CALL. */
            procs[i].rsp = (uint64_t)(kstack + SCHED_STACK_SIZE - 8);
            procs[i].sleep_until = 0;
            procs[i].ring3 = false;
            procs[i].parent_pid = 0;
            procs[i].exit_code = 0;
            procs[i].reaped = true;
            procs[i].fork_rbx = procs[i].fork_rbp = 0;
            procs[i].fork_r12 = procs[i].fork_r13 = 0;
            procs[i].fork_r14 = procs[i].fork_r15 = 0;
            procs[i].fork_rflags = 0;
            ready_enqueue(&procs[i]);
            return &procs[i];
        }
    }
    return NULL;
}

void proc_exit(proc_t *p) {
    if (!p) return;

    /* Free process resources: user page tables + all mapped user pages,
     * and the kernel stack. */
    if (p->pml4_virt) {
        vmm_destroy_user(p->pml4_virt);
        pmm_free_frame(p->pml4_phys);
        p->pml4_virt = NULL;
        p->pml4_phys = 0;
    }
    /* Don't free the kernel stack if we're running on it.
     * The scheduler will context-switch away, and the next process
     * can safely reclaim this memory. We leak it to avoid use-after-free. */
    if (p->kstack && p != current) {
        kfree(p->kstack);
        p->kstack = NULL;
    }

    ready_dequeue(p);
    p->state = PROC_DEAD;
    p->reaped = false;
    p->next = NULL;
    /* Leave `current` as the dead process. sched_yield must not re-queue
     * DEAD tasks; it will pick another READY process. Avoid parking on
     * idle while idle is still linked READY — that duplicated idle on the
     * ready list and froze the system on the second exit. */
}

/* Kill a process by PID. Removes it from the ready queue and frees resources.
 * Returns 1 if killed, 0 if not found. */
void proc_kill(uint64_t pid) {
    proc_t *p = proc_by_pid(pid);
    if (!p || p->pid == 0) return;
    if (p == current) {
        proc_exit(p);
        sched_yield();
        for (;;) __asm__ volatile("cli; hlt");
    }
    ready_dequeue(p);
    proc_exit(p);
}

void proc_sleep(uint64_t ms) {
    if (!current || current->pid == 0) {
        timer_sleep_ms(ms);
        return;
    }
    current->sleep_until = timer_ticks() + ms;
    current->state = PROC_BLOCKED;
    /* Blocked tasks stay on the ready list so yield can wake them. */
    ready_enqueue(current);
    sched_yield();
}

static proc_t *pick_next_ready(void) {
    /* Wake up expired sleepers but keep them in the queue. */
    for (proc_t *p = ready_head; p; p = p->next) {
        if (p->state == PROC_BLOCKED && p->sleep_until &&
            timer_ticks() >= p->sleep_until) {
            p->state = PROC_READY;
            p->sleep_until = 0;
        }
    }

    proc_t **npp = &ready_head;
    while (*npp) {
        if ((*npp)->state == PROC_READY) {
            proc_t *next = *npp;
            *npp = next->next;
            next->next = NULL;
            return next;
        }
        npp = &(*npp)->next;
    }

    /* Recovery: READY but not linked (should not happen). */
    for (int i = 1; i < SCHED_MAX_PROCS; i++) {
        if (procs[i].state == PROC_READY)
            return &procs[i];
    }
    return &procs[0]; /* idle */
}

void sched_yield(void) {
    if (!current) return;

    proc_t *next = pick_next_ready();

    if (next == current) return;

    proc_t *prev = current;
    /* Only runnable tasks go back on the ready list. */
    if (current->state == PROC_RUNNING) {
        current->state = PROC_READY;
        ready_enqueue(current);
    }

    next->state = PROC_RUNNING;
    next->next = NULL;
    current = next;

    if (next->ring3) {
        gdt_set_rsp0((uint64_t)(next->kstack + SCHED_STACK_SIZE));
    }

    context_switch(prev, next);
}
