/*
 * x86_64 architecture-specific boot implementation - module implementation unit
 * Implements the unified boot interface for x86_64
 */

module;

// Architecture detection (global module fragment)
#ifndef MOSS_ARCH_X86_64
#define MOSS_ARCH_X86_64
#endif

module moss.boot;

import moss.abi;

using moss::u8;
using moss::u16;
using moss::u32;
using moss::u64;
using moss::VirtAddr;
using moss::PhysAddr;

namespace moss::boot {

// Global boot status
BootStatus g_boot_status = {
    .current_stage = BootStage::PreInit,
    .completed_stages_mask = 0,
    .stage_timestamps = {0},
    .last_error = ::moss::kernel::ErrorCode::Success};

// Early VGA text output (x86_64 specific)
class EarlyVGA {
private:
    static u16 *const VGA_BUFFER;
    static constexpr u8 VGA_WIDTH = 80;
    static constexpr u8 VGA_HEIGHT = 25;
    static constexpr u8 VGA_COLOR = 0x07;

    static u8 cursor_row;
    static u8 cursor_col;

public:
    static void put_char(char c) {
        if (c == '\n') {
            cursor_col = 0;
            cursor_row++;
            if (cursor_row >= VGA_HEIGHT) {
                cursor_row = VGA_HEIGHT - 1;
                for (u8 row = 0; row < VGA_HEIGHT - 1; row++) {
                    for (u8 col = 0; col < VGA_WIDTH; col++) {
                        VGA_BUFFER[row * VGA_WIDTH + col] = VGA_BUFFER[(row + 1) * VGA_WIDTH + col];
                    }
                }
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

// Static member definitions
u16 *const EarlyVGA::VGA_BUFFER = reinterpret_cast<u16 *>(0xB8000);
u8 EarlyVGA::cursor_row = 0;
u8 EarlyVGA::cursor_col = 0;

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
    return 0;
}

// Boot stage status update
void update_boot_stage(BootStage stage, ::moss::kernel::ErrorCode error) noexcept {
    g_boot_status.current_stage = stage;
    g_boot_status.last_error = error;

    u32 stage_index = static_cast<u32>(stage);
    if (stage_index < 8) {
        g_boot_status.stage_timestamps[stage_index] = get_timestamp_counter();

        if (error == ::moss::kernel::ErrorCode::Success) {
            g_boot_status.completed_stages_mask |= (1u << stage_index);
        }
    }
}

} // namespace moss::boot

// x86_64BootImpl member function implementations
::moss::kernel::VoidResult moss::boot::X86_64BootImpl::hardware_early_init(BootContext &ctx) noexcept {
    moss::boot::update_boot_stage(moss::boot::BootStage::HardwareInit);

    moss::boot::EarlyVGA::clear();
    moss::boot::early_print("=== x86_64 Hardware Early Init ===\n");

    ctx.cpu_id = moss::boot::get_current_cpu_id_impl();
    moss::boot::early_print("CPU ID: ");
    moss::boot::early_print_hex(ctx.cpu_id);
    moss::boot::early_print("\n");

    u32 eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x00000001) : "memory");
    moss::boot::early_print("CPUID Features: ");
    moss::boot::early_print_hex(edx);
    moss::boot::early_print("\n");

    // x86_64 QEMU q35 不提供 DTB，静态填充 PlatformInfo 以统一子系统接口。
    // 后续可扩展为 ACPI/E820 内存映射解析。
    {
        auto &info = moss::fdt::g_platform_info;
        info = {};
        info.dtb_valid = false;
        info.cpu_count = 1; // TODO: 可通过 CPUID 扩展检测
        info.memory_regions[0] = {
            moss::kernel::platform::ram_base(),
            moss::kernel::platform::ram_size()
        };
        info.memory_region_count = 1;
        info.total_memory_start = moss::kernel::platform::ram_base();
        info.total_memory_size = moss::kernel::platform::ram_size();

        ctx.memory_start = info.total_memory_start;
        ctx.memory_size = info.total_memory_size;
        ctx.kernel_phys_base = info.total_memory_start;
    }

    ctx.kernel_virt_base = moss::boot::arch_constants::KERNEL_VIRT_BASE;

    moss::boot::early_print("Memory range: ");
    moss::boot::early_print_hex(ctx.memory_start);
    moss::boot::early_print(" - ");
    moss::boot::early_print_hex(ctx.memory_start + ctx.memory_size);
    moss::boot::early_print("\n");

    moss::boot::early_print("x86_64 hardware init complete\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86_64BootImpl::setup_memory_management(BootContext &ctx) noexcept {
    (void)ctx;
    moss::boot::update_boot_stage(moss::boot::BootStage::MemoryManagement);

    moss::boot::early_print("=== x86_64 Memory Management Setup ===\n");
    moss::boot::early_print("TODO: Implement x86_64 page table and MMU setup\n");
    moss::boot::early_print("x86_64 memory management setup complete\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86_64BootImpl::setup_interrupts_and_exceptions(BootContext &ctx) noexcept {
    (void)ctx;
    moss::boot::update_boot_stage(moss::boot::BootStage::InterruptsExceptions);

    moss::boot::early_print("=== x86_64 Interrupts and Exceptions Setup ===\n");
    moss::boot::early_print("TODO: Implement IDT and interrupt controller init\n");
    moss::boot::early_print("x86_64 interrupt/exception setup complete\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86_64BootImpl::setup_smp_support(BootContext &ctx) noexcept {
    moss::boot::update_boot_stage(moss::boot::BootStage::SmpSupport);

    moss::boot::early_print("=== x86_64 SMP Support Setup ===\n");
    ctx.total_cpus = 1;
    moss::boot::early_print("TODO: Implement x86_64 multi-core boot support\n");
    moss::boot::early_print("x86_64 SMP setup complete\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86_64BootImpl::finalize_arch_init(BootContext &ctx) noexcept {
    (void)ctx;
    moss::boot::update_boot_stage(moss::boot::BootStage::ArchFinalize);

    moss::boot::early_print("=== x86_64 Architecture Init Complete ===\n");

    // Mark runtime heap as ready so operator new uses RuntimeHeapAllocator
    moss::abi::entry::mark_runtime_heap_ready();
    moss::boot::early_print("Runtime heap marked ready\n");

    moss::boot::early_print("x86_64 architecture-specific init all complete\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::X86_64BootImpl::detect_memory_layout(BootContext &ctx) noexcept {
    (void)ctx;
    return ::moss::kernel::VoidResult{};
}

u32 moss::boot::X86_64BootImpl::get_current_cpu_id() noexcept {
    return moss::boot::get_current_cpu_id_impl();
}

[[noreturn]] void moss::boot::X86_64BootImpl::arch_panic(const char *message) noexcept {
    moss::boot::early_print("\n=== x86_64 PANIC ===\n");
    moss::boot::early_print(message);
    moss::boot::early_print("\n====================\n");

    asm volatile("outw %0, %1" : : "a"(static_cast<u16>(0x2000)), "d"(static_cast<u16>(0x604)) : "memory");

    asm volatile("cli");
    while (true) {
        asm volatile("hlt");
    }
}

// === Boot global variables (x86_64 stubs) ===
namespace moss::boot {

moss::kernel::interrupts::GenericInterruptController *g_gic_controller = nullptr;
bool g_gic_hardware_available = false;

void activate_secondary_cpus() noexcept {
    early_print("[x86_64] SMP activation not yet implemented\n");
}

u32 wait_for_all_cpus_active([[maybe_unused]] u32 timeout_ms) noexcept {
    return 1;
}

} // namespace moss::boot
