// =============================================================================
// x86_64 System Call Entry Point
// =============================================================================
//
// This file implements the low-level system call entry/exit path for x86_64.
//
// Execution flow:
//   1. User-space executes `syscall` → CPU loads RIP from LSTAR MSR (points
//      here), saves user RIP in RCX and RFLAGS in R11, switches to ring 0.
//   2. syscall_entry_point switches to the per-task kernel stack (from
//      g_kernel_rsp), saves all general-purpose registers, remaps arguments
//      from the syscall ABI to System V AMD64 C calling convention, and calls
//      system_call_handler().
//   3. On return, the result (in RAX) is patched into the saved frame,
//      registers are restored, user RSP is restored, and SYSRETQ returns
//      to ring 3 (using RCX=user RIP, R11=user RFLAGS).
//
// x86_64 system call ABI (Linux-compatible):
//   RAX       = system call number
//   RDI, RSI, RDX, R10, R8, R9 = arguments 0 - 5
//   Return value delivered in RAX
//
//   NOTE: The kernel uses RCX for arg2 in the C ABI, but user-space RCX is
//         clobbered by the SYSCALL instruction (it receives user RIP).
//         User-space therefore passes arg3 in R10 instead of RCX.
//
// C handler prototype:
//   extern "C" int64_t system_call_handler(
//       uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
//       uint64_t a3, uint64_t a4, uint64_t a5);
// =============================================================================

.section .text

.global syscall_entry_point
.global syscall_return

// =============================================================================
// syscall_entry_point — system call handler for x86_64
// =============================================================================
.type syscall_entry_point, @function
syscall_entry_point:
    // SYSCALL does NOT change RSP.  When entered from ring 3, RSP still
    // points to the user stack.  Switch to the per-task kernel stack.
    // g_kernel_rsp is set by the scheduler before dispatching a user task.
    movq    %rsp, g_user_rsp_save(%rip) // save user RSP
    movq    g_kernel_rsp(%rip), %rsp    // load kernel stack

    // ---- Establish stack frame ----
    pushq   %rbp
    movq    %rsp, %rbp

    // ---- Save callee-saved registers (System V ABI requires preservation) ----
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    // ---- Save syscall argument registers ----
    pushq   %rax                        // syscall number
    pushq   %rdi                        // arg0
    pushq   %rsi                        // arg1
    pushq   %rdx                        // arg2
    pushq   %r10                        // arg3 (R10, not RCX — see ABI note)
    pushq   %r8                         // arg4
    pushq   %r9                         // arg5

    // ---- Save registers clobbered by SYSCALL instruction ----
    pushq   %rcx                        // user RIP (saved by CPU)
    pushq   %r11                        // user RFLAGS (saved by CPU)

    // ---- Remap syscall ABI → System V AMD64 C calling convention ----
    //
    //  C prototype: system_call_handler(nr, a0, a1, a2, a3, a4, a5)
    //               registers:          RDI RSI RDX RCX R8  R9  [stack]
    //
    //  Syscall ABI: RAX=nr, RDI=a0, RSI=a1, RDX=a2, R10=a3, R8=a4, R9=a5
    //
    //  Read saved values from stack (offsets from current RSP after 15 pushes):
    //  [rsp+0]=R11, [rsp+8]=RCX, [rsp+16]=R9, [rsp+24]=R8, [rsp+32]=R10,
    //  [rsp+40]=RDX, [rsp+48]=RSI, [rsp+56]=RDI, [rsp+64]=RAX(nr),
    //  [rsp+72]=R15, [rsp+80]=R14, ...
    movq    64(%rsp), %rdi              // C arg0 (nr)   ← saved RAX
    movq    56(%rsp), %rsi              // C arg1 (a0)   ← saved RDI
    movq    48(%rsp), %rdx              // C arg2 (a1)   ← saved RSI
    movq    40(%rsp), %rcx              // C arg3 (a2)   ← saved RDX
    movq    32(%rsp), %r8               // C arg4 (a3)   ← saved R10
    movq    24(%rsp), %r9               // C arg5 (a4)   ← saved R8

    // C arg6 (a5) passed on the stack per System V ABI
    pushq   16(%rsp)                    // push saved R9 (arg5)

    // ---- Call the C system call dispatcher ----
    call    system_call_handler

    // ---- Clean up stack arg (arg5) ----
    addq    $8, %rsp

    // ---- Patch return value into saved RAX slot (at rsp+64 after 15 pushes) ----
    movq    %rax, 64(%rsp)

    // ---- Restore all registers (reverse of save order) ----
    popq    %r11                        // user RFLAGS
    popq    %rcx                        // user RIP
    popq    %r9
    popq    %r8
    popq    %r10
    popq    %rdx
    popq    %rsi
    popq    %rdi
    popq    %rax                        // return value (patched above)

    // ---- Restore callee-saved registers ----
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx

    // ---- Tear down stack frame ----
    popq    %rbp

    // ---- Restore user RSP and return to user-space ----
    movq    g_user_rsp_save(%rip), %rsp
    sysretq
.size syscall_entry_point, . - syscall_entry_point

// =============================================================================
// syscall_return — extract return value from SyscallContext*
// =============================================================================
//
// Prototype: extern "C" void syscall_return(void* context) noexcept;
//
// SyscallContext layout (relevant field):
//   offset 56 (0x38): ret_value (uint64_t)
//
// Input:   RDI = pointer to SyscallContext (may be nullptr)
// Output:  RAX = context->ret_value, or -1 on nullptr
// =============================================================================
.type syscall_return, @function
syscall_return:
    testq   %rdi, %rdi
    jz      .return_error

    movq    56(%rdi), %rax              // RAX = context->ret_value
    ret

.return_error:
    movq    $-1, %rax                   // error sentinel
    ret
.size syscall_return, . - syscall_return

// =============================================================================
// Per-CPU kernel stack pointer for SYSCALL entry (single-CPU for now)
// =============================================================================
.section .bss
.align 8
.global g_kernel_rsp
g_kernel_rsp:
    .quad 0                             // Set by scheduler before dispatching user task
g_user_rsp_save:
    .quad 0                             // Saved user RSP during syscall

// Prevent executable stack warning from the GNU linker
.section .note.GNU-stack,"",@progbits
