/* Entry point for cmds multi-call binary on x-os.
 * The kernel sets rsp with argc/argv per SysV ABI.
 * Adjust stack alignment then call cmds_main. */
    .text
    .globl _start
_start:
    andq $-16, %rsp       /* align to 16 bytes */
    subq $8, %rsp         /* simulate return address push (rsp%16==8) */
    call cmds_main

    /* cmds_main returned — exit with code 0 */
    movq $0, %rdi
    movq $0, %rax       /* SYS_EXIT */
    syscall

    /* Should never reach here */
1:  jmp 1b
