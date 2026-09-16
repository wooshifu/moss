// =============================================================================
// ARM64 Embedded Userspace Program — execve() Trampoline
// =============================================================================
//
// This is a minimal, self-contained user-mode program compiled directly into
// the kernel image.  It runs in EL0 (user mode) as the init process (PID=1).
//
// Execution flow:
//   1. sys_write(stdout, "init: calling execve...\n") — announce trampoline
//   2. sys_execve("/shell.elf", NULL, NULL) — replace process image with
//      the shell.elf program from initramfs CPIO archive
//   3. On failure: sys_write(stdout, error message) → sys_exit(1)
//
// ARM64 syscall convention (Linux-compatible):
//   x8      = syscall number
//   x0 - x5 = arguments 0 - 5
//   Return value in x0
//
// MOSS syscall numbers used here:
//   #1   sys_exit(status)
//   #11  sys_execve(pathname, argv, envp)
//   #33  sys_write(fd, buf, count)
//
// Linker symbols:
//   _user_program_start  — start of the embedded binary (used by ELF loader)
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
    mov     x8, #33                 // syscall number: sys_write
    mov     x0, #1                  // fd = STDOUT_FILENO
    adr     x1, .Lmsg_execve        // buf = address of message string
    // Label differences include the newline and exclude adjacent inline data.
    mov     x2, #(.Lmsg_execve_end - .Lmsg_execve)
    svc     #0                      // trap to EL1 → system_call_handler

    // ---- Step 2: sys_execve("/shell.elf", NULL, NULL) ----
    mov     x8, #11                 // syscall number: sys_execve
    adr     x0, .Lpath_shell        // pathname = "/shell.elf"
    mov     x1, #0                  // argv = NULL
    mov     x2, #0                  // envp = NULL
    svc     #0                      // trap to EL1 → sys_execve handler

    // ---- If execve returns, it failed ----
    // Save error code
    mov     x19, x0

    // Print error message
    mov     x8, #33                 // sys_write
    mov     x0, #1                  // fd = STDOUT
    adr     x1, .Lmsg_fail          // "init: execve failed!\n"
    mov     x2, #(.Lmsg_fail_end - .Lmsg_fail)
    svc     #0

    // ---- sys_exit(status=1) ----
    mov     x8, #1                  // syscall number: sys_exit
    mov     x0, #1                  // exit status = 1 (failure)
    svc     #0

    // ---- Infinite loop (unreachable — sys_exit never returns) ----
    b       .
.size _user_program_start, . - _user_program_start

// =============================================================================
// Read-only string data (placed inline to allow PC-relative addressing via ADR)
// =============================================================================
.Lmsg_execve:
    .ascii  "init: calling execve...\n"    // Includes newline, no NUL.
.Lmsg_execve_end:
.Lmsg_fail:
    .ascii  "init: execve failed!\n"       // Includes newline, no NUL.
.Lmsg_fail_end:
.Lpath_shell:
    .asciz  "/shell.elf"                   // NUL-terminated VFS path for execve

// =============================================================================
// End-of-program marker
// =============================================================================
.align 3                                // 8-byte alignment for clean boundaries
_user_program_end:
    .word   0                           // sentinel / alignment padding

// Prevent executable stack warning from the GNU linker
.section .note.GNU-stack,"",%progbits
