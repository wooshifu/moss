#pragma once

// CFS (Completely Fair Scheduler) 实现
// 基于红黑树的完全公平调度器，类似Linux CFS

#include "process.hpp"
#include "types.hpp"
#include "result.hpp"
#include "containers/containers.hpp"

namespace moss::kernel::process {

// CFS调度参数
namespace CfsParams {
    // 调度周期相关
    static constexpr u64 SCHED_LATENCY_NS = 6000000;      // 6ms调度延迟
    static constexpr u64 MIN_GRANULARITY_NS = 750000;     // 0.75ms最小粒度
    static constexpr u32 SCHED_NR_LATENCY = 8;            // 调度延迟内的任务数

    // nice值到权重的映射表（类似Linux内核）
    static constexpr u32 NICE_TO_WEIGHT[] = {
        /* -20 */ 88761, 71755, 56483, 46273, 36291,
        /* -15 */ 29154, 23254, 18705, 14949, 11916,
        /* -10 */ 9548,  7620,  6100,  4904,  3906,
        /*  -5 */ 3121,  2501,  1991,  1586,  1277,
        /*   0 */ 1024,  820,   655,   526,   423,
        /*   5 */ 335,   272,   215,   172,   137,
        /*  10 */ 110,   87,    70,    56,    45,
        /*  15 */ 36,    29,    23,    18,    15,
    };

    // nice值权重索引转换
    static constexpr u32 nice_to_weight_index(i32 nice) {
        return static_cast<u32>(nice + 20);
    }

    static constexpr u32 nice_to_weight(i32 nice) {
        u32 index = nice_to_weight_index(nice);
        return (index < 40) ? NICE_TO_WEIGHT[index] : 1;
    }

    // 权重到时间片的转换
    static constexpr u64 sched_slice(u32 weight, u32 total_weight) {
        if (total_weight == 0) return MIN_GRANULARITY_NS;

        u64 slice = (SCHED_LATENCY_NS * weight) / total_weight;
        return (slice < MIN_GRANULARITY_NS) ? MIN_GRANULARITY_NS : slice;
    }
}

// 红黑树节点（简化实现）
template<typename T>
struct RbNode {
    T* data;
    RbNode* left;
    RbNode* right;
    RbNode* parent;
    bool red;

    constexpr RbNode() noexcept : data(nullptr), left(nullptr), right(nullptr),
                                 parent(nullptr), red(true) {}

    constexpr RbNode(T* d) noexcept : data(d), left(nullptr), right(nullptr),
                                     parent(nullptr), red(true) {}
};

// CFS运行队列（红黑树实现）
class CfsRunqueue {
private:
    // 红黑树根节点
    RbNode<Thread>* rb_root_;

    // 最左节点（vruntime最小）
    RbNode<Thread>* rb_leftmost_;

    // 队列统计
    u32 nr_running_;              // 运行队列中的任务数
    u64 min_vruntime_;           // 队列中最小的vruntime
    u64 total_weight_;           // 所有任务的总权重

    // 负载统计
    u64 load_sum_;               // 累计负载
    u64 util_sum_;               // 累计利用率
    u32 load_avg_;               // 平均负载
    u32 util_avg_;               // 平均利用率

    // 节点池（避免动态分配）
    static constexpr usize MAX_NODES = 1024;
    RbNode<Thread> node_pool_[MAX_NODES];
    containers::AtomicCounter<usize> next_node_index_;

public:
    constexpr CfsRunqueue() noexcept
        : rb_root_(nullptr), rb_leftmost_(nullptr), nr_running_(0),
          min_vruntime_(0), total_weight_(0), load_sum_(0), util_sum_(0),
          load_avg_(0), util_avg_(0), next_node_index_(0) {}

    // 将任务加入运行队列
    void enqueue_task(Thread* thread) noexcept {
        if (thread == nullptr) return;

        // 初始化新任务的vruntime
        if (thread->se.vruntime == 0) {
            thread->se.vruntime = calc_initial_vruntime();
        }

        // 获取节点并插入红黑树
        RbNode<Thread>* node = allocate_node(thread);
        if (node != nullptr) {
            rb_insert(node);
            nr_running_++;
            total_weight_ += thread->se.weight;

            // 更新负载统计
            update_load_stats(thread, true);
        }
    }

