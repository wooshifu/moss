#pragma once

// 多核负载均衡器实现
// 基于工作窃取和迁移算法，优化多核性能

#include "cfs_scheduler.hpp"
#include "containers/containers.hpp"
#include "process.hpp"
#include "types.hpp"

namespace moss::kernel::process {

// 负载均衡统计
struct LoadBalanceStats {
  u64 migrations_count;     // 进程迁移次数
  u64 steal_attempts;       // 工作窃取尝试次数
  u64 steal_success;        // 成功窃取次数
  u64 idle_balance_count;   // 空闲平衡次数
  u64 active_balance_count; // 主动平衡次数

  constexpr LoadBalanceStats() noexcept
      : migrations_count(0), steal_attempts(0), steal_success(0),
        idle_balance_count(0), active_balance_count(0) {}
};

// 负载均衡策略
enum class BalancePolicy : u8 {
  Conservative = 0, // 保守策略，减少迁移
  Aggressive = 1,   // 激进策略，频繁迁移优化负载
  NUMA_Aware = 2    // NUMA感知策略
};

// CPU拓扑信息
struct CpuTopology {
  u32 cpu_id;
  u32 core_id;      // 物理核心ID
  u32 cluster_id;   // CPU簇ID (big.LITTLE)
  u32 numa_node;    // NUMA节点ID
  bool is_big_core; // 是否为大核(性能核心)

  constexpr CpuTopology() noexcept
      : cpu_id(0), core_id(0), cluster_id(0), numa_node(0), is_big_core(false) {
  }

  constexpr CpuTopology(u32 cpu, u32 core, u32 cluster, u32 numa,
                        bool big) noexcept
      : cpu_id(cpu), core_id(core), cluster_id(cluster), numa_node(numa),
        is_big_core(big) {}
};

// 负载均衡器类
class LoadBalancer {
private:
  // Per-CPU负载统计
  containers::PerCpuData<LoadBalanceStats> stats_;

  // CPU拓扑信息
  CpuTopology topology_[MAX_CPUS];

  // 负载均衡策略和参数
  BalancePolicy policy_;
  u32 imbalance_threshold_; // 负载不平衡阈值
  u64 migration_cost_;      // 进程迁移开销估计
  u64 last_balance_time_;   // 上次负载均衡时间
  u64 balance_interval_;    // 负载均衡间隔

  // 迁移队列
  containers::PerCpuWorkQueue<Thread *, 64> migration_queue_;

public:
  LoadBalancer() noexcept
      : stats_{},    // 默认初始化统计数据
        topology_{}, // 默认初始化数组
        policy_(BalancePolicy::Conservative),
        imbalance_threshold_(25),                         // 25%的负载差异阈值
        migration_cost_(10000),                           // 10微秒的迁移开销
        last_balance_time_(0), balance_interval_(4000000) // 4ms负载均衡间隔
  {
    // 初始化CPU拓扑（简化实现）
    for (u32 i = 0; i < MAX_CPUS; ++i) {
      topology_[i] = CpuTopology(i, i, 0, 0, true);
    }
  }

  // 空闲时的负载均衡（当CPU空闲时调用）
  bool idle_balance(u32 cpu, CfsScheduler &scheduler) noexcept {
    if (cpu >= MAX_CPUS)
      return false;

    auto &local_stats = stats_.get_cpu(cpu);
    local_stats.idle_balance_count++;

    // 寻找最繁忙的CPU
    u32 busiest_cpu = find_busiest_cpu(cpu, scheduler);
    if (busiest_cpu == cpu || busiest_cpu >= MAX_CPUS) {
      return false; // 没有找到更繁忙的CPU
    }

    // 尝试从最繁忙的CPU窃取任务
    return steal_task(cpu, busiest_cpu, scheduler);
  }

  // 定期负载均衡（由调度器定期调用）
  void periodic_balance(u64 current_time, CfsScheduler &scheduler) noexcept {
    if (current_time - last_balance_time_ < balance_interval_) {
      return; // 还未到负载均衡时间
    }

    last_balance_time_ = current_time;

    // 检查所有CPU的负载情况
    for (u32 cpu = 0; cpu < MAX_CPUS; ++cpu) {
      u32 load = scheduler.get_cpu_load(cpu);
      u32 nr_running = scheduler.get_cpu_nr_running(cpu);

      // 如果CPU负载过高，尝试迁移任务
      if (nr_running > 2 && load > 80) { // 负载超过80%且有多个任务
        u32 target_cpu = find_least_loaded_cpu(scheduler);
        if (target_cpu != cpu && target_cpu < MAX_CPUS) {
          migrate_task(cpu, target_cpu, scheduler);
        }
      }
    }
  }

