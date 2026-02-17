// =============================================================================
// x86_64 Kernel Boot Stub
// =============================================================================
//
// Minimal bootstrap for x86_64 MOSS kernel.  This is a placeholder — the full
// x86_64 boot sequence (GDT setup, paging, AP bringup, etc.) is not yet
// implemented.
//
// Current behavior:
//   1. Clear direction flag (CLD) for safe REP string operations
//   2. Set up a temporary 8 KB kernel stack
//   3. Zero the BSS section
//   4. Jump to kernel_main_stub which halts the CPU
//
// Entry point: _start (set by the linker via ENTRY(_start) in linker.ld)
//
// TODO:
//   - Set up a proper GDT with kernel/user code+data segments
//   - Initialize page tables (identity + high-half mapping)
//   - Set up IDT for exception/interrupt handling
//   - Parse ACPI/MP tables for multi-core topology
//   - Boot application processors (APs) via SIPI
//   - Jump to the real kernel_main
// =============================================================================

.section ".text.boot", "ax"
.global _start
.code64

_start:
    // Clear direction flag — required for REP STOSB below to move forward
    cld

    // Set up temporary kernel stack (8 KB, defined in .bss.stack below)
    lea stack_top(%rip), %rsp

    // Zero BSS section: memset(BSS_START, 0, BSS_END - BSS_START)
    //   RDI = destination (BSS start)
    //   RCX = count in bytes (BSS end - BSS start)
    //   AL  = 0 (fill value)
    //   REP STOSB: store AL to [RDI], increment RDI, decrement RCX, repeat
    lea __bss_start(%rip), %rdi
    lea __bss_end(%rip), %rcx
    sub %rdi, %rcx                      // RCX = BSS size in bytes
    xor %eax, %eax                      // fill with zeros
    rep stosb                           // zero the entire BSS

    // Jump to stub (full x86_64 boot not yet implemented)
    jmp kernel_main_stub

// =============================================================================
// Temporary kernel stack (8 KB, page-aligned, in BSS)
// =============================================================================
.section ".bss.stack", "aw", @nobits
.align 16
stack_bottom:
    .skip 8192                          // 8 KB stack
stack_top:

// =============================================================================
// kernel_main_stub — placeholder halt loop
// =============================================================================
//
// Disables interrupts (CLI) then enters an infinite HLT loop.
// HLT stops the CPU until the next interrupt, but since interrupts are
// disabled, it effectively halts permanently.
// =============================================================================
.section ".text"
kernel_main_stub:
    cli                                 // disable interrupts
1:  hlt                                 // halt CPU
    jmp 1b                              // loop (in case of NMI wakeup)

// =============================================================================
// Weak BSS boundary symbols
// =============================================================================
//
// These are defined as weak so the linker script can override them.
// If no linker script is used, they default to 0 (no BSS to clear).
// =============================================================================
.weak __bss_start
.weak __bss_end
__bss_start = 0
__bss_end = 0
