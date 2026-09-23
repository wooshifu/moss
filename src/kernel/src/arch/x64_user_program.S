// =============================================================================
// x64 Embedded Userspace Program — execve() Trampoline
// =============================================================================
//
// This is a minimal, self-contained user-mode program compiled directly into
// the kernel image.  It runs in ring 3 (user mode) as the init process (PID=1).
//
// Execution flow:
//   1. sys_write(stdout, "init: calling execve...\n") — announce trampoline
//   2. Try /validation.elf; only the validation initramfs contains it.
//      Only ENOENT falls through, so a broken validation image stays a failure.
//   3. Execute /init.elf, which supervises the production shell.
//   4. On launch failure, print the error and exit with a nonzero status.
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

    // The optional validation driver needs no argv/environment.
    movq    $11, %rax               // SYS_EXECVE
    leaq    .Lpath_validation(%rip), %rdi
    xorq    %rsi, %rsi              // argv = NULL
    xorq    %rdx, %rdx              // envp = NULL
    syscall
    cmpq    $-2, %rax               // ENOENT is returned as native errno -2.
    jne     .Llaunch_failed

    // Pass one relocated argv pointer; service policy lives in /init.elf.
    subq    $16, %rsp
    leaq    .Larg_init(%rip), %r8
    movq    %r8, (%rsp)
    movq    $0, 8(%rsp)
    movq    %rsp, %rsi
    xorq    %rdx, %rdx
    leaq    .Lpath_init(%rip), %rdi
    movq    $11, %rax               // SYS_EXECVE
    syscall

.Llaunch_failed:
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
.Lpath_validation:
    .asciz  "/validation.elf"
.Lpath_init:
    .asciz  "/init.elf"
.Larg_init:
    .asciz  "init"
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
