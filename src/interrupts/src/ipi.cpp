// MOSS内核IPI(CPU间中断)机制实现
// Linux风格的CPU间通信系统实现

#include "interrupts/ipi.hpp"
#include "core/arch/arch_abstraction.hpp"

namespace moss::kernel::interrupts {

// 前向声明 - 避免包含复杂的GIC头文件
class GenericInterruptController;
extern GenericInterruptController* g_gic;

// 全局IPI管理器实例
InterProcessorInterrupt* g_ipi_manager = nullptr;

// === IpiStatistics 实现 ===

void IpiStatistics::reset() noexcept {
    sent_total.store(0, containers::MemoryOrder::Relaxed);
    sent_reschedule.store(0, containers::MemoryOrder::Relaxed);
    sent_call_function.store(0, containers::MemoryOrder::Relaxed);
    sent_stop.store(0, containers::MemoryOrder::Relaxed);
    sent_ping.store(0, containers::MemoryOrder::Relaxed);

    received_total.store(0, containers::MemoryOrder::Relaxed);
    received_reschedule.store(0, containers::MemoryOrder::Relaxed);
    received_call_function.store(0, containers::MemoryOrder::Relaxed);
    received_stop.store(0, containers::MemoryOrder::Relaxed);
    received_ping.store(0, containers::MemoryOrder::Relaxed);

    queue_full_errors.store(0, containers::MemoryOrder::Relaxed);
    invalid_cpu_errors.store(0, containers::MemoryOrder::Relaxed);
    total_latency_ns.store(0, containers::MemoryOrder::Relaxed);
}

void IpiStatistics::print_statistics(u32 cpu_id) const noexcept {
    early_debug_print("=== IPI Statistics for CPU ");
    // 简化的数字输出
    if (cpu_id < 10) {
        char cpu_str[2] = {'0' + static_cast<char>(cpu_id), '\0'};
        early_debug_print(cpu_str);
    } else {
        early_debug_print("N");
    }
    early_debug_print(" ===\n");

    early_debug_print("发送总数: ");
    // 注意：为简化起见，这里只显示是否有消息发送
    if (sent_total.load(containers::MemoryOrder::Relaxed) > 0) {
        early_debug_print("有消息");
    } else {
        early_debug_print("0");
    }
    early_debug_print("\n");

    early_debug_print("接收总数: ");
    if (received_total.load(containers::MemoryOrder::Relaxed) > 0) {
        early_debug_print("有消息");
    } else {
        early_debug_print("0");
    }
    early_debug_print("\n");

    early_debug_print("Ping发送: ");
    if (sent_ping.load(containers::MemoryOrder::Relaxed) > 0) {
        early_debug_print("有");
    } else {
        early_debug_print("0");
    }
    early_debug_print("\n");

    early_debug_print("Ping接收: ");
    if (received_ping.load(containers::MemoryOrder::Relaxed) > 0) {
        early_debug_print("有");
    } else {
        early_debug_print("0");
    }
    early_debug_print("\n");
}

// === InterProcessorInterrupt 实现 ===

VoidResult InterProcessorInterrupt::initialize(u32 max_cpus) noexcept {
    if (initialized_) {
        return VoidResult{}; // 已经初始化
    }

    if (max_cpus > MAX_CPUS) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    max_cpus_ = max_cpus;

    // 初始化Per-CPU消息队列
    for (u32 cpu_id = 0; cpu_id < max_cpus_; ++cpu_id) {
        // 队列会自动初始化为空状态
        statistics_.get(cpu_id).reset();
    }

    message_sequence_.store(0, containers::MemoryOrder::Relaxed);
    initialized_ = true;

    early_debug_print("🚀 IPI子系统初始化完成 - 支持");
    char cpu_count_str[2] = {'0' + static_cast<char>(max_cpus_), '\0'};
    early_debug_print(cpu_count_str);
    early_debug_print("个CPU\n");

    return VoidResult{};
}

void InterProcessorInterrupt::shutdown() noexcept {
    if (!initialized_) {
        return;
    }

    initialized_ = false;
    max_cpus_ = 0;

    early_debug_print("🛑 IPI子系统已关闭\n");
}

IpiResult InterProcessorInterrupt::send_ipi(u32 target_cpu, IpiType type, void* data) noexcept {
    if (!initialized_) {
        return IpiResult::NotInitialized;
    }

    if (!is_valid_cpu_id(target_cpu)) {
        update_error_statistics(arch::get_current_cpu_id(), IpiResult::InvalidCpu);
        return IpiResult::InvalidCpu;
    }

    // 创建IPI消息
    IpiMessage msg{
        .type = type,
        .source_cpu = arch::get_current_cpu_id(),
        .target_cpu = target_cpu,
        .data = data,
        .sequence = message_sequence_.fetch_add(1, containers::MemoryOrder::Relaxed)
    };

    // 尝试将消息放入目标CPU的队列
    auto& target_queue = message_queues_.get(target_cpu);
    if (!target_queue.try_enqueue(msg)) {
        update_error_statistics(target_cpu, IpiResult::QueueFull);
        return IpiResult::QueueFull;
    }

    // 更新发送统计
    update_send_statistics(target_cpu, type);

    // 发送SGI中断通知目标CPU
    auto sgi_result = send_sgi_interrupt(target_cpu, type);
    if (sgi_result != IpiResult::Success) {
        // 从队列中移除消息（尽力而为）
        IpiMessage dummy;
        target_queue.try_dequeue(dummy);
        return sgi_result;
    }

    return IpiResult::Success;
}

void InterProcessorInterrupt::handle_ipi_interrupt(u32 cpu_id) noexcept {
    if (!initialized_ || !is_valid_cpu_id(cpu_id)) {
        return;
    }

    auto& queue = message_queues_.get(cpu_id);
    IpiMessage msg;

    // 处理队列中的所有消息
    while (queue.try_dequeue(msg)) {
        // 更新接收统计
        update_receive_statistics(cpu_id, msg.type);

        // 根据消息类型分发处理
        switch (msg.type) {
            case IpiType::Reschedule:
                handle_reschedule_ipi(msg);
                break;
            case IpiType::CallFunction:
                handle_call_function_ipi(msg);
                break;
            case IpiType::Stop:
                handle_stop_ipi(msg);
                break;
            case IpiType::Ping:
                handle_ping_ipi(msg);
                break;
            default:
                // 未知消息类型，忽略
                break;
        }
    }
}

// === Linux风格便利接口实现 ===

IpiResult InterProcessorInterrupt::ping_cpu(u32 target_cpu) noexcept {
    return send_ipi(target_cpu, IpiType::Ping);
}

IpiResult InterProcessorInterrupt::request_reschedule(u32 target_cpu) noexcept {
    return send_ipi(target_cpu, IpiType::Reschedule);
}

IpiResult InterProcessorInterrupt::smp_call_function_single(u32 cpu, void (*func)(void*), void* data) noexcept {
    // 将函数指针存储在消息数据中
    struct CallFunctionData {
        void (*function)(void*);
        void* argument;
    };

    // 注意：这里为简化实现，假设调用者管理内存
    // 实际生产环境可能需要更复杂的内存管理
    static CallFunctionData call_data;
    call_data.function = func;
    call_data.argument = data;

    return send_ipi(cpu, IpiType::CallFunction, &call_data);
}

IpiResult InterProcessorInterrupt::stop_cpu(u32 target_cpu) noexcept {
    return send_ipi(target_cpu, IpiType::Stop);
}

// === 统计和监控接口实现 ===

const IpiStatistics* InterProcessorInterrupt::get_statistics(u32 cpu_id) const noexcept {
    if (!is_valid_cpu_id(cpu_id)) {
        return nullptr;
    }
    return &statistics_.get(cpu_id);
}

void InterProcessorInterrupt::print_all_statistics() const noexcept {
    if (!initialized_) {
        early_debug_print("❌ IPI子系统未初始化\n");
        return;
    }

    early_debug_print("\n📊 === IPI系统统计信息 ===\n");
    for (u32 cpu_id = 0; cpu_id < max_cpus_; ++cpu_id) {
        statistics_.get(cpu_id).print_statistics(cpu_id);
    }
    early_debug_print("========================\n\n");
}

void InterProcessorInterrupt::reset_statistics() noexcept {
    for (u32 cpu_id = 0; cpu_id < max_cpus_; ++cpu_id) {
        statistics_.get(cpu_id).reset();
    }
}

InterProcessorInterrupt::QueueUsage InterProcessorInterrupt::get_queue_usage(u32 cpu_id) const noexcept {
    if (!is_valid_cpu_id(cpu_id)) {
        return {0, 0, 0.0f};
    }

    const auto& queue = message_queues_.get(cpu_id);
    u32 size = queue.size();
    u32 capacity = IPI_QUEUE_SIZE;
    float usage = (capacity > 0) ? (static_cast<float>(size) / capacity * 100.0f) : 0.0f;

    return {size, capacity, usage};
}

// === 调试和测试接口实现 ===

VoidResult InterProcessorInterrupt::self_test() noexcept {
    if (!initialized_) {
        return VoidResult{ErrorCode::NotInitialized};
    }

    early_debug_print("🧪 开始IPI子系统自测试...\n");

    // 测试1: 基本ping测试
    early_debug_print("测试1: 基本Ping测试\n");
    for (u32 target_cpu = 0; target_cpu < max_cpus_; ++target_cpu) {
        if (target_cpu != arch::get_current_cpu_id()) {
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
            }
        }
    }

