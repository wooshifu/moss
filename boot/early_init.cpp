#include "../kernel/include/types.hpp"
#include "../kernel/include/result.hpp"
#include "../kernel/mm/page_table.hpp"

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

    asm volatile("mrs %0, midr_el1" : "=r"(info.midr_el1));
    asm volatile("mrs %0, mpidr_el1" : "=r"(info.mpidr_el1));
    asm volatile("mrs %0, revidr_el1" : "=r"(info.revidr_el1));
    asm volatile("mrs %0, id_aa64pfr0_el1" : "=r"(info.id_aa64pfr0));
    asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(info.id_aa64mmfr0));

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
    early_print("设置MMU和页表管理...");
    auto mmu_result = moss::kernel::mm::setup_mmu();
    if (!mmu_result) {
        early_print("失败\n");
        return VoidResult{mmu_result.error()};
    }
    early_print("完成\n");

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

    // 简单的内核主循环
    while (true) {
        asm volatile("wfi"); // 等待中断
    }
}

// 实现placement new操作符
void* operator new(moss::kernel::usize, void* ptr) noexcept {
    return ptr;
}

void* operator new[](moss::kernel::usize, void* ptr) noexcept {
    return ptr;
}

void operator delete(void*, void*) noexcept {
    // placement delete不需要做任何事情
}

void operator delete[](void*, void*) noexcept {
    // placement delete不需要做任何事情
}