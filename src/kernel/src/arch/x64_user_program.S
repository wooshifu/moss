// =============================================================================
// x64 Embedded Userspace Program — execve() Trampoline
// =============================================================================
//
// This is a minimal, self-contained user-mode program compiled directly into
// the kernel image.  It runs in ring 3 (user mode) as the init process (PID=1).
//
// Execution flow:
//   1. sys_write(stdout, "init: calling execve...\n") — announce trampoline
//   2. sys_execve("/shell.elf", NULL, NULL) — replace process image with
//      the shell.elf program from initramfs CPIO archive
//   3. On failure: sys_write(stdout, error prefix) → sys_exit(1)
//      A nonzero status lets the parent distinguish launch failure from success.
//
// x64 MOSS syscall convention:
//   RAX     = syscall number
//   RDI     = arg0
//   RSI     = arg1
//   RDX     = arg2
//   R10     = arg3
//   R8      = arg4
//   R9      = arg5
//   Invoke via SYSCALL instruction
//   Return value in RAX
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

    // ---- DEBUG: Skip I/O port access in user mode ----
    // outb instruction not allowed in user mode, skip debug output

    // ---- Step 1: sys_write(fd=1, "init: calling execve...\n", message length) ----
    movq    $33, %rax               // syscall number: sys_write
    movq    $1, %rdi                // fd = STDOUT_FILENO
    leaq    .Lmsg_execve(%rip), %rsi // buf = address of message string
    // Label differences exclude adjacent inline strings and alignment padding.
    movq    $(.Lmsg_execve_end - .Lmsg_execve), %rdx
    syscall                         // trap to ring 0 → system_call_handler

    // ---- Step 2: sys_execve("/shell.elf", NULL, NULL) ----
    movq    $11, %rax               // syscall number: sys_execve
    leaq    .Lpath_shell(%rip), %rdi // pathname = "/shell.elf"
    movq    $0, %rsi                // argv = NULL
    movq    $0, %rdx                // envp = NULL
    syscall                         // trap to ring 0 → sys_execve handler

    // ---- If execve returns, it failed ----
    // Save error code (rax contains return value)
    movq    %rax, %r8               // Save error code

    // sys_write(fd=1, "execve failed with error: ", message length)
    movq    $33, %rax               // syscall number: sys_write
    movq    $1, %rdi                // fd = STDOUT_FILENO
    leaq    .Lmsg_error(%rip), %rsi // buf = "execve failed with error: "
    movq    $(.Lmsg_error_end - .Lmsg_error), %rdx
    syscall

    // ---- execve failed: report the same nonzero status as ARM64/RV64 ----
    movq    $1, %rax                // syscall number: sys_exit
    movq    $1, %rdi                // exit status = 1 (launch failure)
    syscall

    // ---- Infinite loop (unreachable — sys_exit never returns) ----
    jmp     .
.size _user_program_start, . - _user_program_start

// =============================================================================
// Read-only string data (placed inline to allow RIP-relative addressing)
// =============================================================================
.Lmsg_execve:
    .ascii  "init: calling execve...\n"    // Includes newline, no NUL.
.Lmsg_execve_end:
.Lpath_shell:
    .asciz  "/shell.elf"                   // 11 bytes (includes NUL terminator)
.Lmsg_error:
    .ascii  "execve failed with error: "   // No newline or NUL.
.Lmsg_error_end:

// =============================================================================
// End-of-program marker
// =============================================================================
.align 8                                // 8-byte alignment for clean boundaries
_user_program_end:
    .long   0                           // sentinel / alignment padding

// Prevent executable stack warning from the GNU linker
.section .note.GNU-stack,"",@progbits
