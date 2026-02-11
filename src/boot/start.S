/*
 * ARM64内核启动汇编代码
 * 处理多核启动序列和早期系统初始化
 */

.section ".text.boot", "ax"
.global _start

// ARM64 系统寄存器定义
#define SCTLR_EL1_RES1    (0x30C50838)  // 保留位必须设为1
#define SCTLR_EL1_RES0    (0xCE8181C1)  // 保留位必须设为0
#define SCTLR_EL1_UCI     (1 << 26)     // 用户空间缓存指令访问
#define SCTLR_EL1_EE      (1 << 25)     // 异常端序
#define SCTLR_EL1_EOE     (1 << 24)     // EL0端序
#define SCTLR_EL1_WXN     (1 << 19)     // Write implies XN
#define SCTLR_EL1_nTWE    (1 << 18)     // EL0的WFE不陷入
#define SCTLR_EL1_nTWI    (1 << 16)     // EL0的WFI不陷入
#define SCTLR_EL1_UCT     (1 << 15)     // 用户空间CTR_EL0访问
#define SCTLR_EL1_DZE     (1 << 14)     // EL0的DC ZVA指令
#define SCTLR_EL1_I       (1 << 12)     // 指令缓存使能
#define SCTLR_EL1_UMA     (1 << 9)      // 用户空间掩码访问
#define SCTLR_EL1_SED     (1 << 8)      // SETEND禁用
#define SCTLR_EL1_ITD     (1 << 7)      // IT禁用
#define SCTLR_EL1_THEE    (1 << 6)      // ThumbEE使能
#define SCTLR_EL1_CP15BEN (1 << 5)      // CP15屏障使能
#define SCTLR_EL1_SA0     (1 << 4)      // EL0栈对齐检查
#define SCTLR_EL1_SA      (1 << 3)      // EL1栈对齐检查
#define SCTLR_EL1_C       (1 << 2)      // 数据缓存使能
#define SCTLR_EL1_A       (1 << 1)      // 对齐检查使能
#define SCTLR_EL1_M       (1 << 0)      // MMU使能

// HCR_EL2 设置值
#define HCR_EL2_RW        (1 << 31)     // 执行状态控制

// CPACR_EL1 设置值
#define CPACR_EL1_FPEN    (0x3 << 20)   // 浮点和NEON访问

// SPSR_EL2 设置值
#define SPSR_EL2_MASK     0x1F
#define SPSR_EL2_EL1h     0x05
#define SPSR_EL2_F        (1 << 6)
#define SPSR_EL2_I        (1 << 7)
#define SPSR_EL2_A        (1 << 8)
#define SPSR_EL2_D        (1 << 9)

_start:
    // 保存设备树指针（在x0中传入）
    mov     x20, x0

    // 获取当前CPU ID
    mrs     x19, mpidr_el1
    and     x19, x19, #0xFF     // 提取CPU ID

    // 检查异常级别
    mrs     x0, CurrentEL
    lsr     x0, x0, #2          // CurrentEL [3:2]

    // 如果在EL3，跳转到EL3处理
    cmp     x0, #3
    b.eq    el3_entry

    // 如果在EL2，跳转到EL2处理
    cmp     x0, #2
    b.eq    el2_entry

    // 如果在EL1，直接跳转到EL1处理
    b       el1_entry

el3_entry:
    // EL3配置：设置安全状态并下降到EL2

    // 配置SCR_EL3：非安全状态，AArch64
    mov     x0, #0x431           // RW | NS | SMD | HCE
    msr     scr_el3, x0

    // 设置SPSR_EL3：切换到EL2h模式
    mov     x0, #(SPSR_EL2_F | SPSR_EL2_I | SPSR_EL2_A | SPSR_EL2_D | SPSR_EL2_EL1h)
    msr     spsr_el3, x0

    // 设置ELR_EL3：返回地址为el2_entry
    adr     x0, el2_entry
    msr     elr_el3, x0

    // 异常返回到EL2
    eret

el2_entry:
    // EL2配置：设置虚拟化并下降到EL1

    // 配置HCR_EL2：设置为AArch64
    mov     x0, #HCR_EL2_RW
    msr     hcr_el2, x0

    // 设置SPSR_EL2：切换到EL1h模式
    mov     x0, #(SPSR_EL2_F | SPSR_EL2_I | SPSR_EL2_A | SPSR_EL2_D | 0x05)
    msr     spsr_el2, x0

    // 设置ELR_EL2：返回地址为el1_entry
    adr     x0, el1_entry
    msr     elr_el2, x0

    // 异常返回到EL1
    eret

el1_entry:
    // EL1初始化：这是我们的目标异常级别

    // 禁用MMU和缓存
    mrs     x0, sctlr_el1
    bic     x0, x0, #SCTLR_EL1_M    // 禁用MMU
    bic     x0, x0, #SCTLR_EL1_C    // 禁用数据缓存
    bic     x0, x0, #SCTLR_EL1_I    // 禁用指令缓存
    msr     sctlr_el1, x0

    // 设置异常向量表
    adr     x0, exception_vectors
    msr     vbar_el1, x0

    // 启用浮点和NEON
    mov     x0, #CPACR_EL1_FPEN
    msr     cpacr_el1, x0

    // 清零BSS段
    ldr     x0, =_bss_start_addr
    ldr     x1, =_bss_end_addr
clear_bss:
    cmp     x0, x1
    b.hs    bss_cleared
    str     xzr, [x0], #8
    b       clear_bss
bss_cleared:

    // 设置栈指针（每个CPU有独立的栈）
    ldr     x0, =_stack_top_addr
    mov     x1, #0x4000           // 每个CPU 16KB栈空间
    mul     x1, x19, x1          // CPU ID * 栈大小
    sub     sp, x0, x1

    // 只有主CPU（CPU 0）继续执行，其他CPU等待
    cmp     x19, #0
    b.ne    secondary_cpu_park

    // 主CPU：跳转到C++代码
    mov     x0, x20              // 传递设备树指针
    bl      early_main

    // 如果early_main返回，进入无限循环
    b       halt

secondary_cpu_park:
    // 从CPU进入等待状态
    // TODO: 实现SMP启动协议
secondary_wait:
    wfe                          // 等待事件
    b       secondary_wait

halt:
    // 系统停机
    wfi                          // 等待中断
    b       halt

// 异常向量表 - 简单实现，稍后扩展
.align 11
exception_vectors:
    // Current EL with SP0
    .align 7
    b       exception_handler    // Synchronous
    .align 7
    b       exception_handler    // IRQ
    .align 7
    b       exception_handler    // FIQ
    .align 7
    b       exception_handler    // SError

    // Current EL with SPx
    .align 7
    b       exception_handler    // Synchronous
    .align 7
    b       exception_handler    // IRQ
    .align 7
    b       exception_handler    // FIQ
    .align 7
    b       exception_handler    // SError

    // Lower EL using AArch64
    .align 7
    b       syscall_entry_point  // Synchronous (SVC from user space)
    .align 7
    b       exception_handler    // IRQ
    .align 7
    b       exception_handler    // FIQ
    .align 7
    b       exception_handler    // SError

    // Lower EL using AArch32
    .align 7
    b       exception_handler    // Synchronous
    .align 7
    b       exception_handler    // IRQ
    .align 7
    b       exception_handler    // FIQ
    .align 7
    b       exception_handler    // SError

exception_handler:
    // 简单的异常处理器 - 目前只是停机
    b       halt

// 导出符号给链接器
.global _bss_start_addr, _bss_end_addr, _stack_top_addr
.global exception_vectors

// 引用外部符号
.extern syscall_entry_point