// 简化的MOSS内核IPI(CPU间中断)机制实现
// 专注于Ping IPI垂直切片验证

#pragma once

#include "types.hpp"
#include "result.hpp"

namespace moss::kernel::interrupts {

// IPI消息类型定义（简化版）
enum class IpiType : u8 {
    Ping = 3           // 简单的ping测试（调试用）
};

// IPI结果状态
enum class IpiResult : u8 {
    Success = 0,
    InvalidCpu = 2,
    NotInitialized = 3
};

// 简化的IPI消息结构
struct IpiMessage {
    IpiType type;
    u32 source_cpu;
    u32 target_cpu;
    u64 sequence;
};

// 简化的IPI子系统主类
class SimpleInterProcessorInterrupt {
public:
    static constexpr u32 MAX_CPUS = 8;

    SimpleInterProcessorInterrupt() noexcept = default;
    ~SimpleInterProcessorInterrupt() noexcept = default;

    // 禁用拷贝和移动
    SimpleInterProcessorInterrupt(const SimpleInterProcessorInterrupt&) = delete;
    SimpleInterProcessorInterrupt& operator=(const SimpleInterProcessorInterrupt&) = delete;

    // === 核心接口 ===

    // 初始化IPI子系统
    VoidResult initialize(u32 max_cpus) noexcept;

    // 发送IPI到指定CPU
    IpiResult send_ipi(u32 target_cpu, IpiType type) noexcept;

    // CPU ping测试
    IpiResult ping_cpu(u32 target_cpu) noexcept;

    // 自测试功能
    VoidResult self_test() noexcept;

    // 系统信息
    struct SystemInfo {
        bool initialized;
        u32 max_cpus;
        u64 total_pings_sent;
    };
    SystemInfo get_system_info() const noexcept;

private:
    bool initialized_ = false;
    u32 max_cpus_ = 0;
    u64 message_sequence_ = 0;
    u64 total_pings_sent_ = 0;

    bool is_valid_cpu_id(u32 cpu_id) const noexcept;
    u32 get_current_cpu_id() const noexcept;
};

// 全局简化IPI管理器实例
extern SimpleInterProcessorInterrupt* g_simple_ipi_manager;

// === 便利函数 ===

// 初始化全局IPI管理器
VoidResult initialize_simple_ipi_system(u32 max_cpus) noexcept;

// 关闭全局IPI管理器
void shutdown_simple_ipi_system() noexcept;

// 快速访问接口
inline IpiResult simple_ipi_ping(u32 target_cpu) noexcept {
    return g_simple_ipi_manager ? g_simple_ipi_manager->ping_cpu(target_cpu) : IpiResult::NotInitialized;
}

// === 调试工具函数 ===

// 将IpiType转换为字符串
const char* ipi_type_to_string(IpiType type) noexcept;

// 将IpiResult转换为字符串
const char* ipi_result_to_string(IpiResult result) noexcept;

} // namespace moss::kernel::interrupts
