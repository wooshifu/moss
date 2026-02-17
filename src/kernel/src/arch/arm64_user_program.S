// Minimal embedded userspace program for ARM64 — fork() test
//
// This program runs in EL0 (user mode) and exercises the fork syscall:
//   1. sys_write(stdout, "Parent: about to fork\n", 22) — syscall #33
//   2. sys_fork() — syscall #10
//   3. Child (x0==0): sys_write("Hello from child!\n")  + sys_exit(0)
//      Parent (x0>0): sys_write("Hello from parent!\n") + sys_exit(0)
//
// ARM64 syscall convention:
//   x8  = syscall number
//   x0-x5 = arguments
//   Return value in x0

.section .text.user, "ax"

.global _user_program_start
.global _user_program_end

_user_program_start:
    // --- sys_write(fd=1, buf="Parent: about to fork\n", count=22) ---
    mov     x8, #33                 // syscall number: write
    mov     x0, #1                  // fd = stdout
    adr     x1, .Lmsg_before_fork
    mov     x2, #22                 // count
    svc     #0

    // --- sys_fork() ---
    mov     x8, #10                 // syscall number: fork
    mov     x0, #0
    mov     x1, #0
    mov     x2, #0
    mov     x3, #0
    mov     x4, #0
    mov     x5, #0
    svc     #0

    // x0 = fork return value: 0 for child, >0 (child PID) for parent
    cmp     x0, #0
    b.eq    .Lchild

    // --- Parent path ---
    mov     x8, #33
    mov     x0, #1
    adr     x1, .Lmsg_parent
    mov     x2, #20                 // "Hello from parent!\n" = 20 bytes
    svc     #0
    b       .Lexit

.Lchild:
    // --- Child path ---
    mov     x8, #33
    mov     x0, #1
    adr     x1, .Lmsg_child
    mov     x2, #19                 // "Hello from child!\n" = 19 bytes
    svc     #0

.Lexit:
    // --- sys_exit(status=0) ---
    mov     x8, #1
    mov     x0, #0
    svc     #0

    // Should never reach here
    b       .

.Lmsg_before_fork:
    .ascii  "Parent: about to fork\n"   // 22 bytes
.Lmsg_parent:
    .ascii  "Hello from parent!\n"      // 20 bytes (includes \n)
.Lmsg_child:
    .ascii  "Hello from child!\n"       // 19 bytes (includes \n)

.align 3
_user_program_end:
    .word   0                       // sentinel / alignment padding

// Prevent executable stack warning
.section .note.GNU-stack,"",%progbits
