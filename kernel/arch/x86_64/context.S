.text
.globl context_switch
.extern g_kernel_rsp0
.align 16

/* void context_switch(proc_t *from, proc_t *to);
 * Save current registers into 'from', restore from 'to', then ret.
 *
 * proc_t layout (must match sched.h):
 *   offset 0: pid     (unused here)
 *   offset 8: state   (unused here)
 *   offset 16: rsp
 *   offset 24: rip
 *   ...
 */
context_switch:
    /* Save return RIP (at top of stack from the call instruction) into from->rip
     * BEFORE we push any callee-saved registers. */
    movq (%rsp), %rax
    movq %rax, 24(%rdi)

    /* Save current callee-saved registers */
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Save current RSP into from->rsp (offset 16) */
    movq %rsp, 16(%rdi)

    /* Switch CR3 if pml4 changed (offset 32 in proc_t) and non-zero */
    movq 32(%rsi), %rax
    testq %rax, %rax
    je 2f
    movq 32(%rdi), %rcx
    cmpq %rax, %rcx
    je 2f
    movq %rax, %cr3
2:

    /* If switching to a ring-3 process (offset 72), update g_kernel_rsp0 */
    cmpb $0, 72(%rsi)
    je 3f
    movq 48(%rsi), %rax
    addq $65536, %rax
    movq %rax, g_kernel_rsp0(%rip)
3:

    /* Restore to->rsp */
    movq 16(%rsi), %rsp

    /* Restore callee-saved registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx

    ret

/* void enter_userspace(uint64_t pml4_phys, uint64_t rip, uint64_t rsp);
 * First-time drop into ring-3.  Sets CR3, sets up IRETQ frame, and
 * transitions to privilege level 3.
 */
.globl enter_userspace
enter_userspace:
    movq %rdi, %cr3         /* pml4_phys */

    /* Build IRETQ frame on current (kernel) stack.
     * Selectors must have RPL=3 so the CPU knows this is a ring-3 return. */
    pushq $0x23             /* SS = user data (index 4, RPL=3) */
    pushq %rdx              /* RSP = user stack pointer */
    pushfq
    popq %rax
    orq $0x200, %rax        /* enable interrupts */
    pushq %rax              /* RFLAGS with IF=1 */
    pushq $0x1B             /* CS = user code (index 3, RPL=3) */
    pushq %rsi              /* RIP = entry point */
    iretq
