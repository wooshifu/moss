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

    // 🔧 调试：发送CPU ID到UART证明CPU正在启动
    mov     x25, #0x09000000         // QEMU UART基址
    mov     x26, #'['
    str     w26, [x25]
    add     x26, x19, #'0'           // 将CPU ID转换为ASCII数字
    str     w26, [x25]
    mov     x26, #']'
    str     w26, [x25]
    mov     x26, #10                 // 换行符
    str     w26, [x25]

    // 只有主CPU（CPU 0）继续执行，其他CPU等待
    cmp     x19, #0
    b.ne    secondary_cpu_park

    // 🔧 主CPU调试信息
    mov     x26, #'M'                // 主CPU标识
    str     w26, [x25]
    mov     x26, #'A'
    str     w26, [x25]
    mov     x26, #'I'
    str     w26, [x25]
    mov     x26, #'N'
    str     w26, [x25]
    mov     x26, #10                 // 换行符
    str     w26, [x25]

    // 主CPU：跳转到C++代码
    mov     x0, x20              // 传递设备树指针
    bl      early_main

    // 如果early_main返回，进入无限循环
    b       halt

secondary_cpu_park:
    // 🔧 关键调试：立即通过UART发送secondary CPU启动信号
    // 这样我们就能知道secondary CPU是否真正启动

    // 发送CPU ID到UART以证明secondary CPU正在运行
    mov     x25, #0x09000000         // QEMU UART基址
    mov     x26, #'C'                // 发送字符'C'表示CPU启动
    str     w26, [x25]
    mov     x26, #'P'                // 发送字符'P'
    str     w26, [x25]
    mov     x26, #'U'                // 发送字符'U'
    str     w26, [x25]

    // 发送CPU ID作为数字
    add     x26, x19, #'0'           // 将CPU ID转换为ASCII数字
    str     w26, [x25]

    mov     x26, #':'                // 发送分隔符
    str     w26, [x25]
    mov     x26, #'S'                // 发送'S'表示Started
    str     w26, [x25]
    mov     x26, #10                 // 发送换行符
    str     w26, [x25]

    // 从CPU进入SMP启动协议
    // 动态CPU检测和启动标志轮询机制

    // 1. 验证CPU ID是否在有效范围内 (MAX_CPUS = 8)
    cmp     x19, #8
    b.hs    invalid_cpu_id        // 如果 CPU ID >= 8，跳转到错误处理

    // 2. 计算启动标志地址：cpu_startup_flags[cpu_id]
    ldr     x21, =cpu_startup_flags  // 加载启动标志数组基址
    mov     x22, #16                 // 每个CPU启动信息占16字节 (entry_point: 8字节 + flags: 8字节)
    mul     x22, x19, x22            // CPU ID * sizeof(CpuStartupEntry)
    add     x21, x21, x22            // x21 = &cpu_startup_flags[cpu_id]

    // 3. 轮询启动标志
cpu_startup_poll:
    // 🔧 添加内存屏障确保从主存读取最新数据
    dmb     sy                       // 数据内存屏障，确保读取一致性

    // 加载启动入口点地址
    ldr     x23, [x21]               // x23 = cpu_startup_flags[cpu_id].entry_point

    // 检查启动标志
    ldr     x24, [x21, #8]           // x24 = cpu_startup_flags[cpu_id].startup_flag

    // 🔧 再次添加内存屏障确保读取完成
    dmb     sy                       // 数据内存屏障

    cbz     x23, cpu_startup_wait    // 如果 entry_point == NULL，继续等待
    cbz     x24, cpu_startup_wait    // 如果 startup_flag == 0，继续等待

    // CPU被唤醒！跳转到初始化代码
    // x23 包含了从CPU的入口点地址

    // 🔧 在跳转前添加指令同步屏障
    dsb     sy                       // 数据同步屏障，确保所有内存操作完成
    isb                              // 指令同步屏障，刷新指令流水线

    br      x23                      // 跳转到 secondary_cpu_entry

cpu_startup_wait:
    // 使用WFE节能等待
    wfe                              // 等待事件 (SEV指令会唤醒)

    // 🔧 添加短暂延迟以降低轮询频率
    mov     x25, #1000               // 延迟计数器
cpu_startup_delay:
    subs    x25, x25, #1             // 递减计数器
    bne     cpu_startup_delay        // 如果不为零，继续延迟

    b       cpu_startup_poll         // 继续轮询

invalid_cpu_id:
    // CPU ID超出范围，进入永久等待
    wfi                              // 等待中断 (永不会来)
    b       invalid_cpu_id

// 导出CPU启动标志数组给链接器
.global cpu_startup_flags

// CPU启动标志数据结构 (在BSS段分配)
.section ".bss", "aw", @nobits
.align 8
cpu_startup_flags:
    // 为每个CPU分配启动控制块: [entry_point:8字节] [startup_flag:8字节]
    .space 8 * 16                    // MAX_CPUS=8, 每个CPU 16字节

// 切换回文本段，放置可执行代码
.section ".text.boot", "ax"

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