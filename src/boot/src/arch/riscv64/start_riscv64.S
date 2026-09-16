// =============================================================================
// RISC-V 64-bit Kernel Boot Entry
// =============================================================================
//
// Bootstrap for RISC-V 64 MOSS kernel running in S-mode (under OpenSBI).
//
// OpenSBI calls _start with:
//   a0 = hart ID
//   a1 = DTB (Device Tree Blob) pointer
//
// This stub:
//   1. Saves hart ID in tp register (S-mode convention)
//   2. Applies boot-image relocations on the firmware-elected boot hart
//   3. Sets up global pointer (GP) for linker relaxation
//   4. Sets up kernel stack via linker script _stack_top
//   5. Zeroes the BSS section
//   6. Calls early_main(device_tree_ptr) to enter the unified boot path
//
// Address loading strategy:
//   PC-relative instructions find the image and relocation table. Absolute
//   address pools are relocated before use; the kernel does not assume a
//   machine-specific RAM base.
//
// Entry point: _start (set by the linker via ENTRY(_start))
// =============================================================================

.section ".text.boot", "ax"
.global _start
.option nopic
.option norelax
.option norvc
// Fixed-width boot instructions preserve the 64-byte Linux Image header;
// text_offset=2 MiB follows RV64 Linux placement, and flags=0 selects little-endian.
// Header version 2 encodes major 0/minor 2, not the kernel version.
_linux_header:
    j _start
    .word 0
    .quad 0x200000                 // RV64 Linux Image placement offset
    .quad _kernel_image_size
    .quad 0
    .word 2                       // header version 0.2
    .word 0
    .quad 0
    .quad 0x5643534952             // legacy little-endian "RISCV" header magic
    .word 0x05435352               // v0.2 little-endian "RSC\x05" loader magic
    .word 0

.type _start, @function
_start:
    // Save hart ID in tp (thread pointer) — S-mode convention.
    // tp is used by get_current_cpu_id() since mhartid is M-mode only.
    mv tp, a0

    // ELF64 RELATIVE relocations. Firmware selects the physical image base.
    // Rela fields at +0/+8/+16 are offset/info/addend; 24 bytes is one entry.
    // Type 3 is R_RISCV_RELATIVE; require symbol zero before adding the load base.
    lla t2, _linux_header
    lla t0, _rela_start
    lla t1, _rela_end
1:
    bgeu t0, t1, 2f
    ld t3, 0(t0)
    ld t4, 8(t0)
    ld t5, 16(t0)
    li t6, 3                     // R_RISCV_RELATIVE, symbol index zero
    bne t4, t6, .Lhalt
    add t3, t3, t2
    add t5, t5, t2
    sd t5, 0(t3)
    addi t0, t0, 24
    j 1b
2:
    fence rw, rw
    fence.i

    // Firmware must enter _start only on its elected boot hart (not necessarily
    // hart zero). HSM targets riscv64_secondary_start below; entering a secondary
    // here would repeat relocations and BSS clearing, since there is no hart gate.

    // Set up global pointer for linker relaxation.
    // .option norelax prevents the assembler from GP-relaxing this instruction
    // itself (circular dependency).
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    // Load addresses from the nearby data pool (PC-relative, always in range)
    // The auipc/ld pairs use RISC-V %pcrel_hi/%pcrel_lo to reach nearby pools.
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
.global riscv64_secondary_start
.type riscv64_secondary_start, @function
riscv64_secondary_start:
    // HSM passes hart ID in a0 and opaque HartStartContext* in a1; its two
    // u64 slots at +0/+8 are stack and satp. Establish these before C++ runs.
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
    tail riscv64_secondary_entry
.size riscv64_secondary_start, . - riscv64_secondary_start

// =============================================================================
// Address data pool (placed in .text.boot, adjacent to code for PC-relative access)
// =============================================================================
// These .quad entries hold 64-bit absolute addresses that are resolved by the
// linker.  The code above uses auipc+ld to read them with a tiny PC-relative
// offset. Keeping data pools nearby avoids instruction-range assumptions for
// firmware-selected absolute addresses; AUIPC itself is not limited to ±1 MiB.
// =============================================================================
.align 3
// RISC-V .align 3 means 2^3=8 bytes, matching the .quad absolute-address slots.
.Lpool_stack_top:
    .quad _stack_top
.Lpool_bss_start:
    .quad _bss_start
.Lpool_bss_end:
    .quad _bss_end

// Prevent executable stack warning
.section .note.GNU-stack,"",@progbits
