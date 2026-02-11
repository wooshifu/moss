/*
 * x86_64 内核启动汇编代码
 * 最小启动存根，待完整实现
 */

.section ".text.boot", "ax"
.global _start
.code64

_start:
    // 清除方向标志
    cld

    // 设置内核栈（临时）
    lea stack_top(%rip), %rsp

    // 清零BSS段
    lea __bss_start(%rip), %rdi
    lea __bss_end(%rip), %rcx
    sub %rdi, %rcx
    xor %eax, %eax
    rep stosb

    // 跳转到C++内核主函数
    // TODO: 实现完整的x86_64启动序列
    jmp kernel_main_stub

// 临时内核栈
.section ".bss.stack", "aw", @nobits
.align 16
stack_bottom:
    .skip 8192  // 8KB栈空间
stack_top:

// 临时符号定义
.section ".text"
kernel_main_stub:
    // 临时实现 - 无限循环
    cli
1:  hlt
    jmp 1b

// BSS段标记（由链接器脚本定义）
.weak __bss_start
.weak __bss_end
__bss_start = 0
__bss_end = 0