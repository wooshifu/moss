#pragma once

#include "process.hpp"
#include "containers/per_cpu_data.hpp"
#include "types.hpp"

namespace moss::kernel::process {

/// Linux风格的idle任务类
/// 每个CPU都有一个独立的idle任务，当CPU没有其他任务可运行时执行
class IdleTask : public Thread {
public:
    /// 构造函数
    /// @param cpu_id 此idle任务绑定的CPU ID
    explicit IdleTask(u32 cpu_id) noexcept;

    /// 析构函数
    ~IdleTask() noexcept = default;

    /// idle任务主循环 - 永不返回
    /// 实现Linux风格的idle行为：WFI指令 + 电源管理
    [[noreturn]] void run() noexcept;

    /// 获取绑定的CPU ID
    /// @return CPU ID
    u32 get_cpu_id() const noexcept { return cpu_id_; }

    /// 获取idle时间统计
    /// @return 累计idle时间(纳秒)
    u64 get_idle_time_ns() const noexcept { return idle_time_ns_; }

    /// 重置idle时间统计
    void reset_idle_time() noexcept { idle_time_ns_ = 0; }

private:
    u32 cpu_id_;                      ///< 绑定的CPU ID
    u64 idle_time_ns_;                ///< 累计idle时间
    [[maybe_unused]] u64 last_idle_start_;  ///< 上次进入idle的时间戳
};

/// Linux风格的do_idle函数
/// 这是每个CPU在没有任务可运行时的核心循环
/// @param cpu_id 当前CPU ID
[[noreturn]] void do_idle(u32 cpu_id) noexcept;

/// 创建指定CPU的idle任务
/// @param cpu_id 目标CPU ID
/// @return 创建的idle任务指针，失败返回nullptr
IdleTask* create_idle_task(u32 cpu_id) noexcept;

/// 全局idle任务管理
/// 每个CPU都有自己的idle任务实例
extern moss::kernel::containers::PerCpuData<IdleTask*> g_idle_tasks;

/// 获取指定CPU的idle任务
/// @param cpu_id CPU ID
/// @return idle任务指针，如果CPU无效或未初始化返回nullptr
inline IdleTask* get_idle_task(u32 cpu_id) noexcept {
    if (cpu_id >= moss::kernel::MAX_CPUS) {
        return nullptr;
    }
    return g_idle_tasks.get_cpu(cpu_id);
}

/// 设置指定CPU的idle任务
/// @param cpu_id CPU ID
/// @param idle_task idle任务指针
/// @return 成功返回true
inline bool set_idle_task(u32 cpu_id, IdleTask* idle_task) noexcept {
    if (cpu_id >= moss::kernel::MAX_CPUS) {
        return false;
    }
    g_idle_tasks.get_cpu(cpu_id) = idle_task;
    return true;
}

/// 检查指定CPU是否正在运行idle任务
/// @param cpu_id CPU ID
/// @return true如果CPU正在运行idle任务
bool is_cpu_idle(u32 cpu_id) noexcept;

/// 从idle状态唤醒CPU(用于IPI机制)
/// @param cpu_id 目标CPU ID
void wakeup_idle_cpu(u32 cpu_id) noexcept;

} // namespace moss::kernel::process
