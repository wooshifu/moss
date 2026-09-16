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
//   3. Execute /busybox.elf as ash -i with the shell environment below.
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

// Native pointers occupy eight bytes on all supported 64-bit ISAs. argv has
// ash, -i and NULL; envp has five environment strings and NULL. Round their
// combined storage up to the 16-byte user-stack ABI alignment.
.equ .Lpointer_bytes, 8
.equ .Largv_bytes, 3 * .Lpointer_bytes
.equ .Lframe_bytes, (.Largv_bytes + 6 * .Lpointer_bytes + 15) & -16

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

    // Build writable pointer vectors using relocated user-mode string addresses.
    // Kernel-linked absolute pointer data would still point into kernel memory.
    sub     sp, sp, #.Lframe_bytes
    adr     x3, .Larg_ash
    str     x3, [sp, #0 * .Lpointer_bytes]
    adr     x3, .Larg_interactive
    str     x3, [sp, #1 * .Lpointer_bytes]
    str     xzr, [sp, #2 * .Lpointer_bytes] // NUL terminator
    adr     x3, .Lenv_path
    str     x3, [sp, #3 * .Lpointer_bytes]
    adr     x3, .Lenv_home
    str     x3, [sp, #4 * .Lpointer_bytes]
    adr     x3, .Lenv_term
    str     x3, [sp, #5 * .Lpointer_bytes]
    adr     x3, .Lenv_ps1
    str     x3, [sp, #6 * .Lpointer_bytes]
    adr     x3, .Lenv_ps2
    str     x3, [sp, #7 * .Lpointer_bytes]
    str     xzr, [sp, #8 * .Lpointer_bytes] // NUL terminator
    mov     x1, sp                  // argv = {"ash", "-i", NULL}
    add     x2, sp, #.Largv_bytes    // envp follows argv
    adr     x0, .Lpath_busybox
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
.Lpath_busybox:
    .asciz  "/busybox.elf"
.Larg_ash:
    .asciz  "ash"
.Larg_interactive:
    .asciz  "-i"
.Lenv_path:
    .asciz  "PATH=/"
.Lenv_home:
    .asciz  "HOME=/"
.Lenv_term:
    .asciz  "TERM=dumb"
.Lenv_ps1:
    .asciz  "PS1=moss$ "
.Lenv_ps2:
    .asciz  "PS2=> "

// =============================================================================
// End-of-program marker
// =============================================================================
.align 3                                // 8-byte alignment for clean boundaries
_user_program_end:
    .word   0                           // sentinel / alignment padding

// Prevent executable stack warning from the GNU linker
.section .note.GNU-stack,"",%progbits
