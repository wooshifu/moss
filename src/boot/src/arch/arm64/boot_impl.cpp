/*
 * ARM64 architecture-specific boot implementation - module implementation unit
 * Implements the unified boot interface for ARM64
 */

module;

// Architecture detection
#include "arch_detect.h"

// PSCI constants (must be in global module fragment as macros)
#define PSCI_CPU_ON_64 0xC4000003

// extern "C" declarations for assembly-callable functions
extern "C" {
void _start();

// Linker script symbols
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

// Exception vectors defined in start_arm64.S
extern char exception_vectors[];

void mark_runtime_heap_ready() noexcept;

// CPU startup protocol assembly symbols
extern volatile unsigned long long cpu_startup_flags[][2];

// Assembly-callable functions
void mark_cpu_online(unsigned int cpu_id) noexcept;
void mark_cpu_parked(unsigned int cpu_id) noexcept;
[[noreturn]] void secondary_cpu_entry() noexcept;
}

module moss.boot;

using moss::u8;
using moss::u16;
using moss::u32;
using moss::u64;
using moss::VirtAddr;
using moss::PhysAddr;
using moss::i32;

// Forward declaration
u32 get_current_cpu_id_impl() noexcept;

// ========================================================================
// SMP startup and CPU detection data structures
// ========================================================================

/// CPU startup control info - aligned with assembly memory layout
struct CpuStartupInfo {
    void (*entry_point)(void);
    volatile u32 startup_flag;
    u32 reserved;
    u64 stack_pointer;
    u32 cpu_id;
    u32 boot_status;
} __attribute__((packed, aligned(8)));

/// CPU online state enumeration
enum class CpuState : u32 {
    Offline = 0,
    Starting = 1,
    Parked = 2,
    Active = 3,
    Online = 4,
    Failed = 5
};

/// Global CPU topology info
struct CpuTopology {
    u32 total_cpus;
    u32 online_cpus;
    volatile CpuState cpu_states[moss::kernel::MAX_CPUS];
    u64 boot_timestamps[moss::kernel::MAX_CPUS];
    bool detection_completed;
};

// Global variable definitions
static CpuTopology g_cpu_topology = {
    .total_cpus = 1,
    .online_cpus = 1,
    .cpu_states = {CpuState::Online},
    .boot_timestamps = {0},
    .detection_completed = false};

// ========================================================================
// Dynamic CPU detection and startup control functions
// ========================================================================

static u32 probe_available_cpus() noexcept {
    // Priority 1: Use DTB-derived CPU count (set during hardware_early_init)
    const auto &plat = moss::fdt::get_platform_info();
    if (plat.dtb_valid && plat.cpu_count > 0) {
        u32 count = plat.cpu_count;
        if (count > moss::kernel::MAX_CPUS) {
            count = moss::kernel::MAX_CPUS;
        }
        return count;
    }

    // Priority 2: Estimate from linker-allocated stack space
    auto total_stack_size = reinterpret_cast<u64>(_stack_top_addr) -
                            reinterpret_cast<u64>(_stack_bottom_addr);
    u32 stack_based = static_cast<u32>(total_stack_size / (32 * 1024));
    if (stack_based >= 1 && stack_based <= moss::kernel::MAX_CPUS) {
        return stack_based;
    }

    // Fallback: single core
    return 1;
}

static u64 get_timestamp() noexcept {
    u64 count;
    asm volatile("mrs %0, cntvct_el0" : "=r"(count));
    return count;
}

static void initialize_cpu_startup_info(u32 detected_cpus) noexcept {
    g_cpu_topology.total_cpus = detected_cpus;
    g_cpu_topology.online_cpus = 1;
    g_cpu_topology.detection_completed = true;

    for (u32 cpu = 0; cpu < moss::kernel::MAX_CPUS; cpu++) {
        if (cpu == 0) {
            g_cpu_topology.cpu_states[cpu] = CpuState::Online;
            g_cpu_topology.boot_timestamps[cpu] = get_timestamp();
        } else {
            g_cpu_topology.cpu_states[cpu] = CpuState::Offline;
            g_cpu_topology.boot_timestamps[cpu] = 0;
        }
    }

    for (u32 cpu = 0; cpu < moss::kernel::MAX_CPUS; cpu++) {
        cpu_startup_flags[cpu][0] = 0;
        cpu_startup_flags[cpu][1] = 0;
    }
}

