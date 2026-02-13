// MOSS内核IPI(CPU间中断)机制实现
// 基于Linux内核IPI架构设计的轻量级CPU间通信系统

#pragma once

#include "containers/atomic_types.hpp"
#include "containers/lockfree_queue.hpp"
#include "containers/per_cpu_data.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

namespace moss::kernel::interrupts {

// IPI消息类型定义
enum class IpiType : u8 {
    Reschedule = 0,    // 触发目标CPU重新调度
    CallFunction = 1,  // 在目标CPU上执行函数
    Stop = 2,          // 停止/暂停目标CPU
    Ping = 3           // 简单的ping测试（调试用）
};

// IPI消息结构
struct IpiMessage {
    IpiType type;
    u32 source_cpu;
    u32 target_cpu;
    void* data;
    u64 sequence;
};

// Per-CPU IPI统计信息
struct IpiStatistics {
    // 发送统计
    containers::AtomicU64 sent_total{0};
    containers::AtomicU64 sent_reschedule{0};
    containers::AtomicU64 sent_call_function{0};
    containers::AtomicU64 sent_stop{0};
    containers::AtomicU64 sent_ping{0};

    // 接收统计
    containers::AtomicU64 received_total{0};
    containers::AtomicU64 received_reschedule{0};
    containers::AtomicU64 received_call_function{0};
    containers::AtomicU64 received_stop{0};
    containers::AtomicU64 received_ping{0};

    // 性能统计
    containers::AtomicU64 queue_full_errors{0};
    containers::AtomicU64 invalid_cpu_errors{0};
    containers::AtomicU64 total_latency_ns{0};

    void reset() noexcept;
    void print_statistics(u32 cpu_id) const noexcept;
};

// IPI结果状态
enum class IpiResult : u8 {
    Success = 0,
    QueueFull = 1,
    InvalidCpu = 2,
    NotInitialized = 3,
    HardwareError = 4
};

// IPI子系统主类
class InterProcessorInterrupt {
public:
    static constexpr u32 IPI_QUEUE_SIZE = 64;
    static constexpr u32 MAX_CPUS = 8;

    InterProcessorInterrupt() noexcept = default;
    ~InterProcessorInterrupt() noexcept = default;

    // 禁用拷贝和移动
    InterProcessorInterrupt(const InterProcessorInterrupt&) = delete;
    InterProcessorInterrupt& operator=(const InterProcessorInterrupt&) = delete;
    InterProcessorInterrupt(InterProcessorInterrupt&&) = delete;
    InterProcessorInterrupt& operator=(InterProcessorInterrupt&&) = delete;

    // === 核心接口 ===

    // 初始化IPI子系统
    VoidResult initialize(u32 max_cpus) noexcept;

    // 关闭IPI子系统
    void shutdown() noexcept;

    // 发送IPI到指定CPU
    IpiResult send_ipi(u32 target_cpu, IpiType type, void* data = nullptr) noexcept;

    // 处理接收到的IPI（由中断处理器调用）
    void handle_ipi_interrupt(u32 cpu_id) noexcept;

    // === Linux风格的便利接口 ===

    // 请求目标CPU重新调度
    IpiResult request_reschedule(u32 target_cpu) noexcept;

    // Linux风格的跨CPU函数调用
    IpiResult smp_call_function_single(u32 cpu, void (*func)(void*), void* data) noexcept;

    // CPU ping测试
    IpiResult ping_cpu(u32 target_cpu) noexcept;

    // 停止目标CPU（调试用）
    IpiResult stop_cpu(u32 target_cpu) noexcept;

    // === 统计和监控接口 ===

    // 获取Per-CPU统计信息
    const IpiStatistics* get_statistics(u32 cpu_id) const noexcept;

    // 打印所有CPU的统计信息
    void print_all_statistics() const noexcept;

    // 重置统计信息
    void reset_statistics() noexcept;

    // 获取队列使用情况
    struct QueueUsage {
        u32 size;
        u32 capacity;
        float usage_percentage;
    };
    QueueUsage get_queue_usage(u32 cpu_id) const noexcept;

    // === 调试和测试接口 ===

    // 自测试功能
    VoidResult self_test() noexcept;

    // 健康状态检查
    bool is_healthy() const noexcept;

    // 获取子系统信息
    struct SystemInfo {
        bool initialized;
        u32 max_cpus;
        u64 total_messages_sent;
        u64 total_messages_received;
        u32 active_cpus;
    };
    SystemInfo get_system_info() const noexcept;

private:
    // === 内部状态 ===
    bool initialized_ = false;
    u32 max_cpus_ = 0;

    // Per-CPU消息队列
    containers::PerCpuData<containers::SPSCQueue<IpiMessage, IPI_QUEUE_SIZE>> message_queues_;

    // Per-CPU统计信息
    containers::PerCpuData<IpiStatistics> statistics_;

    // 全局消息序列号
    containers::AtomicU64 message_sequence_{0};

    // === 内部辅助函数 ===

    // 验证CPU ID有效性
    bool is_valid_cpu_id(u32 cpu_id) const noexcept;

    // 处理特定类型的IPI消息
    void handle_reschedule_ipi(const IpiMessage& msg) noexcept;
    void handle_call_function_ipi(const IpiMessage& msg) noexcept;
    void handle_stop_ipi(const IpiMessage& msg) noexcept;
    void handle_ping_ipi(const IpiMessage& msg) noexcept;

    // 发送底层SGI中断
    IpiResult send_sgi_interrupt(u32 target_cpu, IpiType type) noexcept;

    // 更新统计信息
    void update_send_statistics(u32 target_cpu, IpiType type) noexcept;
    void update_receive_statistics(u32 cpu_id, IpiType type) noexcept;
    void update_error_statistics(u32 cpu_id, IpiResult error) noexcept;
};

// 全局IPI管理器实例
extern InterProcessorInterrupt* g_ipi_manager;

// === 便利函数 ===

// 初始化全局IPI管理器
VoidResult initialize_ipi_system(u32 max_cpus) noexcept;

// 关闭全局IPI管理器
void shutdown_ipi_system() noexcept;

// 快速访问接口
inline IpiResult ipi_ping(u32 target_cpu) noexcept {
    return g_ipi_manager ? g_ipi_manager->ping_cpu(target_cpu) : IpiResult::NotInitialized;
}

inline IpiResult ipi_reschedule(u32 target_cpu) noexcept {
    return g_ipi_manager ? g_ipi_manager->request_reschedule(target_cpu) : IpiResult::NotInitialized;
}

inline IpiResult ipi_call_function(u32 cpu, void (*func)(void*), void* data) noexcept {
    return g_ipi_manager ? g_ipi_manager->smp_call_function_single(cpu, func, data) : IpiResult::NotInitialized;
}

// === 调试工具函数 ===

// 将IpiType转换为字符串
const char* ipi_type_to_string(IpiType type) noexcept;

// 将IpiResult转换为字符串
const char* ipi_result_to_string(IpiResult result) noexcept;

// 格式化IPI消息信息
void print_ipi_message_info(const IpiMessage& msg) noexcept;

} // namespace moss::kernel::interrupts