// =============================================================================
// RISC-V 64-bit Trap Entry Point
// =============================================================================
//
// Unified trap handler for RISC-V S-mode.  All traps (interrupts and
// exceptions) vector here via stvec (direct mode).
//
// Dispatch logic (based on scause):
//   bit 63 = 1 → Interrupt:
//     cause & 0x7F == 5  → S-mode timer interrupt → riscv_timer_handler()
//     cause & 0x7F == 9  → S-mode external IRQ    → riscv_external_handler()
//     other              → spurious, ignore
//   bit 63 = 0 → Exception:
//     cause == 8 or 9    → ecall (U/S-mode)       → system_call_handler()
//     other              → fatal exception         → riscv_exception_handler()
//
// RISC-V system call ABI (Linux-compatible):
//   a7 (x17) = system call number
//   a0 - a5  = arguments 0 - 5
//   Return value delivered in a0
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
//   +0xE0  : sepc           — saved exception PC
// =============================================================================

.section .text

.global syscall_entry_point
.global syscall_return

// =============================================================================
// syscall_entry_point — unified trap handler for RISC-V
// =============================================================================
.type syscall_entry_point, @function
syscall_entry_point:
    // ---- Allocate 256-byte trap frame on kernel stack ----
    addi    sp, sp, -256

    // ---- Save minimal registers for scause dispatch ----
    sd      ra, 0(sp)
    sd      t0, 8(sp)
    sd      t1, 16(sp)
    sd      t2, 24(sp)

    // ---- Read scause to determine trap type ----
    csrr    t0, scause

    // ---- Save sepc (needed for all paths) ----
    csrr    t1, sepc
    sd      t1, 224(sp)                 // +0xE0: saved sepc

    // Check bit 63: interrupt (1) vs exception (0)
    bltz    t0, .Ltrap_interrupt

    // ---- Exception path ----
    // Check for ecall from U-mode (8) or S-mode (9)
    li      t1, 8
    beq     t0, t1, .Ltrap_ecall
    li      t1, 9
    beq     t0, t1, .Ltrap_ecall

    // Unknown exception — call C handler and halt
    // a0 = scause, a1 = sepc, a2 = stval
    mv      a0, t0
    ld      a1, 224(sp)
    csrr    a2, stval
    call    riscv_exception_handler
    // Should not return — halt
    j       .Ltrap_halt

// ---- Interrupt path ----
.Ltrap_interrupt:
    // Mask off bit 63 to get interrupt cause number
    slli    t1, t0, 1
    srli    t1, t1, 1                   // t1 = cause & 0x7FFFFFFFFFFFFFFF

    // S-mode timer interrupt (cause == 5)
    li      t2, 5
    beq     t1, t2, .Ltrap_timer

    // S-mode external interrupt (cause == 9)
    li      t2, 9
    beq     t1, t2, .Ltrap_external

    // Other interrupt — spurious, restore and return
    j       .Ltrap_irq_return

// ---- Timer interrupt ----
.Ltrap_timer:
    // Save remaining caller-saved regs (callee-saved regs are preserved by C)
    sd      t3, 32(sp)
    sd      t4, 40(sp)
    sd      t5, 48(sp)
    sd      t6, 56(sp)
    sd      a0, 64(sp)
    sd      a1, 72(sp)
    sd      a2, 80(sp)
    sd      a3, 88(sp)
    sd      a4, 96(sp)
    sd      a5, 104(sp)
    sd      a6, 112(sp)
    sd      a7, 120(sp)

    call    riscv_timer_handler

    // Restore caller-saved regs
    ld      a7, 120(sp)
    ld      a6, 112(sp)
    ld      a5, 104(sp)
    ld      a4, 96(sp)
    ld      a3, 88(sp)
    ld      a2, 80(sp)
    ld      a1, 72(sp)
    ld      a0, 64(sp)
    ld      t6, 56(sp)
    ld      t5, 48(sp)
    ld      t4, 40(sp)
    ld      t3, 32(sp)
    j       .Ltrap_irq_return

