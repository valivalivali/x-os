/* Entry point for zsh on x-os.
 * The kernel sets rsp to a 16-byte aligned address.
 * x86_64 ABI requires rsp%16==8 at function entry (as if a call pushed
 * a return address). We adjust rsp to match.
 */
    .text
    .globl _start
_start:
    andq $-16, %rsp       /* align to 16 bytes */
    subq $8, %rsp         /* simulate return address push (rsp%16==8) */
    call zsh_entry

    /* zsh_entry returned — exit with code 0 */
    movq $0, %rdi
    movq $0, %rax       /* SYS_EXIT */
    syscall

    /* Should never reach here */
1:  jmp 1b
