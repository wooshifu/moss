/*
 * ARM64内核启动汇编代码
 * 处理多核启动序列和早期系统初始化
 *
 * == DTB (Device Tree Blob) 双重发现机制 ==
 *
 * MOSS 支持两种 DTB 传递方式，按优先级依次尝试：
 *
 *   方式 1（优先）：Linux 启动协议 — x0 寄存器传递
 *     bootloader/QEMU 通过 x0 传递 DTB 物理地址。QEMU -kernel 模式仅在
 *     识别到 Linux ARM64 Image 格式时遵循该协议（检查 "MZ" 和 "ARM\x64"
 *     magic）。使用 moss.img（Linux Image 格式）启动时自动生效。
 *     验证逻辑：x0 非零 + 目标地址 DTB magic 匹配，两个条件都满足才采用。
 *
 *   方式 2（Fallback）：RAM 线性扫描
 *     当 x0 为 0 或 x0 指向的地址不含有效 DTB magic 时，从 RAM 顶部向下
 *     64KB 步进扫描查找 DTB magic (0xD00DFEED big-endian)。
 *     run_qemu.py 通过 -device loader 将 DTB 放入 RAM 已知地址以配合扫描。
 *
 * 两种方式互为补充：Linux Image 模式使用方式 1，裸 ELF 模式使用方式 2。
 * 无论哪种方式失败，C++ 层的 FDT 解析器都有 hardcoded fallback。
 */

.section ".text.boot", "ax"
.global _start

// ============================================================================
// ARM64 Linux Image Header (64 bytes)
//
// This header makes the kernel binary compatible with the Linux ARM64 boot
// protocol. QEMU checks for the magic "ARM\x64" at offset 0x38 to identify
// the image format. When detected, QEMU automatically generates a DTB and
// passes its address via x0, eliminating the need for manual DTB generation.
//
// The `b _start` instruction at offset 0 branches over the header to the
// actual kernel entry point. For ELF boot (where this header is not at the
// file start), _start is reached directly via the ELF entry point.
// ============================================================================
_linux_header:
    b       _start                      // code0: branch to actual entry
    .word   0                           // code1: reserved
    .quad   0                           // text_offset: load at RAM base + 0
    .quad   _kernel_image_size          // image_size: filled by linker
    .quad   0                           // flags: little-endian, 4K pages
    .quad   0                           // res2
    .quad   0                           // res3
    .quad   0                           // res4
    .word   0x644d5241                  // magic: "ARM\x64"
    .word   0                           // res5 (PE offset)

// ============================================================================
// Actual kernel entry point (offset 0x40 from section start)
// ============================================================================

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

    // Secondary CPUs must NOT re-run cold-boot initialization (BSS clear,
    // MMU disable, etc.) because CPU 0 has already set up the system.
    // They only need a stack and then jump to secondary_cpu_park.
    cbnz    x19, secondary_el1_entry

    // === CPU 0 cold-boot path ===

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

    // 清零BSS段 (CPU 0 only — secondary CPUs skip this)
    ldr     x0, =_bss_start_addr
    ldr     x1, =_bss_end_addr
clear_bss:
    cmp     x0, x1
    b.hs    bss_cleared
    str     xzr, [x0], #8
    b       clear_bss
bss_cleared:

    // 设置CPU 0栈指针
    ldr     x0, =_stack_top_addr
    mov     sp, x0

    // 🔧 调试：发送CPU ID到UART证明CPU正在启动 (locked)
    bl      early_uart_acquire
    mov     x25, #0x09000000         // QEMU UART基址
    mov     x26, #'['
    str     w26, [x25]
    mov     x26, #'0'               // CPU 0
    str     w26, [x25]
    mov     x26, #']'
    str     w26, [x25]
    mov     x26, #10                 // 换行符
    str     w26, [x25]
    bl      early_uart_release

    b       primary_cpu_continue

    // === Secondary CPU entry (PSCI CPU_ON lands here via _start) ===
