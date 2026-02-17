/* =============================================================================
 * MOSS User-Space Program Linker Script
 * =============================================================================
 *
 * This linker script defines the virtual memory layout for user-space ELF
 * programs loaded by the MOSS kernel's ELF loader.
 *
 * Virtual Address Layout:
 * ┌──────────────────────────────────────────┐
 * │ 0x0040_0000  ┌────────────┐              │
 * │              │ .text       │  Code (RX)   │
 * │              ├────────────┤              │
 * │              │ .rodata     │  Read-only   │
 * │              ├────────────┤              │
 * │              │ .data       │  Data (RW)   │
 * │              ├────────────┤              │
 * │              │ .bss        │  BSS  (RW)   │
 * │              └────────────┘              │
 * │ ...          ↑ heap grows upward         │
 * │ ...          ↓ stack grows downward      │
 * │ 0x????_????  Stack top (set by kernel)   │
 * └──────────────────────────────────────────┘
 *
 * Key design decisions:
 *   - Base address 0x40_0000 (4 MB) — standard Linux user-space convention,
 *     leaves the first 4 MB unmapped as a NULL-pointer guard region
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
     * 0x40_0000 (4 MB) is the conventional start for user-space programs.
     * This ensures that any NULL-pointer dereference (address 0x0) faults
     * immediately rather than silently accessing valid memory.
     */
    . = 0x400000;

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