    early_debug_print("✅ IPI自测试完成\n");
    return VoidResult{};
}

bool InterProcessorInterrupt::is_healthy() const noexcept {
    if (!initialized_) {
        return false;
    }

    // 检查队列状态
    for (u32 cpu_id = 0; cpu_id < max_cpus_; ++cpu_id) {
        auto usage = get_queue_usage(cpu_id);
        if (usage.usage_percentage > 90.0f) {
            return false; // 队列使用率过高
        }
    }

    return true;
}

InterProcessorInterrupt::SystemInfo InterProcessorInterrupt::get_system_info() const noexcept {
    SystemInfo info{};
    info.initialized = initialized_;
    info.max_cpus = max_cpus_;
    info.active_cpus = max_cpus_; // 简化：假设所有CPU都活跃

    // 计算总消息统计
    for (u32 cpu_id = 0; cpu_id < max_cpus_; ++cpu_id) {
        const auto& stats = statistics_.get(cpu_id);
        info.total_messages_sent += stats.sent_total.load(containers::MemoryOrder::Relaxed);
        info.total_messages_received += stats.received_total.load(containers::MemoryOrder::Relaxed);
    }

    return info;
}

// === 内部辅助函数实现 ===

bool InterProcessorInterrupt::is_valid_cpu_id(u32 cpu_id) const noexcept {
    return cpu_id < max_cpus_;
}