secondary_el1_entry:
    // Do NOT clear BSS, do NOT disable MMU (CPU 0 already set up everything).
    // Just set up per-CPU stack and jump to park.

    // 设置栈指针（每个CPU有独立的栈）
    ldr     x0, =_stack_top_addr
    mov     x1, #0x8000           // 每个CPU 32KB栈空间
    mul     x1, x19, x1          // CPU ID * 栈大小
    sub     sp, x0, x1

    // 🔧 调试：发送CPU ID到UART (locked)
    bl      early_uart_acquire
    mov     x25, #0x09000000
    mov     x26, #'['
    str     w26, [x25]
    add     x26, x19, #'0'
    str     w26, [x25]
    mov     x26, #']'
    str     w26, [x25]
    mov     x26, #10
    str     w26, [x25]
    bl      early_uart_release

    b       secondary_cpu_park

primary_cpu_continue:

    // 🔧 主CPU调试信息 (locked)
    bl      early_uart_acquire
    mov     x25, #0x09000000
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
    bl      early_uart_release

    // --- DTB 双重发现机制 ---
    //
    // DTB magic: big-endian 0xD00DFEED → little-endian ldr 读出 0xEDFE0DD0
    //
    // 策略：
    //   1. 如果 x20 != 0（bootloader 传递），验证其 DTB magic
    //   2. 如果 x20 == 0 或 magic 不匹配，fallback 到 RAM 线性扫描
    //   3. 都失败则 x20 = 0，C++ 层走 hardcoded fallback
    //
    movz    w21, #0x0DD0             // DTB magic 低 16 位
    movk    w21, #0xEDFE, lsl #16   // 完整值: 0xEDFE0DD0

    // --- 方式 1: 验证 x0 传递的 DTB 指针 ---
    cbz     x20, .Ldtb_ram_scan     // x0 == 0 → 跳到 RAM 扫描

    ldr     w26, [x20]              // 读取 x0 指向的前 4 字节
    cmp     w26, w21                // 与 DTB magic 比较
    b.eq    .Ldtb_ready             // magic 匹配 → 使用 x0 传递的 DTB
    mov     x20, xzr                // magic 不匹配 → 清零，进入扫描

    // --- 方式 2 (Fallback): RAM 线性扫描 ---
.Ldtb_ram_scan:
    // 从 RAM 顶部向下扫描，64KB 步进（DTB 至少页对齐）
    // QEMU virt machine: RAM 基址 0x40000000, 大小 256MB (-m 参数)
    movz    x24, #0x4FFF, lsl #16   // 扫描起始：0x4FFF0000
    movz    x22, #0x4000, lsl #16   // 扫描终止：0x40000000

.Ldtb_scan_loop:
    cmp     x24, x22
    b.lo    .Ldtb_ready              // 扫描完毕未找到，x20 = 0 → C++ fallback

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
    // 🔧 关键调试：立即通过UART发送secondary CPU启动信号 (locked)
    bl      early_uart_acquire
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
    bl      early_uart_release

    // 🔧 关键修复：绕过轮询机制，直接跳转到Linux风格延迟激活
    // 从CPU不再等待启动标志，而是立即进入C++代码进行Linux风格的cpu_park()

    // 验证CPU ID是否在有效范围内 (MAX_CPUS = 8)
    cmp     x19, #8
    b.hs    invalid_cpu_id

    // 🔧 发送'D'表示直接跳转模式 (locked)
    bl      early_uart_acquire
    mov     x25, #0x09000000         // QEMU UART基址
    mov     x26, #'D'                // 'D' = Direct jump
    str     w26, [x25]
    add     x26, x19, #'0'           // CPU ID
    str     w26, [x25]
    mov     x26, #10                 // 换行
    str     w26, [x25]
    bl      early_uart_release

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

// Global early UART spinlock — shared between assembly and C++ boot code.
// 0 = unlocked, 1 = locked.  Simple test-and-set via LDXR/STXR.
.global early_uart_lock
.align 4
early_uart_lock:
    .word 0