    // 从运行队列移除任务
    void dequeue_task(Thread* thread) noexcept {
        if (thread == nullptr) return;

        RbNode<Thread>* node = find_node(thread);
        if (node != nullptr) {
            rb_remove(node);
            deallocate_node(node);
            nr_running_--;
            total_weight_ -= thread->se.weight;

            // 更新负载统计
            update_load_stats(thread, false);
        }
    }

    // 选择下一个要运行的任务
    [[nodiscard]] Thread* pick_next_task() noexcept {
        if (rb_leftmost_ == nullptr) {
            return nullptr;
        }

        Thread* next = rb_leftmost_->data;

        // 更新min_vruntime
        if (next != nullptr) {
            min_vruntime_ = next->se.vruntime;
        }

        return next;
    }

    // 更新当前任务的时间统计
    void update_curr_task(Thread* current, u64 delta_exec) noexcept {
        if (current == nullptr) return;

        // 更新执行时间
        current->se.sum_exec_runtime += delta_exec;

        // 计算加权的虚拟运行时间
        u64 weighted_delta = calc_delta_fair(delta_exec, current);
        current->se.vruntime += weighted_delta;

        // 更新最小vruntime（单调递增）
        min_vruntime_ = kernel_max(min_vruntime_, current->se.vruntime);

        // 检查是否需要重新调度
        if (should_preempt(current)) {
            // 设置重调度标志（在实际实现中）
            // current->need_resched = true;
        }

        // 更新负载追踪
        update_load_tracking(current, delta_exec);
    }

    // 检查是否需要抢占当前任务
    [[nodiscard]] bool should_preempt(Thread* current) const noexcept {
        if (current == nullptr || rb_leftmost_ == nullptr) {
            return false;
        }

        Thread* leftmost = rb_leftmost_->data;
        if (leftmost == nullptr || leftmost == current) {
            return false;
        }

        // 计算调度延迟
        u64 ideal_runtime = CfsParams::sched_slice(current->se.weight, static_cast<u32>(total_weight_));
        u64 delta_exec = current->se.sum_exec_runtime - current->se.prev_sum_exec_runtime;

        // 如果当前任务运行时间超过理想时间片，允许抢占
        return delta_exec > ideal_runtime;
    }

    // 获取队列统计信息
    [[nodiscard]] u32 nr_running() const noexcept { return nr_running_; }
    [[nodiscard]] u64 min_vruntime() const noexcept { return min_vruntime_; }
    [[nodiscard]] u64 total_weight() const noexcept { return total_weight_; }
    [[nodiscard]] u32 load_avg() const noexcept { return load_avg_; }
    [[nodiscard]] u32 util_avg() const noexcept { return util_avg_; }

    // 调试接口
    void dump_runqueue() const noexcept {
        // 在实际内核中，这里会输出调试信息
        // 包括红黑树结构、任务vruntime等
    }

private:
    // 计算新任务的初始vruntime
    [[nodiscard]] u64 calc_initial_vruntime() const noexcept {
        // 新任务的vruntime设置为当前min_vruntime
        // 但要减去一个调度延迟，给新任务一些优势
        u64 vruntime = min_vruntime_;
        if (vruntime > CfsParams::SCHED_LATENCY_NS) {
            vruntime -= CfsParams::SCHED_LATENCY_NS;
        }
        return vruntime;
    }

    // 计算加权的时间增量
    [[nodiscard]] u64 calc_delta_fair(u64 delta_exec, Thread* thread) const noexcept {
        if (thread->se.weight == 0) return delta_exec;

        // vruntime = delta_exec * NICE_0_LOAD / weight
        // 权重越高，vruntime增长越慢（优先级越高）
        return (delta_exec * CfsParams::NICE_TO_WEIGHT[20]) / thread->se.weight;
    }

