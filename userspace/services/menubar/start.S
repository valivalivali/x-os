.text
.globl _start
.align 16

/* X OS Menu Bar Service — ring-3 entry point */
_start:
    call menubar_main

    /* menubar_main returned — exit with code 0 */
    movq $0, %rdi
    movq $0, %rax       /* SYS_EXIT */
    syscall

    /* Should never reach here */
1:  jmp 1b
