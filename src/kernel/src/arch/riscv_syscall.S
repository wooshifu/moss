// =============================================================================
// RISC-V 64-bit System Call Entry Point
// =============================================================================
//
// This file implements the low-level system call entry/exit path for RV64.
//
// Execution flow:
//   1. User-space executes `ecall` → CPU traps to S-mode, SCAUSE = 8
//      (Environment call from U-mode).
//   2. The trap vector routes the exception here.
//   3. syscall_entry_point allocates a 256-byte trap frame on the kernel
//      stack, saves ALL 32 general-purpose registers plus ra, remaps
//      arguments from the RISC-V syscall ABI to the standard calling
//      convention, and calls system_call_handler().
//   4. On return, the result (in a0) is patched into the saved frame,
//      registers are restored, and execution returns to user-space.
//
// RISC-V system call ABI (Linux-compatible):
//   a7 (x17) = system call number
//   a0 - a5  = arguments 0 - 5
//   Return value delivered in a0
//
// C handler prototype:
//   extern "C" int64_t system_call_handler(
//       uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
//       uint64_t a3, uint64_t a4, uint64_t a5);
//
// Trap frame layout (256 bytes, SP-relative offsets):
//   +0x00  : ra   (x1)      — return address
//   +0x08  : t0   (x5)      — temporary registers
//   +0x10  : t1   (x6)
//   +0x18  : t2   (x7)
//   +0x20  : t3   (x28)
//   +0x28  : t4   (x29)
//   +0x30  : t5   (x30)
//   +0x38  : t6   (x31)
//   +0x40  : a0   (x10)     — arg0 / return value slot
//   +0x48  : a1   (x11)     — arg1
//   +0x50  : a2   (x12)     — arg2
//   +0x58  : a3   (x13)     — arg3
//   +0x60  : a4   (x14)     — arg4
//   +0x68  : a5   (x15)     — arg5
//   +0x70  : a6   (x16)
//   +0x78  : a7   (x17)     — syscall number
//   +0x80  : s0/fp (x8)     — callee-saved registers
//   +0x88  : s1   (x9)
//   +0x90  : s2   (x18)
//   ...
//   +0xD8  : s11  (x27)
//
// NOTE: Currently uses `ret` instead of `sret` because the kernel runs
//       all code in S-mode.  A future user-space implementation should
//       switch to SRET for proper privilege-level return.
// =============================================================================

.section .text

.global syscall_entry_point
.global syscall_return

// =============================================================================
// syscall_entry_point — system call handler for RISC-V
// =============================================================================
syscall_entry_point:
    // ---- Allocate 256-byte trap frame on kernel stack ----
    addi    sp, sp, -256

    // ---- Save caller-saved (temporary) registers ----
    sd      ra, 0(sp)                   // x1  — return address
    sd      t0, 8(sp)                   // x5  — temporary
    sd      t1, 16(sp)                  // x6
    sd      t2, 24(sp)                  // x7
    sd      t3, 32(sp)                  // x28
    sd      t4, 40(sp)                  // x29
    sd      t5, 48(sp)                  // x30
    sd      t6, 56(sp)                  // x31

    // ---- Save argument registers (a0-a7) ----
    sd      a0, 64(sp)                  // x10 — arg0 / return value slot
    sd      a1, 72(sp)                  // x11 — arg1
    sd      a2, 80(sp)                  // x12 — arg2
    sd      a3, 88(sp)                  // x13 — arg3
    sd      a4, 96(sp)                  // x14 — arg4
    sd      a5, 104(sp)                 // x15 — arg5
    sd      a6, 112(sp)                 // x16 — (unused for syscalls)
    sd      a7, 120(sp)                 // x17 — syscall number

    // ---- Save callee-saved registers (s0-s11) ----
    sd      s0, 128(sp)                 // x8  — frame pointer / s0
    sd      s1, 136(sp)                 // x9
    sd      s2, 144(sp)                 // x18
    sd      s3, 152(sp)                 // x19
    sd      s4, 160(sp)                 // x20
    sd      s5, 168(sp)                 // x21
    sd      s6, 176(sp)                 // x22
    sd      s7, 184(sp)                 // x23
    sd      s8, 192(sp)                 // x24
    sd      s9, 200(sp)                 // x25
    sd      s10, 208(sp)                // x26
    sd      s11, 216(sp)                // x27

    // ---- Remap RISC-V syscall ABI → standard C calling convention ----
    //
    //  C prototype: system_call_handler(nr, a0, a1, a2, a3, a4, a5)
    //               registers:          a0  a1  a2  a3  a4  a5  a6
    //
    //  Syscall ABI: a7=nr, a0=arg0, a1=arg1, a2=arg2, a3=arg3, a4=arg4, a5=arg5
    //
    ld      a0, 120(sp)                 // C arg0 (nr)   ← saved a7
    ld      a1, 64(sp)                  // C arg1 (a0)   ← saved a0
    ld      a2, 72(sp)                  // C arg2 (a1)   ← saved a1
    ld      a3, 80(sp)                  // C arg3 (a2)   ← saved a2
    ld      a4, 88(sp)                  // C arg4 (a3)   ← saved a3
    ld      a5, 96(sp)                  // C arg5 (a4)   ← saved a4
    ld      a6, 104(sp)                 // C arg6 (a5)   ← saved a5

    // ---- Call the C system call dispatcher ----
    call    system_call_handler

    // ---- Patch return value into saved a0 slot ----
    sd      a0, 64(sp)

    // ---- Restore callee-saved registers (s11 → s0, reverse order) ----
    ld      s11, 216(sp)
    ld      s10, 208(sp)
    ld      s9, 200(sp)
    ld      s8, 192(sp)
    ld      s7, 184(sp)
    ld      s6, 176(sp)
    ld      s5, 168(sp)
    ld      s4, 160(sp)
    ld      s3, 152(sp)
    ld      s2, 144(sp)
    ld      s1, 136(sp)
    ld      s0, 128(sp)

    // ---- Restore argument registers ----
    ld      a7, 120(sp)                 // syscall number (for diagnostics)
    ld      a6, 112(sp)
    ld      a5, 104(sp)
    ld      a4, 96(sp)
    ld      a3, 88(sp)
    ld      a2, 80(sp)
    ld      a1, 72(sp)
    ld      a0, 64(sp)                  // return value (patched above)

    // ---- Restore temporary registers ----
    ld      t6, 56(sp)
    ld      t5, 48(sp)
    ld      t4, 40(sp)
    ld      t3, 32(sp)
    ld      t2, 24(sp)
    ld      t1, 16(sp)
    ld      t0, 8(sp)
    ld      ra, 0(sp)                   // return address

    // ---- Deallocate trap frame ----
    addi    sp, sp, 256

    // ---- Return to caller ----
    //      TODO: Replace with SRET when true user-space is implemented.
    //      SRET would atomically restore SSTATUS and branch to SEPC.
    ret

// =============================================================================
// syscall_return — extract return value from SyscallContext*
// =============================================================================
//
// Prototype: extern "C" void syscall_return(void* context) noexcept;
//
// SyscallContext layout (relevant field):
//   offset 56 (0x38): ret_value (uint64_t)
//
// Input:   a0 = pointer to SyscallContext (may be nullptr)
// Output:  a0 = context->ret_value, or -1 on nullptr
// =============================================================================
syscall_return:
    beqz    a0, .return_error           // nullptr guard

    ld      a1, 56(a0)                  // a1 = context->ret_value
    mv      a0, a1                      // return value in a0
    ret

.return_error:
    li      a0, -1                      // error sentinel
    ret
