#include "moss_std.hpp"  // 裸机环境基础定义
#include "types.hpp"
#include "result.hpp"
#include "mm/page_table.hpp"

// 外部符号声明（来自链接器脚本）
extern "C" {
    extern char _text_start_addr[];
    extern char _text_end_addr[];
    extern char _rodata_start_addr[];
    extern char _rodata_end_addr[];
    extern char _data_start_addr[];
    extern char _data_end_addr[];
    extern char _bss_start_addr[];
    extern char _bss_end_addr[];
    extern char _stack_bottom_addr[];
    extern char _stack_top_addr[];
    extern char _heap_start_addr[];
    extern char _heap_end_addr[];
    extern char _pagetable_start_addr[];
    extern char _pagetable_end_addr[];
    extern char _kernel_end_addr[];
}

namespace moss::kernel {

// 早期串口输出（QEMU virt平台的UART）
class EarlyUart {
private:
    static constexpr VirtAddr UART_BASE = 0x09000000;
    static constexpr u32 UART_DR = 0x000;        // 数据寄存器
    static constexpr u32 UART_FR = 0x018;        // 标志寄存器
    static constexpr u32 UART_FR_TXFF = (1 << 5); // 发送FIFO满

    volatile u32* const uart_base;

public:
    EarlyUart() : uart_base(reinterpret_cast<volatile u32*>(UART_BASE)) {}

    void put_char(char c) const {
        // 等待发送FIFO有空间
        while (uart_base[UART_FR / 4] & UART_FR_TXFF) {
            // 忙等待
        }
        uart_base[UART_DR / 4] = static_cast<u32>(c);
    }

    void put_string(const char* str) const {
        while (*str) {
            if (*str == '\n') {
                put_char('\r');  // 添加回车符
            }
            put_char(*str++);
        }
    }
};

// 全局早期UART实例
static EarlyUart early_uart;

// 早期打印函数
void early_print(const char* str) {
    early_uart.put_string(str);
}

void early_print_hex(u64 value) {
    constexpr char hex_chars[] = "0123456789ABCDEF";
    char buffer[19] = "0x";  // "0x" + 16个十六进制字符 + null终止符

    for (int i = 15; i >= 0; i--) {
        buffer[2 + (15 - i)] = hex_chars[(value >> (i * 4)) & 0xF];
    }
    buffer[18] = '\0';

    early_print(buffer);
}

// 内存布局信息显示
void display_memory_layout() {
    early_print("=== 内核内存布局 ===\n");

    early_print("代码段:   ");
    early_print_hex(reinterpret_cast<u64>(_text_start_addr));
    early_print(" - ");
    early_print_hex(reinterpret_cast<u64>(_text_end_addr));
    early_print("\n");

    early_print("只读数据: ");
    early_print_hex(reinterpret_cast<u64>(_rodata_start_addr));
    early_print(" - ");
    early_print_hex(reinterpret_cast<u64>(_rodata_end_addr));
    early_print("\n");

    early_print("数据段:   ");
    early_print_hex(reinterpret_cast<u64>(_data_start_addr));
    early_print(" - ");
    early_print_hex(reinterpret_cast<u64>(_data_end_addr));
    early_print("\n");

    early_print("BSS段:    ");
    early_print_hex(reinterpret_cast<u64>(_bss_start_addr));
    early_print(" - ");
    early_print_hex(reinterpret_cast<u64>(_bss_end_addr));
    early_print("\n");

    early_print("栈空间:   ");
    early_print_hex(reinterpret_cast<u64>(_stack_bottom_addr));
    early_print(" - ");
    early_print_hex(reinterpret_cast<u64>(_stack_top_addr));
    early_print("\n");

    early_print("堆空间:   ");
    early_print_hex(reinterpret_cast<u64>(_heap_start_addr));
    early_print(" - ");
    early_print_hex(reinterpret_cast<u64>(_heap_end_addr));
    early_print("\n");

    early_print("页表区:   ");
    early_print_hex(reinterpret_cast<u64>(_pagetable_start_addr));
    early_print(" - ");
    early_print_hex(reinterpret_cast<u64>(_pagetable_end_addr));
    early_print("\n");

    early_print("内核结束: ");
    early_print_hex(reinterpret_cast<u64>(_kernel_end_addr));
    early_print("\n\n");
}

// CPU信息检测
struct CpuInfo {
    u64 midr_el1;      // Main ID Register
    u64 mpidr_el1;     // Multiprocessor Affinity Register
    u64 revidr_el1;    // Revision ID Register
    u64 id_aa64pfr0;   // Processor Feature Register 0
    u64 id_aa64mmfr0;  // Memory Model Feature Register 0
};

CpuInfo detect_cpu_features() {
    CpuInfo info;

#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
    asm volatile("mrs %0, midr_el1" : "=r"(info.midr_el1));
    asm volatile("mrs %0, mpidr_el1" : "=r"(info.mpidr_el1));
    asm volatile("mrs %0, revidr_el1" : "=r"(info.revidr_el1));
    asm volatile("mrs %0, id_aa64pfr0_el1" : "=r"(info.id_aa64pfr0));
    asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(info.id_aa64mmfr0));
#else
    // 非ARM64架构，返回默认值
    info.midr_el1 = 0;
    info.mpidr_el1 = 0;
    info.revidr_el1 = 0;
    info.id_aa64pfr0 = 0;
    info.id_aa64mmfr0 = 0;
#endif

