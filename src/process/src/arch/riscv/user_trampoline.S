// =============================================================================
// RISC-V User SRET Trampoline — first-time entry to U-mode via context_switch
// =============================================================================
//
// This trampoline is the target of `ret` after context_switch restores a
// freshly-prepared CpuContext for a user task that has never entered U-mode
// (needs_initial_eret).
//
// By going through context_switch instead of calling switch_to_user directly,
// the caller's bootstrap context is properly saved.  This allows waitpid's
// context_switch(my_ctx, bootstrap) to correctly return to the idle loop.
//
// Callee-saved registers set by context_switch_to_task before context_switch:
//   s2 (x18) = user entry point (SEPC)
//   s3 (x19) = user stack pointer (SP)
//   s4 (x20) = user a0 value (arg0: fork=0, execve=argc)
//   s5 (x21) = user a1 value (arg1: fork=0, execve=argv)
//   s6 (x22) = 0 (reserved)
//
// Preconditions (set by context_switch_to_task's normal path):
//   - SATP already points to this process's page tables (if user task)
//   - IRQs are masked (context_switch path masks them)
// =============================================================================

.section .text

.global user_sret_trampoline
.type user_sret_trampoline, @function
user_sret_trampoline:
    // Configure sstatus for U-mode return: clear SPP bit (bit 8)
    // Also enable SPIE (bit 5) so interrupts are enabled after SRET
    li      t0, (1 << 5)                // SPIE=1, SPP=0 → U-mode
    csrw    sstatus, t0

    // Set exception return address to user entry point
    csrw    sepc, s2

    // Set user stack pointer
    mv      sp, s3

    // Set user arguments in the standard calling convention registers
    mv      a0, s4                      // arg0 (fork: 0, execve: argc)
    mv      a1, s5                      // arg1 (fork: 0, execve: argv)

    // Zero remaining user-visible GP registers for clean entry
    li      x1, 0                       // ra
    li      x3, 0                       // gp
    li      x4, 0                       // tp
    li      x5, 0                       // t0
    li      x6, 0                       // t1
    li      x7, 0                       // t2
    li      x8, 0                       // s0/fp
    li      x9, 0                       // s1
    li      x12, 0                      // a2
    li      x13, 0                      // a3
    li      x14, 0                      // a4
    li      x15, 0                      // a5
    li      x16, 0                      // a6
    li      x17, 0                      // a7
    li      x18, 0                      // s2
    li      x19, 0                      // s3
    li      x20, 0                      // s4
    li      x21, 0                      // s5
    li      x22, 0                      // s6
    li      x23, 0                      // s7
    li      x24, 0                      // s8
    li      x25, 0                      // s9
    li      x26, 0                      // s10
    li      x27, 0                      // s11
    li      x28, 0                      // t3
    li      x29, 0                      // t4
    li      x30, 0                      // t5
    li      x31, 0                      // t6

    // Atomically return to U-mode: sets PC=SEPC, privilege=U (SPP=0)
    sret

.size user_sret_trampoline, .-user_sret_trampoline

// Prevent executable stack warning from the GNU linker
.section .note.GNU-stack,"",@progbits
