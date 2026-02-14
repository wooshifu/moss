/*
 * ARM64内核启动汇编代码
 * 处理多核启动序列和早期系统初始化
 *
 * == DTB (Device Tree Blob) 传递机制 ==
 *
 * ARM64 Linux 启动协议规定 bootloader 通过 x0 寄存器传递 DTB 物理地址。
 * 但 QEMU 的 -kernel 选项仅在识别到 Linux ARM64 Image 格式时遵循该协议：
 *
 *   QEMU hw/arm/boot.c 检查内核文件前 2 字节是否为 0x5A4D ("MZ")，
 *   以及偏移 0x38 处是否有 magic 0x644d5241 ("ARM\x64")。
 *   只有两者都匹配，QEMU 才会：
 *     1. 生成 DTB 并放入 RAM 高地址区域
 *     2. 将 DTB 物理地址写入 x0
 *     3. 按 Linux 启动协议跳转到内核入口
 *
 *   对于裸 ELF（如 MOSS）——没有 Linux Image 头部——QEMU 直接跳转到
 *   ELF entry point，x0 = 0，且不加载 DTB 到 RAM。
 *
 * 我们的解决方案分两部分：
 *
 *   1. run_qemu.py 启动前自动执行 QEMU -machine dumpdtb 导出 DTB，
 *      再通过 -device loader,addr=0x48000000 将 DTB 放入 RAM 已知地址。
 *
 *   2. 本文件在 EL1 初始化后、调用 early_main 之前，如果 x0 == 0 则
 *      线性扫描 RAM 区域查找 DTB magic。DTB 头部是大端 0xD00DFEED，
 *      ARM64 小端 ldr 指令读出为 0xEDFE0DD0，以此作为匹配值。
 *
 * 这使得 MOSS 不依赖 Linux 启动协议，同时兼容任何直接传递 DTB 的 bootloader。
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
    // 保存 x0（可能的 DTB 指针）到 callee-saved 寄存器 x20。
    // Linux 启动协议: bootloader 通过 x0 传递 DTB 物理地址。
    // QEMU 裸 ELF 模式: x0 = 0（不传递 DTB，后续由内存扫描补偿）。
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

    // --- DTB 内存扫描 ---
    //
    // 背景：QEMU -kernel 对裸 ELF 不通过 x0 传递 DTB（详见文件头注释）。
    // run_qemu.py 通过 -device loader 将 DTB 放入 RAM 的已知地址
    // (ARM64: 0x48000000)，但我们不硬编码该地址，而是通用地扫描
    // 整个 RAM 区域来查找 DTB magic。
    //
    // 字节序要点：
    //   DTB 规范要求头部 magic 固定为大端 0xD00DFEED，即内存中字节序为
    //   [D0][0D][FE][ED]。ARM64 的 ldr 指令以小端序组装 4 字节，读出
    //   结果为 0xEDFE0DD0。因此比较目标是 0xEDFE0DD0 而非 0xD00DFEED。
    //
    // 时机：此处已完成 EL3→EL2→EL1 下降、BSS 清零和栈设置，MMU 关闭，
    // 可安全读取任意物理地址。如果 bootloader 已通过 x0 传递了 DTB
    // (x20 != 0)，则跳过扫描。
    //
    cbnz    x20, .Ldtb_ready

    movz    w21, #0x0DD0             // DTB magic 的小端表示低 16 位
    movk    w21, #0xEDFE, lsl #16   // 完整值: 0xEDFE0DD0 (= big-endian 0xD00DFEED)

    // 从 RAM 顶部向下扫描，64KB 步进（DTB 至少页对齐）
    // QEMU virt machine: RAM 基址 0x40000000, 大小由 -m 参数决定
    movz    x24, #0x4FFF, lsl #16   // 扫描起始：0x4FFF0000 (接近 256MB RAM 顶端)
    movz    x22, #0x4000, lsl #16   // 扫描终止：0x40000000 (RAM 底部，含内核代码)

.Ldtb_scan_loop:
    cmp     x24, x22
    b.lo    .Ldtb_ready              // 扫描完毕未找到，x20 保持 0 → C++ 层走 fallback

    ldr     w26, [x24]
    cmp     w26, w21
    b.eq    .Ldtb_match

    sub     x24, x24, #0x10000      // 步进 64KB
    b       .Ldtb_scan_loop

.Ldtb_match:
    mov     x20, x24                 // 记录找到的 DTB 物理地址

.Ldtb_ready:
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

    // 🔧 关键修复：绕过轮询机制，直接跳转到Linux风格延迟激活
    // 从CPU不再等待启动标志，而是立即进入C++代码进行Linux风格的cpu_park()

    // 验证CPU ID是否在有效范围内 (MAX_CPUS = 8)
    cmp     x19, #8
    b.hs    invalid_cpu_id

    // 🔧 发送'D'表示直接跳转模式
    mov     x25, #0x09000000         // QEMU UART基址
    mov     x26, #'D'                // 'D' = Direct jump
    str     w26, [x25]
    add     x26, x19, #'0'           // CPU ID
    str     w26, [x25]
    mov     x26, #10                 // 换行
    str     w26, [x25]

    // 🔧 直接跳转到secondary_cpu_entry C++函数
    // 不需要轮询，让Linux风格的延迟激活机制处理同步
    adr     x23, secondary_cpu_entry_wrapper

    // 添加同步屏障确保执行流正确
    dsb     sy
    isb

    br      x23                      // 直接跳转到C++包装器

secondary_cpu_entry_wrapper:
    // 为C++函数调用准备栈和寄存器
    // 调用C++的secondary_cpu_entry函数
    bl      secondary_cpu_entry      // 调用C++函数

    // 如果函数返回（不应该发生），进入halt
    b       halt

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