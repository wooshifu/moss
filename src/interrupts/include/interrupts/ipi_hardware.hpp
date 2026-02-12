#pragma once

// MOSS内核硬件IPI系统 - 基于ARM64 GIC SGI实现
// 替换概念验证系统，提供真正的CPU间中断通信

#include "../../../include/types.hpp"
#include "../../../include/result.hpp"
#include "../../containers/include/containers/lockfree_queue.hpp"
#include "../../containers/include/containers/per_cpu_data.hpp"
#include "../../containers/include/containers/atomic_types.hpp"
#include "gic.hpp"

namespace moss::kernel::interrupts {

// IPI专用SGI中断号分配 (基于Linux内核设计)
enum class IpiSgiId : u8 {
    // 核心IPI功能 (0-3: Linux兼容)
    Reschedule = 0,      // 触发目标CPU重新调度
    CallFunction = 1,    // 在目标CPU上执行函数
    CallFunctionSingle = 2,  // 单CPU函数调用(优化版)
    Timer = 3,           // 定时器同步

    // MOSS扩展 (4-7: 自定义功能)
    Ping = 4,            // Ping测试和心跳检测
    WakeUp = 5,          // 唤醒idle CPU
    Stop = 6,            // CPU停止/暂停
    Debug = 7,           // 调试和性能分析

    // 保留 (8-15: 未来扩展)
    Reserved8 = 8,
    Reserved9 = 9,
    Reserved10 = 10,
    Reserved11 = 11,
    Reserved12 = 12,
    Reserved13 = 13,
    Reserved14 = 14,
    Reserved15 = 15
};

// IPI消息类型 (复用简化版本的定义)
enum class IpiType : u8 {
    Ping = 0,
    Reschedule = 1,
    CallFunction = 2,
    Stop = 3,
    WakeUp = 4,
    Timer = 5,
    Debug = 6
};

// IPI结果代码
enum class IpiResult : u8 {
    Success = 0,
    InvalidCpu = 1,
    InvalidType = 2,
    NotInitialized = 3,
    HardwareError = 4,
    QueueFull = 5,
    Timeout = 6
};

// IPI消息结构
struct IpiMessage {
    IpiType type;
    u32 source_cpu;
    u32 target_cpu;
    u64 sequence;
    u64 timestamp;
    void* data;

    IpiMessage() noexcept
        : type(IpiType::Ping), source_cpu(0), target_cpu(0),
          sequence(0), timestamp(0), data(nullptr) {}
};

// IPI统计信息
struct IpiStatistics {
    u64 total_sent;
    u64 total_received;
    u64 hardware_errors;
    u64 timeouts;
    u64 ping_count;
    u64 reschedule_count;
    u64 call_function_count;
    u32 active_cpus;
    u32 max_cpus;
    bool initialized;
};

// 跨CPU函数调用数据结构
struct IpiCallData {
    void (*function)(void*);
    void* data;
    volatile bool completed;
    volatile bool started;
    u64 sequence;
    u64 submit_time;
    u32 source_cpu;
    u32 target_cpu;

    IpiCallData() noexcept
        : function(nullptr), data(nullptr), completed(false), started(false),
          sequence(0), submit_time(0), source_cpu(0), target_cpu(0) {}

