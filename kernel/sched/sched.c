#include "kernel/sched/sched.h"
#include "kernel/proc/signal.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/vmm.h"
#include "kernel/memory/pmm.h"
#include "kernel/hal/timers/timer.h"
#include "kernel/arch/x86_64/gdt.h"
#include "kernel/hal/apic/spinlock.h"
#include "kernel/hal/apic/smp.h"
#include "kernel/hal/apic/lapic.h"

static proc_t procs[SCHED_MAX_PROCS];
static proc_t *ready_head = NULL;

/* Idle loop entry point for per-CPU idle procs.
 * When need_resched is set (by timer or IPI handler), call sched_yield
 * to pick up the next ready process.  This is the safe point for
 * preemption in the idle loop — follows the XNU/Linux pattern where
 * the idle loop checks the reschedule flag after each interrupt. */
static void __attribute__((noreturn)) idle_loop(void) {
    for (;;) {
        cpu_data_t *cpu = this_cpu();
        if (cpu->need_resched) {
            cpu->need_resched = 0;
            sched_yield();
        }
        __asm__ volatile("hlt");
    }
}

/* Scheduler spinlock — protects the process table and ready queue. */
static spinlock_t sched_lock = SPINLOCK_INIT;

/* Forward decl — proc_sleep calls this while holding sched_lock. */
static void __sched_yield_locked(uint64_t rflags);

/* Deferred preemption check — called from syscall and interrupt return.
 * Like XNU's pending AST and Linux's TIF_NEED_RESCHED, the request remains
 * set while preemption is disabled.  Dropping it loses the wakeup entirely:
 * once the critical section ends there may be no later timer/IPI to retry. */
void sched_check_resched(void) {
    cpu_data_t *cpu = this_cpu();
    if (!cpu->need_resched) return;
    proc_t *cur = proc_current();
    if (cur && cur->no_preempt) return;
    cpu->need_resched = 0;
    sched_yield();
}

/* Wake up expired sleepers without context-switching.
 * Called from timer handlers to ensure sleeping processes are woken up
 * promptly.  With deferred preemption, the timer handler no longer calls
 * sched_yield_try() (which called pick_next_ready and woke sleepers).
 * Without this, sleepers would only be woken up when another process
 * voluntarily yields, causing NSLEEP to hang.  This function walks the
 * ready queue and marks expired sleepers as READY, but does NOT
 * context-switch — the actual switch happens at the next safe point
 * (syscall return, idle loop). */
void sched_wake_sleepers(void) {
    uint64_t rflags;
    if (!spinlock_try_irqsave(&sched_lock, &rflags))
        return;
    uint64_t now = timer_ticks();
    int woke = 0;
    for (proc_t *p = ready_head; p; p = p->next) {
        if (p->state == PROC_BLOCKED && p->sleep_until &&
            now >= p->sleep_until) {
            p->state = PROC_READY;
            p->sleep_until = 0;
            woke = 1;
        }
    }
    spinlock_release_irqrestore(&sched_lock, rflags);
    /* The run queue is shared, so one local scheduler is sufficient to
     * dispatch a woken task.  Broadcasting a reschedule request makes every
     * CPU contend for sched_lock at once on each 1 ms sleep expiry, which
     * turns interactive input into a scheduler-lock storm. */
    if (woke) {
        this_cpu()->need_resched = 1;
    }
}

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
    /* procs[] was already zeroed and reaped by sched_early_init().
     * AP idle procs were already allocated during smp_init() — don't clobber them. */
    procs[0].pid = 0;
    procs[0].state = PROC_RUNNING;
    procs[0].pml4_phys = (uint64_t)vmm_get_cr3();
    procs[0].pml4_virt = vmm_kernel_pml4();
    procs[0].ring3 = false;
    /* Allocate a dedicated kstack for the BSP idle proc. */
    procs[0].kstack = kmalloc(SCHED_STACK_SIZE);
    *(uint64_t *)procs[0].kstack = 0x0BADF00DDEADBEEFULL;  /* stack canary */
    procs[0].canary = 0xDEADBEEFCAFEBABEULL;
    uint64_t *ksp = (uint64_t *)(procs[0].kstack + SCHED_STACK_SIZE);
    ksp--;
    *ksp = 0;  /* dummy slot — discarded by context_switch's addq $8 */
    procs[0].rsp = (uint64_t)ksp;
    procs[0].saved_ret = (uint64_t)idle_loop;
    procs[0].rip = (uint64_t)idle_loop;
    this_cpu()->current_proc = &procs[0];
    this_cpu()->idle_proc = &procs[0];
    ready_head = NULL;
}

