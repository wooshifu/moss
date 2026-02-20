/* =============================================================================
 * MOSS User-Space Program Linker Script
 * =============================================================================
 *
 * This linker script defines the virtual memory layout for user-space ELF
 * programs loaded by the MOSS kernel's ELF loader.
 *
 * Virtual Address Layout:
 * ┌──────────────────────────────────────────┐
 * │ 0x2_0000_0000 ┌────────────┐             │
 * │               │ .text       │  Code (RX)  │
 * │               ├────────────┤             │
 * │               │ .rodata     │  Read-only  │
 * │               ├────────────┤             │
 * │               │ .data       │  Data (RW)  │
 * │               ├────────────┤             │
 * │               │ .bss        │  BSS  (RW)  │
 * │               └────────────┘             │
 * │ ...           ↑ heap grows upward        │
 * │ ...           ↓ stack grows downward     │
 * │ 0x????_????   Stack top (set by kernel)  │
 * └──────────────────────────────────────────┘
 *
 * Key design decisions:
 *   - Base address 0x2_0000_0000 (8 GB) — placed above the kernel identity
 *     map region (PGD[0] L1 entries cover 0-4 GB) to ensure user page faults
 *     trigger demand paging instead of hitting kernel block mappings
 *   - No explicit stack section — the kernel allocates the user stack and
 *     passes the stack pointer via switch_to_user()
 *   - Minimal layout: only .text, .rodata, .data, .bss sections are needed
 *     for freestanding user-space programs (no libc, no dynamic linking)
 *
 * Usage:
 *   aarch64-none-elf-ld -T userspace.ld -o user_program user_program.o
 *
 * Referenced by:
 *   - CMakeLists.txt in userspace/: uses this script to link user programs
 *   - ELF loader (moss.kernel module): loads segments at the virtual addresses
 *     defined here, maps them into the process address space
 * ============================================================================= */

/* Entry point: _start is the first function in the user program */
ENTRY(_start)

SECTIONS
{
    /* ---- Program Load Address ----
     * 0x2_0000_0000 (8 GB) places user code above the kernel identity map
     * region.  The kernel copies PGD[0]→PUD[0..3] (4x 1GB blocks at 0-4GB)
     * into every user page table for kernel code accessibility.  User
     * addresses must fall outside this 0-4GB region so that demand paging
     * (L1/L2/L3 translation faults) works correctly.
     *
     * 0x200000000 maps to PGD[0]→PUD[8], which is empty in the kernel
     * page tables, ensuring user faults are properly handled.
     */
    . = 0x200000000;

    /* ---- Code Section ----
     * All executable code from the user program.
     * The .text* wildcard captures compiler-generated subsections like
     * .text.startup, .text.hot, .text.unlikely, etc.
     *
     * Page permissions: Read + Execute (no write)
     */
    .text : {
        *(.text*)
    }

    /* ---- Read-Only Data Section ----
     * String literals, const arrays, and other immutable data.
     * The .rodata* wildcard captures subsections like .rodata.str1.1.
     *
     * Page permissions: Read-only (no write, no execute)
     */
    .rodata : {
        *(.rodata*)
    }

    /* ---- Initialized Data Section ----
     * Global/static variables with non-zero initial values.
     * These are loaded from the ELF file into memory by the ELF loader.
     *
     * Page permissions: Read + Write (no execute)
     */
    .data : {
        *(.data*)
    }

    /* ---- BSS Section (Zero-Initialized Data) ----
     * Uninitialized global/static variables.  The ELF loader zeros this region.
     * COMMON symbols (C-style tentative definitions) are also placed here.
     *
     * Page permissions: Read + Write (no execute)
     */
    .bss : {
        *(.bss*)
        *(COMMON)
    }

    /* ---- Discarded Sections ----
     * Remove metadata sections that are unnecessary at runtime:
     *   .comment  : Compiler version strings (e.g., "clang version 18.0")
     *   .note*    : ELF note sections (build-id, ABI tags, etc.)
     *   .eh_frame*: C++ exception unwind tables (not used in freestanding code)
     */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
