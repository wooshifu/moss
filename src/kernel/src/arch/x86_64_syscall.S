// SYSCALL constructs the same native frame as IDT entry. IRETQ restores
// PC/flags independently of RCX/R11, including a signal-restored context.
#include "moss/trap_frame_offsets.h"
.section .text
.global syscall_entry_point
.type syscall_entry_point, @function
syscall_entry_point:
    movq %rsp, %gs:8
    movq %gs:0, %rsp
    pushq $0x1b
    pushq %gs:8
    pushq %r11
    pushq $0x23
    pushq %rcx
    pushq $0
    pushq $0
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    call system_call_handler
    cli
    movq %rsp, %rdi
    call user_return_handler
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq
.size syscall_entry_point, . - syscall_entry_point

.global syscall_return
.type syscall_return, @function
syscall_return:
    testq %rdi, %rdi
    jz .Lreturn_error
    movq 56(%rdi), %rax
    ret
.Lreturn_error:
    movq $-1, %rax
    ret
.size syscall_return, . - syscall_return

.section .bss
.align 8
.global g_kernel_rsp
g_kernel_rsp:
    .quad 0
.section .note.GNU-stack,"",@progbits