/* Early init — just marks proc slots as free.
 * Called before smp_init so APs can allocate idle procs. */
void sched_early_init(void) {
    memset(procs, 0, sizeof(procs));
    for (int i = 0; i < SCHED_MAX_PROCS; i++)
        procs[i].reaped = true;
}

proc_t *proc_current(void) {
    return (proc_t *)this_cpu()->current_proc;
}

proc_t *proc_by_pid(uint64_t pid) {
    for (int i = 0; i < SCHED_MAX_PROCS; i++) {
        if (procs[i].pid == pid &&
            (procs[i].state != PROC_DEAD || !procs[i].reaped))
            return &procs[i];
    }
    return NULL;
}

void proc_set_current(proc_t *p) {
    this_cpu()->current_proc = p;
}

void sched_adopt_current(proc_t *p) {
    uint64_t rflags = spinlock_acquire_irqsave(&sched_lock);
    p->state = PROC_RUNNING;
    this_cpu()->current_proc = p;
    ready_dequeue(p);
    spinlock_release_irqrestore(&sched_lock, rflags);
}

proc_t *proc_create(uint64_t entry, uint64_t pml4_phys, uint64_t *pml4_virt,
                    uint8_t *kstack) {
    uint64_t rflags = spinlock_acquire_irqsave(&sched_lock);
    for (int i = 1; i < SCHED_MAX_PROCS; i++) {
        if (procs[i].state == PROC_DEAD && procs[i].reaped) {
            procs[i].pid = (uint64_t)(i);
            procs[i].rip = entry;
            procs[i].pml4_phys = pml4_phys;
            procs[i].pml4_virt = pml4_virt;
            procs[i].kstack = kstack;
            procs[i].rsp = (uint64_t)(kstack + SCHED_STACK_SIZE - 8);
            /* Stack canary at the very bottom of the kernel stack.
             * Checked in timer_tick / sched_yield to detect overflow. */
            if (kstack) {
                uint64_t *canary = (uint64_t *)kstack;
                *canary = 0x0BADF00DDEADBEEFULL;
            }
            procs[i].sleep_until = 0;
            procs[i].ring3 = false;
            procs[i].parent_pid = 0;
            procs[i].exit_code = 0;
            procs[i].name[0] = '\0';
            procs[i].fork_rbx = procs[i].fork_rbp = 0;
            procs[i].fork_r12 = procs[i].fork_r13 = 0;
            procs[i].fork_r14 = procs[i].fork_r15 = 0;
            procs[i].fork_rflags = 0;
            procs[i].saved_rflags = 0;
            procs[i].sig_blocked = 0;
            procs[i].sig_pending = 0;
            for (int s = 0; s < XOS_NSIG; s++) {
                procs[i].sig_handler[s] = 0;
                procs[i].sig_mask[s] = 0;
                procs[i].sig_flags[s] = 0;
            }
            /* Leave state=PROC_DEAD, reaped=false so the slot is reserved
             * but not visible to the scheduler. Caller must proc_make_ready(). */
            procs[i].reaped = false;
            procs[i].exiting = 0;
            procs[i].canary = 0xDEADBEEFCAFEBABEULL;
            procs[i].no_preempt = 0;
            procs[i].switching = 0;
            spinlock_release_irqrestore(&sched_lock, rflags);
            return &procs[i];
        }
    }
    spinlock_release_irqrestore(&sched_lock, rflags);
    return NULL;
}

/* Mark a process as ready to run and notify other CPUs. */
void proc_make_ready(proc_t *p) {
    uint64_t rflags = spinlock_acquire_irqsave(&sched_lock);
    p->state = PROC_READY;
    ready_enqueue(p);
    spinlock_release_irqrestore(&sched_lock, rflags);
    sched_notify_new_proc();
}

