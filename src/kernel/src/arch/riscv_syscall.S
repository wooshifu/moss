// =============================================================================
// RISC-V 64-bit Trap Entry Point
// =============================================================================
//
// Unified trap handler for RISC-V S-mode.  All traps (interrupts and
// exceptions) vector here via stvec (direct mode).
//
// Stack switching protocol (sscratch):
//   - In U-mode: sscratch holds the per-thread kernel stack pointer
//   - In S-mode: sscratch holds 0 (sentinel for "already on kernel stack")
//   - On trap entry from U-mode: csrrw swaps sp↔sscratch
//   - On trap entry from S-mode: sp is already the kernel stack, no swap
//
// Dispatch logic (based on scause):
//   bit 63 = 1 → Interrupt:
//     cause & 0x7F == 5  → S-mode timer interrupt → riscv_timer_handler()
//     cause & 0x7F == 9  → S-mode external IRQ    → riscv_external_handler()
//     other              → spurious, ignore
//   bit 63 = 0 → Exception:
//     cause == 8 or 9    → ecall (U/S-mode)       → system_call_handler()
//     cause == 12        → Instruction page fault  → riscv_page_fault_handler()
//     cause == 13        → Load page fault         → riscv_page_fault_handler()
//     cause == 15        → Store/AMO page fault    → riscv_page_fault_handler()
//     other              → fatal exception         → riscv_exception_handler()
//
// RISC-V system call ABI (Linux-compatible):
//   a7 (x17) = system call number
//   a0 - a5  = arguments 0 - 5
//   Return value delivered in a0
//
// Trap frame layout (288 bytes, SP-relative offsets):
//   +0x00  : ra   (x1)
//   +0x08  : t0   (x5)
//   +0x10  : t1   (x6)
//   +0x18  : t2   (x7)
//   +0x20  : t3   (x28)
//   +0x28  : t4   (x29)
//   +0x30  : t5   (x30)
//   +0x38  : t6   (x31)
//   +0x40  : a0   (x10)     — arg0 / return value slot
//   +0x48  : a1   (x11)
//   +0x50  : a2   (x12)
//   +0x58  : a3   (x13)
//   +0x60  : a4   (x14)
//   +0x68  : a5   (x15)
//   +0x70  : a6   (x16)
//   +0x78  : a7   (x17)     — syscall number
//   +0x80  : s0/fp (x8)
//   +0x88  : s1   (x9)
//   +0x90  : s2   (x18)
//   +0x98  : s3   (x19)
//   +0xA0  : s4   (x20)
//   +0xA8  : s5   (x21)
//   +0xB0  : s6   (x22)
//   +0xB8  : s7   (x23)
//   +0xC0  : s8   (x24)
//   +0xC8  : s9   (x25)
//   +0xD0  : s10  (x26)
//   +0xD8  : s11  (x27)
//   +0xE0  : sepc
//   +0xE8  : sstatus
//   +0xF0  : user sp (saved from sscratch on U-mode trap)
//   +0xF8  : scause (saved for nested dispatch)
//   +0x100 : (padding to 16-byte alignment → 272 bytes, round to 288)
// =============================================================================

// Frame size: 288 bytes (18 × 16, 16-byte aligned)
.equ FRAME_SIZE, 288

// Offsets
.equ OFF_RA,      0x00
.equ OFF_T0,      0x08
.equ OFF_T1,      0x10
.equ OFF_T2,      0x18
.equ OFF_T3,      0x20
.equ OFF_T4,      0x28
.equ OFF_T5,      0x30
.equ OFF_T6,      0x38
.equ OFF_A0,      0x40
.equ OFF_A1,      0x48
.equ OFF_A2,      0x50
.equ OFF_A3,      0x58
.equ OFF_A4,      0x60
.equ OFF_A5,      0x68
.equ OFF_A6,      0x70
.equ OFF_A7,      0x78
.equ OFF_S0,      0x80
.equ OFF_S1,      0x88
.equ OFF_S2,      0x90
.equ OFF_S3,      0x98
.equ OFF_S4,      0xA0
.equ OFF_S5,      0xA8
.equ OFF_S6,      0xB0
.equ OFF_S7,      0xB8
.equ OFF_S8,      0xC0
.equ OFF_S9,      0xC8
.equ OFF_S10,     0xD0
.equ OFF_S11,     0xD8
.equ OFF_SEPC,    0xE0
.equ OFF_SSTATUS, 0xE8
.equ OFF_USP,     0xF0
.equ OFF_SCAUSE,  0xF8

