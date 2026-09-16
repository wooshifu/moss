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
//   3. Execute /busybox.elf as ash -i with the shell environment below.
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

// Native pointers occupy eight bytes on all supported 64-bit ISAs. argv has
// ash, -i and NULL; envp has five environment strings and NULL. Round their
// combined storage up to the 16-byte user-stack ABI alignment.
.equ .Lpointer_bytes, 8
.equ .Largv_bytes, 3 * .Lpointer_bytes
.equ .Lframe_bytes, (.Largv_bytes + 6 * .Lpointer_bytes + 15) & -16

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

    // Build writable pointer vectors using relocated user-mode string addresses.
    // Kernel-linked absolute pointer data would still point into kernel memory.
    addi    sp, sp, -.Lframe_bytes
    la      t0, .Larg_ash
    sd      t0, 0 * .Lpointer_bytes(sp)
    la      t0, .Larg_interactive
    sd      t0, 1 * .Lpointer_bytes(sp)
    sd      zero, 2 * .Lpointer_bytes(sp) // NUL terminator
    la      t0, .Lenv_path
    sd      t0, 3 * .Lpointer_bytes(sp)
    la      t0, .Lenv_home
    sd      t0, 4 * .Lpointer_bytes(sp)
    la      t0, .Lenv_term
    sd      t0, 5 * .Lpointer_bytes(sp)
    la      t0, .Lenv_ps1
    sd      t0, 6 * .Lpointer_bytes(sp)
    la      t0, .Lenv_ps2
    sd      t0, 7 * .Lpointer_bytes(sp)
    sd      zero, 8 * .Lpointer_bytes(sp) // NUL terminator
    mv      a1, sp                  // argv = {"ash", "-i", NULL}
    addi    a2, sp, .Largv_bytes     // envp follows argv
    la      a0, .Lpath_busybox
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
.section .note.GNU-stack,"",@progbits