  // 主动负载均衡（当新任务唤醒时调用）
  [[nodiscard]] u32 select_cpu_for_task(Thread *thread,
                                        CfsScheduler &scheduler) noexcept {
    if (thread == nullptr)
      return 0;

    [[maybe_unused]] u32 current_cpu = current_cpu_id();
    u32 prev_cpu = thread->cpu;

    // CPU亲和性检查
    if (has_cpu_affinity(thread, prev_cpu)) {
      u32 prev_load = scheduler.get_cpu_load(prev_cpu);
      if (prev_load < 70) { // 之前的CPU负载不高
        return prev_cpu;
      }
    }

    // 寻找最佳CPU
    return find_best_cpu_for_task(thread, scheduler);
  }

  // 强制迁移任务
  bool migrate_task(u32 src_cpu, u32 dst_cpu,
                    CfsScheduler &scheduler) noexcept {
    if (src_cpu >= MAX_CPUS || dst_cpu >= MAX_CPUS || src_cpu == dst_cpu) {
      return false;
    }

    // 从源CPU选择一个合适的任务进行迁移
    Thread *task = select_migration_candidate(src_cpu, scheduler);
    if (task == nullptr) {
      return false;
    }

    // 执行迁移
    scheduler.dequeue_task(task);
    scheduler.enqueue_task(task, dst_cpu);

    // 更新统计
    auto &src_stats = stats_.get_cpu(src_cpu);
    src_stats.migrations_count++;

    return true;
  }

  // 获取负载均衡统计
  [[nodiscard]] LoadBalanceStats get_stats(u32 cpu) const noexcept {
    if (cpu >= MAX_CPUS)
      return LoadBalanceStats{};
    return stats_.get_cpu(cpu);
  }

  // 获取全局统计
  [[nodiscard]] LoadBalanceStats get_global_stats() const noexcept {
    LoadBalanceStats global{};

    stats_.for_each_cpu([&global](usize, const LoadBalanceStats &stats) {
      global.migrations_count += stats.migrations_count;
      global.steal_attempts += stats.steal_attempts;
      global.steal_success += stats.steal_success;
      global.idle_balance_count += stats.idle_balance_count;
      global.active_balance_count += stats.active_balance_count;
    });

    return global;
  }

  // 设置负载均衡策略
  void set_policy(BalancePolicy policy) noexcept {
    policy_ = policy;

    switch (policy) {
    case BalancePolicy::Conservative:
      imbalance_threshold_ = 25;
      balance_interval_ = 8000000; // 8ms
      break;
    case BalancePolicy::Aggressive:
      imbalance_threshold_ = 15;
      balance_interval_ = 2000000; // 2ms
      break;
    case BalancePolicy::NUMA_Aware:
      imbalance_threshold_ = 20;
      balance_interval_ = 4000000; // 4ms
      break;
    default:
      // 默认使用保守策略
      imbalance_threshold_ = 25;
      balance_interval_ = 8000000; // 8ms
      break;
    }
  }

private:
  // 查找最繁忙的CPU
  [[nodiscard]] u32 find_busiest_cpu(u32 current_cpu,
                                     CfsScheduler &scheduler) const noexcept {
    u32 busiest_cpu = current_cpu;
    u32 max_load = scheduler.get_cpu_load(current_cpu);

    for (u32 cpu = 0; cpu < MAX_CPUS; ++cpu) {
      if (cpu == current_cpu)
        continue;

      u32 load = scheduler.get_cpu_load(cpu);
      u32 nr_running = scheduler.get_cpu_nr_running(cpu);

      // 考虑负载和任务数量
      if (load > max_load && nr_running > 1) {
        max_load = load;
        busiest_cpu = cpu;
      }
    }

    // 检查负载差异是否足够大
    u32 current_load = scheduler.get_cpu_load(current_cpu);
    if (max_load - current_load < imbalance_threshold_) {
      return current_cpu; // 负载差异不大
    }

    return busiest_cpu;
  }

