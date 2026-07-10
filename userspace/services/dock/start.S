.text
.globl _start
.align 16

_start:
    call dock_main

    movq $0, %rdi
    movq $0, %rax
    syscall

1:  jmp 1b
