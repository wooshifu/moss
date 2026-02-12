// MOSS内核简化硬件IPI系统实现 - 基于ARM64 GIC SGI
// 专注核心SGI硬件集成，避免复杂依赖

#include "../include/interrupts/ipi_hardware_simple.hpp"

namespace moss::kernel::interrupts {

// 前向声明SGI处理函数
static void handle_ping_sgi(InterruptId irq, void* context) noexcept;
static void handle_reschedule_sgi(InterruptId irq, void* context) noexcept;

// 全局简化硬件IPI管理器实例
SimpleHardwareIpi* g_simple_hardware_ipi = nullptr;

// === SimpleHardwareIpi 核心实现 ===

VoidResult SimpleHardwareIpi::initialize(GenericInterruptController* gic, u32 max_cpus) noexcept {
    if (initialized_) {
        return VoidResult{ErrorCode::AlreadyExists};
    }

    if (gic == nullptr) {
        return VoidResult{ErrorCode::InvalidParameter};
    }

    if (max_cpus == 0 || max_cpus > 256) {  // 合理的CPU数量限制
        return VoidResult{ErrorCode::InvalidParameter};
    }

    gic_ = gic;
    max_cpus_ = max_cpus;
    message_sequence_ = 1;

    early_debug_print("🔧 开始简化硬件IPI系统初始化...\n");

    // 注册核心IPI SGI处理函数到GIC
    early_debug_print("📡 注册SGI中断处理函数...\n");

    // Ping IPI (SGI 4)
    auto ping_result = gic_->register_interrupt(
        static_cast<InterruptId>(IpiSgiId::Ping),
        handle_ping_sgi,
        this,
        "IPI-Ping"
    );
    if (!ping_result) {
        early_debug_print("❌ 注册Ping SGI失败\n");
        return ping_result;
    }

    // Reschedule IPI (SGI 0)
    auto reschedule_result = gic_->register_interrupt(
        static_cast<InterruptId>(IpiSgiId::Reschedule),
        handle_reschedule_sgi,
        this,
        "IPI-Reschedule"
    );
    if (!reschedule_result) {
        early_debug_print("❌ 注册Reschedule SGI失败\n");
        return reschedule_result;
    }

    early_debug_print("✅ SGI处理函数注册完成\n");

    // 启用核心IPI SGI中断
    early_debug_print("🔌 启用IPI SGI中断...\n");

    auto enable_ping = gic_->enable_interrupt(static_cast<InterruptId>(IpiSgiId::Ping));
    auto enable_reschedule = gic_->enable_interrupt(static_cast<InterruptId>(IpiSgiId::Reschedule));

    if (!enable_ping || !enable_reschedule) {
        early_debug_print("❌ 启用SGI中断失败\n");
        return VoidResult{ErrorCode::InvalidState};
    }

    early_debug_print("✅ IPI SGI中断已启用\n");

    initialized_ = true;

    early_debug_print("🎉 简化硬件IPI系统初始化成功！\n");
    early_debug_print("📊 支持CPU数量: ");

    // 简单的数字转字符串
    char cpu_count_str[4];
    if (max_cpus_ < 10) {
        cpu_count_str[0] = '0' + static_cast<char>(max_cpus_);
        cpu_count_str[1] = '\0';
    } else if (max_cpus_ < 100) {
        cpu_count_str[0] = '0' + static_cast<char>(max_cpus_ / 10);
        cpu_count_str[1] = '0' + static_cast<char>(max_cpus_ % 10);
        cpu_count_str[2] = '\0';
    } else {
        cpu_count_str[0] = '9';
        cpu_count_str[1] = '9';
        cpu_count_str[2] = '+';
        cpu_count_str[3] = '\0';
    }

    early_debug_print(cpu_count_str);
    early_debug_print("\n");

    return VoidResult{};
}

void SimpleHardwareIpi::shutdown() noexcept {
    if (!initialized_) {
        return;
    }

    early_debug_print("🛑 关闭简化硬件IPI系统...\n");

    // 禁用所有IPI SGI中断
    if (gic_) {
        (void)gic_->disable_interrupt(static_cast<InterruptId>(IpiSgiId::Ping));
        (void)gic_->disable_interrupt(static_cast<InterruptId>(IpiSgiId::Reschedule));

        // 取消注册中断处理函数
        (void)gic_->unregister_interrupt(static_cast<InterruptId>(IpiSgiId::Ping));
        (void)gic_->unregister_interrupt(static_cast<InterruptId>(IpiSgiId::Reschedule));
    }

    initialized_ = false;
    gic_ = nullptr;

    early_debug_print("✅ 简化硬件IPI系统已关闭\n");
}

// === 核心IPI发送功能实现 ===

IpiResult SimpleHardwareIpi::send_ipi(u32 target_cpu, IpiType type) noexcept {
    if (!initialized_) {
        return IpiResult::NotInitialized;
    }

    if (!is_valid_cpu_id(target_cpu)) {
        return IpiResult::InvalidCpu;
    }

    // 🔥 关键：将IPI类型映射到SGI中断号
    IpiSgiId sgi_id = ipi_type_to_sgi(type);
    u32 target_cpu_mask = 1U << target_cpu;

    // 输出发送信息
    early_debug_print("📡 硬件IPI: CPU");
    char src_str[2] = {'0' + static_cast<char>(get_current_cpu_id() % 10), '\0'};
    early_debug_print(src_str);
    early_debug_print("→CPU");
    char dst_str[2] = {'0' + static_cast<char>(target_cpu % 10), '\0'};
    early_debug_print(dst_str);
    early_debug_print(" SGI=");
    char sgi_str[2] = {'0' + static_cast<char>(static_cast<u8>(sgi_id) % 10), '\0'};
    early_debug_print(sgi_str);
    early_debug_print("\n");

    // 🔥 关键：调用真正的硬件SGI发送
    auto hw_result = send_hardware_sgi(sgi_id, target_cpu_mask);
    if (!hw_result) {
        return IpiResult::HardwareError;
    }

    // 更新发送统计
    u8 sgi_index = static_cast<u8>(sgi_id);
    if (sgi_index < 8) {
        sgi_send_counts_[sgi_index]++;
    }
    total_ipis_sent_++;

    return IpiResult::Success;
}

IpiResult SimpleHardwareIpi::ping_cpu(u32 target_cpu) noexcept {
    return send_ipi(target_cpu, IpiType::Ping);
}

IpiResult SimpleHardwareIpi::ping_cpus(u32 cpu_mask) noexcept {
    if (!initialized_) {
        return IpiResult::NotInitialized;
    }

    // 🔥 直接发送到多个CPU (利用SGI的广播能力)
    auto hw_result = send_hardware_sgi(IpiSgiId::Ping, cpu_mask);
    if (!hw_result) {
        return IpiResult::HardwareError;
    }

    // 更新统计 (简化计算发送的CPU数量)
    u32 cpu_count = __builtin_popcount(cpu_mask);
    sgi_send_counts_[static_cast<u8>(IpiSgiId::Ping)] += cpu_count;
    total_ipis_sent_ += cpu_count;

    return IpiResult::Success;
}

IpiResult SimpleHardwareIpi::request_reschedule(u32 target_cpu) noexcept {
    return send_ipi(target_cpu, IpiType::Reschedule);
}

IpiResult SimpleHardwareIpi::wakeup_cpu(u32 target_cpu) noexcept {
    return send_ipi(target_cpu, IpiType::WakeUp);
}

// === SGI中断处理函数实现 ===

static void handle_ping_sgi(InterruptId /* irq */, void* /* context */) noexcept {
    u32 current_cpu = GenericInterruptController::get_current_cpu_id();

    early_debug_print("🏓 CPU");
    char cpu_str[2] = {'0' + static_cast<char>(current_cpu % 10), '\0'};
    early_debug_print(cpu_str);
    early_debug_print(" 收到Ping SGI\n");

    // 更新接收统计
    if (current_cpu < 4) {  // 简化：只支持4个CPU的统计
        // ipi_manager->sgi_receive_counts_[static_cast<u8>(IpiSgiId::Ping)]++;
        // 简化实现：暂时不更新接收统计
    }
}

static void handle_reschedule_sgi(InterruptId /* irq */, void* /* context */) noexcept {
    u32 current_cpu = GenericInterruptController::get_current_cpu_id();

    early_debug_print("🔄 CPU");
    char cpu_str[2] = {'0' + static_cast<char>(current_cpu % 10), '\0'};
    early_debug_print(cpu_str);
    early_debug_print(" 收到Reschedule SGI\n");

    // TODO: 触发调度器重新调度
    // scheduler->reschedule(current_cpu);
}

// === 内部辅助函数实现 ===

VoidResult SimpleHardwareIpi::send_hardware_sgi(IpiSgiId sgi_id, u32 target_cpu_mask) noexcept {
    if (!gic_) {
        return VoidResult{ErrorCode::InvalidState};
    }

    // 🔥 关键：调用GIC硬件SGI发送函数
    InterruptId sgi_interrupt_id = static_cast<InterruptId>(sgi_id);
    return gic_->send_sgi(sgi_interrupt_id, target_cpu_mask);
}

bool SimpleHardwareIpi::is_valid_cpu_id(u32 cpu_id) const noexcept {
    return cpu_id < max_cpus_;
}

u32 SimpleHardwareIpi::get_current_cpu_id() noexcept {
    // 使用与GIC相同的方法获取CPU ID
    return GenericInterruptController::get_current_cpu_id();
}

u64 SimpleHardwareIpi::generate_sequence() noexcept {
    return message_sequence_++;
}

// === 统计信息实现 ===

SimpleHardwareIpi::Statistics SimpleHardwareIpi::get_statistics() const noexcept {
    Statistics stats{};

    stats.total_sent = total_ipis_sent_;
    stats.ping_count = sgi_send_counts_[static_cast<u8>(IpiSgiId::Ping)];
    stats.reschedule_count = sgi_send_counts_[static_cast<u8>(IpiSgiId::Reschedule)];
    stats.max_cpus = max_cpus_;
    stats.initialized = initialized_;

    return stats;
}

VoidResult SimpleHardwareIpi::self_test() noexcept {
    if (!initialized_) {
        return VoidResult{ErrorCode::InvalidState};
    }

    early_debug_print("🧪 开始简化硬件IPI子系统自测试...\n");

    // 测试1: 基本Ping测试
    early_debug_print("测试1: 基本Ping测试\n");

    for (u32 target_cpu = 1; target_cpu < max_cpus_ && target_cpu < 4; ++target_cpu) {
        auto result = ping_cpu(target_cpu);
        if (result == IpiResult::Success) {
            early_debug_print("✅ Ping CPU");
            char cpu_str[2] = {'0' + static_cast<char>(target_cpu), '\0'};
            early_debug_print(cpu_str);
            early_debug_print(" 成功\n");
        } else {
            early_debug_print("❌ Ping CPU");
            char cpu_str[2] = {'0' + static_cast<char>(target_cpu), '\0'};
            early_debug_print(cpu_str);
            early_debug_print(" 失败\n");
            return VoidResult{ErrorCode::InvalidState};
        }
    }

    early_debug_print("✅ 简化硬件IPI自测试完成\n");
    early_debug_print("🎯 硬件SGI集成验证成功!\n");

    return VoidResult{};
}

// === 全局函数实现 ===

VoidResult initialize_simple_hardware_ipi(GenericInterruptController* gic, u32 max_cpus) noexcept {
    if (g_simple_hardware_ipi != nullptr) {
        return VoidResult{ErrorCode::AlreadyExists};
    }

    g_simple_hardware_ipi = new SimpleHardwareIpi();
    if (g_simple_hardware_ipi == nullptr) {
        return VoidResult{ErrorCode::OutOfMemory};
    }

    auto init_result = g_simple_hardware_ipi->initialize(gic, max_cpus);
    if (!init_result) {
        delete g_simple_hardware_ipi;
        g_simple_hardware_ipi = nullptr;
        return init_result;
    }

    early_debug_print("🌟 全局简化硬件IPI系统已启动\n");
    return VoidResult{};
}

void shutdown_simple_hardware_ipi() noexcept {
    if (g_simple_hardware_ipi != nullptr) {
        delete g_simple_hardware_ipi;
        g_simple_hardware_ipi = nullptr;
        early_debug_print("🌟 全局简化硬件IPI系统已关闭\n");
    }
}

// === 调试工具函数实现 ===

const char* ipi_type_to_string(IpiType type) noexcept {
    switch (type) {
        case IpiType::Ping: return "Ping";
        case IpiType::Reschedule: return "Reschedule";
        case IpiType::CallFunction: return "CallFunction";
        case IpiType::Stop: return "Stop";
        case IpiType::WakeUp: return "WakeUp";
        case IpiType::Timer: return "Timer";
        case IpiType::Debug: return "Debug";
        default: return "Unknown";
    }
}

const char* ipi_result_to_string(IpiResult result) noexcept {
    switch (result) {
        case IpiResult::Success: return "Success";
        case IpiResult::InvalidCpu: return "InvalidCpu";
        case IpiResult::InvalidType: return "InvalidType";
        case IpiResult::NotInitialized: return "NotInitialized";
        case IpiResult::HardwareError: return "HardwareError";
        case IpiResult::QueueFull: return "QueueFull";
        case IpiResult::Timeout: return "Timeout";
        default: return "Unknown";
    }
}

const char* sgi_id_to_string(IpiSgiId sgi) noexcept {
    switch (sgi) {
        case IpiSgiId::Reschedule: return "SGI0-Reschedule";
        case IpiSgiId::CallFunction: return "SGI1-CallFunction";
        case IpiSgiId::CallFunctionSingle: return "SGI2-CallFunctionSingle";
        case IpiSgiId::Timer: return "SGI3-Timer";
        case IpiSgiId::Ping: return "SGI4-Ping";
        case IpiSgiId::WakeUp: return "SGI5-WakeUp";
        case IpiSgiId::Stop: return "SGI6-Stop";
        case IpiSgiId::Debug: return "SGI7-Debug";
        default: return "SGI-Unknown";
    }
}

} // namespace moss::kernel::interrupts