[[maybe_unused]] static bool wait_cpu_parked(u32 cpu_id, u32 timeout_ms) noexcept {
    if (cpu_id >= moss::kernel::MAX_CPUS) {
        return false;
    }

    u32 iteration = 0;
    u32 max_iterations = timeout_ms * 10;

    volatile u8 *uart_debug = reinterpret_cast<volatile u8 *>(0x9000000);
    uart_debug[0] = 'W';
    uart_debug[0] = '0' + static_cast<u8>(cpu_id % 10);
    uart_debug[0] = 10;

    while (g_cpu_topology.cpu_states[cpu_id] != CpuState::Parked) {
        if (iteration >= max_iterations) {
            uart_debug[0] = 'T';
            uart_debug[0] = '0' + static_cast<u8>(cpu_id % 10);
            uart_debug[0] = 10;

            g_cpu_topology.cpu_states[cpu_id] = CpuState::Failed;
            return false;
        }

        asm volatile("dmb sy" ::: "memory");

        for (volatile u32 i = 0; i < 10000; i = i + 1) {
            asm volatile("nop");
        }
        iteration++;

        if (iteration % 1000 == 0) {
            uart_debug[0] = 'C';
            uart_debug[0] = '0' + static_cast<u8>(cpu_id % 10);
            uart_debug[0] = '0' + static_cast<u8>(g_cpu_topology.cpu_states[cpu_id]);
            uart_debug[0] = 10;
        }
    }

    uart_debug[0] = 'S';
    uart_debug[0] = '0' + static_cast<u8>(cpu_id % 10);
    uart_debug[0] = 10;

    return true;
}

extern "C" void mark_cpu_online(u32 cpu_id) noexcept {
    if (cpu_id < moss::kernel::MAX_CPUS) {
        g_cpu_topology.cpu_states[cpu_id] = CpuState::Online;
        g_cpu_topology.boot_timestamps[cpu_id] = 0;
        g_cpu_topology.online_cpus++;
    }
}

extern "C" void mark_cpu_parked(u32 cpu_id) noexcept {
    if (cpu_id < moss::kernel::MAX_CPUS) {
        g_cpu_topology.cpu_states[cpu_id] = CpuState::Parked;
        g_cpu_topology.boot_timestamps[cpu_id] = 0;

        asm volatile("dmb sy" ::: "memory");
        asm volatile("dsb sy" ::: "memory");

        volatile u8 *uart_debug = reinterpret_cast<volatile u8 *>(0x9000000);
        uart_debug[0] = 'M';
        uart_debug[0] = '0' + static_cast<u8>(cpu_id % 10);
        uart_debug[0] = 10;
    }
}

void mark_cpu_active(u32 cpu_id) noexcept {
    if (cpu_id < moss::kernel::MAX_CPUS) {
        g_cpu_topology.cpu_states[cpu_id] = CpuState::Active;
    }
}

bool is_cpu_in_state(u32 cpu_id, CpuState expected_state) noexcept {
    if (cpu_id >= moss::kernel::MAX_CPUS) {
        return false;
    }
    return g_cpu_topology.cpu_states[cpu_id] == expected_state;
}

bool wait_for_cpu_state(u32 cpu_id, CpuState expected_state, u32 timeout_ms) noexcept {
    if (cpu_id >= moss::kernel::MAX_CPUS) {
        return false;
    }

    u32 elapsed = 0;
    while (elapsed < timeout_ms) {
        asm volatile("dmb sy" ::: "memory");
        if (g_cpu_topology.cpu_states[cpu_id] == expected_state) {
            return true;
        }
        for (volatile u32 i = 0; i < 10000; i = i + 1) {
            asm volatile("nop");
        }
        elapsed += 10;
    }

    asm volatile("dmb sy" ::: "memory");
    return g_cpu_topology.cpu_states[cpu_id] == expected_state;
}

