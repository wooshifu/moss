// =============================================================================
// RISC-V 64 Embedded Userspace Program — execve() Trampoline
// =============================================================================
//
// This is a minimal, self-contained user-mode program compiled directly into
// the kernel image.  It runs in U-mode (user mode) as the init process (PID=1).
//
// Execution flow:
//   1. sys_write(stdout, "init: calling execve...\n") — announce trampoline
//   2. sys_execve("/shell.elf", NULL, NULL) — replace process image with
//      the shell.elf program from initramfs CPIO archive
//   3. On failure: sys_write(stdout, error message) → sys_exit(1)
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

    // ---- Step 1: sys_write(fd=1, "init: calling execve...\n", 25) ----
    li      a7, 33                  // syscall number: sys_write
    li      a0, 1                   // fd = STDOUT_FILENO
    la      a1, .Lmsg_execve        // buf = address of message string
    li      a2, 25                  // count = 25 bytes
    ecall                           // trap to S-mode → system_call_handler

    // ---- Step 2: sys_execve("/shell.elf", NULL, NULL) ----
    li      a7, 11                  // syscall number: sys_execve
    la      a0, .Lpath_shell        // pathname = "/shell.elf"
    li      a1, 0                   // argv = NULL
    li      a2, 0                   // envp = NULL
    ecall                           // trap to S-mode → sys_execve handler

    // ---- If execve returns, it failed ----
    // Save error code (a0 contains return value)
    mv      s0, a0

    // Print error message
    li      a7, 33                  // sys_write
    li      a0, 1                   // fd = STDOUT
    la      a1, .Lmsg_fail          // "init: execve failed!\n"
    li      a2, 22                  // 22 bytes
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
    .ascii  "init: calling execve...\n"    // 25 bytes (includes \n, no NUL)
.Lmsg_fail:
    .ascii  "init: execve failed!\n"       // 22 bytes (includes \n, no NUL)
.Lpath_shell:
    .asciz  "/shell.elf"                   // NUL-terminated VFS path for execve

// =============================================================================
// End-of-program marker
// =============================================================================
.align 3                                // 8-byte alignment for clean boundaries
_user_program_end:
    .word   0                           // sentinel / alignment padding

// Prevent executable stack warning from the GNU linker
.section .note.GNU-stack,"",@progbits
