.text
.globl irq_stubs_start

/* Assembly stubs for IRQ 0-15.
 * The CPU pushes an interrupt frame (RIP, CS, RFLAGS, RSP, SS from ring-3).
 * We save all GPRs, call irq_dispatch_asm(irq), restore, and iretq.
 *
 * Deferred preemption: after the handler returns, if we're returning to
 * userspace (CS=0x1B in IRETQ frame) and need_resched is set, call
 * sched_yield before restoring GPRs.  The GPRs are still saved on the
 * stack, so sched_yield (a C function) can clobber caller-saved regs
 * safely.  If sched_yield context-switches, this proc resumes here later,
 * pops GPRs, and iretq to the original userspace.  This follows the
 * XNU AST / Linux TIF_NEED_RESCHED pattern.
 */

.macro IRQ_STUB num
.align 16
.globl irq\num\()
irq\num\():
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq $\num, %rdi
    call irq_dispatch_asm

    /* Deferred preemption check — only when returning to userspace.
     * IRETQ frame is at rsp+120 (15 GPRs × 8 bytes).
     * CS is at rsp+128.  User CS = 0x1B.
     * Call sched_check_resched() which checks need_resched AND no_preempt. */
    cmpq $0x1B, 128(%rsp)
    jne .Lirq_no_resched_\num
    call sched_check_resched    /* GPRs still saved on stack — safe */
.Lirq_no_resched_\num:

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rax
    iretq
.endm

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

irq_stubs_start:
