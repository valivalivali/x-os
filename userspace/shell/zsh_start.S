/* Entry point for zsh on x-os.
 * Kernel leaves rsp 16-byte aligned. SysV AMD64 ABI requires
 * rsp % 16 == 0 immediately before CALL so that after the return
 * address is pushed, the callee sees rsp % 16 == 8 (needed for
 * movaps / SSE in the Apple zsh 5.9 objects). */
    .text
    .globl _start
    .extern sigtrapped
_start:
    andq $-16, %rsp       /* rsp % 16 == 0 before call */

    /*
     * Some vendored zsh .o files were built with `extern int *sigtrapped`
     * while the definition is an array. Plant a self-pointer so those
     * loads see a valid base address (VSIGCOUNT ints fit past offset 0x84).
     */
    leaq sigtrapped(%rip), %rax
    movq %rax, (%rax)

    call zsh_entry

    /* zsh_entry returned — exit with code 0 */
    movq $0, %rdi
    movq $0, %rax       /* SYS_EXIT */
    syscall

    /* Should never reach here */
1:  jmp 1b
