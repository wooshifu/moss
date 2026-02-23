// =============================================================================
// x86_64 System Call Entry Point
// =============================================================================
//
// This file implements the low-level system call entry/exit path for x86_64.
//
// Execution flow:
//   1. User-space executes `syscall` → CPU loads RIP from LSTAR MSR (points
//      here), saves user RIP in RCX and RFLAGS in R11, switches to ring 0.
//   2. syscall_entry_point saves all general-purpose registers onto the
//      kernel stack, remaps arguments from the syscall ABI to the System V
//      AMD64 C calling convention, and calls system_call_handler().
//   3. On return, the result (in RAX) is patched into the saved frame,
//      registers are restored, and execution returns to user-space.
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
//
// Stack frame layout after saving all registers (RSP-relative offsets):
//   +0x00 : R11 (user RFLAGS)
//   +0x08 : RCX (user RIP)
//   +0x10 : R9  (arg5)
//   +0x18 : R8  (arg4)
//   +0x20 : R10 (arg3)
//   +0x28 : RDX (arg2/a1)
//   +0x30 : RSI (arg1/a0)
//   +0x38 : RDI (arg0/user a0)
//   +0x40 : RAX (syscall number)
//   +0x48 : R15 ... R12, RBX  (callee-saved)
//   +0x70 : RBP (frame pointer)
//
// NOTE: Currently uses `ret` instead of `sysret` because the kernel runs
//       all code in ring 0.  A future user-space implementation should
//       switch to SYSRET for proper privilege-level return.
// =============================================================================

.section .text

.global syscall_entry_point
.global syscall_return

// =============================================================================
// syscall_entry_point — system call handler for x86_64
// =============================================================================
.type syscall_entry_point, @function
syscall_entry_point:
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
    //  Read saved values from stack (offsets from current RSP):
    movq    72(%rsp), %rdi              // C arg0 (nr)   ← saved RAX
    movq    64(%rsp), %rsi              // C arg1 (a0)   ← saved RDI
    movq    56(%rsp), %rdx              // C arg2 (a1)   ← saved RSI
    movq    48(%rsp), %rcx              // C arg3 (a2)   ← saved RDX
    movq    40(%rsp), %r8               // C arg4 (a3)   ← saved R10
    movq    32(%rsp), %r9               // C arg5 (a4)   ← saved R8

    // C arg6 (a5) passed on the stack per System V ABI
    pushq   24(%rsp)                    // push saved R9 (arg5)

    // ---- Call the C system call dispatcher ----
    call    system_call_handler

    // ---- Clean up stack arg (arg5) ----
    addq    $8, %rsp

    // ---- Patch return value into saved RAX slot ----
    movq    %rax, 72(%rsp)

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

    // ---- Return to caller ----
    // Uses RET while all code runs in ring 0.  Switching to SYSRET requires:
    //   1. GDT reorganization: SYSRET computes CS = STAR[63:48]+16, SS = STAR[63:48]+8
    //      which needs user CS/DS at specific GDT offsets (Linux uses a 6-entry GDT)
    //   2. Ring 3 user-space support with separate address spaces
    // When ready: restore RCX (user RIP) and R11 (user RFLAGS), then `sysretq`.
    ret
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

// Prevent executable stack warning from the GNU linker
.section .note.GNU-stack,"",@progbits
