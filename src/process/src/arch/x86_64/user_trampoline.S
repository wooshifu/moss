// =============================================================================
// x86_64 User IRET Trampoline — first-time entry to ring 3 via context_switch
// =============================================================================
//
// This trampoline is the target of `ret` after context_switch restores a
// freshly-prepared CpuContext for a user task that has never entered ring 3
// (needs_initial_eret).
//
// By going through context_switch instead of calling switch_to_user directly,
// the caller's bootstrap context is properly saved.  This allows waitpid's
// context_switch(my_ctx, bootstrap) to correctly return to the idle loop.
//
// Callee-saved registers set by context_switch_to_task before context_switch:
//   RBX = user entry point (RIP for IRETQ)
//   R12 = user stack pointer (RSP for ring 3)
//   R13 = user RDI value (arg0: fork=0, execve=argc)
//   R14 = user RSI value (arg1: fork=0, execve=argv)
//   R15 = 0 (reserved)
//
// Preconditions (set by context_switch_to_task's normal path):
//   - CR3 already points to this process's page tables (if user task)
//   - IRQs are masked (context_switch path masks them)
// =============================================================================

.section .text

.global user_iret_trampoline
.type user_iret_trampoline, @function
user_iret_trampoline:
    // DEBUG: Add debug output before IRET
    pushq   %rax
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %rbx
    pushq   %r12

    // Call early_debug_print to log trampoline entry
    leaq    .Ltrampoline_msg(%rip), %rdi
    call    early_debug_print

    // Print the user entry point
    leaq    .Lentry_msg(%rip), %rdi
    call    early_debug_print

    popq    %r12
    popq    %rbx
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rax

    // Build IRETQ frame on the current kernel stack.
    // IRETQ pops: RIP, CS, RFLAGS, RSP, SS (bottom to top of stack)
    pushq   $0x23                       // SS  (user data segment, RPL=3)
    pushq   %r12                        // RSP (user stack pointer)
    pushq   $0x202                      // RFLAGS (IF=1, reserved bit 1 = 1)
    pushq   $0x1B                       // CS  (user code segment, RPL=3)
    pushq   %rbx                        // RIP (user entry point)

    // Set all data segment registers to user data segment
    movw    $0x23, %ax
    movw    %ax, %ds
    movw    %ax, %es

    // Set user arguments in the standard calling convention registers
    movq    %r13, %rdi                  // arg0 (fork: 0, execve: argc)
    movq    %r14, %rsi                  // arg1 (fork: 0, execve: argv)

    // Zero remaining user-visible GP registers for clean entry
    xorq    %rax, %rax
    xorq    %rbx, %rbx
    xorq    %rcx, %rcx
    xorq    %rdx, %rdx
    xorq    %rbp, %rbp
    xorq    %r8, %r8
    xorq    %r9, %r9
    xorq    %r10, %r10
    xorq    %r11, %r11
    xorq    %r12, %r12
    xorq    %r13, %r13
    xorq    %r14, %r14
    xorq    %r15, %r15

    // Atomically switch to ring 3: pops RIP, CS, RFLAGS, RSP, SS
    iretq

.size user_iret_trampoline, .-user_iret_trampoline

// Debug messages
.section .rodata
.Ltrampoline_msg:
    .asciz "[trampoline] DEBUG: entering user_iret_trampoline\n"
.Lentry_msg:
    .asciz "[trampoline] DEBUG: about to IRET to user mode\n"

// Prevent executable stack warning from the GNU linker
.section .note.GNU-stack,"",@progbits
