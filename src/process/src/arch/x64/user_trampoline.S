// RBX points to the complete user context preserved on the kernel stack.
// Enter through context_switch so the scheduler's bootstrap context is saved.
.section .text
.global user_iret_trampoline
.type user_iret_trampoline, @function
user_iret_trampoline:
    movq %rbx, %rdi
    movq 56(%rbx), %rsi
    jmp switch_to_user
.size user_iret_trampoline, .-user_iret_trampoline
.section .note.GNU-stack,"",@progbits
