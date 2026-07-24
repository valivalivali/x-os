.text
.globl context_switch
.align 16

/* void context_switch(proc_t *from, proc_t *to);
 * Save current context into 'from', restore from 'to', then jump to
 * to->saved_ret.
 *
 * Callee-saved registers and the return address are saved in the proc_t
 * struct (not on the kstack) so they can't be corrupted by IRETQ frames
 * pushed onto the kstack top while the process is in the ready queue.
 *
 * proc_t offsets (from sched.h):
 *   0:  pid      8:  state     16: rsp       24: rip
 *   32: pml4_phys  40: pml4_virt  48: kstack
 *   72: ring3
 *   888: saved_ret
 *   896: ctx_rbx  904: ctx_rbp  912: ctx_r12
 *   920: ctx_r13  928: ctx_r14  936: ctx_r15
 */
context_switch:
    /* Save return RIP (at top of stack from the call instruction) into
     * from->rip (offset 24) and from->saved_ret (offset 888). */
    movq (%rsp), %rax
    movq %rax, 24(%rdi)
    movq %rax, 888(%rdi)

    /* Save callee-saved registers into from->ctx_* (proc_t struct) */
    movq %rbx, 896(%rdi)
    movq %rbp, 904(%rdi)
    movq %r12, 912(%rdi)
    movq %r13, 920(%rdi)
    movq %r14, 928(%rdi)
    movq %r15, 936(%rdi)

    /* Save current RSP into from->rsp (offset 16).
     * RSP currently points to the return address from the call.
     * When 'from' is resumed, RSP will be restored here, and we jump
     * to saved_ret (not ret), so the stale return address on the stack
     * is harmless — it will be overwritten by subsequent pushes. */
    movq %rsp, 16(%rdi)

    /* Clear from->switching (offset 956) — from's context is now fully
     * saved, so other CPUs can safely pick it from the ready queue. */
    movl $0, 956(%rdi)

    /* Switch CR3 if pml4 changed (offset 32) and non-zero */
    movq 32(%rsi), %rax
    testq %rax, %rax
    je 2f
    movq 32(%rdi), %rcx
    cmpq %rax, %rcx
    je 2f
    movq %rax, %cr3
2:

    /* Update per-CPU RSP0 (GS:0 and TSS) for the 'to' process.
     * For ring-3 processes, use to->kstack + SCHED_STACK_SIZE.
     * For idle (non-ring-3), use idle's kstack or per-CPU TSS stack. */
    cmpb $0, 72(%rsi)
    je .L_idle_rsp0
    movq 48(%rsi), %rax          /* kstack base */
    addq $65536, %rax            /* kstack + SCHED_STACK_SIZE */
    movq %rax, %gs:0             /* update cpu->rsp0 (software shadow) */
    movq %gs:80, %rcx            /* tss pointer */
    testq %rcx, %rcx
    je 3f
    movq %rax, 4(%rcx)           /* tss->rsp0 = kstack_top */
    jmp 3f
.L_idle_rsp0:
    movq 48(%rsi), %rax          /* idle proc kstack base */
    testq %rax, %rax
    je .L_idle_tss_stack
    addq $65536, %rax
    jmp .L_idle_set_rsp0
.L_idle_tss_stack:
    movq %gs:112, %rax           /* cpu->tss_rsp0 */
.L_idle_set_rsp0:
    movq %rax, %gs:0
    movq %gs:80, %rcx
    testq %rcx, %rcx
    je 3f
    movq %rax, 4(%rcx)
