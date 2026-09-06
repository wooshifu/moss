/* RV64 relocatable Linux Image. SBI firmware supplies hart ID and DTB. */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0, LENGTH = 256M
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

    /* RISC-V global pointer for linker relaxation (must be in .sdata range) */
    PROVIDE(__global_pointer$ = .);
    .sdata : {
        *(.sdata)
        *(.sdata.*)
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

    /* Last byte stored in moss.bin. Runtime-only allocations follow. */
    _kernel_file_end = .;

    .bss (NOLOAD) : ALIGN(4K) {
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
        . += 8M;
        _heap_end = .;
    }

    .pagetables (NOLOAD) : ALIGN(4K) {
        _pagetable_start = .;
        . += 64K;
        _pagetable_end = .;
    }

    _kernel_memory_end = .;
    _kernel_end = _kernel_memory_end;
    _kernel_image_size = ABSOLUTE(_kernel_memory_end - ORIGIN(RAM));

    /DISCARD/ : {
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
    }
}

ASSERT(_kernel_file_end <= _bss_start, "file-backed section placed after RISC-V runtime-only storage")
ASSERT(_kernel_memory_end <= ORIGIN(RAM) + LENGTH(RAM), "RISC-V kernel exceeds its RAM region")

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
