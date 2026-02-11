/* 用户空间程序链接脚本 */
ENTRY(_start)

SECTIONS
{
    /* 程序起始地址 */
    . = 0x400000;

    /* 代码段 */
    .text : {
        *(.text*)
    }

    /* 只读数据段 */
    .rodata : {
        *(.rodata*)
    }

    /* 数据段 */
    .data : {
        *(.data*)
    }

    /* BSS段 */
    .bss : {
        *(.bss*)
        *(COMMON)
    }

    /* 丢弃不需要的段 */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}