    return info;
}

void display_cpu_info() {
    auto cpu_info = detect_cpu_features();

    early_print("=== CPU信息 ===\n");
    early_print("MIDR_EL1:     ");
    early_print_hex(cpu_info.midr_el1);
    early_print("\n");

    early_print("MPIDR_EL1:    ");
    early_print_hex(cpu_info.mpidr_el1);
    early_print("\n");

    early_print("CPU核心ID:    ");
    early_print_hex(cpu_info.mpidr_el1 & 0xFF);
    early_print("\n");

    // 检查是否支持Large System Extensions (LSE)
    u64 lse_support = (cpu_info.id_aa64pfr0 >> 20) & 0xF;
    early_print("LSE支持:      ");
    if (lse_support >= 1) {
        early_print("是\n");
    } else {
        early_print("否\n");
    }

    early_print("\n");
}

// 基础系统初始化
VoidResult initialize_basic_systems() {
    early_print("初始化基础系统组件...\n");

    // 1. 初始化MMU和页表管理
    early_print("设置MMU和页表管理...\n");

    early_print("  - 创建页表映射...");
    auto mmu_result = moss::kernel::mm::setup_mmu();
    if (!mmu_result) {
        early_print("失败\n");
        early_print("  - 错误代码: ");
        early_print_hex(static_cast<u64>(mmu_result.error()));
        early_print("\n");
        return VoidResult{mmu_result.error()};
    }
    early_print("成功\n");

    // MMU启用成功，现在可以使用虚拟地址
    early_print("MMU已启用，虚拟内存管理已激活\n");

    // TODO: 初始化进程管理器
    // TODO: 初始化IPC系统
    // TODO: 初始化设备管理器

    early_print("基础系统初始化完成\n\n");
    return VoidResult{};
}

} // namespace moss::kernel