void proc_exit(proc_t *p) {
    if (!p) return;

    /* Atomically claim the exit operation using the exiting flag. */
    uint64_t rflags = spinlock_acquire_irqsave(&sched_lock);
    if (p->exiting) {
        spinlock_release_irqrestore(&sched_lock, rflags);
        return;
    }
    p->exiting = 1;
    p->state = PROC_DEAD;
    p->reaped = false;
    spinlock_release_irqrestore(&sched_lock, rflags);

    kprintf("[proc] exit: pid=%lu code=%d parent=%lu\n",
            p->pid, p->exit_code, p->parent_pid);

    /* Notify parent (SIGCHLD) before tearing down. */
    if (p->parent_pid)
        proc_send_signal(p->parent_pid, XOS_SIGCHLD);

    /* Free process resources: user page tables + all mapped user pages.
     * CRITICAL: only switch CR3 if we are the CPU currently running this
     * process.  Switching CR3 on a remote CPU corrupts its address space. */
    if (p->pml4_virt && p == proc_current()) {
        uint64_t kernel_cr3 = virt_to_phys(vmm_kernel_pml4());
        __asm__ volatile("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
        vmm_destroy_user(p->pml4_virt);
        pmm_free_frame(p->pml4_phys);
        p->pml4_virt = NULL;
        p->pml4_phys = 0;
    }
    if (p->kstack && p != proc_current()) {
        kfree(p->kstack);
        p->kstack = NULL;
    }

    rflags = spinlock_acquire_irqsave(&sched_lock);
    ready_dequeue(p);
    p->next = NULL;
    spinlock_release_irqrestore(&sched_lock, rflags);
}

/* Kill a process by PID. */
void proc_kill(uint64_t pid) {
    proc_t *p = proc_by_pid(pid);
    if (!p || p->pid == 0) return;

    uint64_t rflags = spinlock_acquire_irqsave(&sched_lock);
    if (p->exiting) {
        spinlock_release_irqrestore(&sched_lock, rflags);
        return;
    }
    if (p == proc_current()) {
        spinlock_release_irqrestore(&sched_lock, rflags);
        proc_exit(p);
        sched_yield();
        for (;;) __asm__ volatile("cli; hlt");
    }
    /* If the target is RUNNING on another CPU, mark it DEAD and send an IPI;
     * the target CPU's sched_yield will see the DEAD state and call proc_exit. */
    if (p->state == PROC_RUNNING) {
        p->state = PROC_DEAD;
        p->exiting = 0;
        p->reaped = false;
        spinlock_release_irqrestore(&sched_lock, rflags);
        if (g_smp_enabled)
            lapic_send_ipi_all_others(IPI_VECTOR_RESCHED);
        return;
    }
    /* Target is READY or BLOCKED — safe to remove and exit here. */
    ready_dequeue(p);
    spinlock_release_irqrestore(&sched_lock, rflags);
    proc_exit(p);
}

void proc_sleep(uint64_t ms) {
    proc_t *cur = proc_current();
    if (!cur || cur->pid == 0) {
        timer_sleep_ms(ms);
        return;
    }
    /* Convert ms to global ticks.  g_global_ticks increments at
     * timer_ticks_hz() ticks/sec (1000 BSP + 250 per AP), so
     * ticks = ms * hz / 1000.  Without this, NSLEEP(1) on an 8-CPU
     * system would sleep only ~0.36ms instead of 1ms, causing the
     * composer to run at ~2750 FPS and flood the GPU cursor queue. */
    uint64_t hz = timer_ticks_hz();
    uint64_t ticks = (ms * hz + 999) / 1000;  /* round up */
    if (ticks == 0) ticks = 1;
    uint64_t rflags = spinlock_acquire_irqsave(&sched_lock);
    cur->sleep_until = timer_ticks() + ticks;
    cur->state = PROC_BLOCKED;
    /* Blocked tasks stay on the ready list so yield can wake them.
     * CRITICAL: hold sched_lock across sched_yield_locked so that
     * another CPU can't wake+pick this process before context_switch
     * has saved its rsp/rip. */
    ready_enqueue(cur);
    __sched_yield_locked(rflags);
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
        /* Skip processes whose context hasn't been saved yet (another
         * CPU is in the middle of context_switching away from them). */
        if ((*npp)->state == PROC_READY && !(*npp)->switching) {
            proc_t *next = *npp;
            *npp = next->next;
            next->next = NULL;
            return next;
        }
        npp = &(*npp)->next;
    }

    /* Recovery: READY but not linked (should not happen). */
    for (int i = 1; i < SCHED_MAX_PROCS; i++) {
        if (procs[i].state == PROC_READY && !procs[i].switching)
            return &procs[i];
    }
    return (proc_t *)this_cpu()->current_proc; /* idle — stay on current */
}

