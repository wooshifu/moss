/*
 * RISC-V 64位内核启动汇编代码
 * 最小启动存根，待完整实现
 */

.section ".text.boot", "ax"
.global _start

_start:
    // 设置全局指针寄存器
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    // 设置内核栈（临时）
    la sp, stack_top

    // 清零BSS段
    la t0, __bss_start
    la t1, __bss_end
    bgeu t0, t1, 2f
1:
    sd zero, (t0)
    addi t0, t0, 8
    bltu t0, t1, 1b
2:

    // 跳转到C++内核主函数
    // TODO: 实现完整的RISC-V启动序列
    call kernel_main_stub

// 临时内核栈
.section ".bss.stack", "aw", @nobits
.align 16
stack_bottom:
    .skip 8192  // 8KB栈空间
stack_top:

// 临时符号定义
.section ".text"
kernel_main_stub:
    // 临时实现 - 无限循环等待中断
1:  wfi
    j 1b

// BSS段和全局指针标记（由链接器脚本定义）
.weak __bss_start
.weak __bss_end
.weak __global_pointer$
__bss_start = 0
__bss_end = 0
__global_pointer$ = 0