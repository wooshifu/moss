/*
 * 统一启动入口文件
 * 为所有架构提供统一的启动流程控制
 */

#include "boot/arch/boot_interface.hpp"
#include "boot/arch/arch_selector.hpp"
#include "moss_std.hpp"

// 包含架构特定的实现定义
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
// 包含ARM64的完整实现
extern "C" {
    [[noreturn]] void early_main(void* device_tree_ptr); // 与ARM64汇编代码的C链接
}
// 注意：ARM64BootImpl的实现在arch/arm64/boot_impl.cpp中定义
// 会通过链接器与此文件链接在一起
#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X86_64)
// x86_64实现（待开发）
#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV)
// RISC-V实现（待开发）
#endif

namespace moss::boot {

// 早期启动打印函数（架构无关）
static void boot_print(const char* message) {
    // 根据架构选择合适的早期打印方式
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
    // ARM64使用UART
    static constexpr VirtAddr UART_BASE = 0x09000000;
    volatile u32 *uart_base = reinterpret_cast<volatile u32 *>(UART_BASE);
    const char* p = message;
    while (*p) {
        if (*p == '\n') {
            // 等待FIFO不满
            while (uart_base[0x018 / 4] & (1 << 5)) {}
            uart_base[0x000 / 4] = '\r';
        }
        // 等待FIFO不满
        while (uart_base[0x018 / 4] & (1 << 5)) {}
        uart_base[0x000 / 4] = *p++;
    }
#else
    // 其他架构的早期打印（待实现）
    (void)message;
#endif
}

/**
 * 统一启动主函数
 * 所有架构的启动流程都经过这个统一入口
 */
extern "C" [[noreturn]] void unified_boot_main(void* device_tree_ptr) {
    // 显示启动信息
    boot_print("\n=== Moss 多架构统一启动系统 ===\n");
    boot_print("目标架构: ");
    boot_print(MOSS_CURRENT_ARCH);
    boot_print("\n");

    // 初始化启动上下文
    BootContext ctx{
        .device_tree_ptr = device_tree_ptr,
        .memory_start = 0,    // 将在hardware_early_init中设置
        .memory_size = 0,     // 将在hardware_early_init中设置
        .cpu_id = 0,          // 将在hardware_early_init中设置
        .total_cpus = 1,      // 默认单核
        .kernel_phys_base = 0,
        .kernel_virt_base = 0
    };

    boot_print("启动上下文初始化完成\n\n");

    // 执行架构特定的标准化启动流程
    boot_print("开始标准化启动序列...\n");

    // 阶段1：硬件层初始化
    boot_print("阶段1: 硬件早期初始化\n");
    auto hw_result = ArchBoot::hardware_early_init(ctx);
    if (!hw_result) {
        boot_print("错误: 硬件初始化失败\n");
        ArchBoot::arch_panic("Hardware initialization failed");
    }

    // 阶段2：内存管理设置
    boot_print("阶段2: 内存管理设置\n");
    auto mem_result = ArchBoot::setup_memory_management(ctx);
    if (!mem_result) {
        boot_print("错误: 内存管理设置失败\n");
        ArchBoot::arch_panic("Memory management setup failed");
    }

    // 阶段3：中断异常设置
    boot_print("阶段3: 中断和异常设置\n");
    auto int_result = ArchBoot::setup_interrupts_and_exceptions(ctx);
    if (!int_result) {
        boot_print("错误: 中断异常设置失败\n");
        ArchBoot::arch_panic("Interrupt/exception setup failed");
    }

    // 阶段4：多核支持（如果需要）
    boot_print("阶段4: SMP支持设置\n");
    auto smp_result = ArchBoot::setup_smp_support(ctx);
    if (!smp_result) {
        boot_print("错误: SMP设置失败\n");
        ArchBoot::arch_panic("SMP setup failed");
    }

    // 阶段5：架构特定的最终化
    boot_print("阶段5: 架构初始化完成\n");
    auto finalize_result = ArchBoot::finalize_arch_init(ctx);
    if (!finalize_result) {
        boot_print("错误: 架构初始化完成失败\n");
        ArchBoot::arch_panic("Architecture finalization failed");
    }

    // 更新启动状态
    update_boot_stage(BootStage::SystemInit);

    boot_print("=== 架构特定启动完成 ===\n");
    boot_print("转交给架构无关的系统初始化...\n\n");

    // 转交给架构无关的系统初始化
    extern void kernel_main(void) noexcept;
    boot_print("🚀 启动MOSS内核主程序...\n");

    // 标记启动完成
    update_boot_stage(BootStage::Complete);

    // 调用内核主程序
    kernel_main();

    // 如果kernel_main返回，说明出错了
    ArchBoot::arch_panic("Kernel main returned unexpectedly");
}

} // namespace moss::boot

// C语言入口点，由各架构的汇编代码调用
extern "C" [[noreturn]] void early_main(void* device_tree_ptr) {
    // 直接调用统一启动主函数
    moss::boot::unified_boot_main(device_tree_ptr);
}