/* Internal scheduler yield — assumes sched_lock is already held (irqsave). */
static void __sched_yield_locked(uint64_t rflags) {
    cpu_data_t *cpu = this_cpu();
    proc_t *cur = (proc_t *)cpu->current_proc;

    if (!cur) {
        spinlock_release_irqrestore(&sched_lock, rflags);
        return;
    }

    /* If a remote CPU marked us DEAD via proc_kill, clean up locally. */
    if (cur->state == PROC_DEAD && cur->pid != 0) {
        spinlock_release_irqrestore(&sched_lock, rflags);
        proc_exit(cur);
        rflags = spinlock_acquire_irqsave(&sched_lock);
    }

    proc_t *next = pick_next_ready();

    /* If the current process is still RUNNING and pick_next_ready
     * returned it (no other READY process), just continue. */
    if (next == cur && cur->state == PROC_RUNNING) {
        spinlock_release_irqrestore(&sched_lock, rflags);
        return;
    }

    /* No real process ready — idle.
     * If the current process is BLOCKED, context-switch to the idle proc. */
    if (next->pid == 0 || next == cur) {
        if (cur->state == PROC_BLOCKED && cur->pid != 0) {
            proc_t *idle = (proc_t *)cpu->idle_proc;
            if (idle && idle != cur) {
                idle->state = PROC_RUNNING;
                idle->next = NULL;
                cpu->current_proc = idle;
                if (idle->kstack)
                    cpu_set_rsp0((uint64_t)(idle->kstack + SCHED_STACK_SIZE));
                else
                    cpu_set_rsp0(cpu->tss_rsp0);
                /* Mark cur as switching so other CPUs don't pick it
                 * before context_switch saves its state.  Release the
                 * lock BEFORE context_switch to avoid blocking all
                 * other CPUs' timers during the CR3 switch. */
                cur->switching = 1;
                cur->saved_rflags = rflags;
                spinlock_release(&sched_lock);
                context_switch(cur, idle);
                /* We're now on idle's stack (or cur resumed later).
                 * context_switch already cleared from->switching.
                 * Just restore interrupts. */
                cpu = this_cpu();
                rflags = ((proc_t *)cpu->current_proc)->saved_rflags;
                if (rflags & (1 << 9)) __asm__ volatile("sti");
                return;
            }
        }
        spinlock_release_irqrestore(&sched_lock, rflags);
        return;
    }

    /* Switch from cur to next. */
    if (cur->state == PROC_RUNNING) {
        if (cur->pid != 0) {
            cur->state = PROC_READY;
            ready_enqueue(cur);
        }
    }

    next->state = PROC_RUNNING;
    next->next = NULL;
    cpu->current_proc = next;

    if (next->ring3) {
        cpu_set_rsp0((uint64_t)(next->kstack + SCHED_STACK_SIZE));
    } else {
        cpu_set_rsp0(cpu->tss_rsp0);
    }

    /* Mark cur as switching so other CPUs don't pick it before
     * context_switch saves its rsp/rip.  Release sched_lock BEFORE
     * context_switch to avoid blocking all other CPUs' timer
     * interrupts during the CR3 switch + register restore. */
    cur->switching = 1;
    cur->saved_rflags = rflags;
    spinlock_release(&sched_lock);
    context_switch(cur, next);
    /* We're now on next's stack, in next's context.
     * context_switch already cleared from->switching.
     * Just restore interrupts.  sched_lock is NOT held. */
    cpu = this_cpu();
    rflags = ((proc_t *)cpu->current_proc)->saved_rflags;
    if (rflags & (1 << 9)) __asm__ volatile("sti");
}

void sched_yield(void) {
    uint64_t rflags = spinlock_acquire_irqsave(&sched_lock);
    __sched_yield_locked(rflags);
}

/* Try to yield without blocking.  Returns true if we yielded, false if
 * sched_lock was held by another CPU.  Used by timer interrupt handlers
 * to avoid freezing all timer ticks when another CPU is context-switching. */
bool sched_yield_try(void) {
    uint64_t rflags;
    if (!spinlock_try_irqsave(&sched_lock, &rflags))
        return false;
    __sched_yield_locked(rflags);
    return true;
}

/* Release sched_lock without restoring IRQ state.
 * Called by ring3_trampoline when context_switch returns to a newly
 * scheduled process (which doesn't go through sched_yield's release path). */
void sched_release_lock(void) {
    spinlock_release(&sched_lock);
}

/* Send a reschedule IPI to all other CPUs. */
void sched_notify_new_proc(void) {
    if (!g_smp_enabled) return;
    lapic_send_ipi_all_others(IPI_VECTOR_RESCHED);
}

/* Check all process kernel stack canaries.  Called from the BSP timer
 * tick to detect stack overflows early (before they corrupt proc_t or
 * cause mysterious page faults in the syscall return path). */
