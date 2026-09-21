// Fault-contained byte copies. The kernel operand has no exception fixup:
// a kernel-buffer bug must not be mistaken for bad userspace input.
// Return the remaining byte count, including the faulting byte. Advance the
// count only after the user access succeeds so callers can report partial I/O.
.text
.balign 16
.global moss_raw_copy_from_user
.type moss_raw_copy_from_user, @function
moss_raw_copy_from_user:
#if defined(MOSS_ARCH_ARM64)
    cbz x2, .Lfrom_done
.Lfrom_loop:
.Lfrom_user:
    ldtrb w3, [x1]
    strb w3, [x0], #1
    add x1, x1, #1
    subs x2, x2, #1
    b.ne .Lfrom_loop
.Lfrom_done:
    mov x0, x2
    ret
#elif defined(MOSS_ARCH_RISCV64)
    // Raw callers must already permit S-mode user access (sstatus.SUM).
    // The normal exception entry enables it; these leaf helpers do not change
    // access state or provide a scope for boot-time callers.
    beqz a2, .Lfrom_done
.Lfrom_loop:
.Lfrom_user:
    lbu t0, 0(a1)
    sb t0, 0(a0)
    addi a0, a0, 1
    addi a1, a1, 1
    addi a2, a2, -1
    bnez a2, .Lfrom_loop
.Lfrom_done:
    mv a0, a2
    ret
#elif defined(MOSS_ARCH_X64)
    testq %rdx, %rdx
    jz .Lfrom_done
.Lfrom_loop:
.Lfrom_user:
    movb (%rsi), %al
    movb %al, (%rdi)
    incq %rdi
    incq %rsi
    decq %rdx
    jnz .Lfrom_loop
.Lfrom_done:
    movq %rdx, %rax
    ret
#endif
.size moss_raw_copy_from_user, . - moss_raw_copy_from_user

.balign 16
.global moss_raw_copy_to_user
.type moss_raw_copy_to_user, @function
moss_raw_copy_to_user:
#if defined(MOSS_ARCH_ARM64)
    cbz x2, .Lto_done
.Lto_loop:
    ldrb w3, [x1], #1
.Lto_user:
    sttrb w3, [x0]
    add x0, x0, #1
    subs x2, x2, #1
    b.ne .Lto_loop
.Lto_done:
    mov x0, x2
    ret
#elif defined(MOSS_ARCH_RISCV64)
    beqz a2, .Lto_done
.Lto_loop:
    lbu t0, 0(a1)
.Lto_user:
    sb t0, 0(a0)
    addi a0, a0, 1
    addi a1, a1, 1
    addi a2, a2, -1
    bnez a2, .Lto_loop
.Lto_done:
    mv a0, a2
    ret
#elif defined(MOSS_ARCH_X64)
    testq %rdx, %rdx
    jz .Lto_done
.Lto_loop:
    movb (%rsi), %al
.Lto_user:
    movb %al, (%rdi)
    incq %rdi
    incq %rsi
    decq %rdx
    jnz .Lto_loop
.Lto_done:
    movq %rdx, %rax
    ret
#endif
.size moss_raw_copy_to_user, . - moss_raw_copy_to_user

.section .rodata.moss_uaccess,"a"
// Each fixup is two signed 32-bit displacements, each relative to its own
// 4-byte field. This is the pair/stride consumed by abi::uaccess::fixup and
// needs no absolute pointers when firmware relocates the kernel image.
.balign 4
.global moss_uaccess_table_start
.global moss_uaccess_table_end
moss_uaccess_table_start:
    .long .Lfrom_user - .
    .long .Lfrom_done - .
    .long .Lto_user - .
    .long .Lto_done - .
moss_uaccess_table_end:
.section .note.GNU-stack,"",@progbits
