/* =============================================================================
 * MOSS Kernel Linker Script (RISC-V 64 / QEMU virt platform)
 * =============================================================================
 *
 * Memory layout for RISC-V 64-bit on QEMU virt machine.
 *
 * RISC-V QEMU virt memory map:
 *   0x0000_0000 - 0x7FFF_FFFF : I/O devices (PLIC, UART, etc.)
 *   0x8000_0000 - ...         : RAM start
 *   0x8000_0000 - 0x801F_FFFF : OpenSBI firmware (first 2 MB)
 *   0x8020_0000 - ...         : Kernel load address
 *
 * OpenSBI occupies the first 2 MB of RAM, so the kernel is loaded at
 * RAM + 2 MB (0x8020_0000).  The DTB pointer is passed in a1 by OpenSBI.
 * ============================================================================= */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80200000, LENGTH = 254M
}

KERNEL_VMA = 0xFFFF800000000000;

SECTIONS
{
    . = ORIGIN(RAM);

    .text.boot : {
        *(.text.boot)
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

    /* RISC-V global pointer for linker relaxation (must be in .sdata range) */
    PROVIDE(__global_pointer$ = .);
    .sdata : {
        *(.sdata)
        *(.sdata.*)
    }

    .bss : ALIGN(4K) {
        _bss_start = .;
        *(.sbss)
        *(.sbss.*)
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

    _kernel_end = .;
    _kernel_image_size = . - ORIGIN(RAM);

    /DISCARD/ : {
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
    }
}

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