  // 查找负载最轻的CPU
  [[nodiscard]] u32
  find_least_loaded_cpu(CfsScheduler &scheduler) const noexcept {
    u32 least_loaded_cpu = 0;
    u32 min_load = scheduler.get_cpu_load(0);

    for (u32 cpu = 1; cpu < MAX_CPUS; ++cpu) {
      u32 load = scheduler.get_cpu_load(cpu);
      if (load < min_load) {
        min_load = load;
        least_loaded_cpu = cpu;
      }
    }

    return least_loaded_cpu;
  }

  // 为任务寻找最佳CPU
  [[nodiscard]] u32
  find_best_cpu_for_task(Thread *thread,
                         CfsScheduler &scheduler) const noexcept {
    u32 best_cpu = 0;
    u32 min_load = UINT32_MAX;

    for (u32 cpu = 0; cpu < MAX_CPUS; ++cpu) {
      // 检查CPU亲和性
      if (!has_cpu_affinity(thread, cpu)) {
        continue;
      }

      u32 load = scheduler.get_cpu_load(cpu);

      // NUMA感知选择
      if (policy_ == BalancePolicy::NUMA_Aware) {
        u32 thread_numa = get_thread_numa_node(thread);
        u32 cpu_numa = topology_[cpu].numa_node;

        if (thread_numa != cpu_numa) {
          load += 20; // NUMA跨节点访问惩罚
        }
      }

      if (load < min_load) {
        min_load = load;
        best_cpu = cpu;
      }
    }

    return best_cpu;
  }

  // 工作窃取
  bool steal_task(u32 dst_cpu, u32 src_cpu, CfsScheduler &scheduler) noexcept {
    auto &stats = stats_.get_cpu(dst_cpu);
    stats.steal_attempts++;

    // 选择合适的任务进行窃取
    Thread *task = select_migration_candidate(src_cpu, scheduler);
    if (task == nullptr) {
      return false;
    }

    // 检查迁移是否值得（简化的成本效益分析）
    u64 expected_benefit =
        calculate_migration_benefit(task, src_cpu, dst_cpu, scheduler);
    if (expected_benefit < migration_cost_) {
      return false; // 迁移成本太高
    }

    // 执行窃取
    scheduler.dequeue_task(task);
    scheduler.enqueue_task(task, dst_cpu);

    stats.steal_success++;
    stats.migrations_count++;

    return true;
  }

  // 选择迁移候选任务
  [[nodiscard]] Thread *select_migration_candidate(
      [[maybe_unused]] u32 cpu,
      [[maybe_unused]] CfsScheduler &scheduler) const noexcept {
    // 简化实现：返回nullptr表示没有合适的候选任务
    // 实际实现需要从运行队列中选择合适的任务
    return nullptr;
  }

  // 计算迁移收益
  [[nodiscard]] u64
  calculate_migration_benefit(Thread *thread, u32 src_cpu, u32 dst_cpu,
                              CfsScheduler &scheduler) const noexcept {
    if (thread == nullptr)
      return 0;

    u32 src_load = scheduler.get_cpu_load(src_cpu);
    u32 dst_load = scheduler.get_cpu_load(dst_cpu);

    // 简化的收益计算：基于负载差异
    return (src_load > dst_load) ? (src_load - dst_load) * 1000 : 0;
  }

  // 检查线程的CPU亲和性
  [[nodiscard]] bool has_cpu_affinity([[maybe_unused]] Thread *thread,
                                      [[maybe_unused]] u32 cpu) const noexcept {
    // 简化实现：假设所有线程可以在任何CPU上运行
    return cpu < MAX_CPUS;
  }

  // 获取线程的NUMA节点
  [[nodiscard]] u32
  get_thread_numa_node([[maybe_unused]] Thread *thread) const noexcept {
    // 简化实现：返回0表示NUMA节点0
    return 0;
  }

  // 获取当前CPU ID (多架构支持)
  [[nodiscard]] static u32 current_cpu_id() noexcept {
    return arch::get_current_cpu_id();
  }
};

} // namespace moss::kernel::process
