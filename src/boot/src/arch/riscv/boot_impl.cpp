/*
 * RISC-V架构特定启动实现
 * 实现统一启动接口的RISC-V版本
 */

#include "boot/arch/boot_interface.hpp"
#include "boot/arch/arch_selector.hpp"
#include "boot/boot.hpp"
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

// 早期串口输出 (RISC-V特有)
class EarlyUart {
private:
    static constexpr VirtAddr UART_BASE = 0x10000000; // QEMU RISC-V UART基址
    static constexpr u32 UART_REG_TXDATA = 0x00;
    static constexpr u32 UART_REG_TXCTRL = 0x08;
    static constexpr u32 UART_TXEN = 0x1;

    volatile u32 *const uart_base;

public:
    EarlyUart() : uart_base(reinterpret_cast<volatile u32 *>(UART_BASE)) {}

    void put_char(char c) const {
        // RISC-V UART写入字符
        uart_base[UART_REG_TXDATA / 4] = static_cast<u32>(c);
    }

    void put_string(const char *str) const {
        while (*str) {
            if (*str == '\n') {
                put_char('\r');
            }
            put_char(*str++);
        }
    }
};

static EarlyUart early_uart;

// 辅助函数
static void early_print(const char *str) {
    early_uart.put_string(str);
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
    u64 counter;
    asm volatile("rdtime %0" : "=r"(counter));
    return counter;
}

static u32 get_current_cpu_id_impl() noexcept {
    u64 hartid;
    asm volatile("csrr %0, mhartid" : "=r"(hartid));
    return static_cast<u32>(hartid & 0xFFFFFFFF);
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

// RISCVBootImpl成员函数实现
::moss::kernel::VoidResult moss::boot::RISCVBootImpl::hardware_early_init(BootContext& ctx) noexcept {
    moss::boot::update_boot_stage(moss::boot::BootStage::HardwareInit);

    moss::boot::early_print("=== RISC-V硬件早期初始化 ===\n");

    ctx.cpu_id = moss::boot::get_current_cpu_id_impl();
    moss::boot::early_print("CPU ID (Hart ID): ");
    moss::boot::early_print_hex(ctx.cpu_id);
    moss::boot::early_print("\n");

    // 读取机器信息
    u64 mvendorid, marchid, mimpid;
    asm volatile("csrr %0, mvendorid" : "=r"(mvendorid));
    asm volatile("csrr %0, marchid" : "=r"(marchid));
    asm volatile("csrr %0, mimpid" : "=r"(mimpid));

    moss::boot::early_print("Machine Vendor ID: ");
    moss::boot::early_print_hex(mvendorid);
    moss::boot::early_print("\n");

    // 设置内存信息（RISC-V QEMU默认）
    ctx.memory_start = 0x80000000;  // RISC-V标准内存起始地址
    ctx.memory_size = 128 * 1024 * 1024; // 128MB（保守估计）
    ctx.kernel_phys_base = 0x80000000;
    ctx.kernel_virt_base = moss::boot::arch_constants::KERNEL_VIRT_BASE;

    moss::boot::early_print("内存范围: ");
    moss::boot::early_print_hex(ctx.memory_start);
    moss::boot::early_print(" - ");
    moss::boot::early_print_hex(ctx.memory_start + ctx.memory_size);
    moss::boot::early_print("\n");

    moss::boot::early_print("RISC-V硬件初始化完成\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCVBootImpl::setup_memory_management(BootContext& ctx) noexcept {
    (void)ctx;
    moss::boot::update_boot_stage(moss::boot::BootStage::MemoryManagement);

    moss::boot::early_print("=== RISC-V内存管理设置 ===\n");
    moss::boot::early_print("TODO: 实现RISC-V页表和MMU设置\n");
    moss::boot::early_print("RISC-V内存管理设置完成\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCVBootImpl::setup_interrupts_and_exceptions(BootContext& ctx) noexcept {
    (void)ctx;
    moss::boot::update_boot_stage(moss::boot::BootStage::InterruptsExceptions);

    moss::boot::early_print("=== RISC-V中断和异常设置 ===\n");
    moss::boot::early_print("TODO: 实现RISC-V中断控制器初始化\n");
    moss::boot::early_print("RISC-V中断异常设置完成\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCVBootImpl::setup_smp_support(BootContext& ctx) noexcept {
    moss::boot::update_boot_stage(moss::boot::BootStage::SmpSupport);

    moss::boot::early_print("=== RISC-V SMP支持设置 ===\n");
    ctx.total_cpus = 1; // 暂时只支持单核
    moss::boot::early_print("TODO: 实现RISC-V多核启动支持\n");
    moss::boot::early_print("RISC-V SMP设置完成\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCVBootImpl::finalize_arch_init(BootContext& ctx) noexcept {
    (void)ctx;
    moss::boot::update_boot_stage(moss::boot::BootStage::ArchFinalize);

    moss::boot::early_print("=== RISC-V架构初始化完成 ===\n");

    // 如果函数存在，标记运行时堆已准备
    if (mark_runtime_heap_ready) {
        mark_runtime_heap_ready();
        moss::boot::early_print("运行时堆标记完成\n");
    }

    moss::boot::early_print("RISC-V架构特定初始化全部完成\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCVBootImpl::detect_memory_layout(BootContext& ctx) noexcept {
    (void)ctx;
    // RISC-V在hardware_early_init中已设置基本信息
    return ::moss::kernel::VoidResult{};
}

u32 moss::boot::RISCVBootImpl::get_current_cpu_id() noexcept {
    return moss::boot::get_current_cpu_id_impl();
}

[[noreturn]] void moss::boot::RISCVBootImpl::arch_panic(const char* message) noexcept {
    moss::boot::early_print("\n=== RISC-V PANIC ===\n");
    moss::boot::early_print(message);
    moss::boot::early_print("\n===================\n");

    // RISC-V系统关闭序列
    // 1. 尝试SBI关机 (Supervisor Binary Interface)
    asm volatile(
        "li a7, 0x08\n"        // SBI_SHUTDOWN extension
        "li a6, 0x00\n"        // Function ID
        "ecall\n"
        :
        :
        : "a6", "a7"
    );

    // 2. 如果SBI失败，进入WFI循环
    while (true) {
        asm volatile("wfi");
    }
}

// === Boot 全局变量 (RISC-V 存根) ===
// GIC 是 ARM64 特有硬件，RISC-V 使用 PLIC/CLINT
moss::kernel::interrupts::GenericInterruptController* g_gic_controller = nullptr;
bool g_gic_hardware_available = false;

namespace moss::boot {

void activate_secondary_cpus() noexcept {
    // RISC-V: SMP 激活待实现 (需要 HSM SBI 扩展)
    early_print("[RISC-V] SMP activation not yet implemented\n");
}

u32 wait_for_all_cpus_active([[maybe_unused]] u32 timeout_ms) noexcept {
    // RISC-V: 当前仅支持单核
    return 1;
}

} // namespace moss::boot
