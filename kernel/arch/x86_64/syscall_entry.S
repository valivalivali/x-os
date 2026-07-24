.data

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
 *
 * Userspace expects Linux/SysV syscall ABI: only rax, rcx, and r11 are
 * clobbered. All other GPRs (including arg regs) must be preserved so
 * callers can keep pointers in rsi/rdx across the call.
 *
 * When delivering a signal, rdi is overwritten with the signo (handler
 * first argument); other preserved regs are still restored.
 */
syscall_entry:
    /* Save callee-saved r15 on the user stack before anything else. */
    pushq %r15

    /* Save user callee-saved GPRs in per-CPU storage for fork().
     * Must happen before any C code clobbers them.
     * GS:32=user_rbx, GS:40=user_rbp, GS:48=user_r12, GS:56=user_r13, GS:64=user_r14 */
    movq %rbx, %gs:32
    movq %rbp, %gs:40
    movq %r12, %gs:48
    movq %r13, %gs:56
    movq %r14, %gs:64

    /* Switch to kernel stack, saving user state in fixed slots.
     * Layout at kernel_rsp0-24..kernel_rsp0:
     *   -24: user rflags (will be restored into r11)
     *   -16: user rip     (will be restored into rcx)
     *   -8:  user rsp     (will be restored into rsp)
     */
    movq %gs:0, %r15
    subq $24, %r15
    movq %r11, 0(%r15)   /* save user rflags */
    movq %rcx, 8(%r15)   /* save user rip */
    movq %rsp, 16(%r15)  /* save user rsp (includes pushed r15) */
    movq %r15, %rsp      /* now on kernel stack */

    /* Preserve user arg regs + build C dispatcher frame.
     * Stack layout (112 bytes):
     *   0..48:  num,a1..a6 for syscall_dispatch
     *   56:     saved rdi
     *   64:     saved rsi
     *   72:     saved rdx
     *   80:     saved r10
     *   88:     saved r8
     *   96:     saved r9
     *   104:    pad / signo out later
     */
    subq $112, %rsp
    movq %rax,  0(%rsp)   /* num */
    movq %rdi,  8(%rsp)   /* a1 */
    movq %rsi, 16(%rsp)   /* a2 */
    movq %rdx, 24(%rsp)   /* a3 */
    movq %r10, 32(%rsp)   /* a4 */
    movq %r8,  40(%rsp)   /* a5 */
    movq %r9, 48(%rsp)   /* a6 */
    movq %rdi, 56(%rsp)   /* saved rdi */
    movq %rsi, 64(%rsp)   /* saved rsi */
    movq %rdx, 72(%rsp)   /* saved rdx */
    movq %r10, 80(%rsp)   /* saved r10 */
    movq %r8,  88(%rsp)   /* saved r8 */
    movq %r9, 96(%rsp)   /* saved r9 */

    movq %rsp, %rdi
    call syscall_dispatch
    /* rax = retval */

    /* Pop arg/save frame; keep saved regs via stack below ret frame. */
    movq 56(%rsp), %rdi
    movq 64(%rsp), %rsi
    movq 72(%rsp), %rdx
    movq 80(%rsp), %r10
    movq 88(%rsp), %r8
    movq 96(%rsp), %r9
    addq $112, %rsp       /* rsp -> ret frame */

    /* Stack: [signo][pad][rflags][rip][user_rsp]
     * Re-push saved arg regs below signo so the C call can clobber them;
     * we restore from this spill after signal_on_syscall_return.
     */
    subq $64, %rsp
    movq %rdi,  8(%rsp)   /* spill rdi */
    movq %rsi, 16(%rsp)   /* spill rsi */
    movq %rdx, 24(%rsp)   /* spill rdx */
    movq %r10, 32(%rsp)   /* spill r10 */
    movq %r8,  40(%rsp)   /* spill r8 */
    movq %r9, 48(%rsp)   /* spill r9 */
    movq %rax, 56(%rsp)   /* spill retval */
    movq $0, 0(%rsp)      /* signo out */

    movq %rsp, %rdx       /* &signo */
    leaq 64(%rsp), %rdi   /* &ret frame */
    movq 56(%rsp), %rsi   /* retval */
    call signal_on_syscall_return
    /* rax = retval */

    movq 0(%rsp), %rcx    /* signo (temp; rcx is clobbered by sysret anyway) */
    movq 8(%rsp), %rdi
    movq 16(%rsp), %rsi
    movq 24(%rsp), %rdx
    movq 32(%rsp), %r10
    movq 40(%rsp), %r8
    movq 48(%rsp), %r9
    /* rax already = retval */
    addq $64, %rsp

    testq %rcx, %rcx
    jz 1f
    movq %rcx, %rdi       /* delivering: handler arg1 = signo */
1:

    /* Restore user rflags, rip, and stack pointer; sysret */
    popq %r11             /* user rflags */
    popq %rcx             /* user rip (handler or original) */
    movq 0(%rsp), %rsp    /* user stack (points at pushed r15) */
    popq %r15             /* restore user r15 */
    sysretq
