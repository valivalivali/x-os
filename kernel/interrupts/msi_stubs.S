.text

/* Assembly stubs for MSI-X interrupt vectors (0x40-0x5F).
 * Each stub saves all GPRs, sends LAPIC EOI, calls msi_dispatch_asm(vector),
 * checks for deferred preemption, restores GPRs, and iretq.
 *
 * This follows the same pattern as lapic_stubs.S — EOI before handler
 * so that a context switch inside the handler doesn't starve interrupts.
 */

.macro MSI_STUB idx
.align 16
.globl msi_isr_\idx
msi_isr_\idx:
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

    /* Send EOI before handler — same rationale as lapic_stubs.S */
    call lapic_eoi

    movq $(0x40 + \idx), %rdi
    call msi_dispatch_asm

    /* Deferred preemption check — only when returning to userspace.
     * IRETQ frame is at rsp+120 (15 GPRs × 8 bytes).
     * CS is at rsp+128.  User CS = 0x1B. */
    cmpq $0x1B, 128(%rsp)
    jne .Lmsi_no_resched_\idx
    call sched_check_resched
.Lmsi_no_resched_\idx:

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

MSI_STUB 0
MSI_STUB 1
MSI_STUB 2
MSI_STUB 3
MSI_STUB 4
MSI_STUB 5
MSI_STUB 6
MSI_STUB 7
MSI_STUB 8
MSI_STUB 9
MSI_STUB 10
MSI_STUB 11
MSI_STUB 12
MSI_STUB 13
MSI_STUB 14
MSI_STUB 15
MSI_STUB 16
MSI_STUB 17
MSI_STUB 18
MSI_STUB 19
MSI_STUB 20
MSI_STUB 21
MSI_STUB 22
MSI_STUB 23
MSI_STUB 24
MSI_STUB 25
MSI_STUB 26
MSI_STUB 27
MSI_STUB 28
MSI_STUB 29
MSI_STUB 30
MSI_STUB 31
