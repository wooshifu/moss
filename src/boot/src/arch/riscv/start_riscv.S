// =============================================================================
// RISC-V 64-bit Kernel Boot Entry
// =============================================================================
//
// Bootstrap for RISC-V MOSS kernel running in S-mode (under OpenSBI).
//
// OpenSBI calls _start with:
//   a0 = hart ID
//   a1 = DTB (Device Tree Blob) pointer
//
// This stub:
//   1. Saves hart ID in tp register (S-mode convention)
//   2. Parks non-boot harts in a WFI loop (SBI HSM wakes them later)
//   3. Sets up global pointer (GP) for linker relaxation
//   4. Sets up a temporary 8 KB kernel stack
//   5. Zeroes the BSS section
//   6. Calls early_main(device_tree_ptr) to enter the unified boot path
//
// Entry point: _start (set by the linker via ENTRY(_start) in linker.ld)
// =============================================================================

.section ".text.boot", "ax"
.global _start

.type _start, @function
_start:
    // Save hart ID in tp (thread pointer) — S-mode convention.
    // tp is used by get_current_cpu_id() since mhartid is M-mode only.
    mv tp, a0

    // Non-boot harts: park in WFI loop.
    // SBI HSM extension will wake them via sbi_hart_start() when ready.
    bnez a0, .Lsecondary_park

    // ---- Boot hart (hart 0) only ----

    // Set up global pointer for linker relaxation.
    // .option norelax prevents the assembler from GP-relaxing this instruction
    // itself (circular dependency).
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    // Set up temporary kernel stack (8 KB)
    la sp, stack_top

    // Zero BSS section
    la t0, __bss_start
    la t1, __bss_end
    bgeu t0, t1, 2f
1:
    sd zero, (t0)
    addi t0, t0, 8
    bltu t0, t1, 1b
2:

    // Call unified boot entry.
    // a1 still holds the DTB pointer from OpenSBI → pass as first arg (a0).
    mv a0, a1
    call early_main

    // Should not return — WFI loop as safety net
1:  wfi
    j 1b
.size _start, . - _start

// =============================================================================
// Secondary hart parking loop
// =============================================================================
.Lsecondary_park:
    wfi
    j .Lsecondary_park

// =============================================================================
// Temporary kernel stack (8 KB, 16-byte aligned, in BSS)
// =============================================================================
.section ".bss.stack", "aw", @nobits
.align 16
stack_bottom:
    .skip 8192                          // 8 KB stack
stack_top:

// =============================================================================
// Weak BSS and GP boundary symbols
// =============================================================================
.weak __bss_start
.weak __bss_end
.weak __global_pointer$
__bss_start = 0
__bss_end = 0
__global_pointer$ = 0