.section .text

.global syscall_entry_point
.global syscall_return

// =============================================================================
// syscall_entry_point — unified trap handler for RISC-V
// =============================================================================
// stvec requires 4-byte aligned address (low 2 bits are mode field)
.balign 4
.type syscall_entry_point, @function
syscall_entry_point:
    // ---- Step 1: Kernel/User stack switch via sscratch ----
    // Convention: sscratch = kernel SP when in U-mode, 0 when in S-mode.
    // Swap sp ↔ sscratch.  After the swap:
    //   If trap from U-mode: sp = kernel stack, sscratch = user sp
    //   If trap from S-mode: sp = 0, sscratch = kernel sp (wrong!)
    csrrw   sp, sscratch, sp
    bnez    sp, .Lfrom_user

    // Came from S-mode (sp was 0 after swap) — undo the swap
    csrrw   sp, sscratch, sp
    // Now sp = original kernel sp, sscratch = 0 (restored)

.Lfrom_user:
    // sp = kernel stack pointer (either from sscratch or already kernel)
    // For U-mode traps: sscratch = user sp (to be saved in frame)

    // ---- Step 2: Allocate trap frame ----
    addi    sp, sp, -FRAME_SIZE

    // ---- Step 3: Save minimal registers for dispatch ----
    sd      ra,  OFF_RA(sp)
    sd      t0,  OFF_T0(sp)
    sd      t1,  OFF_T1(sp)
    sd      t2,  OFF_T2(sp)

    sd      tp, 0x100(sp)
    sd      gp, 0x108(sp)
    csrr    t0, sscratch
    beqz    t0, .Lkernel_identity_ready
    ld      tp, FRAME_SIZE(sp)
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
.Lkernel_identity_ready:
    // Save sepc, sstatus, scause
    csrr    t0, sepc
    sd      t0, OFF_SEPC(sp)
    csrr    t0, sstatus
    sd      t0, OFF_SSTATUS(sp)
    csrr    t0, scause
    sd      t0, OFF_SCAUSE(sp)

    // Save user sp (from sscratch; 0 if from S-mode)
    csrr    t1, sscratch
    sd      t1, OFF_USP(sp)
    // Clear sscratch = 0 (S-mode sentinel: if we take a nested trap, we're on kernel stack)
    csrw    sscratch, zero

    // ---- Step 4: Dispatch based on scause ----
    // t0 = scause (still in t0 from above)
    bltz    t0, .Ltrap_interrupt          // bit 63 set → interrupt

    // ---- Exception path ----
    // ecall from U-mode (8) or S-mode (9)
    li      t1, 8
    beq     t0, t1, .Ltrap_ecall
    li      t1, 9
    beq     t0, t1, .Ltrap_ecall

    // Page faults: Instruction (12), Load (13), Store/AMO (15)
    li      t1, 12
    beq     t0, t1, .Ltrap_page_fault
    li      t1, 13
    beq     t0, t1, .Ltrap_page_fault
    li      t1, 15
    beq     t0, t1, .Ltrap_page_fault

    // Unknown exception — save remaining regs, call C handler
    sd      t3, OFF_T3(sp)
    sd      t4, OFF_T4(sp)
    sd      t5, OFF_T5(sp)
    sd      t6, OFF_T6(sp)
    sd      a0, OFF_A0(sp)
    sd      a1, OFF_A1(sp)
    sd      a2, OFF_A2(sp)

    mv      a0, t0                        // scause
    ld      a1, OFF_SEPC(sp)              // sepc
    csrr    a2, stval                     // stval
    call    riscv_exception_handler
    // [[noreturn]] — handler halts or terminates the user process
    j       .Ltrap_halt

