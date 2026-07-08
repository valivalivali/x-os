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
    call zsh_start_c

    .globl zsh_start_c
zsh_start_c:
    /* Now rsp is properly aligned for C code */
    /* Write welcome banner */
    subq $8, %rsp
    movl $28, %edx
    leaq msg_welcome(%rip), %rsi
    movl $1, %edi
    call write
    addq $8, %rsp
    /* Write prompt */
    subq $8, %rsp
    movl $4, %edx
    leaq msg_prompt(%rip), %rsi
    movl $1, %edi
    call write
    addq $8, %rsp
    /* Echo loop: read 1 char from stdin, write it back */
    subq $8, %rsp
1:
    /* read(0, buf, 1) */
    leaq buf(%rip), %rsi
    movl $1, %edx
    xorl %edi, %edi
    call read
    /* Check if read returned 0 */
    testl %eax, %eax
    jz 2f
    /* write(1, buf, 1) */
    leaq buf(%rip), %rsi
    movl $1, %edx
    movl $1, %edi
    call write
    /* Check for newline - echo it and write new prompt */
    movzbl buf(%rip), %eax
    cmpb $10, %al
    jne 1b
    /* Write prompt */
    leaq msg_prompt(%rip), %rsi
    movl $4, %edx
    movl $1, %edi
    call write
    jmp 1b
2:
    addq $8, %rsp
    /* Call zsh_main with -f */
    leaq argv(%rip), %rsi
    movl $2, %edi
    call zsh_main
    movl %eax, %edi
    call _exit
3:  hlt
    jmp 3b

    .section .bss
buf:
    .byte 0

    .section .rodata
msg_welcome:
    .asciz "x-os zsh shell ready\n"
msg_prompt:
    .asciz "x> "
argv:
    .quad argv0
    .quad argv1
    .quad 0
argv0:
    .asciz "zsh"
argv1:
    .asciz "-f"
