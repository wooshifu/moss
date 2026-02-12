// 简化的MOSS内核IPI(CPU间中断)机制实现
// 专注于Ping IPI垂直切片验证

#include "interrupts/ipi_simple.hpp"

// 外部声明early_debug_print函数
extern "C" void early_debug_print(const char* message) noexcept;

namespace moss::kernel::interrupts {

// 全局简化IPI管理器实例
SimpleInterProcessorInterrupt* g_simple_ipi_manager = nullptr;

// === SimpleInterProcessorInterrupt 实现 ===

VoidResult SimpleInterProcessorInterrupt::initialize(u32 max_cpus) noexcept {
    if (initialized_) {
        return VoidResult{}; // 已经初始化
    }

    if (max_cpus > MAX_CPUS) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    max_cpus_ = max_cpus;
    message_sequence_ = 0;
    total_pings_sent_ = 0;
    initialized_ = true;

    early_debug_print("🚀 简化IPI子系统初始化完成 - 支持");
    char cpu_count_str[2] = {'0' + static_cast<char>(max_cpus_), '\0'};
    early_debug_print(cpu_count_str);
    early_debug_print("个CPU\n");

    return VoidResult{};
}

IpiResult SimpleInterProcessorInterrupt::send_ipi(u32 target_cpu, IpiType type) noexcept {
    if (!initialized_) {
        return IpiResult::NotInitialized;
    }

    if (!is_valid_cpu_id(target_cpu)) {
        return IpiResult::InvalidCpu;
    }

    // 创建IPI消息
    IpiMessage msg{
        .type = type,
        .source_cpu = get_current_cpu_id(),
        .target_cpu = target_cpu,
        .sequence = message_sequence_++
    };

    // 输出发送消息
    early_debug_print("📡 发送IPI: CPU");
    char src_str[2] = {'0' + static_cast<char>(msg.source_cpu), '\0'};
    early_debug_print(src_str);
    early_debug_print("→CPU");
    char dst_str[2] = {'0' + static_cast<char>(msg.target_cpu), '\0'};
    early_debug_print(dst_str);
    early_debug_print(" 类型=");
    early_debug_print(ipi_type_to_string(type));
    early_debug_print(" 序列=");
    char seq_str[2] = {'0' + static_cast<char>(msg.sequence & 0xF), '\0'};
    early_debug_print(seq_str);
    early_debug_print("\n");

    // 针对Ping IPI的特殊处理
    if (type == IpiType::Ping) {
        total_pings_sent_++;

        // 模拟目标CPU收到并处理Ping消息
        early_debug_print("🏓 CPU");
        early_debug_print(dst_str);
        early_debug_print(" 收到来自CPU");
        early_debug_print(src_str);
        early_debug_print("的Ping IPI (seq=");
        early_debug_print(seq_str);
        early_debug_print(")\n");
    }

    return IpiResult::Success;
}

IpiResult SimpleInterProcessorInterrupt::ping_cpu(u32 target_cpu) noexcept {
    return send_ipi(target_cpu, IpiType::Ping);
}

VoidResult SimpleInterProcessorInterrupt::self_test() noexcept {
    if (!initialized_) {
        return VoidResult{ErrorCode::InvalidState};
    }

    early_debug_print("🧪 开始简化IPI子系统自测试...\n");

    // 测试1: 基本ping测试
    early_debug_print("测试1: 基本Ping测试\n");
    u32 current_cpu = get_current_cpu_id();

    for (u32 target_cpu = 0; target_cpu < max_cpus_; ++target_cpu) {
        if (target_cpu != current_cpu) {
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
                early_debug_print(" 失败: ");
                early_debug_print(ipi_result_to_string(result));
                early_debug_print("\n");
            }
        }
    }

    early_debug_print("✅ 简化IPI自测试完成\n");
    return VoidResult{};
}

SimpleInterProcessorInterrupt::SystemInfo SimpleInterProcessorInterrupt::get_system_info() const noexcept {
    SystemInfo info{};
    info.initialized = initialized_;
    info.max_cpus = max_cpus_;
    info.total_pings_sent = total_pings_sent_;
    return info;
}

bool SimpleInterProcessorInterrupt::is_valid_cpu_id(u32 cpu_id) const noexcept {
    return cpu_id < max_cpus_;
}

u32 SimpleInterProcessorInterrupt::get_current_cpu_id() const noexcept {
    // 简化实现：总是返回CPU 0
    // TODO: 在完整实现中使用 arch::get_current_cpu_id()
    return 0;
}

// === 全局初始化函数实现 ===

VoidResult initialize_simple_ipi_system(u32 max_cpus) noexcept {
    if (g_simple_ipi_manager != nullptr) {
        return VoidResult{}; // 已经初始化
    }

    g_simple_ipi_manager = new SimpleInterProcessorInterrupt();
    if (g_simple_ipi_manager == nullptr) {
        return VoidResult{ErrorCode::OutOfMemory};
    }

    auto result = g_simple_ipi_manager->initialize(max_cpus);
    if (!result) {
        delete g_simple_ipi_manager;
        g_simple_ipi_manager = nullptr;
        return result;
    }

    return VoidResult{};
}

void shutdown_simple_ipi_system() noexcept {
    if (g_simple_ipi_manager != nullptr) {
        delete g_simple_ipi_manager;
        g_simple_ipi_manager = nullptr;
        early_debug_print("🛑 简化IPI子系统已关闭\n");
    }
}

// === 调试工具函数实现 ===
// 注意：调试函数已移至 ipi_hardware_simple.cpp 以避免重复定义

} // namespace moss::kernel::interrupts