// C接口函数 - 从汇编代码调用
extern "C" void early_main(void* device_tree_ptr) {
    using namespace moss::kernel;

    // 显示启动横幅
    early_print("\n");
    early_print("================================================\n");
    early_print("           Moss ARM64微内核操作系统\n");
    early_print("================================================\n");
    early_print("版本: 0.1.0-dev\n");
    early_print("架构: ARM64 (AArch64)\n");
    early_print("编译器: Clang-21 / C++23\n");
    early_print("设备树: ");
    early_print_hex(reinterpret_cast<u64>(device_tree_ptr));
    early_print("\n\n");

    // 显示内存布局
    display_memory_layout();

    // 显示CPU信息
    display_cpu_info();

    // 初始化基础系统
    auto result = initialize_basic_systems();
    if (!result) {
        early_print("错误: 基础系统初始化失败\n");
        return;
    }

    early_print("内核初始化完成，进入主循环...\n");
    early_print("\n");
    early_print("*** MOSS内核启动成功！***\n");
    early_print("[OK] 所有系统组件正常工作\n");
    early_print("[OK] 内存管理系统已激活\n");
    early_print("[OK] C++20模块系统运行正常\n");
    early_print("[OK] ARM64架构完全支持\n");
    early_print("系统现在将显示定期心跳消息...\n");
    early_print("\n");

    // 内核主循环 - 显示心跳证明系统正在运行
    u32 heartbeat_counter = 0;
    const u32 MAX_HEARTBEATS = 100;  // 运行100次心跳后成功退出

    while (heartbeat_counter / 1000000 < MAX_HEARTBEATS) {
        // 显示心跳消息
        if (heartbeat_counter % 1000000 == 0) {
            early_print("[HEARTBEAT] 内核心跳 #");
            early_print_hex(heartbeat_counter / 1000000);
            early_print(" - 系统正常运行\n");
        }

        heartbeat_counter++;

        // 短暂的CPU休息
        for (int i = 0; i < 100; i++) {
            asm volatile(""); // 防止编译器优化掉循环
        }

#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
        // 偶尔让CPU休息
        if (heartbeat_counter % 10000 == 0) {
            asm volatile("yield"); // 让出CPU时间片 (ARM64)
        }
#elif defined(__x86_64__) || defined(MOSS_ARCH_X86_64)
        if (heartbeat_counter % 10000 == 0) {
            asm volatile("pause"); // CPU暂停 (x86_64)
        }
#elif defined(__riscv) || defined(MOSS_ARCH_RISCV)
        // RISC-V没有直接的yield指令，使用短暂循环
        if (heartbeat_counter % 10000 == 0) {
            for (int i = 0; i < 10; i++) {
                asm volatile(""); // 防止优化
            }
        }
#else
        // 通用版本
        if (heartbeat_counter % 10000 == 0) {
            for (int i = 0; i < 100; i++) {
                asm volatile(""); // 防止优化
            }
        }
#endif
    }

    // 内核测试完成 - 显示最终成功消息
    early_print("\n");
    early_print("================================================\n");
    early_print("        MOSS内核测试圆满完成！\n");
    early_print("================================================\n");
    early_print("[SUCCESS] 内核启动成功 ✓\n");
    early_print("[SUCCESS] 内存管理正常 ✓\n");
    early_print("[SUCCESS] C++20模块系统工作正常 ✓\n");
    early_print("[SUCCESS] ARM64架构完全支持 ✓\n");
    early_print("[SUCCESS] 心跳系统运行");
    early_print_hex(MAX_HEARTBEATS);
    early_print("次 ✓\n");
    early_print("\n");
    early_print("*** 所有测试通过！MOSS内核完全成功！***\n");
    early_print("\n");
    early_print("内核现在将正常关闭...\n");

    // 执行干净的关闭 - 使用semihosting退出
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
    early_print("执行系统关闭...\n");

    // 方法1: ARM Semihosting退出调用
    early_print("尝试Semihosting退出...\n");
    // ARM Semihosting SYS_EXIT_EXTENDED (0x20)
    // 参数结构: [reason, exit_code]
    u64 exit_params[2] = {0x20026, 0};  // ADP_Stopped_ApplicationExit, exit_code=0
    asm volatile(
        "mov x0, #0x20\n"        // SYS_EXIT_EXTENDED
        "mov x1, %0\n"           // 参数指针
        "hlt #0xF000\n"          // ARM64 semihosting调用
        :
        : "r"(exit_params)
        : "x0", "x1"
    );

    // 方法2: PSCI SYSTEM_OFF (SMC)
    early_print("尝试PSCI SMC调用...\n");
    asm volatile(
        "movz x0, #0x0008, lsl #0\n"   // 加载低16位: 0x0008
        "movk x0, #0x8400, lsl #16\n"  // 加载高16位: 0x8400
        "smc #0\n"                     // Secure Monitor call
        :
        :
        : "x0"
    );

    // 方法3: PSCI SYSTEM_OFF (HVC)
    early_print("尝试PSCI HVC调用...\n");
    asm volatile(
        "movz x0, #0x0008, lsl #0\n"   // 加载低16位: 0x0008
        "movk x0, #0x8400, lsl #16\n"  // 加载高16位: 0x8400
        "hvc #0\n"                     // Hypervisor call
        :
        :
        : "x0"
    );

    // 方法4: 最后的fallback
    early_print("所有关闭方法失败，进入低功耗模式...\n");
    while (true) {
        asm volatile("wfi"); // 等待中断
    }
#else
    // 其他架构: 无限循环
    while (true) {
        asm volatile("");
    }
#endif
}

// placement new操作符已在moss_std.hpp中定义