// =============================================================================
// Interrupt path
// =============================================================================
.Ltrap_interrupt:
    // Mask off bit 63 to get interrupt cause number
    slli    t1, t0, 1
    srli    t1, t1, 1

    // S-mode timer interrupt (cause == 5)
    li      t2, 5
    beq     t1, t2, .Ltrap_timer

    // S-mode external interrupt (cause == 9)
    li      t2, 9
    beq     t1, t2, .Ltrap_external
    li      t2, 1
    bne     t1, t2, .Ltrap_return
    csrci   sip, 2

    // Other interrupt — spurious, return immediately
    j       .Ltrap_return

// ---- Timer interrupt ----
.Ltrap_timer:
    sd      t3, OFF_T3(sp)
    sd      t4, OFF_T4(sp)
    sd      t5, OFF_T5(sp)
    sd      t6, OFF_T6(sp)
    sd      a0, OFF_A0(sp)
    sd      a1, OFF_A1(sp)
    sd      a2, OFF_A2(sp)
    sd      a3, OFF_A3(sp)
    sd      a4, OFF_A4(sp)
    sd      a5, OFF_A5(sp)
    sd      a6, OFF_A6(sp)
    sd      a7, OFF_A7(sp)

    call    riscv_timer_handler

    ld      a7, OFF_A7(sp)
    ld      a6, OFF_A6(sp)
    ld      a5, OFF_A5(sp)
    ld      a4, OFF_A4(sp)
    ld      a3, OFF_A3(sp)
    ld      a2, OFF_A2(sp)
    ld      a1, OFF_A1(sp)
    ld      a0, OFF_A0(sp)
    ld      t6, OFF_T6(sp)
    ld      t5, OFF_T5(sp)
    ld      t4, OFF_T4(sp)
    ld      t3, OFF_T3(sp)
    j       .Ltrap_return

// ---- External interrupt ----
.Ltrap_external:
    sd      t3, OFF_T3(sp)
    sd      t4, OFF_T4(sp)
    sd      t5, OFF_T5(sp)
    sd      t6, OFF_T6(sp)
    sd      a0, OFF_A0(sp)
    sd      a1, OFF_A1(sp)
    sd      a2, OFF_A2(sp)
    sd      a3, OFF_A3(sp)
    sd      a4, OFF_A4(sp)
    sd      a5, OFF_A5(sp)
    sd      a6, OFF_A6(sp)
    sd      a7, OFF_A7(sp)

    call    riscv_external_handler

    ld      a7, OFF_A7(sp)
    ld      a6, OFF_A6(sp)
    ld      a5, OFF_A5(sp)
    ld      a4, OFF_A4(sp)
    ld      a3, OFF_A3(sp)
    ld      a2, OFF_A2(sp)
    ld      a1, OFF_A1(sp)
    ld      a0, OFF_A0(sp)
    ld      t6, OFF_T6(sp)
    ld      t5, OFF_T5(sp)
    ld      t4, OFF_T4(sp)
    ld      t3, OFF_T3(sp)
    j       .Ltrap_return

