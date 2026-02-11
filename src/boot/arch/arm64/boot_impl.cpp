/*
 * ARM64架构特定启动实现 - 修复版本
 * 实现统一启动接口的ARM64版本
 */

#include "../../include/arch/boot_interface.hpp"
#include "../../include/arch/arch_selector.hpp"
#include "mm/page_table.hpp"
#include "../../../mm/page_frame_allocator.hpp"
#include "../../../mm/runtime_heap_allocator.hpp"
#include "moss_std.hpp"
#include "result.hpp"

// 声明汇编入口点和外部符号
extern "C" {
    void _start();

    // 链接器脚本定义的符号
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

    void mark_runtime_heap_ready() noexcept;
}

namespace moss::boot {

// 全局启动状态
BootStatus g_boot_status = {
    .current_stage = BootStage::PreInit,
    .completed_stages_mask = 0,
    .stage_timestamps = {0},
    .last_error = ::moss::kernel::ErrorCode::Success
};

// 早期串口输出
class EarlyUart {
private:
  static constexpr VirtAddr UART_BASE = 0x09000000;
  static constexpr u32 UART_DR = 0x000;
  static constexpr u32 UART_FR = 0x018;
  static constexpr u32 UART_FR_TXFF = (1 << 5);

  volatile u32 *const uart_base;

public:
  EarlyUart() : uart_base(reinterpret_cast<volatile u32 *>(UART_BASE)) {}

  void put_char(char c) const {
    while (uart_base[UART_FR / 4] & UART_FR_TXFF) {}
    uart_base[UART_DR / 4] = static_cast<u32>(c);
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
    asm volatile("mrs %0, cntvct_el0" : "=r"(counter));
    return counter;
}

static u32 get_current_cpu_id_impl() noexcept {
    u64 mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return static_cast<u32>(mpidr & 0xFF);
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

// ARM64BootImpl成员函数实现
::moss::kernel::VoidResult moss::boot::ARM64BootImpl::hardware_early_init(BootContext& ctx) noexcept {
    moss::boot::update_boot_stage(moss::boot::BootStage::HardwareInit);

    moss::boot::early_print("=== ARM64硬件早期初始化 ===\n");

    ctx.cpu_id = moss::boot::get_current_cpu_id_impl();
    moss::boot::early_print("CPU ID: ");
    moss::boot::early_print_hex(ctx.cpu_id);
    moss::boot::early_print("\n");

    // 设置内存信息
    ctx.memory_start = 0x40000000;
    ctx.memory_size = 1024 * 1024 * 1024; // 1GB
    ctx.kernel_phys_base = 0x40000000;
    ctx.kernel_virt_base = moss::boot::arch_constants::KERNEL_VIRT_BASE;

    moss::boot::early_print("ARM64硬件初始化完成\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::ARM64BootImpl::setup_memory_management(BootContext& ctx) noexcept {
    (void)ctx;
    moss::boot::update_boot_stage(moss::boot::BootStage::MemoryManagement);

    moss::boot::early_print("=== ARM64内存管理设置 ===\n");

    auto mmu_result = ::moss::kernel::mm::setup_mmu();
    if (!mmu_result) {
        moss::boot::early_print("MMU设置失败\n");
        return ::moss::kernel::VoidResult{mmu_result.error()};
    }

    auto pfa_result = ::moss::kernel::mm::PageFrameAllocator::initialize();
    if (!pfa_result) {
        moss::boot::early_print("物理页面分配器初始化失败\n");
        return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::OutOfMemory};
    }

    VirtAddr heap_start = reinterpret_cast<VirtAddr>(_heap_start_addr);
    ::moss::kernel::usize initial_heap_size = 256 * 1024;
    auto heap_result = ::moss::kernel::mm::RuntimeHeapAllocator::initialize_heap(heap_start, initial_heap_size);
    if (!heap_result) {
        moss::boot::early_print("运行时堆分配器初始化失败\n");
        return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::OutOfMemory};
    }

    moss::boot::early_print("ARM64内存管理设置完成\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::ARM64BootImpl::setup_interrupts_and_exceptions(BootContext& ctx) noexcept {
    (void)ctx;
    moss::boot::update_boot_stage(moss::boot::BootStage::InterruptsExceptions);

    moss::boot::early_print("=== ARM64中断和异常设置 ===\n");
    moss::boot::early_print("TODO: GIC中断控制器初始化\n");
    moss::boot::early_print("ARM64中断异常设置完成\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::ARM64BootImpl::setup_smp_support(BootContext& ctx) noexcept {
    moss::boot::update_boot_stage(moss::boot::BootStage::SmpSupport);

    moss::boot::early_print("=== ARM64 SMP支持设置 ===\n");
    ctx.total_cpus = 1; // 暂时只支持单核
    moss::boot::early_print("ARM64 SMP设置完成\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::ARM64BootImpl::finalize_arch_init(BootContext& ctx) noexcept {
    (void)ctx;
    moss::boot::update_boot_stage(moss::boot::BootStage::ArchFinalize);

    moss::boot::early_print("=== ARM64架构初始化完成 ===\n");
    mark_runtime_heap_ready();
    moss::boot::early_print("架构特定初始化全部完成\n\n");

    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::ARM64BootImpl::detect_memory_layout(BootContext& ctx) noexcept {
    (void)ctx;
    return ::moss::kernel::VoidResult{};
}

u32 moss::boot::ARM64BootImpl::get_current_cpu_id() noexcept {
    return moss::boot::get_current_cpu_id_impl();
}

[[noreturn]] void moss::boot::ARM64BootImpl::arch_panic(const char* message) noexcept {
    moss::boot::early_print("\n=== ARM64 PANIC ===\n");
    moss::boot::early_print(message);
    moss::boot::early_print("\n==================\n");

    // ARM64系统关闭序列
    asm volatile("movz x0, #0x0008, lsl #0\n"
                 "movk x0, #0x8400, lsl #16\n"
                 "smc #0\n"
                 :
                 :
                 : "x0");

    while (true) {
        asm volatile("wfi");
    }
}