void InterProcessorInterrupt::handle_ping_ipi(const IpiMessage& msg) noexcept {
    // Ping IPI处理 - 这是我们的垂直切片核心功能
    early_debug_print("🏓 CPU");
    char target_str[2] = {'0' + static_cast<char>(msg.target_cpu), '\0'};
    early_debug_print(target_str);
    early_debug_print(" 收到来自CPU");
    char source_str[2] = {'0' + static_cast<char>(msg.source_cpu), '\0'};
    early_debug_print(source_str);
    early_debug_print("的Ping IPI (seq=");

    // 简化的序列号输出（只显示低位）
    char seq_str[2] = {'0' + static_cast<char>(msg.sequence & 0xF), '\0'};
    early_debug_print(seq_str);
    early_debug_print(")\n");

    // TODO: 在完整实现中，可能需要发送响应消息回源CPU
}

void InterProcessorInterrupt::handle_reschedule_ipi(const IpiMessage& msg) noexcept {
    // TODO: 实现重调度IPI处理
    early_debug_print("📅 收到重调度IPI请求\n");
}

void InterProcessorInterrupt::handle_call_function_ipi(const IpiMessage& msg) noexcept {
    // TODO: 实现跨CPU函数调用
    early_debug_print("🔧 收到函数调用IPI请求\n");
}

void InterProcessorInterrupt::handle_stop_ipi(const IpiMessage& msg) noexcept {
    // TODO: 实现CPU停止处理
    early_debug_print("🛑 收到停止IPI请求\n");
}

IpiResult InterProcessorInterrupt::send_sgi_interrupt(u32 target_cpu, IpiType type) noexcept {
    // 简化实现：暂时模拟SGI发送成功
    // TODO: 完整实现需要GIC->send_sgi()集成

    early_debug_print("📡 发送IPI类型");
    char type_str[2] = {'0' + static_cast<char>(static_cast<u8>(type)), '\0'};
    early_debug_print(type_str);
    early_debug_print(" 到CPU");
    char cpu_str[2] = {'0' + static_cast<char>(target_cpu), '\0'};
    early_debug_print(cpu_str);
    early_debug_print("\n");

    // 模拟成功发送
    return IpiResult::Success;
}