    IpiCallData(void (*func)(void*), void* ptr, u64 seq, u32 src, u32 tgt) noexcept
        : function(func), data(ptr), completed(false), started(false),
          sequence(seq), submit_time(0), source_cpu(src), target_cpu(tgt) {}
};

// IPI类型到SGI ID的映射函数
constexpr IpiSgiId ipi_type_to_sgi(IpiType type) noexcept {
    switch (type) {
        case IpiType::Ping: return IpiSgiId::Ping;
        case IpiType::Reschedule: return IpiSgiId::Reschedule;
        case IpiType::CallFunction: return IpiSgiId::CallFunction;
        case IpiType::Stop: return IpiSgiId::Stop;
        case IpiType::WakeUp: return IpiSgiId::WakeUp;
        case IpiType::Timer: return IpiSgiId::Timer;
        case IpiType::Debug: return IpiSgiId::Debug;
        default: return IpiSgiId::Ping; // 默认fallback
    }
}

// SGI ID到IPI类型的逆向映射
constexpr IpiType sgi_to_ipi_type(IpiSgiId sgi) noexcept {
    switch (sgi) {
        case IpiSgiId::Ping: return IpiType::Ping;
        case IpiSgiId::Reschedule: return IpiType::Reschedule;
        case IpiSgiId::CallFunction: return IpiType::CallFunction;
        case IpiSgiId::CallFunctionSingle: return IpiType::CallFunction; // 映射到CallFunction
        case IpiSgiId::Stop: return IpiType::Stop;
        case IpiSgiId::WakeUp: return IpiType::WakeUp;
        case IpiSgiId::Timer: return IpiType::Timer;
        case IpiSgiId::Debug: return IpiType::Debug;
        case IpiSgiId::Reserved8:
        case IpiSgiId::Reserved9:
        case IpiSgiId::Reserved10:
        case IpiSgiId::Reserved11:
        case IpiSgiId::Reserved12:
        case IpiSgiId::Reserved13:
        case IpiSgiId::Reserved14:
        case IpiSgiId::Reserved15:
            return IpiType::Ping; // 保留值映射到Ping
        default:
            return IpiType::Ping; // 默认fallback
    }
}

// 硬件IPI管理器主类
class HardwareInterProcessorInterrupt {
private:
    // 核心组件
    GenericInterruptController* gic_;  // GIC驱动实例
    bool initialized_;
    u32 max_cpus_;
    u64 message_sequence_;

    // IPI统计 - Per-SGI ID
    containers::PerCpuData<u64> sgi_send_counts_[16];
    containers::PerCpuData<u64> sgi_receive_counts_[16];
    containers::AtomicCounter<u64> total_ipis_sent_;
    containers::AtomicCounter<u64> total_ipis_received_;
    containers::AtomicCounter<u64> hardware_errors_;

    // 函数调用支持
    containers::PerCpuData<containers::MPSCQueue<IpiCallData*>> call_queues_;
    containers::AtomicCounter<u64> call_sequence_;

public:
    HardwareInterProcessorInterrupt() noexcept
        : gic_(nullptr), initialized_(false), max_cpus_(0), message_sequence_(1),
          total_ipis_sent_(0), total_ipis_received_(0), hardware_errors_(0),
          call_sequence_(1000) {}

    ~HardwareInterProcessorInterrupt() noexcept;

    // 禁用拷贝和移动
    NON_COPYABLE_NON_MOVABLE(HardwareInterProcessorInterrupt)

    // === 核心初始化和控制接口 ===

    /// 初始化硬件IPI系统
    /// @param gic GIC驱动实例指针
    /// @param max_cpus 系统最大CPU数量
    /// @return 初始化结果
    [[nodiscard]] VoidResult initialize(GenericInterruptController* gic, u32 max_cpus) noexcept;

    /// 关闭IPI系统
    void shutdown() noexcept;

    /// 检查系统是否已初始化
    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

    // === Linux风格IPI发送接口 ===

    /// 发送IPI到指定CPU
    /// @param target_cpu 目标CPU ID
    /// @param type IPI类型
    /// @param data 可选的数据指针
    /// @return 发送结果
    [[nodiscard]] IpiResult send_ipi(u32 target_cpu, IpiType type, void* data = nullptr) noexcept;

    /// 向目标CPU发送Ping IPI
    /// @param target_cpu 目标CPU ID
    /// @return 发送结果
    [[nodiscard]] IpiResult ping_cpu(u32 target_cpu) noexcept;

    /// 向多个CPU发送Ping IPI
    /// @param cpu_mask 目标CPU掩码
    /// @return 发送结果
    [[nodiscard]] IpiResult ping_cpus(u32 cpu_mask) noexcept;

    // === Linux兼容的跨CPU函数调用 ===

    /// 在指定CPU上执行函数 (Linux smp_call_function_single)
    /// @param cpu 目标CPU ID
    /// @param func 要执行的函数
    /// @param data 函数参数
    /// @param wait 是否等待完成
    /// @return 调用结果
    [[nodiscard]] IpiResult smp_call_function_single(u32 cpu, void (*func)(void*),
                                                    void* data, bool wait = true) noexcept;

    /// 在多个CPU上执行函数 (Linux smp_call_function_many)
    /// @param cpu_mask 目标CPU掩码
    /// @param func 要执行的函数
    /// @param data 函数参数
    /// @param wait 是否等待完成
    /// @return 调用结果
    [[nodiscard]] IpiResult smp_call_function_many(u32 cpu_mask, void (*func)(void*),
                                                  void* data, bool wait = true) noexcept;

