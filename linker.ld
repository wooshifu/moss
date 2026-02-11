/* ARM64内核链接器脚本 */
ENTRY(_start)

/* 定义内存布局 */
MEMORY
{
    /* QEMU virt平台的内存布局 */
    RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 256M
}

/* 内核虚拟地址基址 */
KERNEL_VMA = 0xFFFF800000000000;

SECTIONS
{
    . = ORIGIN(RAM);

    /* 启动代码段 - 必须在最前面 */
    .text.boot : {
        *(.text.boot)
    }

    /* 主代码段 */
    .text : ALIGN(4K) {
        _text_start = .;
        *(.text)
        *(.text.*)
        _text_end = .;
    }

    /* 只读数据段 */
    .rodata : ALIGN(4K) {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        _rodata_end = .;
    }

    /* 数据段 */
    .data : ALIGN(4K) {
        _data_start = .;
        *(.data)
        *(.data.*)
        _data_end = .;
    }

    /* BSS段（零初始化数据） */
    .bss : ALIGN(4K) {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        _bss_end = .;
    }

    /* 栈空间 */
    .stack : ALIGN(4K) {
        _stack_bottom = .;
        . += 64K;  /* 64KB栈空间 */
        _stack_top = .;
    }

    /* 内核堆空间标记 - 运行时动态分配，预留少量占位空间 */
    .heap (NOLOAD) : ALIGN(4K) {
        _heap_start = .;
        . += 4K;  /* 预留4KB占位空间，避免与页表区重叠 */
        _heap_end = .;
    }

    /* 页表空间（早期启动使用） */
    .pagetables : ALIGN(4K) {
        _pagetable_start = .;
        . += 512K;  /* 512KB页表空间 */
        _pagetable_end = .;
    }

    /* 内核结束标记 */
    _kernel_end = .;

    /* 丢弃不需要的段 */
    /DISCARD/ : {
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
    }
}

/* 导出符号供C++代码使用 */
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