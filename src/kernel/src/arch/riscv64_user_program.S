// =============================================================================
// RISC-V 64 Embedded Userspace Program — execve() Trampoline
// =============================================================================
//
// This is a minimal, self-contained user-mode program compiled directly into
// the kernel image.  It runs in U-mode (user mode) as the init process (PID=1).
//
// Execution flow:
//   1. sys_write(stdout, "init: calling execve...\n") — announce trampoline
//   2. Try /validation.elf; only the validation initramfs contains it.
//      Only ENOENT falls through, so a broken validation image stays a failure.
//   3. Execute /init.elf, which supervises the production shell.
//   4. On launch failure, print the error and exit with a nonzero status.
//
// RISC-V 64 MOSS syscall convention:
//   a7 (x17) = syscall number
//   a0 - a5  = arguments 0 - 5
//   Invoke via ECALL instruction
//   Return value in a0
//
// MOSS syscall numbers used here:
//   #1   sys_exit(status)
//   #11  sys_execve(pathname, argv, envp)
//   #33  sys_write(fd, buf, count)
//
// Linker symbols:
//   _user_program_start  — start of the embedded binary
//   _user_program_end    — end of the embedded binary (used to compute size)
//
// The program occupies section .text.user so the linker script can place it
// at a known address separate from kernel code.
// =============================================================================

.section .text.user, "ax"

.global _user_program_start
.global _user_program_end

.type _user_program_start, @function
_user_program_start:

    // ---- Step 1: sys_write(fd=1, "init: calling execve...\n", message length) ----
    li      a7, 33                  // syscall number: sys_write
    li      a0, 1                   // fd = STDOUT_FILENO
    la      a1, .Lmsg_execve        // buf = address of message string
    // Label differences include the newline and exclude adjacent inline data.
    li      a2, (.Lmsg_execve_end - .Lmsg_execve)
    ecall                           // trap to S-mode → system_call_handler

    // The optional validation driver needs no argv/environment.
    li      a7, 11                  // SYS_EXECVE
    la      a0, .Lpath_validation
    li      a1, 0                   // argv = NULL
    li      a2, 0                   // envp = NULL
    ecall
    li      t0, -2                  // ENOENT is returned as native errno -2.
    bne     a0, t0, .Llaunch_failed

    // Pass one relocated argv pointer; service policy lives in /init.elf.
    addi    sp, sp, -16
    la      t0, .Larg_init
    sd      t0, 0(sp)
    sd      zero, 8(sp)
    mv      a1, sp
    li      a2, 0
    la      a0, .Lpath_init
    li      a7, 11                  // SYS_EXECVE
    ecall

.Llaunch_failed:
    // Print error message
    li      a7, 33                  // sys_write
    li      a0, 1                   // fd = STDOUT
    la      a1, .Lmsg_fail          // "init: execve failed!\n"
    li      a2, (.Lmsg_fail_end - .Lmsg_fail)
    ecall

    // ---- sys_exit(status=1) ----
    li      a7, 1                   // syscall number: sys_exit
    li      a0, 1                   // exit status = 1 (failure)
    ecall

    // ---- Infinite loop (unreachable — sys_exit never returns) ----
    j       .
.size _user_program_start, . - _user_program_start

// =============================================================================
// Read-only string data (placed in .text.user for PC-relative addressing via LA)
// =============================================================================
.Lmsg_execve:
    .ascii  "init: calling execve...\n"    // Includes newline, no NUL.
.Lmsg_execve_end:
.Lmsg_fail:
    .ascii  "init: execve failed!\n"       // Includes newline, no NUL.
.Lmsg_fail_end:
.Lpath_validation:
    .asciz  "/validation.elf"
.Lpath_init:
    .asciz  "/init.elf"
.Larg_init:
    .asciz  "init"

// =============================================================================
// End-of-program marker
// =============================================================================
.align 3                                // 8-byte alignment for clean boundaries
_user_program_end:
    .word   0                           // sentinel / alignment padding

// Prevent executable stack warning from the GNU linker
.section .note.GNU-stack,"",@progbits
