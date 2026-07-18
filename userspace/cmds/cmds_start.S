/* Entry point for cmds multi-call binary on x-os.
 * Kernel leaves SysV-style stack: [argc][argv…][NULL][envp NULL][auxv].
 * Load argc/argv into registers, then call cmds_main with rsp%16==0. */
    .text
    .globl _start
_start:
    movq (%rsp), %rdi        /* argc */
    leaq 8(%rsp), %rsi       /* argv */

    /* SysV: rsp must be 16-byte aligned *before* call (entry then has rsp%16==8). */
    andq $-16, %rsp
    call cmds_main

    /* libc exit flushes stdio so bridge sees ls/printf output */
    movq %rax, %rdi
    call exit

    /* Should never reach here */
1:  jmp 1b