3:

    /* Restore to->rsp */
    movq 16(%rsi), %rsp

    /* Restore callee-saved registers from to->ctx_* (proc_t struct) */
    movq 896(%rsi), %rbx
    movq 904(%rsi), %rbp
    movq 912(%rsi), %r12
    movq 920(%rsi), %r13
    movq 928(%rsi), %r14
    movq 936(%rsi), %r15

    /* Pop and discard the stale return address from the stack,
     * then jump to to->saved_ret.  The addq $8 is critical:
     * without it, RSP would be 8 bytes too low, causing the
     * caller's ret to pop the wrong return address. */
    addq $8, %rsp
    movq 888(%rsi), %rax       /* to->saved_ret */
    /* Validate saved_ret is in kernel higher-half code (0xffffffff80000000+).
     * A corrupted saved_ret would jump to a random address (e.g. the
     * kstack), causing a page fault from ring-0 with ip on the stack. */
    movq %rax, %rcx
    shrq $32, %rcx
    cmpl $0xffffffff, %ecx
    je .L_ctx_valid
    /* Bad saved_ret — halt with diagnostic */
    movq %rax, %rdi            /* bad saved_ret */
    movq %rsi, %rsi            /* to proc_t (already in rsi) */
    call context_switch_panic
.L_ctx_valid:
    jmpq *%rax

/* void enter_userspace(uint64_t pml4_phys, uint64_t rip, uint64_t rsp,
 *                      uint64_t ret_val);
 * First-time drop into ring-3.  Sets CR3, sets up IRETQ frame, and
 * transitions to privilege level 3. RAX is set to ret_val so fork()
 * children can return 0.
 */
.globl enter_userspace
enter_userspace:
    movq %rdi, %cr3         /* pml4_phys */

    pushq $0x23             /* SS = user data (index 4, RPL=3) */
    pushq %rdx              /* RSP = user stack pointer */
    pushfq
    popq %r8
    orq $0x200, %r8         /* enable interrupts */
    pushq %r8               /* RFLAGS with IF=1 */
    pushq $0x1B             /* CS = user code (index 3, RPL=3) */
    pushq %rsi              /* RIP = entry point */
    xorl %eax, %eax         /* set return value = 0 */
    iretq

/* void enter_userspace_fork(uint64_t pml4_phys, uint64_t rip, uint64_t rsp,
 *                           uint64_t ret_val, proc_t *child);
 * Args: rdi, rsi, rdx, rcx, r8
 * Restores fork_* callee-saved from child before iretq. */
.globl enter_userspace_fork
enter_userspace_fork:
    movq %rdi, %cr3         /* pml4_phys */

    .equ PROC_FORK_RBX, 96
    .equ PROC_FORK_RBP, 104
    .equ PROC_FORK_R12, 112
    .equ PROC_FORK_R13, 120
    .equ PROC_FORK_R14, 128
    .equ PROC_FORK_R15, 136
    .equ PROC_FORK_RFLAGS, 144
    .equ PROC_FORK_RDI, 152
    .equ PROC_FORK_RSI, 160
    .equ PROC_FORK_RDX, 168
    .equ PROC_FORK_R8,  176
    .equ PROC_FORK_R9,  184
    .equ PROC_FORK_R10, 192

    movq PROC_FORK_RBX(%r8), %rbx
    movq PROC_FORK_RBP(%r8), %rbp
    movq PROC_FORK_R12(%r8), %r12
    movq PROC_FORK_R13(%r8), %r13
    movq PROC_FORK_R14(%r8), %r14
    movq PROC_FORK_R15(%r8), %r15

    pushq $0x23             /* SS */
    pushq %rdx              /* RSP (user stack pointer) */
    movq PROC_FORK_RFLAGS(%r8), %rax
    testq %rax, %rax
    jne 1f
    pushfq
    popq %rax
    orq $0x200, %rax
1:
    pushq %rax              /* RFLAGS */
    pushq $0x1B             /* CS */
    pushq %rsi              /* RIP (user entry point) */
    /* Restore argument registers (SysV ABI: preserved across syscall).
     * Must happen AFTER pushing rdx/rsi for IRETQ frame above.
     * CRITICAL: r8 holds the proc_t pointer — restore r8 LAST. */
    movq PROC_FORK_RDI(%r8), %rdi
    movq PROC_FORK_RSI(%r8), %rsi
    movq PROC_FORK_RDX(%r8), %rdx
    movq PROC_FORK_R10(%r8), %r10
    movq PROC_FORK_R9(%r8),  %r9
    movq PROC_FORK_R8(%r8),  %r8
    xorl %eax, %eax         /* return value = 0 (fork child) */
    iretq