[[noreturn]] void cpu_park(u32 cpu_id) noexcept {
    volatile u8 *uart_base = reinterpret_cast<volatile u8 *>(0x9000000);

    uart_base[0] = 'P';
    uart_base[0] = 'A';
    uart_base[0] = 'R';
    uart_base[0] = 'K';
    uart_base[0] = '0' + static_cast<u8>(cpu_id % 10);
    uart_base[0] = 10;

    mark_cpu_parked(cpu_id);

    while (!is_cpu_in_state(cpu_id, CpuState::Active)) {
        asm volatile("wfi");

        for (volatile u32 i = 0; i < 100; i = i + 1) {
            asm volatile("nop");
        }
    }

    uart_base[0] = 'A';
    uart_base[0] = 'C';
    uart_base[0] = 'T';
    uart_base[0] = 'V';
    uart_base[0] = '0' + static_cast<u8>(cpu_id % 10);
    uart_base[0] = 10;

    uart_base[0] = 'W';
    uart_base[0] = 'A';
    uart_base[0] = 'I';
    uart_base[0] = 'T';
    uart_base[0] = '0' + static_cast<u8>(cpu_id % 10);
    uart_base[0] = 10;

    while (true) {
        asm volatile("wfi");

        for (u32 i = 0; i < 1000; i++) {
            asm volatile("nop");
        }
    }
}

// Secondary CPU entry point — park first, then full subsystem initialization
// on activation by CPU 0 (after GIC distributor and timer are ready).
extern "C" [[noreturn]] void secondary_cpu_entry() noexcept {
    // 1. Read CPU ID from hardware
    u64 mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    u32 cpu_id = static_cast<u32>(mpidr & 0xFF);

    // Minimal UART output — avoid flooding FIFO while CPU 0 is printing
    volatile u8 *uart_out = reinterpret_cast<volatile u8 *>(0x09000000);
    uart_out[0] = 'S';
    uart_out[0] = '0' + static_cast<u8>(cpu_id % 10);
    uart_out[0] = '\n';

    // --- Phase 1: Park and wait for CPU 0 to finish initialization ---
    // Directly set state without UART output to avoid FIFO contention
    if (cpu_id < moss::kernel::MAX_CPUS) {
        g_cpu_topology.cpu_states[cpu_id] = CpuState::Parked;
        asm volatile("dmb sy" ::: "memory");
    }

    // Wait until CPU 0 marks us as Active (meaning all subsystems are ready)
    while (!is_cpu_in_state(cpu_id, CpuState::Active)) {
        asm volatile("wfe");
    }

    uart_out[0] = 'I';
    uart_out[0] = '0' + static_cast<u8>(cpu_id % 10);
    uart_out[0] = '\n';

    // --- Phase 2: Full subsystem initialization (GIC/timer are ready) ---

    // 2. Set exception vectors (same as CPU 0)
    asm volatile("msr vbar_el1, %0" :: "r"(exception_vectors));
    asm volatile("isb");

    // 3. Enable FP/NEON access
    u64 cpacr = (3ULL << 20);
    asm volatile("msr cpacr_el1, %0" :: "r"(cpacr));
    asm volatile("isb");

    // 4. Initialize GIC CPU interface for this CPU
    const auto &plat = moss::fdt::get_platform_info();
    moss::kernel::VirtAddr gic_cpu_base =
        (plat.dtb_valid && plat.intc.valid)
            ? static_cast<moss::kernel::VirtAddr>(plat.intc.cpu_base)
            : moss::kernel::platform::intc_cpu_base();
    (void)moss::kernel::hal::intc::init_cpu_interface(gic_cpu_base);

    // 5. Enable per-CPU timer
    moss::kernel::hal::timer::enable();
    // Set compare far in the future to avoid spurious interrupt
    u64 counter_now = moss::kernel::hal::timer::read_counter();
    moss::kernel::hal::timer::set_compare(
        counter_now + moss::kernel::timer::TimerSubsystem::instance().clocksource().ns_to_cycles(1000000000ULL));

    // 6. Mark CPU as online (init complete)
    asm volatile("dmb sy" ::: "memory");
    mark_cpu_online(cpu_id);
    asm volatile("dmb sy" ::: "memory");
    asm volatile("sev" ::: "memory");

    uart_out[0] = 'R';
    uart_out[0] = '0' + static_cast<u8>(cpu_id % 10);
    uart_out[0] = '\n';

    // 7. Enable IRQs and enter scheduling loop (never returns)
    asm volatile("msr daifclr, #2" ::: "memory");
    moss::kernel::process::secondary_cpu_schedule_loop(cpu_id);
}

// === Linux-style global GIC hardware instances ===

namespace moss::boot {

moss::kernel::interrupts::GenericInterruptController *g_gic_controller = nullptr;
bool g_gic_hardware_available = false;

} // namespace moss::boot

