// =============================================================================
// ARM64 Embedded Userspace Program — execve() Trampoline
// =============================================================================
//
// This is a minimal, self-contained user-mode program compiled directly into
// the kernel image.  It runs in EL0 (user mode) as the init process (PID=1).
//
// Execution flow:
//   1. sys_write(stdout, "init: calling execve...\n") — announce trampoline
//   2. Try /validation.elf; only the validation initramfs contains it.
//      Only ENOENT falls through, so a broken validation image stays a failure.
//   3. Execute /init.elf, which supervises the production shell.
//   4. On launch failure, print the error and exit with a nonzero status.
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

    // The optional validation driver needs no argv/environment.
    mov     x8, #11                 // SYS_EXECVE
    adr     x0, .Lpath_validation
    mov     x1, #0                  // argv = NULL
    mov     x2, #0                  // envp = NULL
    svc     #0
    cmn     x0, #2                  // ENOENT is returned as native errno -2.
    b.ne    .Llaunch_failed

    // Pass one relocated argv pointer; service policy lives in /init.elf.
    sub     sp, sp, #16
    adr     x3, .Larg_init
    str     x3, [sp]
    str     xzr, [sp, #8]
    mov     x1, sp
    mov     x2, #0
    adr     x0, .Lpath_init
    mov     x8, #11                 // SYS_EXECVE
    svc     #0

.Llaunch_failed:
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
.section .note.GNU-stack,"",%progbits
