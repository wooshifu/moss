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
//   4. Sets up kernel stack via linker script _stack_top
//   5. Zeroes the BSS section
//   6. Calls early_main(device_tree_ptr) to enter the unified boot path
//
// Address loading strategy:
//   RISC-V auipc+addi (la) has a ±1 MB PC-relative range.  When the kernel
//   image exceeds this, .text.boot cannot directly reference symbols in .bss
//   or .stack.  We work around this by placing 64-bit address constants in a
//   data pool within .text.boot and loading them with auipc+ld (always in
//   range since the pool is adjacent to the code).
//
// Entry point: _start (set by the linker via ENTRY(_start))
// =============================================================================

.section ".text.boot", "ax"
.global _start

.type _start, @function
_start:
    // Save hart ID in tp (thread pointer) — S-mode convention.
    // tp is used by get_current_cpu_id() since mhartid is M-mode only.
    mv tp, a0

    // Non-boot harts: park in WFI loop.
    // OpenSBI starts only the boot hart initially.  If sbi_hart_start()
    // sends secondary harts here, they must not re-run BSS clearing or
    // early_main — park them immediately.
    // OpenSBI elects one boot hart; it need not be physical hart zero.

    // ---- Boot hart (hart 0) only ----

    // Set up global pointer for linker relaxation.
    // .option norelax prevents the assembler from GP-relaxing this instruction
    // itself (circular dependency).
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    // Load addresses from the nearby data pool (PC-relative, always in range)
    // Each "ld rX, .Lpool_xxx - .(%rip)" uses auipc+ld with tiny offset.
1:  auipc  sp, %pcrel_hi(.Lpool_stack_top)
    ld     sp, %pcrel_lo(1b)(sp)

1:  auipc  t0, %pcrel_hi(.Lpool_bss_start)
    ld     t0, %pcrel_lo(1b)(t0)

1:  auipc  t1, %pcrel_hi(.Lpool_bss_end)
    ld     t1, %pcrel_lo(1b)(t1)

    // Zero BSS section
    bgeu t0, t1, 2f
.Lbss_loop:
    sd zero, (t0)
    addi t0, t0, 8
    bltu t0, t1, .Lbss_loop
2:

    // Call unified boot entry.
    // a1 still holds the DTB pointer from OpenSBI → pass as first arg (a0).
    mv a0, a1
    call early_main

    // Should not return — WFI loop as safety net
.Lhalt:
    wfi
    j .Lhalt
.size _start, . - _start

// =============================================================================
// Secondary hart parking loop
// =============================================================================
.Lsecondary_park:
    wfi
    j .Lsecondary_park

.balign 4
.global riscv_secondary_start
.type riscv_secondary_start, @function
riscv_secondary_start:
    mv tp, a0
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    ld sp, 0(a1)
    ld t0, 8(a1)
    csrw satp, t0
    sfence.vma
    csrw sscratch, zero
    tail riscv_secondary_entry
.size riscv_secondary_start, . - riscv_secondary_start

// =============================================================================
// Address data pool (placed in .text.boot, adjacent to code for PC-relative access)
// =============================================================================
// These .quad entries hold 64-bit absolute addresses that are resolved by the
// linker.  The code above uses auipc+ld to read them with a tiny PC-relative
// offset, avoiding the ±1 MB limit of auipc+addi.
// =============================================================================
.align 3
.Lpool_stack_top:
    .quad _stack_top
.Lpool_bss_start:
    .quad _bss_start
.Lpool_bss_end:
    .quad _bss_end

// Prevent executable stack warning
.section .note.GNU-stack,"",@progbits