/// Activate parked secondary CPUs: mark Active (to unblock their init),
/// then wait for them to reach Online (init complete, scheduling loop entered).
namespace moss::boot {
void activate_secondary_cpus() noexcept {
    u32 successfully_activated = 0;

    for (u32 cpu_id = 1; cpu_id < g_cpu_topology.total_cpus; ++cpu_id) {
        // Wait for secondary CPU to reach Parked state (PSCI may take time)
        if (!wait_for_cpu_state(cpu_id, CpuState::Parked, 3000)) {
            continue;
        }

        // Unblock secondary CPU from its WFE loop
        mark_cpu_active(cpu_id);
        asm volatile("dmb sy" ::: "memory");
        asm volatile("sev");

        // Wait for it to finish init and reach Online state
        if (wait_for_cpu_state(cpu_id, CpuState::Online, 3000)) {
            successfully_activated++;
        }
    }

    g_cpu_topology.online_cpus = 1 + successfully_activated;
}

u32 wait_for_all_cpus_active(u32 timeout_ms) noexcept {
    u32 active_count = 1;
    u32 elapsed = 0;

    while (elapsed < timeout_ms) {
        active_count = 1;

        for (u32 cpu_id = 1; cpu_id < g_cpu_topology.total_cpus; ++cpu_id) {
            if (is_cpu_in_state(cpu_id, CpuState::Active) ||
                is_cpu_in_state(cpu_id, CpuState::Online)) {
                active_count++;
            }
        }

        if (active_count >= g_cpu_topology.total_cpus) {
            break;
        }

        for (volatile u32 i = 0; i < 100000; i = i + 1) {
            asm volatile("nop");
        }
        elapsed += 10;
    }

    return active_count;
}

} // namespace moss::boot

// ARM64 PSCI call function
static u64 psci_call(u32 function_id, u64 arg0 = 0, u64 arg1 = 0, u64 arg2 = 0, u64 arg3 = 0) noexcept {
    u64 result;

    asm volatile(
        "mov x0, %1\n"
        "mov x1, %2\n"
        "mov x2, %3\n"
        "mov x3, %4\n"
        "mov x4, %5\n"
        "hvc #0\n"
        "mov %0, x0\n"
        : "=r"(result)
        : "r"(static_cast<u64>(function_id)), "r"(arg0), "r"(arg1), "r"(arg2), "r"(arg3)
        : "x0", "x1", "x2", "x3", "x4", "memory");

    return result;
}

namespace moss::boot {

// Global boot status
BootStatus g_boot_status = {
    .current_stage = BootStage::PreInit,
    .completed_stages_mask = 0,
    .stage_timestamps = {0},
    .last_error = ::moss::kernel::ErrorCode::Success};

// Early UART output
class EarlyUart {
private:
    static constexpr VirtAddr UART_BASE = moss::kernel::platform::uart_base();
    static constexpr u32 UART_DR = 0x000;
    static constexpr u32 UART_FR = 0x018;
    static constexpr u32 UART_FR_TXFF = (1 << 5);

    volatile u32 *const uart_base;

public:
    EarlyUart() : uart_base(reinterpret_cast<volatile u32 *>(UART_BASE)) {}