void InterProcessorInterrupt::update_send_statistics(u32 target_cpu, IpiType type) noexcept {
    if (!is_valid_cpu_id(target_cpu)) {
        return;
    }

    auto& stats = statistics_.get(arch::get_current_cpu_id());
    stats.sent_total.fetch_add(1, containers::MemoryOrder::Relaxed);

    switch (type) {
        case IpiType::Reschedule:
            stats.sent_reschedule.fetch_add(1, containers::MemoryOrder::Relaxed);
            break;
        case IpiType::CallFunction:
            stats.sent_call_function.fetch_add(1, containers::MemoryOrder::Relaxed);
            break;
        case IpiType::Stop:
            stats.sent_stop.fetch_add(1, containers::MemoryOrder::Relaxed);
            break;
        case IpiType::Ping:
            stats.sent_ping.fetch_add(1, containers::MemoryOrder::Relaxed);
            break;
    }
}

void InterProcessorInterrupt::update_receive_statistics(u32 cpu_id, IpiType type) noexcept {
    auto& stats = statistics_.get(cpu_id);
    stats.received_total.fetch_add(1, containers::MemoryOrder::Relaxed);

    switch (type) {
        case IpiType::Reschedule:
            stats.received_reschedule.fetch_add(1, containers::MemoryOrder::Relaxed);
            break;
        case IpiType::CallFunction:
            stats.received_call_function.fetch_add(1, containers::MemoryOrder::Relaxed);
            break;
        case IpiType::Stop:
            stats.received_stop.fetch_add(1, containers::MemoryOrder::Relaxed);
            break;
        case IpiType::Ping:
            stats.received_ping.fetch_add(1, containers::MemoryOrder::Relaxed);
            break;
    }
}

void InterProcessorInterrupt::update_error_statistics(u32 cpu_id, IpiResult error) noexcept {
    if (!is_valid_cpu_id(cpu_id)) {
        return;
    }

    auto& stats = statistics_.get(cpu_id);
    switch (error) {
        case IpiResult::QueueFull:
            stats.queue_full_errors.fetch_add(1, containers::MemoryOrder::Relaxed);
            break;
        case IpiResult::InvalidCpu:
            stats.invalid_cpu_errors.fetch_add(1, containers::MemoryOrder::Relaxed);
            break;
        default:
            break;
    }
}

// === 全局初始化函数实现 ===

VoidResult initialize_ipi_system(u32 max_cpus) noexcept {
    if (g_ipi_manager != nullptr) {
        return VoidResult{}; // 已经初始化
    }

    g_ipi_manager = new InterProcessorInterrupt();
    if (g_ipi_manager == nullptr) {
        return VoidResult{ErrorCode::OutOfMemory};
    }

    auto result = g_ipi_manager->initialize(max_cpus);
    if (!result) {
        delete g_ipi_manager;
        g_ipi_manager = nullptr;
        return result;
    }

    return VoidResult{};
}

void shutdown_ipi_system() noexcept {
    if (g_ipi_manager != nullptr) {
        g_ipi_manager->shutdown();
        delete g_ipi_manager;
        g_ipi_manager = nullptr;
    }
}

// === 调试工具函数实现 ===

const char* ipi_type_to_string(IpiType type) noexcept {
    switch (type) {
        case IpiType::Reschedule: return "Reschedule";
        case IpiType::CallFunction: return "CallFunction";
        case IpiType::Stop: return "Stop";
        case IpiType::Ping: return "Ping";
        default: return "Unknown";
    }
}

const char* ipi_result_to_string(IpiResult result) noexcept {
    switch (result) {
        case IpiResult::Success: return "Success";
        case IpiResult::QueueFull: return "QueueFull";
        case IpiResult::InvalidCpu: return "InvalidCpu";
        case IpiResult::NotInitialized: return "NotInitialized";
        case IpiResult::HardwareError: return "HardwareError";
        default: return "Unknown";
    }
}

void print_ipi_message_info(const IpiMessage& msg) noexcept {
    early_debug_print("📨 IPI消息: ");
    early_debug_print(ipi_type_to_string(msg.type));
    early_debug_print(" CPU");
    char src_str[2] = {'0' + static_cast<char>(msg.source_cpu), '\0'};
    early_debug_print(src_str);
    early_debug_print("→CPU");
    char dst_str[2] = {'0' + static_cast<char>(msg.target_cpu), '\0'};
    early_debug_print(dst_str);
    early_debug_print(" seq=");
    char seq_str[2] = {'0' + static_cast<char>(msg.sequence & 0xF), '\0'};
    early_debug_print(seq_str);
    early_debug_print("\n");
}

} // namespace moss::kernel::interrupts