// 切换回文本段，放置可执行代码
.section ".text.boot", "ax"

// ============================================================================
// Early UART spinlock acquire/release subroutines
// Uses LDXR/STXR test-and-set on early_uart_lock.
// Clobbers: x24, x27, x28 (scratch registers, not used by UART print sequences).
// ============================================================================
early_uart_acquire:
    adr     x27, early_uart_lock
1:
    ldxr    w28, [x27]
    cbnz    w28, 1b               // spin while locked
    mov     w28, #1
    stxr    w24, w28, [x27]       // w24 = status (must differ from w28)
    cbnz    w24, 1b               // retry if store-exclusive failed
    dmb     sy                     // acquire barrier
    ret

early_uart_release:
    adr     x27, early_uart_lock
    dmb     sy                     // release barrier
    str     wzr, [x27]
    ret

halt:
    // 系统停机
    wfi                          // 等待中断
    b       halt

// 异常向量表
// Each vector entry is 128 bytes (2^7 aligned).
// IRQ vectors route to irq_trampoline which saves context, calls C handler,
// restores context, and returns via eret.
.align 11
exception_vectors:
    // Current EL with SP0
    .align 7
    b       exception_handler    // Synchronous
    .align 7
    b       irq_trampoline       // IRQ
    .align 7
    b       exception_handler    // FIQ
    .align 7
    b       exception_handler    // SError

    // Current EL with SPx (kernel running mode)
    .align 7
    b       exception_handler    // Synchronous
    .align 7
    b       irq_trampoline       // IRQ
    .align 7
    b       exception_handler    // FIQ
    .align 7
    b       exception_handler    // SError

    // Lower EL using AArch64
    // NOTE: SP_EL1 is pre-set to the current thread's kernel stack top before
    // entering EL0 (in switch_to_user and after irq_trampoline cleanup).
    // So when exceptions from EL0 arrive here, SP is already correct.
    .align 7
    b       lower_el_sync_dispatch  // Synchronous (SVC / Data Abort / Instr Abort)
    .align 7
    b       irq_trampoline       // IRQ — SP_EL1 pre-set to per-thread kernel stack
    .align 7
    b       exception_handler    // FIQ
    .align 7
    b       exception_handler    // SError

    // Lower EL using AArch32
    .align 7
    b       exception_handler    // Synchronous
    .align 7
    b       irq_trampoline       // IRQ — SP_EL1 pre-set to per-thread kernel stack
    .align 7
    b       exception_handler    // FIQ
    .align 7
    b       exception_handler    // SError

