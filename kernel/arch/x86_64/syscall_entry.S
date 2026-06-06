.data
.globl g_kernel_rsp0
g_kernel_rsp0:
    .quad 0

.text
.globl syscall_entry
.align 16

/* void syscall_entry(void);
 * Invoked by the `syscall` instruction from ring-3.
 *
 * On entry:
 *   rax = syscall number
 *   rdi, rsi, rdx, r10, r8, r9 = arguments 1..6
 *   rcx = user rip (saved by syscall)
 *   r11 = user rflags (saved by syscall)
 *   rsp = user stack
 *   cs = 0x08 (kernel code), ss = 0x10 (kernel data)
 *
 * We switch to the kernel stack, save user state (rcx, r11, rsp) there
 * so the C dispatcher can't clobber them, build an arg frame, call C,
 * then restore and sysret.
 */
syscall_entry:
    /* Save callee-saved r15 on the user stack before anything else. */
    pushq %r15

    /* Switch to kernel stack, saving user state in fixed slots.
     * Layout at kernel_rsp0-24..kernel_rsp0:
     *   -24: user rflags (will be restored into r11)
     *   -16: user rip     (will be restored into rcx)
     *   -8:  user rsp     (will be restored into rsp)
     */
    movq g_kernel_rsp0(%rip), %r15
    subq $24, %r15
    movq %r11, 0(%r15)   /* save user rflags */
    movq %rcx, 8(%r15)   /* save user rip */
    movq %rsp, 16(%r15)  /* save user rsp (includes pushed r15) */
    movq %r15, %rsp      /* now on kernel stack */

    /* Build arg frame for C dispatcher (num + 6 args = 56 bytes).
     * Stack is 16-byte aligned before call (rsp = kernel_rsp0 - 80). */
    subq $56, %rsp
    movq %rax,  0(%rsp)   /* num */
    movq %rdi,  8(%rsp)   /* a1 */
    movq %rsi, 16(%rsp)   /* a2 */
    movq %rdx, 24(%rsp)   /* a3 */
    movq %r10, 32(%rsp)   /* a4 */
    movq %r8,  40(%rsp)   /* a5 */
    movq %r9,  48(%rsp)   /* a6 */

    movq %rsp, %rdi
    call syscall_dispatch

    /* RAX = return value for userspace.  Tear down arg frame. */
    addq $56, %rsp

    /* Restore user rflags, rip, and stack pointer; sysret */
    popq %r11            /* user rflags */
    popq %rcx            /* user rip */
    movq 0(%rsp), %rsp   /* user stack (points at pushed r15) */
    popq %r15            /* restore user r15 */
    sysretq
