/* RV64 relocatable Linux Image. SBI firmware supplies hart ID and DTB. */

ENTRY(_start)

MEMORY
{
    /* 256 MiB bounds link-image capacity; it is not firmware RAM discovery.
     * Origin zero allows relocation. The exact capacity budget is undocumented. */
    RAM (rwx) : ORIGIN = 0, LENGTH = 256M
}

KERNEL_VMA = 0xFFFF800000000000;

SECTIONS
{
    /* 4 KiB boundaries separate RX/RO/RW page permissions. 8-byte init-array,
     * GOT and relocation alignment matches the 64-bit pointers/ELF64 fields. */
    . = ORIGIN(RAM);
    _text_start = .; /* Boot code and ordinary text share the RX protection range. */

    .text.boot : {
        KEEP(*(.text.boot))
        KEEP(*(.text.boot.*))
    }

    .text : ALIGN(4K) {
        *(.text)
        *(.text.*)
        . = ALIGN(4K);
        _text_end = .;
    }

    .rodata : ALIGN(4K) {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4K);
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

    /* RISC-V 64 global pointer for linker relaxation (must be in .sdata range) */
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

    /* 512 KiB = 16 BOOT_MAX_CPUS * 32 KiB stacks, matching ARM64 boot slices.
     * Stack budget is fixed policy, without recorded maximum-depth evidence. */
    .stack (NOLOAD) : ALIGN(4K) {
        _stack_bottom = .;
        . += 512K;
        _stack_top = .;
    }

    /* Fixed 8 MiB heap reserve; boot initializes a 256 KiB subspan. The exact
     * reserve budget is undocumented; enlarging it consumes image-reserved RAM. */
    .heap (NOLOAD) : ALIGN(4K) {
        _heap_start = .;
        . += 8M;
        _heap_end = .;
    }

    /* 64 KiB holds sixteen 4 KiB bootstrap tables before buddy allocation is
     * ready; exhaustion fails boot. Exact worst-case sizing is undocumented. */
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

ASSERT(_kernel_file_end <= _bss_start, "file-backed section placed after RISC-V 64 runtime-only storage")
ASSERT(_kernel_memory_end <= ORIGIN(RAM) + LENGTH(RAM), "RISC-V 64 kernel exceeds its RAM region")

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
