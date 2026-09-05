/* =============================================================================
 * MOSS Kernel Linker Script (x86_64 / QEMU q35 platform)
 * =============================================================================
 *
 * Memory layout for x86_64 long mode on QEMU q35 machine.
 *
 * x86_64 QEMU q35 memory map:
 *   0x0000_0000 - 0x0009_FFFF : Conventional memory (640 KB)
 *   0x000A_0000 - 0x000F_FFFF : VGA + ROM area
 *   0x0010_0000 - ...         : Extended memory (usable RAM)
 *
 * The kernel is loaded at 1 MB (0x0010_0000), the traditional x86 extended
 * memory start.  The boot stub (start_x86_64.S) runs in 64-bit long mode
 * assuming the firmware/loader already set up identity-mapped page tables.
 * ============================================================================= */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x00100000, LENGTH = 255M
}

KERNEL_VMA = 0xFFFF800000000000;

SECTIONS
{
    . = ORIGIN(RAM);

    .text.boot : {
        KEEP(*(.text.boot))
        KEEP(*(.text.boot.*))
    }

    .text : ALIGN(4K) {
        _text_start = .;
        *(.text)
        *(.text.*)
        _text_end = .;
    }

    .rodata : ALIGN(4K) {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        _rodata_end = .;
    }

    .data : ALIGN(4K) {
        _data_start = .;
        *(.data)
        *(.data.*)
        _data_end = .;
    }

    .init_array : ALIGN(8) {
        __init_array_start = .;
        KEEP(*(.init_array))
        KEEP(*(SORT(.init_array.*)))
        __init_array_end = .;
    }

    /* QEMU discovers the 32-bit PVH entry through this ELF note. Keep it in
     * the file-backed prefix: placing it after NOBITS sections makes objcopy
     * materialize the entire intervening runtime address range in moss.bin. */
    .note.Xen : ALIGN(4) {
        KEEP(*(.note.Xen))
    }

    _kernel_file_end = .;

    /* PVH page tables — used by _pvh_start to build identity map before
     * enabling paging.  Placed OUTSIDE .bss so that _start's BSS clearing
     * does not destroy the live page tables we are running on. */
    .pvh_pagetables (NOLOAD) : ALIGN(4K) {
        KEEP(*(.pvh.pagetables))
    }

    .bss (NOLOAD) : ALIGN(4K) {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        _bss_end = .;
    }

    .stack (NOLOAD) : ALIGN(4K) {
        _stack_bottom = .;
        . += 512K;
        _stack_top = .;
    }

    .heap (NOLOAD) : ALIGN(4K) {
        _heap_start = .;
        . += 4K;
        _heap_end = .;
    }

    .pagetables (NOLOAD) : ALIGN(4K) {
        _pagetable_start = .;
        . += 64K;
        _pagetable_end = .;
    }

    _kernel_memory_end = .;
    _kernel_end = _kernel_memory_end;
    _kernel_image_size = _kernel_memory_end - ORIGIN(RAM);

    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.note.gnu*)
        *(.comment*)
        *(.eh_frame*)
    }
}

ASSERT(_kernel_file_end <= ADDR(.pvh_pagetables), "file-backed section placed after x86_64 runtime-only storage")
ASSERT(_kernel_memory_end <= ORIGIN(RAM) + LENGTH(RAM), "x86_64 kernel exceeds its RAM region")

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