    void put_char(char c) const {
        while (uart_base[UART_FR / 4] & UART_FR_TXFF) {
        }
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

u32 get_current_cpu_id_impl() noexcept {
    u64 mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return static_cast<u32>(mpidr & 0xFF);
}

// Boot stage status update implementation
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

// ARM64BootImpl member function implementations
::moss::kernel::VoidResult moss::boot::ARM64BootImpl::hardware_early_init(BootContext &ctx) noexcept {
    moss::boot::update_boot_stage(moss::boot::BootStage::HardwareInit);

    early_print("=== ARM64 Hardware Early Init ===\n");

    ctx.cpu_id = moss::boot::get_current_cpu_id_impl();
    early_print("CPU ID: ");
    early_print_hex(ctx.cpu_id);
    early_print("\n");

    // --- DTB 解析：从 Device Tree 获取真实硬件拓扑 ---
    // QEMU 通过 x0 寄存器传递 DTB 指针，已保存在 ctx.device_tree_ptr 中。
    // 解析成功后用真实值填充 BootContext，否则回退到硬编码默认值。
    if (ctx.device_tree_ptr) {
        early_print("DTB pointer: ");
        early_print_hex(reinterpret_cast<u64>(ctx.device_tree_ptr));
        early_print("\n");

        if (moss::fdt::parse_dtb(ctx.device_tree_ptr)) {
            const auto &info = moss::fdt::get_platform_info();

            early_print("DTB parse OK: ");
            early_print_hex(info.cpu_count);
            early_print(" CPUs, memory ");
            early_print_hex(info.total_memory_start);
            early_print(" + ");
            early_print_hex(info.total_memory_size);
            early_print("\n");

            ctx.memory_start = info.total_memory_start;
            ctx.memory_size = info.total_memory_size;
            ctx.kernel_phys_base = info.total_memory_start;
            ctx.total_cpus = info.cpu_count;
        } else {
            early_print("DTB parse failed, using platform defaults\n");
            ctx.memory_start = moss::kernel::platform::ram_base();
            ctx.memory_size = moss::kernel::platform::ram_size();
            ctx.kernel_phys_base = moss::kernel::platform::ram_base();
        }
    } else {
        early_print("No DTB pointer, using platform defaults\n");
        ctx.memory_start = moss::kernel::platform::ram_base();
        ctx.memory_size = moss::kernel::platform::ram_size();
        ctx.kernel_phys_base = moss::kernel::platform::ram_base();
    }

    ctx.kernel_virt_base = moss::boot::arch_constants::KERNEL_VIRT_BASE;

    early_print("ARM64 hardware init complete\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::ARM64BootImpl::setup_memory_management(BootContext &ctx) noexcept {
    (void)ctx;
    moss::boot::update_boot_stage(moss::boot::BootStage::MemoryManagement);

    early_print("=== ARM64 Memory Management Setup ===\n");

    auto mmu_result = ::moss::kernel::mm::setup_mmu();
    if (!mmu_result) {
        early_print("MMU setup failed\n");
        return ::moss::kernel::VoidResult{mmu_result.error()};
    }

    auto pfa_result = ::moss::kernel::mm::PageFrameAllocator::initialize();
    if (!pfa_result) {
        early_print("Physical page allocator init failed\n");
        return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::OutOfMemory};
    }

    VirtAddr heap_start = reinterpret_cast<VirtAddr>(_heap_start_addr);
    ::moss::kernel::usize initial_heap_size = 256 * 1024;
    auto heap_result = ::moss::kernel::mm::RuntimeHeapAllocator::initialize_heap(heap_start, initial_heap_size);
    if (!heap_result) {
        early_print("Runtime heap allocator init failed\n");
        return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::OutOfMemory};
    }

    early_print("ARM64 memory management setup complete\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::ARM64BootImpl::setup_interrupts_and_exceptions(BootContext &ctx) noexcept {
    (void)ctx;
    moss::boot::update_boot_stage(moss::boot::BootStage::InterruptsExceptions);

    early_print("=== ARM64 Interrupts and Exceptions Setup ===\n");

    early_print("ARM64 GIC hardware init...\n");

    using namespace moss::kernel::interrupts;
    g_gic_controller = new GenericInterruptController();
    if (!g_gic_controller) {
        early_print("GIC controller memory allocation failed\n");
        g_gic_hardware_available = false;
        early_print("System will use IPI proof-of-concept mode\n");
    } else {
        // 从 DTB 解析结果获取 GIC 地址，若 DTB 无效则回退到硬编码默认值
        const auto &plat = moss::fdt::get_platform_info();
        moss::kernel::VirtAddr gic_dist_base =
            (plat.dtb_valid && plat.intc.valid)
                ? static_cast<moss::kernel::VirtAddr>(plat.intc.dist_base)
                : moss::kernel::platform::intc_dist_base();
        moss::kernel::VirtAddr gic_cpu_base =
            (plat.dtb_valid && plat.intc.valid)
                ? static_cast<moss::kernel::VirtAddr>(plat.intc.cpu_base)
                : moss::kernel::platform::intc_cpu_base();

        early_print("GIC GICD=");
        early_print_hex(gic_dist_base);
        early_print(" GICC=");
        early_print_hex(gic_cpu_base);
        early_print("\n");

        auto gic_result = g_gic_controller->initialize(gic_dist_base, gic_cpu_base);
        if (gic_result) {
            early_print("GIC hardware init success\n");
            early_print("GIC features: SGI 0-15, PPI 16-31, SPI 32+\n");
            g_gic_hardware_available = true;

            early_print("GIC SGI verification...\n");
            early_print("SGI 0-15 available for IPI communication\n");
        } else {
            early_print("GIC hardware init failed\n");
            delete g_gic_controller;
            g_gic_controller = nullptr;
            g_gic_hardware_available = false;
            early_print("System will use IPI proof-of-concept mode\n");
        }
    }

    if (g_gic_hardware_available) {
        early_print("GIC hardware integration success - real hardware IPI available\n");
    } else {
        early_print("GIC hardware unavailable - will use proof-of-concept mode\n");
    }

    early_print("ARM64 interrupt/exception setup complete\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::ARM64BootImpl::setup_smp_support(BootContext &ctx) noexcept {
    moss::boot::update_boot_stage(moss::boot::BootStage::SmpSupport);

    early_print("=== ARM64 SMP Support Setup (Dynamic Detection) ===\n");

    u32 detected_cpus = probe_available_cpus();
    early_print("Detected CPU count: ");
    early_print_hex(static_cast<u64>(detected_cpus));
    early_print("\n");

    initialize_cpu_startup_info(detected_cpus);

    if (detected_cpus == 1) {
        early_print("Single-core mode\n");
        ctx.total_cpus = 1;
    } else {
        early_print("Multi-core boot sequence:\n");

        u32 successful_cpus = 1;

        for (u32 cpu_id = 1; cpu_id < detected_cpus; cpu_id++) {
            early_print("   Starting CPU ");
            early_print_hex(static_cast<u64>(cpu_id));
            early_print(" via PSCI...\n");

            // Mark Starting BEFORE PSCI call to avoid race: secondary CPU
            // may reach Parked before we return from PSCI, and we must not
            // overwrite its Parked state with Starting.
            g_cpu_topology.cpu_states[cpu_id] = CpuState::Starting;
            asm volatile("dmb sy" ::: "memory");

            cpu_startup_flags[cpu_id][0] = reinterpret_cast<u64>(secondary_cpu_entry);
            cpu_startup_flags[cpu_id][1] = 1;

            asm volatile("dmb sy" ::: "memory");
            asm volatile("dsb sy" ::: "memory");

            asm volatile("dc civac, %0" : : "r"(&cpu_startup_flags[cpu_id][0]) : "memory");
            asm volatile("dc civac, %0" : : "r"(&cpu_startup_flags[cpu_id][1]) : "memory");
            asm volatile("dsb sy" ::: "memory");

            asm volatile("sev" ::: "memory");

            u64 target_mpidr = static_cast<u64>(cpu_id);
            u64 entry_addr = reinterpret_cast<u64>(_start);
            u64 context_id = static_cast<u64>(cpu_id);

            early_print("   PSCI_CPU_ON: target=");
            early_print_hex(target_mpidr);
            early_print(" entry=");
            early_print_hex(entry_addr);
            early_print("\n");

            u64 psci_result = psci_call(PSCI_CPU_ON_64, target_mpidr, entry_addr, context_id);

            early_print("   PSCI result: ");
            early_print_hex(psci_result);

            if (psci_result == 0) {
                early_print(" (success)\n");
            } else {
                early_print(" (failed)\n");
                continue;
            }
        }

        // Assume all PSCI-started CPUs are successful
        successful_cpus = detected_cpus;
        ctx.total_cpus = successful_cpus;

        if (successful_cpus > 1) {
            early_print("SMP boot complete: ");
            early_print_hex(static_cast<u64>(successful_cpus));
            early_print(" CPUs online\n");
        } else {
            early_print("Secondary CPU startup failed, fallback to single-core mode\n");
            ctx.total_cpus = 1;
        }
    }

    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::ARM64BootImpl::finalize_arch_init(BootContext & /* ctx */) noexcept {
    early_print("=== ARM64 Architecture Init Complete ===\n");

    // Mark runtime heap as ready so operator new uses RuntimeHeapAllocator
    // instead of the 64KB early static buffer
    mark_runtime_heap_ready();
    early_print("Runtime heap marked ready\n");

    early_print("ARM64 architecture-specific init all complete\n\n");
    return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::ARM64BootImpl::detect_memory_layout(BootContext &ctx) noexcept {
    (void)ctx;
    return ::moss::kernel::VoidResult{};
}

u32 moss::boot::ARM64BootImpl::get_current_cpu_id() noexcept {
    return moss::boot::get_current_cpu_id_impl();
}

[[noreturn]] void moss::boot::ARM64BootImpl::arch_panic(const char *message) noexcept {
    early_print("\n=== ARM64 PANIC ===\n");
    early_print(message);
    early_print("\n==================\n");

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
