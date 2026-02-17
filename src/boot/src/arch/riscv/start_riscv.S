// =============================================================================
// RISC-V 64-bit Kernel Boot Stub
// =============================================================================
//
// Minimal bootstrap for RISC-V MOSS kernel.  This is a placeholder — the full
// RISC-V boot sequence (SBI init, page tables, trap vectors, etc.) is not yet
// implemented.
//
// Current behavior:
//   1. Set up the global pointer (GP) register for linker relaxation
//   2. Set up a temporary 8 KB kernel stack
//   3. Zero the BSS section
//   4. Call kernel_main_stub which enters a WFI loop
//
// Entry point: _start (set by the linker via ENTRY(_start) in linker.ld)
//
// RISC-V specifics:
//   - GP (x3) must be set early for linker relaxation to work.  The
//     `.option norelax` directive prevents the assembler from relaxing the
//     `la gp, __global_pointer$` instruction itself (chicken-and-egg problem).
//   - WFI (Wait For Interrupt) is the RISC-V equivalent of ARM's WFI / x86 HLT.
//
// TODO:
//   - Initialize S-mode trap vector (stvec)
//   - Set up page tables (Sv39/Sv48)
//   - Initialize PLIC for interrupt routing
//   - Parse DTB from a0 (passed by OpenSBI/bootloader)
//   - Boot secondary harts via SBI HSM extension
//   - Jump to the real kernel_main
// =============================================================================

.section ".text.boot", "ax"
.global _start

_start:
    // ---- Set up global pointer for linker relaxation ----
    //
    // RISC-V uses GP-relative addressing for small data access.
    // __global_pointer$ is defined by the linker at the center of the
    // small data section (.sdata).  We must use `.option norelax` here
    // to prevent the assembler from trying to GP-relax this very
    // instruction (which would create a circular dependency).
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    // ---- Set up temporary kernel stack (8 KB) ----
    la sp, stack_top

    // ---- Zero BSS section ----
    //   t0 = BSS start, t1 = BSS end
    //   Loop: store doubleword zeros until t0 >= t1
    la t0, __bss_start
    la t1, __bss_end
    bgeu t0, t1, 2f                    // skip if BSS is empty
1:
    sd zero, (t0)                       // store 8 bytes of zeros
    addi t0, t0, 8                      // advance pointer
    bltu t0, t1, 1b                     // loop while t0 < t1
2:

    // ---- Jump to stub (full RISC-V boot not yet implemented) ----
    call kernel_main_stub

// =============================================================================
// Temporary kernel stack (8 KB, 16-byte aligned, in BSS)
// =============================================================================
.section ".bss.stack", "aw", @nobits
.align 16
stack_bottom:
    .skip 8192                          // 8 KB stack
stack_top:

// =============================================================================
// kernel_main_stub — placeholder WFI loop
// =============================================================================
//
// WFI (Wait For Interrupt) puts the hart into a low-power state until the
// next interrupt.  Since no interrupt handler is set up, this effectively
// halts the hart permanently.
// =============================================================================
.section ".text"
kernel_main_stub:
1:  wfi                                 // wait for interrupt (halt)
    j 1b                                // loop (in case of spurious wakeup)

// =============================================================================
// Weak BSS and GP boundary symbols
// =============================================================================
//
// These are defined as weak so the linker script can override them.
// If no linker script is used, they default to 0.
// =============================================================================
.weak __bss_start
.weak __bss_end
.weak __global_pointer$
__bss_start = 0
__bss_end = 0
__global_pointer$ = 0