// ---- External interrupt ----
.Ltrap_external:
    sd      t3, 32(sp)
    sd      t4, 40(sp)
    sd      t5, 48(sp)
    sd      t6, 56(sp)
    sd      a0, 64(sp)
    sd      a1, 72(sp)
    sd      a2, 80(sp)
    sd      a3, 88(sp)
    sd      a4, 96(sp)
    sd      a5, 104(sp)
    sd      a6, 112(sp)
    sd      a7, 120(sp)

    call    riscv_external_handler

    ld      a7, 120(sp)
    ld      a6, 112(sp)
    ld      a5, 104(sp)
    ld      a4, 96(sp)
    ld      a3, 88(sp)
    ld      a2, 80(sp)
    ld      a1, 72(sp)
    ld      a0, 64(sp)
    ld      t6, 56(sp)
    ld      t5, 48(sp)
    ld      t4, 40(sp)
    ld      t3, 32(sp)
    j       .Ltrap_irq_return

// ---- Interrupt return (sret) ----
.Ltrap_irq_return:
    // Restore sepc and minimal regs, then sret
    ld      t1, 224(sp)
    csrw    sepc, t1
    ld      t2, 24(sp)
    ld      t1, 16(sp)
    ld      t0, 8(sp)
    ld      ra, 0(sp)
    addi    sp, sp, 256
    sret

// ---- Ecall (syscall) path ----
.Ltrap_ecall:
    // Save ALL registers for full syscall frame
    sd      t3, 32(sp)
    sd      t4, 40(sp)
    sd      t5, 48(sp)
    sd      t6, 56(sp)
    sd      a0, 64(sp)
    sd      a1, 72(sp)
    sd      a2, 80(sp)
    sd      a3, 88(sp)
    sd      a4, 96(sp)
    sd      a5, 104(sp)
    sd      a6, 112(sp)
    sd      a7, 120(sp)
    sd      s0, 128(sp)
    sd      s1, 136(sp)
    sd      s2, 144(sp)
    sd      s3, 152(sp)
    sd      s4, 160(sp)
    sd      s5, 168(sp)
    sd      s6, 176(sp)
    sd      s7, 184(sp)
    sd      s8, 192(sp)
    sd      s9, 200(sp)
    sd      s10, 208(sp)
    sd      s11, 216(sp)

    // Remap RISC-V syscall ABI → C calling convention
    ld      a0, 120(sp)                 // C arg0 (nr)   ← saved a7
    ld      a1, 64(sp)                  // C arg1 (a0)   ← saved a0
    ld      a2, 72(sp)                  // C arg2 (a1)   ← saved a1
    ld      a3, 80(sp)                  // C arg3 (a2)   ← saved a2
    ld      a4, 88(sp)                  // C arg4 (a3)   ← saved a3
    ld      a5, 96(sp)                  // C arg5 (a4)   ← saved a4
    ld      a6, 104(sp)                 // C arg6 (a5)   ← saved a5

    call    system_call_handler

    // Patch return value into saved a0 slot
    sd      a0, 64(sp)

    // Restore all registers
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
    ld      a7, 120(sp)
    ld      a6, 112(sp)
    ld      a5, 104(sp)
    ld      a4, 96(sp)
    ld      a3, 88(sp)
    ld      a2, 80(sp)
    ld      a1, 72(sp)
    ld      a0, 64(sp)
    ld      t6, 56(sp)
    ld      t5, 48(sp)
    ld      t4, 40(sp)
    ld      t3, 32(sp)
    ld      t2, 24(sp)
    ld      t1, 16(sp)
    ld      t0, 8(sp)
    ld      ra, 0(sp)

    addi    sp, sp, 256

    // Return via ret (all code in S-mode currently)
    ret

// ---- Halt loop for fatal exceptions ----
.Ltrap_halt:
    wfi
    j       .Ltrap_halt
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
// Input:   a0 = pointer to SyscallContext (may be nullptr)
// Output:  a0 = context->ret_value, or -1 on nullptr
// =============================================================================
.type syscall_return, @function
syscall_return:
    beqz    a0, .return_error           // nullptr guard

    ld      a1, 56(a0)                  // a1 = context->ret_value
    mv      a0, a1                      // return value in a0
    ret

.return_error:
    li      a0, -1                      // error sentinel
    ret
.size syscall_return, . - syscall_return
