/*
 * x86_64架构特定启动实现
 * 实现统一启动接口的x86_64版本
 */

#include "arch/boot_interface.hpp"
#include "arch/arch_selector.hpp"
#include "moss_std.hpp"
#include "result.hpp"

// 声明汇编入口点和外部符号
extern "C" {
    void _start();

    // 链接器脚本定义的符号（待实现）
    extern char _text_start_addr[] __attribute__((weak));
    extern char _text_end_addr[] __attribute__((weak));
    extern char _rodata_start_addr[] __attribute__((weak));
    extern char _rodata_end_addr[] __attribute__((weak));
    extern char _data_start_addr[] __attribute__((weak));
    extern char _data_end_addr[] __attribute__((weak));
    extern char _bss_start_addr[] __attribute__((weak));
    extern char _bss_end_addr[] __attribute__((weak));
    extern char _stack_bottom_addr[] __attribute__((weak));
    extern char _stack_top_addr[] __attribute__((weak));
    extern char _heap_start_addr[] __attribute__((weak));
    extern char _heap_end_addr[] __attribute__((weak));
    extern char _kernel_end_addr[] __attribute__((weak));

    void mark_runtime_heap_ready() noexcept __attribute__((weak));
}

namespace moss::boot {

// 全局启动状态
BootStatus g_boot_status = {
    .current_stage = BootStage::PreInit,
    .completed_stages_mask = 0,
    .stage_timestamps = {0},
    .last_error = ::moss::kernel::ErrorCode::Success
};

// 早期VGA文本输出 (x86_64特有)
class EarlyVGA {
private:
    static u16* const VGA_BUFFER;
    static constexpr u8 VGA_WIDTH = 80;
    static constexpr u8 VGA_HEIGHT = 25;
    static constexpr u8 VGA_COLOR = 0x07; // 白字黑底

    static u8 cursor_row;
    static u8 cursor_col;

public:
    static void put_char(char c) {
        if (c == '\n') {
            cursor_col = 0;
            cursor_row++;
            if (cursor_row >= VGA_HEIGHT) {
                cursor_row = VGA_HEIGHT - 1;
                // 简单滚屏（向上移动一行）
                for (u8 row = 0; row < VGA_HEIGHT - 1; row++) {
                    for (u8 col = 0; col < VGA_WIDTH; col++) {
                        VGA_BUFFER[row * VGA_WIDTH + col] = VGA_BUFFER[(row + 1) * VGA_WIDTH + col];
                    }
                }
                // 清空最后一行
                for (u8 col = 0; col < VGA_WIDTH; col++) {
                    VGA_BUFFER[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = (VGA_COLOR << 8) | ' ';
                }
            }
            return;
        }

        if (cursor_col >= VGA_WIDTH) {
            cursor_col = 0;
            cursor_row++;
        }

        if (cursor_row >= VGA_HEIGHT) {
            cursor_row = VGA_HEIGHT - 1;
        }

        VGA_BUFFER[cursor_row * VGA_WIDTH + cursor_col] = (VGA_COLOR << 8) | static_cast<u8>(c);
        cursor_col++;
    }

    static void put_string(const char *str) {
        while (*str) {
            put_char(*str++);
        }
    }

    static void clear() {
        for (u8 row = 0; row < VGA_HEIGHT; row++) {
            for (u8 col = 0; col < VGA_WIDTH; col++) {
                VGA_BUFFER[row * VGA_WIDTH + col] = (VGA_COLOR << 8) | ' ';
            }
        }
        cursor_row = 0;
        cursor_col = 0;
    }
};

// 静态成员定义
u16* const EarlyVGA::VGA_BUFFER = reinterpret_cast<u16*>(0xB8000);
u8 EarlyVGA::cursor_row = 0;
u8 EarlyVGA::cursor_col = 0;

// 辅助函数
static void early_print(const char *str) {
    EarlyVGA::put_string(str);
}

static void early_print_hex(u64 value) {
    constexpr char hex_chars[] = "0123456789ABCDEF";
    char buffer[19] = "0x";

    for (int i = 15; i >= 0; i--) {
        buffer[2 + (15 - i)] = hex_chars[(value >> (i * 4)) & 0xF];
    }
    buffer[18] = '\0';

    early_print(buffer);
}

static u64 get_timestamp_counter() noexcept {
    u32 low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high) : : "memory");
    return (static_cast<u64>(high) << 32) | low;
}

static u32 get_current_cpu_id_impl() noexcept {
    // x86_64简单实现：总是返回0（单核）
    // TODO: 实现APIC ID读取
    return 0;
}

