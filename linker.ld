/* =============================================================================
 * MOSS Kernel Linker Script (ARM64 / QEMU virt platform)
 * =============================================================================
 *
 * This linker script defines the physical memory layout for the MOSS kernel
 * image when running on the QEMU "virt" machine for AArch64.
 *
 * Memory Map Overview (physical addresses):
 * ┌──────────────────────────────────────────────────────────────┐
 * │ 0x4000_0000  RAM start (QEMU virt platform)                  │
 * │ ...          QEMU stub loader / DTB (first 2 MB)             │
 * │ 0x4020_0000  ┌─────────────────┐  Kernel load address        │
 * │              │ .text.boot       │  Boot code (entry point)   │
 * │              ├─────────────────┤  4K aligned                 │
 * │              │ .text            │  Kernel code               │
 * │              ├─────────────────┤  4K aligned                 │
 * │              │ .rodata          │  Read-only data            │
 * │              ├─────────────────┤  4K aligned                 │
 * │              │ .data            │  Initialized data          │
 * │              ├─────────────────┤  4K aligned                 │
 * │              │ .bss             │  Zero-initialized data     │
 * │              ├─────────────────┤  4K aligned                 │
 * │              │ .stack (NOLOAD)  │  256 KB (8 CPUs × 32 KB)  │
 * │              ├─────────────────┤  4K aligned                 │
 * │              │ .heap  (NOLOAD)  │  4 KB placeholder          │
 * │              ├─────────────────┤  4K aligned                 │
 * │              │ .pagetables      │  64 KB early page tables   │
 * │              ├─────────────────┤                             │
 * │              │ _kernel_end      │  End of kernel image       │
 * │              └─────────────────┘                             │
 * │ ...          Free RAM for runtime allocation                 │
 * │ 0x4FFF_FFFF  End of 256 MB RAM region                       │
 * └──────────────────────────────────────────────────────────────┘
 *
 * Key design decisions:
 *   - All sections are 4K-aligned for page-granularity protection
 *   - .text.boot is placed FIRST so _start is at the load address (0x4020_0000)
 *   - NOLOAD sections (.stack, .heap, .pagetables) don't occupy ELF file space
 *   - KERNEL_VMA is defined for future high-half kernel mapping (0xFFFF_8000_0000_0000)
 *   - Symbols exported via PROVIDE() are referenced by C++ code through extern "C"
 *
 * Platform notes:
 *   - QEMU "virt" places RAM at 0x4000_0000 (1 GB mark)
 *   - DTB is passed by QEMU in x0 register at boot
 *   - The Linux ARM64 Image header in start_arm64.S references _kernel_image_size
 *
 * Referenced by:
 *   - start_arm64.S: _start entry, _bss_start/_bss_end, _stack_top, _pagetable_start
 *   - boot module C++ code: all PROVIDE'd _*_addr symbols via extern "C"
 *   - mm module: _heap_start_addr, _pagetable_start_addr for runtime allocators
 * ============================================================================= */

/* Entry point: _start is defined in start_arm64.S (.text.boot section) */
ENTRY(_start)

/* =============================================================================
 * Physical Memory Region
 * =============================================================================
 * QEMU virt machine memory map:
 *   0x0000_0000 - 0x3FFF_FFFF : I/O devices (GIC, UART, etc.)
 *   0x4000_0000 - 0x4FFF_FFFF : RAM (256 MB, configurable with -m)
 *
 * QEMU's Linux Image loader places the kernel at RAM + 2 MB (0x4020_0000)
 * when text_offset = 0, reserving the first 2 MB for the stub loader and DTB.
 * The ORIGIN must match this actual load address so that absolute addresses
 * (function pointers, global variables, vtables) resolve correctly.
 *
 * The (rwx) attributes allow read/write/execute for all sections.
 * Fine-grained permissions are enforced at the page table level, not here.
 * ============================================================================= */
MEMORY
{
    RAM (rwx) : ORIGIN = 0x40200000, LENGTH = 254M
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
     * exactly at ORIGIN(RAM) = 0x4000_0000.  The bootloader / QEMU jumps here.
     * Contains: Linux ARM64 Image header, EL3→EL2→EL1 transition, exception
     * vector table installation, BSS clearing, stack setup, and jump to kernel_main.
     */
    .text.boot : {
        *(.text.boot)
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

    /* ---- BSS Section (Zero-Initialized Data) ----
     * Uninitialized globals and statics.  Occupies no space in the ELF file;
     * the boot code (start_arm64.S) zeros this region before calling kernel_main.
     * Also includes .bss.* subsections and COMMON symbols (C tentative definitions).
     */
    .bss : ALIGN(4K) {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        _bss_end = .;
    }

    /* ---- Per-CPU Kernel Stack ----
     * NOLOAD: does not occupy ELF file space; only reserves virtual address space.
     * Layout: 256 KB total = MAX_CPUS (8) × 32 KB per CPU.
     *
     * Stack assignment at boot (start_arm64.S):
     *   cpu_sp = _stack_top - (cpu_id × 32KB)
     *
     * Stack grows downward (high → low address) on ARM64.
     *   CPU 0 stack: [_stack_top - 32K .. _stack_top)
     *   CPU 1 stack: [_stack_top - 64K .. _stack_top - 32K)
     *   ...
     *   CPU 7 stack: [_stack_bottom .. _stack_bottom + 32K)
     */
    .stack (NOLOAD) : ALIGN(4K) {
        _stack_bottom = .;
        . += 256K;
        _stack_top = .;
    }

    /* ---- Kernel Heap Placeholder ----
     * NOLOAD: reserves a small placeholder region for the runtime heap.
     * The actual heap is managed by RuntimeHeapAllocator which may extend
     * beyond this 4K region into free RAM after _kernel_end.
     * The 4K placeholder prevents overlap with the page table region below.
     */
    .heap (NOLOAD) : ALIGN(4K) {
        _heap_start = .;
        . += 4K;
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
    _kernel_end = .;

    /* Total kernel image size in bytes (used by Linux ARM64 Image header) */
    _kernel_image_size = . - ORIGIN(RAM);

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
