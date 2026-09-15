// Read-only backing bytes for each process's RX signal-return page.
// A rejected signal frame terminates the caller; never execute padding.
.section .rodata
.balign 16
.global moss_sigreturn_start
.global moss_sigreturn_end
moss_sigreturn_start:
#if defined(MOSS_ARCH_ARM64)
    mov x8, #17
    svc #0
    mov x8, #1
    mov x0, #-14
    svc #0
    b .
#elif defined(MOSS_ARCH_RISCV64)
    li a7, 17
    ecall
    li a7, 1
    li a0, -14
    ecall
    j .
#elif defined(MOSS_ARCH_X64)
    movq $17, %rax
    syscall
    movq $1, %rax
    movq $-14, %rdi
    syscall
    jmp .
#endif
moss_sigreturn_end:
.section .note.GNU-stack,"",@progbits