    // 更新负载追踪（PELT算法简化版本）
    void update_load_tracking(Thread* thread, u64 delta_exec) noexcept {
        if (thread == nullptr) return;

        // 简化的负载追踪实现
        // 实际的PELT算法更复杂，考虑衰减因子等
        [[maybe_unused]] constexpr u64 LOAD_AVG_PERIOD = 32;
        constexpr u64 LOAD_AVG_MAX = 47742;  // 32 * 1024 * 1.5

        // 更新负载统计
        thread->se.load_sum += delta_exec;
        thread->se.util_sum += delta_exec;

        // 计算平均值（简化版本）
        if (thread->se.load_sum > LOAD_AVG_MAX) {
            thread->se.load_avg = LOAD_AVG_MAX >> 10;
            thread->se.load_sum = LOAD_AVG_MAX;
        } else {
            thread->se.load_avg = thread->se.load_sum >> 10;
        }

        if (thread->se.util_sum > LOAD_AVG_MAX) {
            thread->se.util_avg = LOAD_AVG_MAX >> 10;
            thread->se.util_sum = LOAD_AVG_MAX;
        } else {
            thread->se.util_avg = thread->se.util_sum >> 10;
        }
    }

    // 更新队列负载统计
    void update_load_stats(Thread* thread, bool add) noexcept {
        if (thread == nullptr) return;

        if (add) {
            load_sum_ += thread->se.load_avg;
            util_sum_ += thread->se.util_avg;
        } else {
            load_sum_ = (load_sum_ > thread->se.load_avg) ?
                        (load_sum_ - thread->se.load_avg) : 0;
            util_sum_ = (util_sum_ > thread->se.util_avg) ?
                        (util_sum_ - thread->se.util_avg) : 0;
        }

        // 更新平均值
        load_avg_ = static_cast<u32>((nr_running_ > 0) ? (load_sum_ / nr_running_) : 0);
        util_avg_ = static_cast<u32>((nr_running_ > 0) ? (util_sum_ / nr_running_) : 0);
    }

    // 红黑树操作（简化实现）
    void rb_insert(RbNode<Thread>* node) noexcept {
        if (node == nullptr || node->data == nullptr) return;

        RbNode<Thread>** new_node = &rb_root_;
        RbNode<Thread>* parent = nullptr;
        u64 vruntime = node->data->se.vruntime;

        // 标准BST插入
        while (*new_node != nullptr) {
            parent = *new_node;

            if (vruntime < parent->data->se.vruntime) {
                new_node = &parent->left;
            } else {
                new_node = &parent->right;
            }
        }

        // 链接节点
        *new_node = node;
        node->parent = parent;

        // 更新最左节点
        if (rb_leftmost_ == nullptr || vruntime < rb_leftmost_->data->se.vruntime) {
            rb_leftmost_ = node;
        }

        // 红黑树性质维护（简化版本，省略复杂的旋转逻辑）
        rb_insert_fixup(node);
    }

    void rb_remove(RbNode<Thread>* node) noexcept {
        if (node == nullptr) return;

        // 更新最左节点
        if (node == rb_leftmost_) {
            rb_leftmost_ = rb_next(node);
        }

        // 标准BST删除（简化版本）
        rb_delete_node(node);
    }

    [[nodiscard]] RbNode<Thread>* find_node(Thread* thread) const noexcept {
        RbNode<Thread>* node = rb_root_;

        while (node != nullptr) {
            if (node->data == thread) {
                return node;
            } else if (thread->se.vruntime < node->data->se.vruntime) {
                node = node->left;
            } else {
                node = node->right;
            }
        }

        return nullptr;
    }

    [[nodiscard]] RbNode<Thread>* rb_next(RbNode<Thread>* node) const noexcept {
        if (node == nullptr) return nullptr;

        if (node->right != nullptr) {
            // 找右子树的最小节点
            node = node->right;
            while (node->left != nullptr) {
                node = node->left;
            }
            return node;
        }

        // 向上找第一个左孩子是祖先的节点
        RbNode<Thread>* parent = node->parent;
        while (parent != nullptr && node == parent->right) {
            node = parent;
            parent = parent->parent;
        }

        return parent;
    }

    // 红黑树性质维护（简化实现）
    void rb_insert_fixup(RbNode<Thread>* node) noexcept {
        // 简化的红黑树修复逻辑
        // 实际实现需要完整的旋转和重新着色逻辑
        if (node != nullptr && node->parent == nullptr) {
            node->red = false;  // 根节点必须是黑色
        }
    }

