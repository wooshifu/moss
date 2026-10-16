/* JTAG handoff from Pi 3B+ U-Boot at EL2h to the ARM64 Linux Image contract.
 * Run this before overwriting RAM: U-Boot may still own dirty cache lines.
 * No stack is used, and the stub executes from U-Boot's identity-mapped RAM.
 */
.section .text, "ax"
.global _start, cache_off
_start:
    msr daifset, #0xf             // Mask D/A/I/F while dismantling U-Boot's execution environment.
    dsb sy
    mrs x0, clidr_el1
    ubfx x3, x0, #24, #3         // CLIDR.LoC is the number of coherent cache levels.
    lsl x3, x3, #1              // CSSELR encodes a level in bits [3:1].
    mov x10, #0
1:
    cmp x10, x3
    b.hs 5f
    add x2, x10, x10, lsr #1    // CLIDR allocates three cache-type bits per level.
    lsr x1, x0, x2
    and x1, x1, #7
    cmp x1, #2                  // Type 0/1 has no data cache; types 2/3/4 do.
    b.lo 4f
    msr csselr_el1, x10
    isb
    mrs x1, ccsidr_el1
    and x2, x1, #7
    add x2, x2, #4              // CCSIDR.LineSize is log2(bytes)-4 on ARMv8.0 Cortex-A53.
    ubfx x4, x1, #3, #10        // ARMv8.0 CCSIDR associativity is ways-1 in bits [12:3].
    clz w5, w4                 // DC CISW places the way at the top of its low 32-bit operand.
2:
    ubfx x6, x1, #13, #15       // ARMv8.0 CCSIDR NumSets is sets-1 in bits [27:13].
3:
    lsl x11, x4, x5
    orr x11, x11, x10
    lsl x12, x6, x2
    orr x11, x11, x12
    dc cisw, x11               // Clean AND invalidate every set/way, preserving dirty firmware data.
    subs x6, x6, #1
    b.ge 3b
    subs x4, x4, #1
    b.ge 2b
4:
    add x10, x10, #2            // Advance to the next CSSELR cache level.
    b 1b
5:
    dsb sy
    msr csselr_el1, xzr
    mrs x0, sctlr_el2
    bic x0, x0, #1             // SCTLR.M: identity mappings stop being needed after this instruction.
    bic x0, x0, #4             // SCTLR.C: disable data caching before host loads the new image.
    bic x0, x0, #0x1000        // SCTLR.I: disable instruction caching before host loads the new image.
    msr sctlr_el2, x0
    isb
    ic iallu
    tlbi alle2
    dsb sy
    isb
    mov x0, #3                 // CNTHCTL_EL2.EL1PCEN/EL1PCTEN allow the kernel's physical timer access.
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr        // The Linux Image boot contract uses an unshifted virtual counter.
    msr cnthp_ctl_el2, xzr      // Disarm U-Boot's EL2 physical timer before transferring control.
cache_off:
    b cache_off                // Host breakpoint here; RAM is loaded only after reaching this barrier.
