#pragma once

// MOSS内核简化硬件IPI系统 - 基于ARM64 GIC SGI实现
// 避免复杂依赖，专注核心SGI硬件集成功能

#include "../../../include/types.hpp"
#include "../../../include/result.hpp"
#include "gic.hpp"

// 外部调试打印函数
extern "C" void early_debug_print(const char* message) noexcept;

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
};

// IPI消息类型 (简化版本)
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

// 简化的硬件IPI管理器
class SimpleHardwareIpi {
private:
    GenericInterruptController* gic_;  // GIC驱动实例
    bool initialized_;
    u32 max_cpus_;
    u64 total_ipis_sent_;
    u64 message_sequence_;

    // 静态统计数组 (避免动态分配)
    u64 sgi_send_counts_[8];  // 只跟踪前8个SGI
    u64 sgi_receive_counts_[8];

public:
    SimpleHardwareIpi() noexcept
        : gic_(nullptr), initialized_(false), max_cpus_(0),
          total_ipis_sent_(0), message_sequence_(1000) {
        // 初始化统计数组
        for (u32 i = 0; i < 8; ++i) {
            sgi_send_counts_[i] = 0;
            sgi_receive_counts_[i] = 0;
        }
    }

    ~SimpleHardwareIpi() noexcept {
        shutdown();
    }

    // 禁用拷贝和移动
    SimpleHardwareIpi(const SimpleHardwareIpi&) = delete;
    SimpleHardwareIpi& operator=(const SimpleHardwareIpi&) = delete;

    /// 初始化简化硬件IPI系统
    VoidResult initialize(GenericInterruptController* gic, u32 max_cpus) noexcept;

    /// 关闭IPI系统
    void shutdown() noexcept;

    /// 检查系统是否已初始化
    bool is_initialized() const noexcept { return initialized_; }

    /// 发送IPI到指定CPU (核心功能)
    IpiResult send_ipi(u32 target_cpu, IpiType type) noexcept;

    /// 向目标CPU发送Ping IPI
    IpiResult ping_cpu(u32 target_cpu) noexcept;

    /// 向多个CPU发送Ping IPI
    IpiResult ping_cpus(u32 cpu_mask) noexcept;

    /// 请求目标CPU重新调度
    IpiResult request_reschedule(u32 target_cpu) noexcept;

    /// 唤醒idle CPU
    IpiResult wakeup_cpu(u32 target_cpu) noexcept;

    /// 获取统计信息
    struct Statistics {
        u64 total_sent;
        u64 ping_count;
        u64 reschedule_count;
        u32 max_cpus;
        bool initialized;
    };

    Statistics get_statistics() const noexcept;

    /// 执行自测试
    VoidResult self_test() noexcept;

private:
    /// 发送硬件SGI (核心函数)
    VoidResult send_hardware_sgi(IpiSgiId sgi_id, u32 target_cpu_mask) noexcept;

    /// 验证CPU ID有效性
    bool is_valid_cpu_id(u32 cpu_id) const noexcept;

    /// 获取当前CPU ID
    static u32 get_current_cpu_id() noexcept;

    /// 生成序列号
    u64 generate_sequence() noexcept;
};

// === 全局实例和便利接口 ===

/// 全局简化硬件IPI管理器实例
extern SimpleHardwareIpi* g_simple_hardware_ipi;

/// 初始化简化硬件IPI系统 (全局函数)
VoidResult initialize_simple_hardware_ipi(GenericInterruptController* gic, u32 max_cpus) noexcept;

/// 关闭简化硬件IPI系统
void shutdown_simple_hardware_ipi() noexcept;

// === 调试工具函数 ===

/// IPI类型转字符串
const char* ipi_type_to_string(IpiType type) noexcept;

/// IPI结果转字符串
const char* ipi_result_to_string(IpiResult result) noexcept;

/// SGI ID转字符串
const char* sgi_id_to_string(IpiSgiId sgi) noexcept;

} // namespace moss::kernel::interrupts