    void rb_delete_node(RbNode<Thread>* node) noexcept {
        // 简化的节点删除逻辑
        // 实际实现需要考虑各种删除情况和红黑树性质维护
        if (node == nullptr) return;

        if (node->parent == nullptr) {
            rb_root_ = nullptr;
        } else if (node == node->parent->left) {
            node->parent->left = nullptr;
        } else {
            node->parent->right = nullptr;
        }
    }

    // 节点内存管理
    [[nodiscard]] RbNode<Thread>* allocate_node(Thread* thread) noexcept {
        usize index = next_node_index_.fetch_add(1, containers::MemoryOrder::Relaxed);
        if (index >= MAX_NODES) {
            return nullptr;
        }

        RbNode<Thread>* node = &node_pool_[index];
        node->data = thread;
        node->left = nullptr;
        node->right = nullptr;
        node->parent = nullptr;
        node->red = true;

        return node;
    }

    void deallocate_node(RbNode<Thread>* node) noexcept {
        if (node != nullptr) {
            node->data = nullptr;
            node->left = nullptr;
            node->right = nullptr;
            node->parent = nullptr;
            node->red = true;
        }
    }

    // 工具函数
    template<typename T>
    constexpr const T& kernel_max(const T& a, const T& b) noexcept {
        return (a < b) ? b : a;
    }
};

// CFS调度器类
class CfsScheduler {
private:
    // 每个CPU的运行队列
    containers::PerCpuData<CfsRunqueue> runqueues_;

    // 全局统计
    containers::PerCpuAtomicCounter<u64> total_switches_;
    containers::PerCpuAtomicCounter<u64> total_preemptions_;

public:
    constexpr CfsScheduler() noexcept = default;

    // 任务管理
    void enqueue_task(Thread* thread, u32 cpu) noexcept {
        if (thread == nullptr || cpu >= MAX_CPUS) return;

        runqueues_.get_cpu(cpu).enqueue_task(thread);
        thread->cpu = cpu;
        thread->state = ProcessState::Ready;
    }

    void dequeue_task(Thread* thread) noexcept {
        if (thread == nullptr) return;

        u32 cpu = thread->cpu;
        if (cpu < MAX_CPUS) {
            runqueues_.get_cpu(cpu).dequeue_task(thread);
        }
    }

    // 调度决策
    [[nodiscard]] Thread* pick_next_task(u32 cpu) noexcept {
        if (cpu >= MAX_CPUS) return nullptr;

        return runqueues_.get_cpu(cpu).pick_next_task();
    }

    // 时间片更新
    void update_current(Thread* current, u64 delta_exec) noexcept {
        if (current == nullptr) return;

        u32 cpu = current->cpu;
        if (cpu < MAX_CPUS) {
            runqueues_.get_cpu(cpu).update_curr_task(current, delta_exec);
        }
    }

    // 抢占检查
    [[nodiscard]] bool should_preempt_current(Thread* current) const noexcept {
        if (current == nullptr) return false;

        u32 cpu = current->cpu;
        if (cpu >= MAX_CPUS) return false;

        return runqueues_.get_cpu(cpu).should_preempt(current);
    }

    // 负载均衡相关
    [[nodiscard]] u32 get_cpu_load(u32 cpu) const noexcept {
        if (cpu >= MAX_CPUS) return 0;
        return runqueues_.get_cpu(cpu).load_avg();
    }

    [[nodiscard]] u32 get_cpu_nr_running(u32 cpu) const noexcept {
        if (cpu >= MAX_CPUS) return 0;
        return runqueues_.get_cpu(cpu).nr_running();
    }

    // 统计信息
    [[nodiscard]] u64 total_context_switches() const noexcept {
        return total_switches_.load_total();
    }

    [[nodiscard]] u64 total_preemptions() const noexcept {
        return total_preemptions_.load_total();
    }

    // 调试接口
    void dump_runqueue(u32 cpu) const noexcept {
        if (cpu < MAX_CPUS) {
            runqueues_.get_cpu(cpu).dump_runqueue();
        }
    }

