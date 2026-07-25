.text

/* Assembly stubs for LAPIC timer and IPI vectors.
 * Vectors: 0xE0=resched, 0xE1=TLB, 0xE2=stop, 0xE3=call_func, 0xF0=timer
 * Each saves all GPRs, calls the C handler, sends LAPIC EOI, restores, iretq.
 *
 * Deferred preemption: after the handler returns, if we're returning to
 * userspace (CS=0x1B in IRETQ frame) and need_resched is set, call
 * sched_yield before restoring GPRs.  GPRs are still saved on the stack,
 * so sched_yield can clobber caller-saved regs safely.  This follows the
 * XNU AST / Linux TIF_NEED_RESCHED pattern.
 */

.macro LAPIC_STUB vec, handler
.align 16
.globl lapic_isr_\vec
lapic_isr_\vec:
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

    /* Send EOI *before* calling the handler.  If the handler calls
     * sched_yield() and context-switches to a ring-3 process via
     * ring3_trampoline → enter_userspace → iretq, the handler never
     * returns and a post-handler EOI would never execute, starving
     * the CPU of further interrupts. */
    call lapic_eoi

    call \handler

    /* Deferred preemption check — only when returning to userspace.
     * IRETQ frame is at rsp+120 (15 GPRs × 8 bytes).
     * CS is at rsp+128.  User CS = 0x1B.
     * Call sched_check_resched() which checks need_resched AND no_preempt. */
    cmpq $0x1B, 128(%rsp)
    jne .Llapic_no_resched_\vec
    call sched_check_resched    /* GPRs still saved on stack — safe */
.Llapic_no_resched_\vec:

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

LAPIC_STUB resched,   ipi_resched_handler
LAPIC_STUB tlb,       ipi_tlb_handler
LAPIC_STUB stop,      ipi_stop_handler
LAPIC_STUB call_func, ipi_call_func_handler
LAPIC_STUB timer,     lapic_timer_handler
