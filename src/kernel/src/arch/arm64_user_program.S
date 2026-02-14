// Minimal embedded userspace program for ARM64
//
// This program runs in EL0 (user mode) and exercises the syscall path:
//   1. sys_write(stdout, "Hello from userspace!\n", 22) — syscall #33
//   2. sys_exit(0) — syscall #1
//
// It is linked into the kernel binary and its address is used directly
// as the entry point for the init user process (TID=1000).
//
// ARM64 syscall convention:
//   x8  = syscall number
//   x0-x5 = arguments
//   Return value in x0

.section .text.user, "ax"

.global _user_program_start
.global _user_program_end

_user_program_start:
    // --- sys_write(fd=1, buf=msg, count=22) ---
    mov     x8, #33                 // syscall number: write (index 33)
    mov     x0, #1                  // fd = stdout
    adr     x1, .Luser_msg          // buf = address of message
    mov     x2, #22                 // count = 22 bytes
    svc     #0                      // trap to EL1

    // --- sys_exit(status=0) ---
    mov     x8, #1                  // syscall number: exit (index 1)
    mov     x0, #0                  // exit code = 0
    svc     #0                      // trap to EL1

    // Should never reach here (exit never returns)
    b       .

.Luser_msg:
    .ascii  "Hello from userspace!\n"

.align 3
_user_program_end:
    .word   0                       // sentinel / alignment padding

// Prevent executable stack warning
.section .note.GNU-stack,"",%progbits
