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

/* void enter_userspace(uint64_t pml4_phys, uint64_t rip, uint64_t rsp,
 *                      uint64_t ret_val);
 * First-time drop into ring-3.  Sets CR3, sets up IRETQ frame, and
 * transitions to privilege level 3. RAX is set to ret_val so fork()
 * children can return 0.
 */
.globl enter_userspace
enter_userspace:
    movq %rdi, %cr3         /* pml4_phys */

    /* Build IRETQ frame on current (kernel) stack.
     * Selectors must have RPL=3 so the CPU knows this is a ring-3 return. */
    pushq $0x23             /* SS = user data (index 4, RPL=3) */
    pushq %rdx              /* RSP = user stack pointer */
    pushfq
    popq %r8
    orq $0x200, %r8         /* enable interrupts */
    pushq %r8               /* RFLAGS with IF=1 */
    pushq $0x1B             /* CS = user code (index 3, RPL=3) */
    pushq %rsi              /* RIP = entry point */
    movq %rcx, %rax         /* set return value for user (0 for fork child) */
    iretq

/* void enter_userspace_fork(uint64_t pml4_phys, uint64_t rip, uint64_t rsp,
 *                           uint64_t ret_val, proc_t *child);
 * Args: rdi, rsi, rdx, rcx, r8
 * Restores fork_* callee-saved from child before iretq. */
.globl enter_userspace_fork
enter_userspace_fork:
    movq %rdi, %cr3         /* pml4_phys */

    /* Load callee-saved from proc_t (offsets after reaped bool).
     * Layout (see sched.h): fork_rbx at offset 88 if packed as:
     * pid(8)+state(4)+pad(4)+rsp(8)+rip(8)+pml4_phys(8)+pml4_virt(8)+
     * kstack(8)+sleep_until(8)+next(8)+ring3(1)+pad(7)+parent_pid(8)+
     * exit_code(4)+reaped(1)+pad(3)+fork_rbx...
     * Use a stable approach: pass child in r8 and read via known offsets
     * computed to match the C struct. */
    /* Offsets relative to proc_t* in r8 — keep in sync with sched.h */
    .equ PROC_FORK_RBX, 96
    .equ PROC_FORK_RBP, 104
    .equ PROC_FORK_R12, 112
    .equ PROC_FORK_R13, 120
    .equ PROC_FORK_R14, 128
    .equ PROC_FORK_R15, 136
    .equ PROC_FORK_RFLAGS, 144

    movq PROC_FORK_RBX(%r8), %rbx
    movq PROC_FORK_RBP(%r8), %rbp
    movq PROC_FORK_R12(%r8), %r12
    movq PROC_FORK_R13(%r8), %r13
    movq PROC_FORK_R14(%r8), %r14
    movq PROC_FORK_R15(%r8), %r15

    pushq $0x23             /* SS */
    pushq %rdx              /* RSP */
    movq PROC_FORK_RFLAGS(%r8), %rax
    testq %rax, %rax
    jne 1f
    pushfq
    popq %rax
    orq $0x200, %rax
1:
    pushq %rax              /* RFLAGS */
    pushq $0x1B             /* CS */
    pushq %rsi              /* RIP */
    movq %rcx, %rax         /* return value (0) */
    iretq
