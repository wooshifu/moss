// =============================================================================
// ARM64 Embedded Userspace Program — fork() Test
// =============================================================================
//
// This is a minimal, self-contained user-mode program compiled directly into
// the kernel image.  It runs in EL0 (user mode) to exercise the fork syscall.
//
// Execution flow:
//   1. sys_write(stdout, "Parent: about to fork\n", 22) — syscall #33
//   2. sys_fork()                                        — syscall #10
//   3. Branch on fork return value:
//      - Child  (x0 == 0): sys_write("Hello from child!\n")  → sys_exit(0)
//      - Parent (x0 >  0): sys_write("Hello from parent!\n") → sys_exit(0)
//
// ARM64 syscall convention (Linux-compatible):
//   x8      = syscall number
//   x0 - x5 = arguments 0 - 5
//   Return value in x0
//
// MOSS syscall numbers used here:
//   #1   sys_exit(status)
//   #10  sys_fork()
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

    // ---- Step 1: sys_write(fd=1, "Parent: about to fork\n", 22) ----
    mov     x8, #33                 // syscall number: sys_write
    mov     x0, #1                  // fd = STDOUT_FILENO
    adr     x1, .Lmsg_before_fork   // buf = address of message string
    mov     x2, #22                 // count = 22 bytes
    svc     #0                      // trap to EL1 → system_call_handler

    // ---- Step 2: sys_fork() ----
    mov     x8, #10                 // syscall number: sys_fork
    mov     x0, #0                  // clear argument registers (unused)
    mov     x1, #0
    mov     x2, #0
    mov     x3, #0
    mov     x4, #0
    mov     x5, #0
    svc     #0                      // trap to EL1 → sys_fork handler

    // ---- Step 3: Branch on fork return value ----
    //      x0 == 0  → we are the child process
    //      x0 >  0  → we are the parent (x0 = child PID)
    cmp     x0, #0
    b.eq    .Lchild

    // ---- Parent path: sys_write + sys_exit ----
    mov     x8, #33                 // sys_write
    mov     x0, #1                  // fd = STDOUT
    adr     x1, .Lmsg_parent        // "Hello from parent!\n"
    mov     x2, #20                 // 20 bytes including newline
    svc     #0
    b       .Lexit

.Lchild:
    // ---- Child path: sys_write + sys_exit ----
    mov     x8, #33                 // sys_write
    mov     x0, #1                  // fd = STDOUT
    adr     x1, .Lmsg_child         // "Hello from child!\n"
    mov     x2, #19                 // 19 bytes including newline
    svc     #0

.Lexit:
    // ---- sys_exit(status=0) ----
    mov     x8, #1                  // syscall number: sys_exit
    mov     x0, #0                  // exit status = 0 (success)
    svc     #0

    // ---- Infinite loop (unreachable — sys_exit never returns) ----
    b       .
.size _user_program_start, . - _user_program_start

// =============================================================================
// Read-only string data (placed inline to allow PC-relative addressing via ADR)
// =============================================================================
.Lmsg_before_fork:
    .ascii  "Parent: about to fork\n"   // 22 bytes
.Lmsg_parent:
    .ascii  "Hello from parent!\n"      // 20 bytes
.Lmsg_child:
    .ascii  "Hello from child!\n"       // 19 bytes

// =============================================================================
// End-of-program marker
// =============================================================================
.align 3                                // 8-byte alignment for clean boundaries
_user_program_end:
    .word   0                           // sentinel / alignment padding

// Prevent executable stack warning from the GNU linker
.section .note.GNU-stack,"",%progbits