    // 内部统计更新
    void record_context_switch() noexcept {
        (void)total_switches_.fetch_add_local(1);
    }

    void record_preemption() noexcept {
        (void)total_preemptions_.fetch_add_local(1);
    }

    // 启动调度循环（主调度入口）
    [[noreturn]] void start_scheduling() noexcept {
        while (true) {
            u32 current_cpu = get_current_cpu_id();

            // 选择下一个任务
            Thread* next_task = pick_next_task(current_cpu);

            if (next_task != nullptr) {
                // 切换到选定的任务
                context_switch_to_task(next_task);
            } else {
                // 没有可运行的任务，进入空闲状态
                idle_task(current_cpu);
            }

            // 检查是否需要重新调度
            check_need_resched();
        }
    }

private:
    // 空闲任务
    void idle_task([[maybe_unused]] u32 cpu) noexcept {
        // 启用中断并等待
        #if defined(MOSS_ARCH_ARM64)
            asm volatile("msr daifclr, #2" ::: "memory");  // 启用IRQ
            asm volatile("wfi" ::: "memory");              // 等待中断
            asm volatile("msr daifset, #2" ::: "memory");  // 禁用IRQ
        #elif defined(MOSS_ARCH_X86_64)
            asm volatile("sti" ::: "memory");              // 启用中断
            asm volatile("hlt" ::: "memory");              // 停机等待中断
            asm volatile("cli" ::: "memory");              // 禁用中断
        #elif defined(MOSS_ARCH_RISCV)
            asm volatile("csrsi mstatus, 0x8" ::: "memory"); // 启用机器级中断
            asm volatile("wfi" ::: "memory");              // 等待中断
            asm volatile("csrci mstatus, 0x8" ::: "memory"); // 禁用机器级中断
        #else
            // 通用回退：简单的CPU循环
            for (volatile int i = 0; i < 1000; ++i) {}
        #endif
    }

    // 上下文切换到指定任务
    void context_switch_to_task(Thread* task) noexcept {
        if (task == nullptr) return;

        // 更新任务状态
        task->state = ProcessState::Running;
        // task->last_run_time = get_current_time();  // 暂时注释，Thread结构需要添加此字段

        // 记录上下文切换
        record_context_switch();

        // 这里应该执行实际的上下文切换
        // 简化实现：模拟任务执行（实际需要汇编实现）
        // 在真实实现中，这里会调用 context_switch() 汇编函数
    }

    // 检查是否需要重新调度
    void check_need_resched() noexcept {
        // 简化实现：定期检查抢占
        u32 current_cpu = get_current_cpu_id();

        if (runqueues_.get_cpu(current_cpu).nr_running() > 0) {
            // 有其他任务在等待，可能需要抢占
        }
    }

    // 获取当前CPU ID
    [[nodiscard]] static u32 get_current_cpu_id() noexcept {
        #if defined(MOSS_ARCH_ARM64)
            u64 mpidr;
            asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
            return static_cast<u32>(mpidr & 0xFF) % MAX_CPUS;
        #elif defined(MOSS_ARCH_X86_64)
            // x86_64: 使用 APIC ID 或简单递增计数器
            return 0; // 简化实现，单核
        #elif defined(MOSS_ARCH_RISCV)
            // RISC-V: 读取 hart ID
            u64 hart_id;
            asm volatile("csrr %0, mhartid" : "=r"(hart_id));
            return static_cast<u32>(hart_id) % MAX_CPUS;
        #else
            return 0; // 回退实现
        #endif
    }

    // 获取当前时间
    [[nodiscard]] static u64 get_current_time() noexcept {
        u64 count;
        #if defined(MOSS_ARCH_ARM64)
            asm volatile("mrs %0, cntvct_el0" : "=r"(count));
        #elif defined(MOSS_ARCH_X86_64)
            asm volatile("rdtsc" : "=A"(count));
        #elif defined(MOSS_ARCH_RISCV)
            asm volatile("rdcycle %0" : "=r"(count));
        #else
            count = 0; // 回退实现
        #endif
        return count;
    }
};

} // namespace moss::kernel::process