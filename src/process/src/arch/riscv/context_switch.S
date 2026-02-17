// =============================================================================
// RISC-V 64-bit Context Switch Implementation
// =============================================================================
//
// This file provides the core process/thread switching primitives for RV64:
//
//   context_switch(prev, next) — save prev's CPU state, restore next's
//   switch_to_user(ctx, usp)  — enter U-mode via SRET
//   kernel_thread_entry()     — trampoline for newly created kernel threads
//
// CpuContext structure layout (defined in moss.process module):
//   Offset  Size   Field
//   ------  ----   -----
//   0x000   256    x[32]    — all 32 GP registers (x0-x31), 8 bytes each
//                             x0 = zero (hardwired), x1 = ra, x2 = sp, ...
//   0x100   8      pc       — program counter (saved as ra for context_switch)
//   0x108   8      pstate   — supervisor status register (sstatus)
//   0x110   8      sp       — dedicated stack pointer field
//
// RISC-V register naming:
//   x0  = zero (hardwired 0)    x16 = a6
//   x1  = ra (return address)   x17 = a7
//   x2  = sp (stack pointer)    x18 = s2
//   x3  = gp (global pointer)   x19 = s3
//   x4  = tp (thread pointer)   x20 = s4
//   x5  = t0 (temporary)        x21 = s5
//   x6  = t1                    x22 = s6
//   x7  = t2                    x23 = s7
//   x8  = s0/fp (frame pointer) x24 = s8
//   x9  = s1                    x25 = s9
//   x10 = a0 (arg0/return)      x26 = s10
//   x11 = a1 (arg1)             x27 = s11
//   x12 = a2                    x28 = t3
//   x13 = a3                    x29 = t4
//   x14 = a4                    x30 = t5
//   x15 = a5                    x31 = t6
//
// RISC-V calling convention:
//   Arguments:    a0-a7 (x10-x17)
//   Callee-saved: s0-s11 (x8-x9, x18-x27), ra (x1), sp (x2)
//   Return value: a0 (x10)
// =============================================================================

.section .text

.extern current_thread_ptr
.extern next_thread_ptr

// =============================================================================
// context_switch — save current thread, restore next thread
// =============================================================================
//
// Prototype: extern "C" void context_switch(CpuContext* prev, CpuContext* next);
//
// Parameters:
//   a0 (x10) = prev — pointer to current thread's CpuContext
//   a1 (x11) = next — pointer to next thread's CpuContext
// =============================================================================
.global context_switch
.type context_switch, @function
context_switch:
    // ---- Phase 1: Save current (prev) context ----

    // Save all 32 general-purpose registers into prev->x[] array
    sd      x0, 0(a0)                  // x0  = zero (always 0, saved for uniformity)
    sd      x1, 8(a0)                  // x1  = ra  (return address)
    sd      x2, 16(a0)                 // x2  = sp  (stack pointer)
    sd      x3, 24(a0)                 // x3  = gp  (global pointer)
    sd      x4, 32(a0)                 // x4  = tp  (thread pointer)
    sd      x5, 40(a0)                 // x5  = t0  (temporary)
    sd      x6, 48(a0)                 // x6  = t1
    sd      x7, 56(a0)                 // x7  = t2
    sd      x8, 64(a0)                 // x8  = s0/fp
    sd      x9, 72(a0)                 // x9  = s1
    sd      x10, 80(a0)               // x10 = a0  (this save captures a0 itself)
    sd      x11, 88(a0)               // x11 = a1
    sd      x12, 96(a0)               // x12 = a2
    sd      x13, 104(a0)              // x13 = a3
    sd      x14, 112(a0)              // x14 = a4
    sd      x15, 120(a0)              // x15 = a5
    sd      x16, 128(a0)              // x16 = a6
    sd      x17, 136(a0)              // x17 = a7
    sd      x18, 144(a0)              // x18 = s2
    sd      x19, 152(a0)              // x19 = s3
    sd      x20, 160(a0)              // x20 = s4
    sd      x21, 168(a0)              // x21 = s5
    sd      x22, 176(a0)              // x22 = s6
    sd      x23, 184(a0)              // x23 = s7
    sd      x24, 192(a0)              // x24 = s8
    sd      x25, 200(a0)              // x25 = s9
    sd      x26, 208(a0)              // x26 = s10
    sd      x27, 216(a0)              // x27 = s11
    sd      x28, 224(a0)              // x28 = t3
    sd      x29, 232(a0)              // x29 = t4
    sd      x30, 240(a0)              // x30 = t5
    sd      x31, 248(a0)              // x31 = t6

    // Save PC (return address = ra) → prev->pc  (offset 256)
    sd      x1, 256(a0)

    // Save sstatus (supervisor status register) → prev->pstate  (offset 264)
    csrr    t0, sstatus
    sd      t0, 264(a0)

    // Save stack pointer to dedicated sp field → prev->sp  (offset 272)
    sd      x2, 272(a0)

    // ---- Phase 2: Restore next context ----

    // Restore sstatus from next->pstate
    ld      t0, 264(a1)
    csrw    sstatus, t0

    // Restore stack pointer from dedicated sp field and sync to x[2]
    ld      t1, 272(a1)                 // next->sp
    sd      t1, 16(a1)                  // sync to x[2] in the context struct

    // Restore all general-purpose registers
    ld      x1, 8(a1)                  // ra
    ld      x2, 16(a1)                // sp
    ld      x3, 24(a1)                // gp
    ld      x4, 32(a1)                // tp
    ld      x5, 40(a1)                // t0
    ld      x6, 48(a1)                // t1
    ld      x7, 56(a1)                // t2
    ld      x8, 64(a1)                // s0/fp
    ld      x9, 72(a1)                // s1
    // Skip x10 (a0) and x11 (a1) — restored last
    ld      x12, 96(a1)               // a2
    ld      x13, 104(a1)              // a3
    ld      x14, 112(a1)              // a4
    ld      x15, 120(a1)              // a5
    ld      x16, 128(a1)              // a6
    ld      x17, 136(a1)              // a7
    ld      x18, 144(a1)              // s2
    ld      x19, 152(a1)              // s3
    ld      x20, 160(a1)              // s4
    ld      x21, 168(a1)              // s5
    ld      x22, 176(a1)              // s6
    ld      x23, 184(a1)              // s7
    ld      x24, 192(a1)              // s8
    ld      x25, 200(a1)              // s9
    ld      x26, 208(a1)              // s10
    ld      x27, 216(a1)              // s11
    ld      x28, 224(a1)              // t3
    ld      x29, 232(a1)              // t4
    ld      x30, 240(a1)              // t5
    ld      x31, 248(a1)              // t6

    // Restore a0 (x10) and a1 (x11) last — a1 is our base pointer
    ld      x10, 80(a1)               // a0
    ld      x11, 88(a1)               // a1

    // Return to new thread via saved ra (x1)
    ret