void sched_check_canaries(void) {
    for (int i = 0; i < SCHED_MAX_PROCS; i++) {
        if (procs[i].state == PROC_DEAD) continue;
        if (!procs[i].kstack) continue;
        uint64_t *canary = (uint64_t *)procs[i].kstack;
        if (*canary != 0x0BADF00DDEADBEEFULL) {
            kprintf("[PANIC] kernel stack overflow detected! pid=%lu kstack=%p "
                    "canary=%lx (expected %lx)\n",
                    procs[i].pid, (void *)procs[i].kstack,
                    *canary, 0x0BADF00DDEADBEEFULL);
            for (;;) __asm__ volatile("cli; hlt");
        }
        /* Also check proc_t canary */
        if (procs[i].canary != 0xDEADBEEFCAFEBABEULL) {
            kprintf("[PANIC] proc_t corruption detected! pid=%lu canary=%lx\n",
                    procs[i].pid, procs[i].canary);
            for (;;) __asm__ volatile("cli; hlt");
        }
    }
}

/* Called from context.S when to->saved_ret is not in kernel higher-half.
 * rdi = bad saved_ret, rsi = to proc_t pointer.
 * This catches stack/proc_t corruption before the CPU jumps to garbage. */
void context_switch_panic(uint64_t bad_ret, proc_t *to) {
    uint64_t krsp0;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(krsp0));
    kprintf("\n*** CONTEXT SWITCH PANIC ***\n");
    kprintf("    bad saved_ret=%lx to=%p pid=%lu\n", bad_ret, to, to ? to->pid : 0);
    if (to) {
        kprintf("    to->rsp=%lx rip=%lx state=%d ring3=%d\n",
                to->rsp, to->rip, to->state, to->ring3);
        kprintf("    to->kstack=%lx-%lx canary=%lx switching=%d\n",
                (uint64_t)to->kstack, (uint64_t)to->kstack + SCHED_STACK_SIZE,
                to->canary, to->switching);
    }
    kprintf("    krsp0=%lx\n", krsp0);
    /* Dump kstack top of the target process */
    if (to && to->kstack) {
        uint64_t *ktop = (uint64_t *)((uint64_t)to->kstack + SCHED_STACK_SIZE);
        kprintf("    kstack top:\n");
        for (int i = -10; i < 0; i++)
            kprintf("      ktop%d: %lx\n", i*8, ktop[i]);
    }
    for (;;) __asm__ volatile("cli; hlt");
}

/* Initialize scheduler state for an AP.
 * Creates a per-CPU idle proc (kernel-space, PID 0) so the AP has a
 * valid current_proc for context_switch. */
void sched_init_ap(void) {
    cpu_data_t *cpu = this_cpu();
    uint64_t rflags = spinlock_acquire_irqsave(&sched_lock);
    for (int i = 1; i < SCHED_MAX_PROCS; i++) {
        if (procs[i].state == PROC_DEAD && procs[i].reaped) {
            procs[i].pid = 0;
            procs[i].state = PROC_RUNNING;
            procs[i].pml4_phys = (uint64_t)vmm_get_cr3();
            procs[i].pml4_virt = vmm_kernel_pml4();
            procs[i].ring3 = false;
            procs[i].reaped = true;
            procs[i].kstack = kmalloc(SCHED_STACK_SIZE);
            *(uint64_t *)procs[i].kstack = 0x0BADF00DDEADBEEFULL;  /* canary */
            *(uint64_t *)(procs[i].kstack + SCHED_STACK_SIZE - 32) = 0xCAFEBABEDEADC0DEULL;
            procs[i].canary = 0xDEADBEEFCAFEBABEULL;
            uint64_t *ksp = (uint64_t *)(procs[i].kstack + SCHED_STACK_SIZE);
            ksp--;
            *ksp = 0;
            procs[i].rsp = (uint64_t)ksp;
            procs[i].saved_ret = (uint64_t)idle_loop;
            procs[i].rip = (uint64_t)idle_loop;
            cpu->current_proc = &procs[i];
            cpu->idle_proc = &procs[i];
            cpu_set_rsp0((uint64_t)(procs[i].kstack + SCHED_STACK_SIZE));
            spinlock_release_irqrestore(&sched_lock, rflags);
            return;
        }
    }
    spinlock_release_irqrestore(&sched_lock, rflags);
    kprintf("[sched] AP %u: no free proc slot for idle task!\n",
            cpu->cpu_id);
}