// ============================================================================
// IRQ trampoline: save caller-saved registers, call C handler, restore, eret
//
// For exceptions from EL0 (user-mode), SP_EL1 is pre-set to the current
// thread's per-thread kernel stack top before entering user mode:
//   - switch_to_user: sets SP = kernel_stack_top via TPIDR_EL1 before eret
//   - irq_trampoline: after `add sp, #(34*8)`, SP_EL1 = kernel_stack_top
// So when we arrive here from either Same EL or Lower EL, SP is always valid.
// ============================================================================
irq_trampoline:
    // Save general-purpose registers (all caller-saved + some callee-saved
    // needed for safe return). Save ELR_EL1 and SPSR_EL1 for nested handling.
    sub     sp, sp, #(34 * 8)    // 34 slots: x0-x30, ELR, SPSR, pad

    stp     x0,  x1,  [sp, #(0  * 8)]
    stp     x2,  x3,  [sp, #(2  * 8)]
    stp     x4,  x5,  [sp, #(4  * 8)]
    stp     x6,  x7,  [sp, #(6  * 8)]
    stp     x8,  x9,  [sp, #(8  * 8)]
    stp     x10, x11, [sp, #(10 * 8)]
    stp     x12, x13, [sp, #(12 * 8)]
    stp     x14, x15, [sp, #(14 * 8)]
    stp     x16, x17, [sp, #(16 * 8)]
    stp     x18, x19, [sp, #(18 * 8)]
    stp     x20, x21, [sp, #(20 * 8)]
    stp     x22, x23, [sp, #(22 * 8)]
    stp     x24, x25, [sp, #(24 * 8)]
    stp     x26, x27, [sp, #(26 * 8)]
    stp     x28, x29, [sp, #(28 * 8)]
    str     x30,       [sp, #(30 * 8)]

    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0,  x1,  [sp, #(31 * 8)]

    // Call C interrupt handler
    bl      irq_handler_c

    // Restore ELR_EL1 and SPSR_EL1
    ldp     x0,  x1,  [sp, #(31 * 8)]
    msr     elr_el1, x0
    msr     spsr_el1, x1

    // Restore general-purpose registers
    ldp     x0,  x1,  [sp, #(0  * 8)]
    ldp     x2,  x3,  [sp, #(2  * 8)]
    ldp     x4,  x5,  [sp, #(4  * 8)]
    ldp     x6,  x7,  [sp, #(6  * 8)]
    ldp     x8,  x9,  [sp, #(8  * 8)]
    ldp     x10, x11, [sp, #(10 * 8)]
    ldp     x12, x13, [sp, #(12 * 8)]
    ldp     x14, x15, [sp, #(14 * 8)]
    ldp     x16, x17, [sp, #(16 * 8)]
    ldp     x18, x19, [sp, #(18 * 8)]
    ldp     x20, x21, [sp, #(20 * 8)]
    ldp     x22, x23, [sp, #(22 * 8)]
    ldp     x24, x25, [sp, #(24 * 8)]
    ldp     x26, x27, [sp, #(26 * 8)]
    ldp     x28, x29, [sp, #(28 * 8)]
    ldr     x30,       [sp, #(30 * 8)]

    add     sp, sp, #(34 * 8)

    eret

// ============================================================================
// Synchronous exception handler — dispatch by Exception Class (EC)
//
// Reads ESR_EL1, FAR_EL1, ELR_EL1, extracts EC field, and routes to:
//   EC=0x25 (Data Abort, same EL)        → kernel_page_fault_handler (C++)
//   EC=0x21 (Instruction Abort, same EL) → kernel_page_fault_handler (C++)
//   Other EC                              → unhandled_exception_handler (C++) → halt
//
// Register save/restore follows the same 34-slot layout as irq_trampoline.
// ============================================================================
exception_handler:
    // Save all general-purpose registers
    sub     sp, sp, #(34 * 8)

    stp     x0,  x1,  [sp, #(0  * 8)]
    stp     x2,  x3,  [sp, #(2  * 8)]
    stp     x4,  x5,  [sp, #(4  * 8)]
    stp     x6,  x7,  [sp, #(6  * 8)]
    stp     x8,  x9,  [sp, #(8  * 8)]
    stp     x10, x11, [sp, #(10 * 8)]
    stp     x12, x13, [sp, #(12 * 8)]
    stp     x14, x15, [sp, #(14 * 8)]
    stp     x16, x17, [sp, #(16 * 8)]
    stp     x18, x19, [sp, #(18 * 8)]
    stp     x20, x21, [sp, #(20 * 8)]
    stp     x22, x23, [sp, #(22 * 8)]
    stp     x24, x25, [sp, #(24 * 8)]
    stp     x26, x27, [sp, #(26 * 8)]
    stp     x28, x29, [sp, #(28 * 8)]
    str     x30,       [sp, #(30 * 8)]

    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0,  x1,  [sp, #(31 * 8)]

    // Read exception information into C calling convention registers
    mrs     x0, esr_el1              // arg0: ESR_EL1 (exception syndrome)
    mrs     x1, far_el1              // arg1: FAR_EL1 (fault address)
    mrs     x2, elr_el1              // arg2: ELR_EL1 (return address)

    // Extract EC = ESR_EL1[31:26]
    lsr     x3, x0, #26
    and     x3, x3, #0x3F

    // EC=0x25: Data Abort from same EL
    cmp     x3, #0x25
    b.eq    .Lcall_page_fault

    // EC=0x21: Instruction Abort from same EL
    cmp     x3, #0x21
    b.eq    .Lcall_page_fault

    // All other exceptions: print diagnostics then halt
    bl      unhandled_exception_handler
    // Restore stack and halt (do NOT eret — exception is fatal)
    add     sp, sp, #(34 * 8)
    b       halt

.Lcall_page_fault:
    // x0=esr, x1=far, x2=elr already set
    bl      kernel_page_fault_handler

    // If the handler returns, the fault was resolved — restore and eret

    // Restore ELR_EL1 and SPSR_EL1
    ldp     x0,  x1,  [sp, #(31 * 8)]
    msr     elr_el1, x0
    msr     spsr_el1, x1

    // Restore general-purpose registers
    ldp     x0,  x1,  [sp, #(0  * 8)]
    ldp     x2,  x3,  [sp, #(2  * 8)]
    ldp     x4,  x5,  [sp, #(4  * 8)]
    ldp     x6,  x7,  [sp, #(6  * 8)]
    ldp     x8,  x9,  [sp, #(8  * 8)]
    ldp     x10, x11, [sp, #(10 * 8)]
    ldp     x12, x13, [sp, #(12 * 8)]
    ldp     x14, x15, [sp, #(14 * 8)]
    ldp     x16, x17, [sp, #(16 * 8)]
    ldp     x18, x19, [sp, #(18 * 8)]
    ldp     x20, x21, [sp, #(20 * 8)]
    ldp     x22, x23, [sp, #(22 * 8)]
    ldp     x24, x25, [sp, #(24 * 8)]
    ldp     x26, x27, [sp, #(26 * 8)]
    ldp     x28, x29, [sp, #(28 * 8)]
    ldr     x30,       [sp, #(30 * 8)]

    add     sp, sp, #(34 * 8)
    eret

// ============================================================================
// Lower EL Synchronous exception dispatch
//
// All synchronous exceptions from EL0 (userspace) arrive here.
// We save context, read ESR_EL1, extract EC, and route:
//   EC=0x15 (SVC AArch64)                → system_call_handler (C)
//   EC=0x24 (Data Abort, lower EL)       → user_page_fault_handler (C)
//   EC=0x20 (Instruction Abort, lower EL)→ user_page_fault_handler (C)
//   Other EC                              → unhandled_user_exception_handler (C)
// ============================================================================
lower_el_sync_dispatch:
    // Save all general-purpose registers using the standard 34-slot frame
    sub     sp, sp, #(34 * 8)

    stp     x0,  x1,  [sp, #(0  * 8)]
    stp     x2,  x3,  [sp, #(2  * 8)]
    stp     x4,  x5,  [sp, #(4  * 8)]
    stp     x6,  x7,  [sp, #(6  * 8)]
    stp     x8,  x9,  [sp, #(8  * 8)]
    stp     x10, x11, [sp, #(10 * 8)]
    stp     x12, x13, [sp, #(12 * 8)]
    stp     x14, x15, [sp, #(14 * 8)]
    stp     x16, x17, [sp, #(16 * 8)]
    stp     x18, x19, [sp, #(18 * 8)]
    stp     x20, x21, [sp, #(20 * 8)]
    stp     x22, x23, [sp, #(22 * 8)]
    stp     x24, x25, [sp, #(24 * 8)]
    stp     x26, x27, [sp, #(26 * 8)]
    stp     x28, x29, [sp, #(28 * 8)]
    str     x30,       [sp, #(30 * 8)]

    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0,  x1,  [sp, #(31 * 8)]

    // Read ESR_EL1 and extract EC = bits[31:26]
    mrs     x9, esr_el1
    lsr     x10, x9, #26
    and     x10, x10, #0x3F

    // EC=0x15: SVC from AArch64 (system call)
    cmp     x10, #0x15
    b.eq    .Llower_syscall

    // EC=0x24: Data Abort from lower EL (user page fault)
    cmp     x10, #0x24
    b.eq    .Llower_page_fault

    // EC=0x20: Instruction Abort from lower EL (user page fault)
    cmp     x10, #0x20
    b.eq    .Llower_page_fault

    // All other EC: unhandled user exception
    mov     x0, x9              // arg0: ESR_EL1
    mrs     x1, far_el1         // arg1: FAR_EL1
    mrs     x2, elr_el1         // arg2: ELR_EL1
    bl      unhandled_user_exception_handler
    b       .Llower_restore_eret

.Llower_syscall:
    // Syscall: x8 = syscall number, x0-x5 = args (saved on stack)
    // Prepare arguments for system_call_handler(nr, a0..a5)
    ldr     x0, [sp, #(8  * 8)]    // x8 = syscall number → arg0
    ldr     x1, [sp, #(0  * 8)]    // x0 = user arg0 → arg1
    ldr     x2, [sp, #(1  * 8)]    // x1 = user arg1 → arg2
    ldr     x3, [sp, #(2  * 8)]    // x2 = user arg2 → arg3
    ldr     x4, [sp, #(3  * 8)]    // x3 = user arg3 → arg4
    ldr     x5, [sp, #(4  * 8)]    // x4 = user arg4 → arg5
    ldr     x6, [sp, #(5  * 8)]    // x5 = user arg5 → arg6
    bl      system_call_handler

    // Store return value into saved x0 slot so user sees it after eret
    str     x0, [sp, #(0  * 8)]
    b       .Llower_restore_eret

.Llower_page_fault:
    // User page fault: pass ESR, FAR, ELR to C handler
    mov     x0, x9              // arg0: ESR_EL1 (already in x9)
    mrs     x1, far_el1         // arg1: FAR_EL1
    mrs     x2, elr_el1         // arg2: ELR_EL1
    bl      user_page_fault_handler
    // Fall through to restore — handler may have resolved the fault,
    // or terminated the process (scheduler will pick next task).
    b       .Llower_restore_eret

.Llower_restore_eret:
    // Restore ELR_EL1 and SPSR_EL1
    ldp     x0,  x1,  [sp, #(31 * 8)]
    msr     elr_el1, x0
    msr     spsr_el1, x1

    // Restore general-purpose registers
    ldp     x0,  x1,  [sp, #(0  * 8)]
    ldp     x2,  x3,  [sp, #(2  * 8)]
    ldp     x4,  x5,  [sp, #(4  * 8)]
    ldp     x6,  x7,  [sp, #(6  * 8)]
    ldp     x8,  x9,  [sp, #(8  * 8)]
    ldp     x10, x11, [sp, #(10 * 8)]
    ldp     x12, x13, [sp, #(12 * 8)]
    ldp     x14, x15, [sp, #(14 * 8)]
    ldp     x16, x17, [sp, #(16 * 8)]
    ldp     x18, x19, [sp, #(18 * 8)]
    ldp     x20, x21, [sp, #(20 * 8)]
    ldp     x22, x23, [sp, #(22 * 8)]
    ldp     x24, x25, [sp, #(24 * 8)]
    ldp     x26, x27, [sp, #(26 * 8)]
    ldp     x28, x29, [sp, #(28 * 8)]
    ldr     x30,       [sp, #(30 * 8)]

    add     sp, sp, #(34 * 8)
    eret

// 导出符号给链接器
.global _bss_start_addr, _bss_end_addr, _stack_top_addr
.global exception_vectors

// 引用外部符号
.extern system_call_handler
.extern irq_handler_c
.extern kernel_page_fault_handler
.extern unhandled_exception_handler
.extern user_page_fault_handler
.extern unhandled_user_exception_handler