// =============================================================================
// Common trap return path (IRQ and exception)
// =============================================================================
// Checks OFF_USP: if non-zero we came from U-mode and must restore user sp
// via sscratch before sret.
//
// IMPORTANT: The U-mode and S-mode paths are separated to avoid clobbering
// user registers.  We must check OFF_USP BEFORE restoring user t0, then
// use csrrw to atomically swap sp↔sscratch without an extra temp register.
.Ltrap_return:
    // Restore sepc and sstatus
    ld      t0, OFF_SEPC(sp)
    csrw    sepc, t0
    ld      t0, OFF_SSTATUS(sp)
    csrw    sstatus, t0

    // Check U-mode vs S-mode return BEFORE restoring t0 (we use t0 as temp)
    ld      t0, OFF_USP(sp)              // saved user sp (0 if from S-mode)
    beqz    t0, .Lreturn_to_smode

    // ---- Return to U-mode ----
    // Stash user sp in sscratch temporarily; we'll swap sp↔sscratch later.
    csrw    sscratch, t0
    ld      tp, 0x100(sp)
    ld      gp, 0x108(sp)

    // Now restore ALL minimal registers (t0 gets the user's real value)
    ld      t2, OFF_T2(sp)
    ld      t1, OFF_T1(sp)
    ld      t0, OFF_T0(sp)
    ld      ra, OFF_RA(sp)

    // Deallocate frame: sp = kernel stack top
    addi    sp, sp, FRAME_SIZE
    // Atomically: sp = user sp (from sscratch), sscratch = kernel stack top (from sp)
    csrrw   sp, sscratch, sp
    sret

.Lreturn_to_smode:
    // ---- Return to S-mode ----
    ld      t2, OFF_T2(sp)
    ld      t1, OFF_T1(sp)
    ld      t0, OFF_T0(sp)
    ld      ra, OFF_RA(sp)
    addi    sp, sp, FRAME_SIZE
    sret

// =============================================================================
// Return to user-mode after exception handler (non-fatal path)
// =============================================================================
.Ltrap_return_to_user:
    ld      a2, OFF_A2(sp)
    ld      a1, OFF_A1(sp)
    ld      a0, OFF_A0(sp)
    ld      t6, OFF_T6(sp)
    ld      t5, OFF_T5(sp)
    ld      t4, OFF_T4(sp)
    ld      t3, OFF_T3(sp)
    j       .Ltrap_return

// =============================================================================
// Page fault path (scause 12, 13, 15)
// =============================================================================
.Ltrap_page_fault:
    // Save caller-saved registers (C handler may clobber them)
    sd      t3, OFF_T3(sp)
    sd      t4, OFF_T4(sp)
    sd      t5, OFF_T5(sp)
    sd      t6, OFF_T6(sp)
    sd      a0, OFF_A0(sp)
    sd      a1, OFF_A1(sp)
    sd      a2, OFF_A2(sp)
    sd      a3, OFF_A3(sp)
    sd      a4, OFF_A4(sp)
    sd      a5, OFF_A5(sp)
    sd      a6, OFF_A6(sp)
    sd      a7, OFF_A7(sp)

    // Enable SUM so demand paging can write to user pages
    li      t0, (1 << 18)
    csrs    sstatus, t0

    // Call riscv_page_fault_handler(scause, stval, sepc)
    ld      a0, OFF_SCAUSE(sp)           // a0 = scause
    csrr    a1, stval                    // a1 = fault address
    ld      a2, OFF_SEPC(sp)             // a2 = faulting PC
    call    riscv_page_fault_handler

    // Restore caller-saved registers
    ld      a7, OFF_A7(sp)
    ld      a6, OFF_A6(sp)
    ld      a5, OFF_A5(sp)
    ld      a4, OFF_A4(sp)
    ld      a3, OFF_A3(sp)
    ld      a2, OFF_A2(sp)
    ld      a1, OFF_A1(sp)
    ld      a0, OFF_A0(sp)
    ld      t6, OFF_T6(sp)
    ld      t5, OFF_T5(sp)
    ld      t4, OFF_T4(sp)
    ld      t3, OFF_T3(sp)
    j       .Ltrap_return

