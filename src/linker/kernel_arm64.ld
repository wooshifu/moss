/* ARM64 relocatable Linux Image. Link at zero; firmware chooses physical RAM. */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0, LENGTH = 256M
}

/* =============================================================================
 * High-Half Kernel Virtual Address Base
 * =============================================================================
 * For future virtual memory support:
 *   - Kernel will be mapped at KERNEL_VMA + physical_offset
 *   - This is the canonical upper-half address on AArch64 (TTBR1_EL1 range)
 *   - Currently unused — kernel runs with identity mapping
 * ============================================================================= */
KERNEL_VMA = 0xFFFF800000000000;

/* =============================================================================
 * Section Layout
 * ============================================================================= */
SECTIONS
{
    /* Start placing sections at the beginning of RAM */
    . = ORIGIN(RAM);

    /* ---- Boot Code Section ----
     * MUST be the first section so that _start (the entry point) is located
     * exactly at the image base.  The bootloader jumps here.
     * Contains: Linux ARM64 Image header, EL3→EL2→EL1 transition, exception
     * vector table installation, BSS clearing, stack setup, and jump to kernel_main.
     */
    .text.boot : {
        KEEP(*(.text.boot))
        KEEP(*(.text.boot.*))
    }

    /* ---- Kernel Code Section ----
     * All compiled kernel code (.text) and compiler-generated code (.text.*).
     * 4K-aligned so it can be mapped as read-only + executable in page tables.
     */
    .text : ALIGN(4K) {
        _text_start = .;
        *(.text)
        *(.text.*)
        _text_end = .;
    }

    /* ---- Read-Only Data Section ----
     * String literals, const tables, vtables, etc.
     * 4K-aligned for read-only page protection (no write, no execute).
     */
    .rodata : ALIGN(4K) {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        _rodata_end = .;
    }

    /* ---- Initialized Data Section ----
     * Global/static variables with non-zero initial values.
     * 4K-aligned for read-write (no execute) page protection.
     */
    .data : ALIGN(4K) {
        _data_start = .;
        *(.data)
        *(.data.*)
        _data_end = .;
    }

    /* ---- C++ Global Constructors (.init_array) ----
     * Function pointers for C++ global/static object constructors.
     * The boot code must iterate from __init_array_start to __init_array_end
     * and call each entry before entering kernel_main().
     */
    .init_array : ALIGN(8) {
        __init_array_start = .;
        KEEP(*(.init_array))
        KEEP(*(SORT(.init_array.*)))
        __init_array_end = .;
    }

    /* Relocations are applied once by the boot CPU before using absolute pointers. */
    .got : ALIGN(8) { *(.got) *(.got.*) }
    .dynamic : { *(.dynamic) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .rela.dyn : ALIGN(8) {
        _rela_start = .;
        *(.rela.dyn)
        _rela_end = .;
    }

    /* Last byte that must be present in the flat on-disk image.  Everything
     * below this marker is runtime-only storage and must remain NOLOAD. */
    _kernel_file_end = .;

    /* ---- BSS Section (Zero-Initialized Data) ----
     * Uninitialized globals and statics.  Occupies no space in the ELF file;
     * the boot code (start_arm64.S) zeros this region before calling kernel_main.
     * Also includes .bss.* subsections and COMMON symbols (C tentative definitions).
     */
    .bss (NOLOAD) : ALIGN(4K) {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        _bss_end = .;
    }

    /* ---- Per-CPU Kernel Stack ----
     * NOLOAD: does not occupy ELF file space; only reserves virtual address space.
     * Layout: 512 KB total = BOOT_MAX_CPUS (16) × 32 KB per CPU.
     *
     * Stack assignment at boot (start_arm64.S):
     *   cpu_sp = _stack_top - (cpu_id × 32KB)
     *
     * Stack grows downward (high → low address) on ARM64.
     *   CPU 0 stack: [_stack_top - 32K .. _stack_top)
     *   CPU 1 stack: [_stack_top - 64K .. _stack_top - 32K)
     *   ...
     *   CPU 15 stack: [_stack_bottom .. _stack_bottom + 32K)
     */
    .stack (NOLOAD) : ALIGN(4K) {
        _stack_bottom = .;
        . += 512K;
        _stack_top = .;
    }

    /* Reserved identity-mapped runtime heap, disjoint from page allocator RAM. */
    .heap (NOLOAD) : ALIGN(4K) {
        _heap_start = .;
        . += 8M;
        _heap_end = .;
    }

    /* ---- Early Page Table Area ----
     * NOLOAD: 64 KB reserved for boot-time page table construction.
     * Used by the boot code to set up identity mapping and (future) high-half
     * mapping before the full memory manager is initialized.
     *
     * Size calculation:
     *   - 1 L1 table (4 KB) + several L2/L3 tables
     *   - 64 KB supports ~16 page table pages, sufficient for early boot
     */
    .pagetables (NOLOAD) : ALIGN(4K) {
        _pagetable_start = .;
        . += 64K;
        _pagetable_end = .;
    }

    /* ---- End-of-Kernel Marker ----
     * Everything after this address is free RAM available for runtime allocation
     * (page frame allocator, slab allocator, user-space memory, etc.)
     */
    _kernel_memory_end = .;
    _kernel_end = _kernel_memory_end;

    /* Total kernel image size in bytes (used by Linux ARM64 Image header) */
    _kernel_image_size = ABSOLUTE(_kernel_memory_end - ORIGIN(RAM));

    /* ---- Discarded Sections ----
     * Remove sections that are unnecessary for a freestanding kernel:
     *   .note*     : ELF note sections (build-id, ABI tags)
     *   .comment*  : Compiler version strings
     *   .eh_frame* : C++ exception handling unwind tables (exceptions disabled)
     */
    /DISCARD/ : {
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
    }
}

ASSERT(_kernel_file_end <= _bss_start, "file-backed section placed after ARM64 runtime-only storage")
ASSERT(_kernel_memory_end <= ORIGIN(RAM) + LENGTH(RAM), "ARM64 kernel exceeds its RAM region")

/* =============================================================================
 * Exported Symbols for C++ Code
 * =============================================================================
 * PROVIDE() makes these symbols available to C++ code without causing
 * duplicate-symbol errors if they are not referenced.
 *
 * Usage in C++ (typically in boot or mm modules):
 *   extern "C" {
 *       extern char _text_start_addr;
 *       extern char _kernel_end_addr;
 *   }
 *   uintptr_t text_base = reinterpret_cast<uintptr_t>(&_text_start_addr);
 *
 * Note: These are address labels, not variables.  Use & (address-of) to get
 * the actual address value.  The _addr suffix distinguishes them from the
 * internal linker symbols used within this script.
 * ============================================================================= */
PROVIDE(_text_start_addr = _text_start);
PROVIDE(_text_end_addr = _text_end);
PROVIDE(_rodata_start_addr = _rodata_start);
PROVIDE(_rodata_end_addr = _rodata_end);
PROVIDE(_data_start_addr = _data_start);
PROVIDE(_data_end_addr = _data_end);
PROVIDE(_bss_start_addr = _bss_start);
PROVIDE(_bss_end_addr = _bss_end);
PROVIDE(_stack_bottom_addr = _stack_bottom);
PROVIDE(_stack_top_addr = _stack_top);
PROVIDE(_heap_start_addr = _heap_start);
PROVIDE(_heap_end_addr = _heap_end);
PROVIDE(_pagetable_start_addr = _pagetable_start);
PROVIDE(_pagetable_end_addr = _pagetable_end);
PROVIDE(_kernel_end_addr = _kernel_end);
