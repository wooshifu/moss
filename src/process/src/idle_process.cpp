#include "process/idle_process.hpp"
#include "types.hpp"

namespace moss::kernel::process {

// 全局Per-CPU idle任务数组
moss::kernel::containers::PerCpuData<IdleTask*> g_idle_tasks{nullptr};

// 标记哪些CPU正在运行idle
static moss::kernel::containers::PerCpuData<bool> g_cpu_idle_status{false};

IdleTask::IdleTask(u32 cpu_id) noexcept
    : Thread(ThreadId{1000000 + cpu_id}, ProcessId{0})
    , cpu_id_(cpu_id)
    , idle_time_ns_(0)
    , last_idle_start_(0) {

    // idle任务的调度参数设置
    this->se.nice = 19;        // 最低优先级
    this->se.weight = 15;      // 最小权重
    this->se.vruntime = 0;     // 虚拟运行时间为0

    // idle任务绑定到特定CPU
    this->cpu = cpu_id;
    this->wake_cpu = cpu_id;

    // idle任务状态为可运行
    this->state = ProcessState::Ready;
    this->sched_class = SchedClass::Idle;
}

[[noreturn]] void IdleTask::run() noexcept {
    // 简单的调试输出 - 后续可以集成更完整的日志系统
    // TODO: 添加调试输出当日志系统可用时

    // 进入Linux风格的do_idle循环
    do_idle(cpu_id_);
}

[[noreturn]] void do_idle(u32 cpu_id) noexcept {
    // 标记当前CPU进入idle状态
    g_cpu_idle_status.get_cpu(cpu_id) = true;

    while (true) {
        // 🔧 Linux风格的idle循环核心机制

        // 1. 检查是否有待处理的重调度请求
        // TODO: 实现need_resched检查机制

        // 2. 检查是否有中断等待处理
        // TODO: 实现中断检查机制

        // 3. 电源管理：进入低功耗状态
        // 在ARM64上使用WFI(Wait For Interrupt)指令
        asm volatile("dsb sy");  // 数据同步屏障
        asm volatile("wfi");     // 等待中断 - CPU进入低功耗状态
        asm volatile("isb");     // 指令同步屏障

        // 4. WFI被中断唤醒后，检查是否需要退出idle
        // 这里我们需要检查调度器是否有可运行的任务

        // 5. 短暂的活动检测
        // 给其他子系统一些处理时间
        for (volatile u32 i = 0; i < 100; i = i + 1) {
            asm volatile("nop");
        }

        // TODO: 实现真正的idle退出条件检查
        // 当前暂时使用简单循环，后续集成调度器后改进
    }
}

IdleTask* create_idle_task(u32 cpu_id) noexcept {
    if (cpu_id >= moss::kernel::MAX_CPUS) {
        return nullptr;
    }

    // 检查是否已经创建过
    if (g_idle_tasks.get_cpu(cpu_id) != nullptr) {
        return g_idle_tasks.get_cpu(cpu_id);
    }

    // 为idle任务分配内存
    // 使用标准的new操作符，后续可以集成更完整的内存分配器
    auto* idle_task = new IdleTask(cpu_id);
    if (idle_task == nullptr) {
        // TODO: 添加错误日志当日志系统可用时
        return nullptr;
    }

    // 存储到Per-CPU数组
    g_idle_tasks.get_cpu(cpu_id) = idle_task;

    // TODO: 添加成功日志当日志系统可用时
    return idle_task;
}

bool is_cpu_idle(u32 cpu_id) noexcept {
    if (cpu_id >= moss::kernel::MAX_CPUS) {
        return false;
    }
    return g_cpu_idle_status.get_cpu(cpu_id);
}

void wakeup_idle_cpu(u32 cpu_id) noexcept {
    if (cpu_id >= moss::kernel::MAX_CPUS) {
        return;
    }

    // 标记CPU不再idle
    g_cpu_idle_status.get_cpu(cpu_id) = false;

    // TODO: 发送IPI中断唤醒目标CPU
    // 这将在后续IPI机制实现中完成
}

} // namespace moss::kernel::process