// =============================================================================
// Ecall (syscall) path
// =============================================================================
.Ltrap_ecall:
    // Save ALL registers for full syscall frame
    sd      t3, OFF_T3(sp)
    sd      t4, OFF_T4(sp)
    sd      t5, OFF_T5(sp)
    sd      t6, OFF_T6(sp)
    sd      a0, OFF_A0(sp)
    sd      a1, OFF_A1(sp)
    sd      a2, OFF_A2(sp)
    sd      a3, OFF_A3(sp)
    sd      a4, OFF_A4(sp)
    sd      a5, OFF_A5(sp)
    sd      a6, OFF_A6(sp)
    sd      a7, OFF_A7(sp)
    sd      s0, OFF_S0(sp)
    sd      s1, OFF_S1(sp)
    sd      s2, OFF_S2(sp)
    sd      s3, OFF_S3(sp)
    sd      s4, OFF_S4(sp)
    sd      s5, OFF_S5(sp)
    sd      s6, OFF_S6(sp)
    sd      s7, OFF_S7(sp)
    sd      s8, OFF_S8(sp)
    sd      s9, OFF_S9(sp)
    sd      s10, OFF_S10(sp)
    sd      s11, OFF_S11(sp)

    // Advance sepc past the ecall instruction (ecall is 4 bytes).
    // RISC-V sets sepc to the ecall address itself; we must skip it
    // so sret resumes at the next instruction.
    ld      t0, OFF_SEPC(sp)
    addi    t0, t0, 4
    sd      t0, OFF_SEPC(sp)

    // Enable Supervisor User Memory access (SUM, sstatus bit 18).
    // Syscall handlers may read/write user-space buffers (e.g. sys_write).
    // Without SUM=1, S-mode loads/stores to U-mode pages cause page faults.
    // SUM is automatically cleared when we restore sstatus from the trap
    // frame in .Ltrap_return (the saved sstatus has the pre-trap value).
    li      t0, (1 << 18)
    csrs    sstatus, t0

    // Remap RISC-V syscall ABI → C calling convention
    ld      a0, OFF_A7(sp)               // C arg0 (nr)   ← saved a7
    ld      a1, OFF_A0(sp)               // C arg1 (a0)   ← saved a0
    ld      a2, OFF_A1(sp)               // C arg2 (a1)   ← saved a1
    ld      a3, OFF_A2(sp)               // C arg3 (a2)   ← saved a2
    ld      a4, OFF_A3(sp)               // C arg4 (a3)   ← saved a3
    ld      a5, OFF_A4(sp)               // C arg5 (a4)   ← saved a4
    ld      a6, OFF_A5(sp)               // C arg6 (a5)   ← saved a5

    call    system_call_handler

    // Patch return value into saved a0 slot
    sd      a0, OFF_A0(sp)

    // Restore all registers
    ld      s11, OFF_S11(sp)
    ld      s10, OFF_S10(sp)
    ld      s9, OFF_S9(sp)
    ld      s8, OFF_S8(sp)
    ld      s7, OFF_S7(sp)
    ld      s6, OFF_S6(sp)
    ld      s5, OFF_S5(sp)
    ld      s4, OFF_S4(sp)
    ld      s3, OFF_S3(sp)
    ld      s2, OFF_S2(sp)
    ld      s1, OFF_S1(sp)
    ld      s0, OFF_S0(sp)
    ld      a7, OFF_A7(sp)
    ld      a6, OFF_A6(sp)
    ld      a5, OFF_A5(sp)
    ld      a4, OFF_A4(sp)
    ld      a3, OFF_A3(sp)
    ld      a2, OFF_A2(sp)
    ld      a1, OFF_A1(sp)
    ld      a0, OFF_A0(sp)
    ld      t6, OFF_T6(sp)
    ld      t5, OFF_T5(sp)
    ld      t4, OFF_T4(sp)
    ld      t3, OFF_T3(sp)

    j       .Ltrap_return

// ---- Halt loop for fatal exceptions ----
.Ltrap_halt:
    wfi
    j       .Ltrap_halt
.size syscall_entry_point, . - syscall_entry_point

// =============================================================================
// syscall_return — extract return value from SyscallContext*
// =============================================================================
.type syscall_return, @function
syscall_return:
    beqz    a0, .return_error
    ld      a1, 56(a0)                  // a1 = context->ret_value
    mv      a0, a1
    ret

.return_error:
    li      a0, -1
    ret
.size syscall_return, . - syscall_return
