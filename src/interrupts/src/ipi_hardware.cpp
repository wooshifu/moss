// MOSS内核硬件IPI系统实现 - 基于ARM64 GIC SGI
// 真正的CPU间中断通信，替换概念验证系统

#include "../include/interrupts/ipi_hardware.hpp"
#include "core/arch/arch_abstraction.hpp"

// 外部调试打印函数
extern "C" void early_debug_print(const char* message) noexcept;

namespace moss::kernel::interrupts {

// 全局硬件IPI管理器实例
HardwareInterProcessorInterrupt* g_hardware_ipi_manager = nullptr;

// === HardwareInterProcessorInterrupt 实现 ===

HardwareInterProcessorInterrupt::~HardwareInterProcessorInterrupt() noexcept {
    shutdown();
}

VoidResult HardwareInterProcessorInterrupt::initialize(GenericInterruptController* gic, u32 max_cpus) noexcept {
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

    early_debug_print("🔧 开始硬件IPI系统初始化...\n");

    // 注册所有IPI SGI处理函数到GIC
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

    // CallFunction IPI (SGI 1)
    auto call_result = gic_->register_interrupt(
        static_cast<InterruptId>(IpiSgiId::CallFunction),
        handle_call_function_sgi,
        this,
        "IPI-CallFunction"
    );
    if (!call_result) {
        early_debug_print("❌ 注册CallFunction SGI失败\n");
        return call_result;
    }

    // WakeUp IPI (SGI 5)
    auto wakeup_result = gic_->register_interrupt(
        static_cast<InterruptId>(IpiSgiId::WakeUp),
        handle_wakeup_sgi,
        this,
        "IPI-WakeUp"
    );
    if (!wakeup_result) {
        early_debug_print("❌ 注册WakeUp SGI失败\n");
        return wakeup_result;
    }

    early_debug_print("✅ SGI处理函数注册完成\n");

    // 启用所有IPI SGI中断
    early_debug_print("🔌 启用IPI SGI中断...\n");

    auto enable_ping = gic_->enable_interrupt(static_cast<InterruptId>(IpiSgiId::Ping));
    auto enable_reschedule = gic_->enable_interrupt(static_cast<InterruptId>(IpiSgiId::Reschedule));
    auto enable_call = gic_->enable_interrupt(static_cast<InterruptId>(IpiSgiId::CallFunction));
    auto enable_wakeup = gic_->enable_interrupt(static_cast<InterruptId>(IpiSgiId::WakeUp));

    if (!enable_ping || !enable_reschedule || !enable_call || !enable_wakeup) {
        early_debug_print("❌ 启用SGI中断失败\n");
        return VoidResult{ErrorCode::HardwareError};
    }

    early_debug_print("✅ IPI SGI中断已启用\n");

    // 初始化Per-CPU统计数组
    for (u32 i = 0; i < 16; ++i) {
        sgi_send_counts_[i].initialize();
        sgi_receive_counts_[i].initialize();
    }

    // 初始化Per-CPU函数调用队列
    call_queues_.initialize();

    initialized_ = true;

    early_debug_print("🎉 硬件IPI系统初始化成功！\n");
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

void HardwareInterProcessorInterrupt::shutdown() noexcept {
    if (!initialized_) {
        return;
    }

    early_debug_print("🛑 关闭硬件IPI系统...\n");

    // 禁用所有IPI SGI中断
    if (gic_) {
        (void)gic_->disable_interrupt(static_cast<InterruptId>(IpiSgiId::Ping));
        (void)gic_->disable_interrupt(static_cast<InterruptId>(IpiSgiId::Reschedule));
        (void)gic_->disable_interrupt(static_cast<InterruptId>(IpiSgiId::CallFunction));
        (void)gic_->disable_interrupt(static_cast<InterruptId>(IpiSgiId::WakeUp));

        // 取消注册中断处理函数
        (void)gic_->unregister_interrupt(static_cast<InterruptId>(IpiSgiId::Ping));
        (void)gic_->unregister_interrupt(static_cast<InterruptId>(IpiSgiId::Reschedule));
        (void)gic_->unregister_interrupt(static_cast<InterruptId>(IpiSgiId::CallFunction));
        (void)gic_->unregister_interrupt(static_cast<InterruptId>(IpiSgiId::WakeUp));
    }

    cleanup();
    initialized_ = false;
    gic_ = nullptr;

    early_debug_print("✅ 硬件IPI系统已关闭\n");
}

// === IPI发送接口实现 ===

IpiResult HardwareInterProcessorInterrupt::send_ipi(u32 target_cpu, IpiType type, void* data) noexcept {
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
    char sgi_str[3] = {'0' + static_cast<char>(static_cast<u8>(sgi_id) / 10),
                       '0' + static_cast<char>(static_cast<u8>(sgi_id) % 10), '\0'};
    early_debug_print(sgi_str);
    early_debug_print("\n");

    // 🔥 关键：调用真正的硬件SGI发送
    auto hw_result = send_hardware_sgi(sgi_id, target_cpu_mask);
    if (!hw_result) {
        (void)hardware_errors_.fetch_add(1, containers::MemoryOrder::Relaxed);
        return IpiResult::HardwareError;
    }

    // 更新发送统计
    u8 sgi_index = static_cast<u8>(sgi_id);
    if (sgi_index < 16) {
        sgi_send_counts_[sgi_index].get_current_cpu()++;
    }
    (void)total_ipis_sent_.fetch_add(1, containers::MemoryOrder::Relaxed);

    return IpiResult::Success;
}

IpiResult HardwareInterProcessorInterrupt::ping_cpu(u32 target_cpu) noexcept {
    return send_ipi(target_cpu, IpiType::Ping);
}

IpiResult HardwareInterProcessorInterrupt::ping_cpus(u32 cpu_mask) noexcept {
    if (!initialized_) {
        return IpiResult::NotInitialized;
    }

    // 🔥 直接发送到多个CPU (利用SGI的广播能力)
    auto hw_result = send_hardware_sgi(IpiSgiId::Ping, cpu_mask);
    if (!hw_result) {
        (void)hardware_errors_.fetch_add(1, containers::MemoryOrder::Relaxed);
        return IpiResult::HardwareError;
    }

    // 更新统计 (简化计算发送的CPU数量)
    u32 cpu_count = __builtin_popcount(cpu_mask);
    sgi_send_counts_[static_cast<u8>(IpiSgiId::Ping)].get_current_cpu() += cpu_count;
    (void)total_ipis_sent_.fetch_add(cpu_count, containers::MemoryOrder::Relaxed);

    return IpiResult::Success;
}

IpiResult HardwareInterProcessorInterrupt::request_reschedule(u32 target_cpu) noexcept {
    return send_ipi(target_cpu, IpiType::Reschedule);
}

IpiResult HardwareInterProcessorInterrupt::wakeup_cpu(u32 target_cpu) noexcept {
    return send_ipi(target_cpu, IpiType::WakeUp);
}

// === Linux兼容跨CPU函数调用 ===

IpiResult HardwareInterProcessorInterrupt::smp_call_function_single(u32 cpu, void (*func)(void*),
                                                                  void* data, bool wait) noexcept {
    if (!initialized_) {
        return IpiResult::NotInitialized;
    }

    if (!is_valid_cpu_id(cpu) || func == nullptr) {
        return IpiResult::InvalidCpu;
    }

    // 创建函数调用数据
    u64 sequence = (void)call_sequence_.fetch_add(1, containers::MemoryOrder::Relaxed);
    IpiCallData* call_data = new IpiCallData(func, data, sequence, get_current_cpu_id(), cpu);
    if (call_data == nullptr) {
        return IpiResult::QueueFull; // 使用已有的错误码
    }

    call_data->submit_time = get_timestamp();

    // 将调用放入目标CPU的队列
    auto& target_queue = call_queues_.get_cpu(cpu);
    if (!target_queue.enqueue(call_data)) {
        delete call_data;
        return IpiResult::QueueFull;
    }

    // 🔥 发送CallFunction SGI到目标CPU
    auto sgi_result = send_hardware_sgi(IpiSgiId::CallFunction, 1U << cpu);
    if (!sgi_result) {
        // 尝试从队列中移除 (最简实现：让目标CPU处理时忽略)
        delete call_data;
        return IpiResult::HardwareError;
    }

    early_debug_print("🚀 跨CPU函数调用: CPU");
    char src_str[2] = {'0' + static_cast<char>(get_current_cpu_id() % 10), '\0'};
    early_debug_print(src_str);
    early_debug_print("→CPU");
    char dst_str[2] = {'0' + static_cast<char>(cpu % 10), '\0'};
    early_debug_print(dst_str);
    early_debug_print("\n");

    // 等待完成 (如果需要)
    if (wait) {
        u32 timeout_iterations = 100000; // 简化超时机制
        u32 iterations = 0;

        while (!call_data->completed && iterations < timeout_iterations) {
            arch::cpu_yield();
            iterations++;
        }

        if (!call_data->completed) {
            return IpiResult::Timeout;
        }
    }

    return IpiResult::Success;
}

// === SGI中断处理函数实现 ===

void HardwareInterProcessorInterrupt::handle_ping_sgi(InterruptId irq, void* context) noexcept {
    auto* ipi_manager = static_cast<HardwareInterProcessorInterrupt*>(context);
    u32 current_cpu = get_current_cpu_id();

    early_debug_print("🏓 CPU");
    char cpu_str[2] = {'0' + static_cast<char>(current_cpu % 10), '\0'};
    early_debug_print(cpu_str);
    early_debug_print(" 收到Ping SGI\n");

    // 更新接收统计
    ipi_manager->sgi_receive_counts_[static_cast<u8>(IpiSgiId::Ping)].get_cpu(current_cpu)++;
    (void)ipi_manager->total_ipis_received_.fetch_add(1, containers::MemoryOrder::Relaxed);
}

void HardwareInterProcessorInterrupt::handle_reschedule_sgi(InterruptId irq, void* context) noexcept {
    auto* ipi_manager = static_cast<HardwareInterProcessorInterrupt*>(context);
    u32 current_cpu = get_current_cpu_id();

    early_debug_print("🔄 CPU");
    char cpu_str[2] = {'0' + static_cast<char>(current_cpu % 10), '\0'};
    early_debug_print(cpu_str);
    early_debug_print(" 收到Reschedule SGI\n");

    // TODO: 触发调度器重新调度
    // scheduler->reschedule(current_cpu);

    ipi_manager->sgi_receive_counts_[static_cast<u8>(IpiSgiId::Reschedule)].get_cpu(current_cpu)++;
    (void)ipi_manager->total_ipis_received_.fetch_add(1, containers::MemoryOrder::Relaxed);
}

void HardwareInterProcessorInterrupt::handle_call_function_sgi(InterruptId irq, void* context) noexcept {
    auto* ipi_manager = static_cast<HardwareInterProcessorInterrupt*>(context);
    u32 current_cpu = get_current_cpu_id();

    early_debug_print("⚡ CPU");
    char cpu_str[2] = {'0' + static_cast<char>(current_cpu % 10), '\0'};
    early_debug_print(cpu_str);
    early_debug_print(" 处理CallFunction SGI\n");

    // 处理当前CPU队列中的所有函数调用
    auto& queue = ipi_manager->call_queues_.get_cpu(current_cpu);
    IpiCallData* call_data;
    u32 processed_count = 0;

    while (queue.dequeue(call_data) && processed_count < 100) { // 限制处理数量防止饿死
        if (call_data && call_data->function) {
            call_data->started = true;

            // 🔥 关键：在目标CPU上执行函数
            call_data->function(call_data->data);

            call_data->completed = true;
        }

        if (call_data) {
            delete call_data;
        }
        processed_count++;
    }

    if (processed_count > 0) {
        early_debug_print("✅ 执行了");
        char count_str[2] = {'0' + static_cast<char>(processed_count % 10), '\0'};
        early_debug_print(count_str);
        early_debug_print("个函数调用\n");
    }

    ipi_manager->sgi_receive_counts_[static_cast<u8>(IpiSgiId::CallFunction)].get_cpu(current_cpu)++;
    (void)ipi_manager->total_ipis_received_.fetch_add(1, containers::MemoryOrder::Relaxed);
}

void HardwareInterProcessorInterrupt::handle_wakeup_sgi(InterruptId irq, void* context) noexcept {
    auto* ipi_manager = static_cast<HardwareInterProcessorInterrupt*>(context);
    u32 current_cpu = get_current_cpu_id();

    early_debug_print("⏰ CPU");
    char cpu_str[2] = {'0' + static_cast<char>(current_cpu % 10), '\0'};
    early_debug_print(cpu_str);
    early_debug_print(" 被唤醒\n");

    // TODO: 唤醒idle CPU的逻辑
    // idle_scheduler->wakeup(current_cpu);

    ipi_manager->sgi_receive_counts_[static_cast<u8>(IpiSgiId::WakeUp)].get_cpu(current_cpu)++;
    (void)ipi_manager->total_ipis_received_.fetch_add(1, containers::MemoryOrder::Relaxed);
}

void HardwareInterProcessorInterrupt::handle_stop_sgi(InterruptId irq, void* context) noexcept {
    auto* ipi_manager = static_cast<HardwareInterProcessorInterrupt*>(context);
    u32 current_cpu = get_current_cpu_id();

    early_debug_print("🛑 CPU");
    char cpu_str[2] = {'0' + static_cast<char>(current_cpu % 10), '\0'};
    early_debug_print(cpu_str);
    early_debug_print(" 收到Stop信号\n");

    ipi_manager->sgi_receive_counts_[static_cast<u8>(IpiSgiId::Stop)].get_cpu(current_cpu)++;
    (void)ipi_manager->total_ipis_received_.fetch_add(1, containers::MemoryOrder::Relaxed);
}

void HardwareInterProcessorInterrupt::handle_timer_sgi(InterruptId irq, void* context) noexcept {
    // 暂未实现
}

void HardwareInterProcessorInterrupt::handle_debug_sgi(InterruptId irq, void* context) noexcept {
    // 暂未实现
}

// === 内部辅助函数实现 ===

VoidResult HardwareInterProcessorInterrupt::send_hardware_sgi(IpiSgiId sgi_id, u32 target_cpu_mask) noexcept {
    if (!gic_) {
        return VoidResult{ErrorCode::NotInitialized};
    }

    // 🔥 关键：调用GIC硬件SGI发送函数
    InterruptId sgi_interrupt_id = static_cast<InterruptId>(sgi_id);
    return gic_->send_sgi(sgi_interrupt_id, target_cpu_mask);
}

bool HardwareInterProcessorInterrupt::is_valid_cpu_id(u32 cpu_id) const noexcept {
    return cpu_id < max_cpus_;
}

u64 HardwareInterProcessorInterrupt::generate_sequence() noexcept {
    return message_sequence_++;
}

u32 HardwareInterProcessorInterrupt::get_current_cpu_id() noexcept {
    // 使用与GIC相同的方法获取CPU ID
    return GenericInterruptController::get_current_cpu_id();
}

u64 HardwareInterProcessorInterrupt::get_timestamp() noexcept {
    // 简化实现：使用基础计数器
    static u64 counter = 0;
    return ++counter;
}

void HardwareInterProcessorInterrupt::cleanup() noexcept {
    // 清理Per-CPU函数调用队列
    for (u32 cpu = 0; cpu < max_cpus_; ++cpu) {
        auto& queue = call_queues_.get_cpu(cpu);
        IpiCallData* call_data;
        while (queue.dequeue(call_data)) {
            delete call_data;
        }
    }
}

// === 统计信息实现 ===

IpiStatistics HardwareInterProcessorInterrupt::get_statistics() const noexcept {
    IpiStatistics stats{};

    stats.total_sent = total_ipis_sent_.load(containers::MemoryOrder::Relaxed);
    stats.total_received = total_ipis_received_.load(containers::MemoryOrder::Relaxed);
    stats.hardware_errors = hardware_errors_.load(containers::MemoryOrder::Relaxed);
    stats.timeouts = 0; // 暂不实现

    // 累计特定类型的统计
    stats.ping_count = 0;
    stats.reschedule_count = 0;
    stats.call_function_count = 0;

    for (u32 cpu = 0; cpu < max_cpus_; ++cpu) {
        stats.ping_count += sgi_send_counts_[static_cast<u8>(IpiSgiId::Ping)].get_cpu(cpu);
        stats.reschedule_count += sgi_send_counts_[static_cast<u8>(IpiSgiId::Reschedule)].get_cpu(cpu);
        stats.call_function_count += sgi_send_counts_[static_cast<u8>(IpiSgiId::CallFunction)].get_cpu(cpu);
    }

    stats.active_cpus = max_cpus_; // 简化
    stats.max_cpus = max_cpus_;
    stats.initialized = initialized_;

    return stats;
}

// === 全局函数实现 ===

VoidResult initialize_hardware_ipi_system(GenericInterruptController* gic, u32 max_cpus) noexcept {
    if (g_hardware_ipi_manager != nullptr) {
        return VoidResult{ErrorCode::AlreadyExists};
    }

    g_hardware_ipi_manager = new HardwareInterProcessorInterrupt();
    if (g_hardware_ipi_manager == nullptr) {
        return VoidResult{ErrorCode::OutOfMemory};
    }

    auto init_result = g_hardware_ipi_manager->initialize(gic, max_cpus);
    if (!init_result) {
        delete g_hardware_ipi_manager;
        g_hardware_ipi_manager = nullptr;
        return init_result;
    }

    early_debug_print("🌟 全局硬件IPI系统已启动\n");
    return VoidResult{};
}

void shutdown_hardware_ipi_system() noexcept {
    if (g_hardware_ipi_manager != nullptr) {
        delete g_hardware_ipi_manager;
        g_hardware_ipi_manager = nullptr;
        early_debug_print("🌟 全局硬件IPI系统已关闭\n");
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
        default: return "SGI-Reserved";
    }
}

} // namespace moss::kernel::interrupts