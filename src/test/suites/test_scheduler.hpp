#pragma once

// MOSS内核调度器单元测试套件
// 测试CFS调度器的核心逻辑：任务入队、vruntime计算、红黑树操作、调度公平性

#include "../framework/test_framework.hpp"
#include "../framework/test_registry.hpp"
#include "../../process/include/process/cfs_scheduler.hpp"
// No need for cstring in freestanding environment

namespace moss::kernel::test {

// 手动注册函数声明
void register_scheduler_test_suite() noexcept;

// ============================================================================
// 测试用的简化Thread结构 - 只包含调度相关字段
// ============================================================================

struct TestThread {
    ThreadId tid;              // 与Thread布局匹配: 位置0
    ProcessId owner_pid; // 与Thread布局匹配: 位置8

    // CPU上下文 - 简化版本，只保持内存布局匹配
    char context_placeholder[256]; // 占位符，确保后续字段位置正确
    u32 cpu;                    // 当前运行的CPU
    u32 wake_cpu;              // 唤醒时的CPU（占位）

    // 调度相关 - 按真实Thread顺序排列
    process::ProcessState state;     // 线程状态
    process::SchedClass sched_class; // 调度类别（占位）
    process::SchedEntity se;         // CFS调度实体 - 现在在正确位置！

    TestThread() noexcept
        : tid(0), owner_pid(1000), cpu(0), wake_cpu(0),
          state(process::ProcessState::Created),
          sched_class(process::SchedClass::Normal) {
        // 清零占位符
        for (usize i = 0; i < sizeof(context_placeholder); ++i) {
            context_placeholder[i] = 0;
        }

        // 初始化调度实体
        se.vruntime = 0;
        se.nice = 0;
        se.weight = process::CfsParams::nice_to_weight(0);
        se.sum_exec_runtime = 0;
        se.prev_sum_exec_runtime = 0;
        se.load_sum = 0;
        se.load_avg = 0;
        se.util_sum = 0;
        se.util_avg = 0;
    }

    TestThread(ThreadId id, i32 nice_val, u64 initial_vruntime = 0) noexcept
        : tid(id), owner_pid(1000), cpu(0), wake_cpu(0),
          state(process::ProcessState::Ready),
          sched_class(process::SchedClass::Normal) {
        // 清零占位符
        for (usize i = 0; i < sizeof(context_placeholder); ++i) {
            context_placeholder[i] = 0;
        }

        // 初始化调度实体
        se.vruntime = initial_vruntime;
        se.nice = nice_val;
        se.weight = process::CfsParams::nice_to_weight(nice_val);
        se.sum_exec_runtime = 0;
        se.prev_sum_exec_runtime = 0;
        se.load_sum = 0;
        se.load_avg = 0;
        se.util_sum = 0;
        se.util_avg = 0;
    }

    // 转换为真正的Thread指针以供CFS使用
    process::Thread* as_thread() noexcept {
        return reinterpret_cast<process::Thread*>(this);
    }

    bool operator==(const TestThread& other) const noexcept {
        return tid == other.tid && se.vruntime == other.se.vruntime;
    }
};

// ============================================================================
// 测试工具类
// ============================================================================

// 调度器测试助手
class SchedulerTestHelper {
public:
    // 创建指定数量的测试任务
    static void create_test_tasks(TestThread* tasks, usize count, i32 base_nice = 0) noexcept;

    // 验证队列状态
    static bool verify_queue_order(process::CfsRunqueue& queue) noexcept;

    // 获取队列中所有任务的vruntime序列
    static void get_vruntime_sequence(process::CfsRunqueue& queue, u64* vruntimes, usize* count) noexcept;

    // 验证权重到时间片转换
    static bool verify_time_slice_calculation(u32 weight, u32 total_weight, u64 expected_slice) noexcept;
};

// vruntime跟踪器
class VruntimeTracker {
private:
    struct TaskRecord {
        ThreadId tid;
        u64 initial_vruntime;
        u64 current_vruntime;
        u32 weight;
    };

    TaskRecord records_[16];
    usize record_count_;

public:
    VruntimeTracker() noexcept : record_count_(0) {}

    // 记录任务的初始状态
    void track_task(const TestThread& task) noexcept;

    // 更新任务的vruntime
    void update_task_vruntime(ThreadId tid, u64 new_vruntime) noexcept;

    // 验证vruntime增长的公平性
    bool verify_fairness() const noexcept;

    // 获取任务的vruntime增长量
    u64 get_vruntime_delta(ThreadId tid) const noexcept;
};

// 公平性验证器
class FairnessValidator {
public:
    // 验证不同nice值任务的时间片比例
    static bool verify_nice_proportion(i32 nice1, i32 nice2, u64 time1, u64 time2) noexcept;

    // 计算期望的权重比例（使用缩放因子1000的定点运算）
    static u64 calculate_weight_ratio_scaled(i32 nice1, i32 nice2) noexcept;

    // 验证vruntime推进速度符合权重比例
    static bool verify_vruntime_progression(const TestThread& task1, const TestThread& task2,
                                          u64 delta1, u64 delta2) noexcept;
};

// ============================================================================
// CFS参数和权重系统测试
// ============================================================================

// nice值权重转换测试
MOSS_TEST_FUNCTION(test_nice_to_weight_conversion);

// 时间片计算测试
MOSS_TEST_FUNCTION(test_sched_slice_calculation);

// 边界条件测试
MOSS_TEST_FUNCTION(test_cfs_params_boundary_cases);

// ============================================================================
// 红黑树运行队列测试
// ============================================================================

// 基本入队操作测试
MOSS_TEST_FUNCTION(test_runqueue_enqueue_basic);

// 按vruntime排序测试
MOSS_TEST_FUNCTION(test_runqueue_vruntime_ordering);

// 最左节点维护测试
MOSS_TEST_FUNCTION(test_runqueue_leftmost_maintenance);

// 任务出队测试
MOSS_TEST_FUNCTION(test_runqueue_dequeue_operations);

// tie-breaking逻辑测试（vruntime相同时使用TID）
MOSS_TEST_FUNCTION(test_runqueue_tie_breaking);

// ============================================================================
// Vruntime计算和更新测试
// ============================================================================

// delta计算测试
MOSS_TEST_FUNCTION(test_calc_delta_fair);

// vruntime更新测试
MOSS_TEST_FUNCTION(test_vruntime_update_logic);

// 权重对vruntime影响测试
MOSS_TEST_FUNCTION(test_weight_impact_on_vruntime);

// min_vruntime推进测试
MOSS_TEST_FUNCTION(test_min_vruntime_progression);

// ============================================================================
// 任务选择和公平性测试
// ============================================================================

// 基本任务选择测试
MOSS_TEST_FUNCTION(test_pick_next_task_basic);

// 相同nice值公平性测试
MOSS_TEST_FUNCTION(test_equal_nice_fairness);

// 不同nice值优先级测试
MOSS_TEST_FUNCTION(test_different_nice_priority);

// 调度公平性验证测试
MOSS_TEST_FUNCTION(test_scheduling_fairness_validation);

// ============================================================================
// 队列统计和状态管理测试
// ============================================================================

// 队列计数统计测试
MOSS_TEST_FUNCTION(test_runqueue_statistics);

// 负载追踪基础测试
MOSS_TEST_FUNCTION(test_load_tracking_basics);

// ============================================================================
// 测试套件声明和注册
// ============================================================================

// 声明调度器测试套件
MOSS_DECLARE_TEST_SUITE(scheduler);

} // namespace moss::kernel::test