    // === 调度器协作接口 ===

    /// 请求目标CPU重新调度
    /// @param target_cpu 目标CPU ID
    /// @return 发送结果
    [[nodiscard]] IpiResult request_reschedule(u32 target_cpu) noexcept;

    /// 唤醒idle CPU
    /// @param target_cpu 目标CPU ID
    /// @return 发送结果
    [[nodiscard]] IpiResult wakeup_cpu(u32 target_cpu) noexcept;

    // === 统计和监控接口 ===

    /// 获取IPI系统统计信息
    /// @return 统计信息结构
    [[nodiscard]] IpiStatistics get_statistics() const noexcept;

    /// 获取指定CPU的统计信息
    /// @param cpu_id CPU ID
    /// @return Per-CPU统计信息
    [[nodiscard]] Result<IpiStatistics> get_cpu_statistics(u32 cpu_id) const noexcept;

    /// 重置统计信息
    void reset_statistics() noexcept;

    // === 调试和测试接口 ===

    /// 执行IPI系统自测试
    /// @return 测试结果
    [[nodiscard]] VoidResult self_test() noexcept;

    /// 性能基准测试
    /// @param iterations 测试迭代次数
    /// @return 测试结果
    [[nodiscard]] VoidResult benchmark_performance(u32 iterations = 1000) noexcept;

    /// 获取当前CPU ID
    [[nodiscard]] static u32 get_current_cpu_id() noexcept;

private:
    // === SGI中断处理函数 (静态函数，注册到GIC) ===

    static void handle_ping_sgi(InterruptId irq, void* context) noexcept;
    static void handle_reschedule_sgi(InterruptId irq, void* context) noexcept;
    static void handle_call_function_sgi(InterruptId irq, void* context) noexcept;
    static void handle_stop_sgi(InterruptId irq, void* context) noexcept;
    static void handle_wakeup_sgi(InterruptId irq, void* context) noexcept;
    static void handle_timer_sgi(InterruptId irq, void* context) noexcept;
    static void handle_debug_sgi(InterruptId irq, void* context) noexcept;

    // === 内部辅助函数 ===

    /// 发送硬件SGI
    /// @param sgi_id SGI中断号
    /// @param target_cpu_mask 目标CPU掩码
    /// @return 发送结果
    [[nodiscard]] VoidResult send_hardware_sgi(IpiSgiId sgi_id, u32 target_cpu_mask) noexcept;

    /// 验证CPU ID有效性
    /// @param cpu_id CPU ID
    /// @return 是否有效
    [[nodiscard]] bool is_valid_cpu_id(u32 cpu_id) const noexcept;

    /// 生成唯一序列号
    /// @return 序列号
    [[nodiscard]] u64 generate_sequence() noexcept;

    /// 获取时间戳 (简化版本)
    /// @return 时间戳
    [[nodiscard]] static u64 get_timestamp() noexcept;

    /// 清理资源
    void cleanup() noexcept;
};

// === 全局实例和便利接口 ===

/// 全局硬件IPI管理器实例
extern HardwareInterProcessorInterrupt* g_hardware_ipi_manager;

/// 初始化硬件IPI系统 (全局函数)
/// @param gic GIC驱动实例
/// @param max_cpus 最大CPU数量
/// @return 初始化结果
[[nodiscard]] VoidResult initialize_hardware_ipi_system(GenericInterruptController* gic, u32 max_cpus) noexcept;

/// 关闭硬件IPI系统
void shutdown_hardware_ipi_system() noexcept;

// === 调试工具函数 ===

/// IPI类型转字符串
/// @param type IPI类型
/// @return 类型名称字符串
[[nodiscard]] const char* ipi_type_to_string(IpiType type) noexcept;

/// IPI结果转字符串
/// @param result IPI结果
/// @return 结果名称字符串
[[nodiscard]] const char* ipi_result_to_string(IpiResult result) noexcept;

/// SGI ID转字符串
/// @param sgi SGI ID
/// @return SGI名称字符串
[[nodiscard]] const char* sgi_id_to_string(IpiSgiId sgi) noexcept;

} // namespace moss::kernel::interrupts
