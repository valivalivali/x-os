.text
.globl _start
.align 16

/* X OS Init — PID 1 entry point.
 * This is the first code executed when the kernel drops into ring-3.
 * It calls init_main(), then exits cleanly if that returns. */
_start:
    call init_main

    /* init_main returned — exit with code 0 */
    movq $0, %rdi       /* exit code */
    movq $0, %rax       /* SYS_EXIT */
    syscall

    /* Should never reach here */
1:  jmp 1b