.size context_switch, .-context_switch

// =============================================================================
// switch_to_user — enter U-mode via SRET
// =============================================================================
//
// Prototype: extern "C" void switch_to_user(CpuContext* ctx, uint64_t user_sp);
//
// Parameters:
//   a0 (x10) = ctx     — pointer to user task's CpuContext
//   a1 (x11) = user_sp — user-space stack pointer
//
// This function:
//   1. Clears sstatus.SPP = 0 (return to U-mode on SRET)
//   2. Sets SEPC to ctx->pc (user entry point)
//   3. Restores all GP registers from ctx
//   4. Sets SP to user_sp
//   5. Executes SRET to atomically drop to U-mode at SEPC
// =============================================================================
.global switch_to_user
.type switch_to_user, @function
switch_to_user:
    // Configure sstatus for U-mode return: clear SPP bit (bit 8)
    li      t0, 0x00000000              // SPP=0 → U-mode
    csrw    sstatus, t0

    // Set exception return address to user entry point
    ld      t0, 256(a0)                 // ctx->pc
    csrw    sepc, t0

    // Set user stack pointer
    mv      sp, a1

    // Also update ctx->sp fields for consistency
    sd      a1, 272(a0)                 // ctx->sp (dedicated field)
    sd      a1, 16(a0)                  // ctx->x[2] (register array)

    // Restore all general-purpose registers from ctx
    ld      x1, 8(a0)                  // ra
    ld      x2, 16(a0)                // sp (will be overwritten by mv below)
    ld      x3, 24(a0)                // gp
    ld      x4, 32(a0)                // tp
    ld      x5, 40(a0)                // t0
    ld      x6, 48(a0)                // t1
    ld      x7, 56(a0)                // t2
    ld      x8, 64(a0)                // s0/fp
    ld      x9, 72(a0)                // s1
    ld      x11, 88(a0)               // a1
    ld      x12, 96(a0)               // a2
    ld      x13, 104(a0)              // a3
    ld      x14, 112(a0)              // a4
    ld      x15, 120(a0)              // a5
    ld      x16, 128(a0)              // a6
    ld      x17, 136(a0)              // a7
    ld      x18, 144(a0)              // s2
    ld      x19, 152(a0)              // s3
    ld      x20, 160(a0)              // s4
    ld      x21, 168(a0)              // s5
    ld      x22, 176(a0)              // s6
    ld      x23, 184(a0)              // s7
    ld      x24, 192(a0)              // s8
    ld      x25, 200(a0)              // s9
    ld      x26, 208(a0)              // s10
    ld      x27, 216(a0)              // s11
    ld      x28, 224(a0)              // t3
    ld      x29, 232(a0)              // t4
    ld      x30, 240(a0)              // t5
    ld      x31, 248(a0)              // t6

    // Re-set user stack pointer (may have been clobbered by x2 restore above)
    mv      sp, a1

    // Restore a0 last (it was our context pointer)
    ld      x10, 80(a0)               // a0

    // Atomically return to U-mode: sets PC=SEPC, privilege=U (SPP=0)
    sret

.size switch_to_user, .-switch_to_user

// =============================================================================
// kernel_thread_entry — trampoline for newly created kernel threads
// =============================================================================
//
// The thread creator sets up the context so that when context_switch returns,
// it "returns" to this trampoline with:
//   a0 = function pointer
//   a1 = argument
//
// Flow:
//   1. Save func in t0, move arg to a0
//   2. JALR to func(arg)
//   3. If func returns, invoke sys_exit(0) via ECALL
// =============================================================================
.global kernel_thread_entry
.type kernel_thread_entry, @function
kernel_thread_entry:
    mv      t0, a0                      // save function pointer
    mv      a0, a1                      // arg → first argument
    jalr    ra, t0, 0                   // call func(arg)

    // Thread returned — invoke exit (should not normally reach here)
    li      a7, 93                      // RISC-V Linux exit syscall number
    li      a0, 0                       // exit code = 0
    ecall

.size kernel_thread_entry, .-kernel_thread_entry