// 更新启动阶段状态的实现
void update_boot_stage(BootStage stage, ::moss::kernel::ErrorCode error) noexcept {
    g_boot_status.current_stage = stage;
    g_boot_status.last_error = error;

    // 记录时间戳
    u32 stage_index = static_cast<u32>(stage);
    if (stage_index < 8) {
        g_boot_status.stage_timestamps[stage_index] = get_timestamp_counter();

        if (error == ::moss::kernel::ErrorCode::Success) {
            g_boot_status.completed_stages_mask |= (1u << stage_index);
        }
    }
}

} // namespace moss::boot

// x86_64BootImpl成员函数实现
::moss::kernel::VoidResult moss::boot::X86_64BootImpl::hardware_early_init(BootContext& ctx) noexcept {
    moss::boot::update_boot_stage(moss::boot::BootStage::HardwareInit);

    // 清屏并显示启动信息
    moss::boot::EarlyVGA::clear();
    moss::boot::early_print("=== x86_64硬件早期初始化 ===\n");

    ctx.cpu_id = moss::boot::get_current_cpu_id_impl();
    moss::boot::early_print("CPU ID: ");
    moss::boot::early_print_hex(ctx.cpu_id);
    moss::boot::early_print("\n");

    // 检查CPU特性
    u32 eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x00000001) : "memory");
    moss::boot::early_print("CPUID Features: ");
    moss::boot::early_print_hex(edx);
    moss::boot::early_print("\n");

    // 设置内存信息（x86_64 QEMU默认）
    ctx.memory_start = 0x00100000;  // 1MB起始
    ctx.memory_size = 128 * 1024 * 1024; // 128MB（保守估计）
    ctx.kernel_phys_base = 0x00100000;
    ctx.kernel_virt_base = moss::boot::arch_constants::KERNEL_VIRT_BASE;

    moss::boot::early_print("内存范围: ");
    moss::boot::early_print_hex(ctx.memory_start);
    moss::boot::early_print(" - ");
    moss::boot::early_print_hex(ctx.memory_start + ctx.memory_size);
    moss::boot::early_print("\n");

    moss::boot::early_print("x86_64硬件初始化完成\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86_64BootImpl::setup_memory_management(BootContext& ctx) noexcept {
    (void)ctx;
    moss::boot::update_boot_stage(moss::boot::BootStage::MemoryManagement);

    moss::boot::early_print("=== x86_64内存管理设置 ===\n");
    moss::boot::early_print("TODO: 实现x86_64页表和MMU设置\n");
    moss::boot::early_print("x86_64内存管理设置完成\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86_64BootImpl::setup_interrupts_and_exceptions(BootContext& ctx) noexcept {
    (void)ctx;
    moss::boot::update_boot_stage(moss::boot::BootStage::InterruptsExceptions);

    moss::boot::early_print("=== x86_64中断和异常设置 ===\n");
    moss::boot::early_print("TODO: 实现IDT和中断控制器初始化\n");
    moss::boot::early_print("x86_64中断异常设置完成\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86_64BootImpl::setup_smp_support(BootContext& ctx) noexcept {
    moss::boot::update_boot_stage(moss::boot::BootStage::SmpSupport);

    moss::boot::early_print("=== x86_64 SMP支持设置 ===\n");
    ctx.total_cpus = 1; // 暂时只支持单核
    moss::boot::early_print("TODO: 实现x86_64多核启动支持\n");
    moss::boot::early_print("x86_64 SMP设置完成\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86_64BootImpl::finalize_arch_init(BootContext& ctx) noexcept {
    (void)ctx;
    moss::boot::update_boot_stage(moss::boot::BootStage::ArchFinalize);

    moss::boot::early_print("=== x86_64架构初始化完成 ===\n");

    // 如果函数存在，标记运行时堆已准备
    if (mark_runtime_heap_ready) {
        mark_runtime_heap_ready();
        moss::boot::early_print("运行时堆标记完成\n");
    }

    moss::boot::early_print("x86_64架构特定初始化全部完成\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86_64BootImpl::detect_memory_layout(BootContext& ctx) noexcept {
    (void)ctx;
    // x86_64在hardware_early_init中已设置基本信息
    return ::moss::kernel::VoidResult{};
}

u32 moss::boot::X86_64BootImpl::get_current_cpu_id() noexcept {
    return moss::boot::get_current_cpu_id_impl();
}

[[noreturn]] void moss::boot::X86_64BootImpl::arch_panic(const char* message) noexcept {
    moss::boot::early_print("\n=== x86_64 PANIC ===\n");
    moss::boot::early_print(message);
    moss::boot::early_print("\n====================\n");

    // x86_64系统关闭序列
    // 1. 尝试ACPI关机
    asm volatile("outw %0, %1" : : "a"(static_cast<u16>(0x2000)), "d"(static_cast<u16>(0x604)) : "memory"); // QEMU ACPI shutdown

    // 2. 如果ACPI失败，进入HLT循环
    asm volatile("cli"); // 禁用中断
    while (true) {
        asm volatile("hlt");
    }
}

