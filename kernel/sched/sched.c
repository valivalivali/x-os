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

void sched_init(void) {
    memset(procs, 0, sizeof(procs));
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
        if (procs[i].state != PROC_DEAD && procs[i].pid == pid)
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
    /* Remove p from ready_head if present */
    proc_t **pp = &ready_head;
    while (*pp) {
        if (*pp == p) {
            *pp = p->next;
            p->next = NULL;
            break;
        }
        pp = &(*pp)->next;
    }
}

proc_t *proc_create(uint64_t entry, uint64_t pml4_phys, uint64_t *pml4_virt,
                    uint8_t *kstack) {
    for (int i = 1; i < SCHED_MAX_PROCS; i++) {
        if (procs[i].state == PROC_DEAD) {
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
            procs[i].next = ready_head;
            ready_head = &procs[i];
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
    if (p->kstack) {
        kfree(p->kstack);
        p->kstack = NULL;
    }

    p->state = PROC_DEAD;
    if (p == current && p->pid != 0) {
        /* Switch back to idle task */
        current = &procs[0];
        current->state = PROC_RUNNING;
    }
}

/* Kill a process by PID. Removes it from the ready queue and frees resources.
 * Returns 1 if killed, 0 if not found. */
void proc_kill(uint64_t pid) {
    proc_t *p = proc_by_pid(pid);
    if (!p || p->pid == 0) return;
    if (p == current) {
        proc_exit(p);
        sched_yield();
        return;
    }
    /* Remove from ready queue if present. */
    proc_t **pp = &ready_head;
    while (*pp) {
        if (*pp == p) {
            *pp = p->next;
            break;
        }
        pp = &(*pp)->next;
    }
    proc_exit(p);
}

void proc_sleep(uint64_t ms) {
    if (!current || current->pid == 0) {
        timer_sleep_ms(ms);
        return;
    }
    current->sleep_until = timer_ticks() + ms;
    current->state = PROC_BLOCKED;
    sched_yield();
}

void sched_yield(void) {
    if (!current) return;

    /* Remove blocked/sleeping processes from ready queue */
    proc_t **pp = &ready_head;
    while (*pp) {
        proc_t *p = *pp;
        if (p->state == PROC_BLOCKED && p->sleep_until &&
            timer_ticks() >= p->sleep_until) {
            p->state = PROC_READY;
            p->sleep_until = 0;
        }
        if (p->state != PROC_READY) {
            *pp = p->next;
            p->next = NULL;
        } else {
            pp = &p->next;
        }
    }

    proc_t *next = ready_head;
    if (next) {
        ready_head = next->next;
        next->next = NULL;
    } else {
        next = &procs[0]; /* idle task */
    }

    if (next == current) return;

    proc_t *prev = current;
    if (current->state == PROC_RUNNING) {
        current->state = PROC_READY;
        current->next = NULL;
        proc_t **tail = &ready_head;
        while (*tail) tail = &(*tail)->next;
        *tail = current;
    }
    next->state = PROC_RUNNING;
    current = next;

    if (next->ring3) {
        gdt_set_rsp0((uint64_t)(next->kstack + SCHED_STACK_SIZE));
    }

    context_switch(prev, next);
}
