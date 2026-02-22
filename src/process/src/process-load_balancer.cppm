// MOSS Process Module - Partition: load_balancer
// Multi-core load balancer

export module moss.process:load_balancer;

import :types;
import :scheduler;

import moss.types;
import moss.arch;
import moss.containers;

// ============================================================================
// load_balancer.hpp - Multi-core load balancer
// ============================================================================
export namespace moss::kernel::process {

// Load balance statistics
struct LoadBalanceStats {
  u64 migrations_count;
  u64 steal_attempts;
  u64 steal_success;
  u64 idle_balance_count;
  u64 active_balance_count;

  constexpr LoadBalanceStats() noexcept
      : migrations_count(0), steal_attempts(0), steal_success(0), idle_balance_count(0), active_balance_count(0) {}
};

// Load balance policy
enum class BalancePolicy : u8 { Conservative = 0, Aggressive = 1, NUMA_Aware = 2 };

// CPU topology information
struct CpuTopology {
  u32 cpu_id;
  u32 core_id;
  u32 cluster_id;
  u32 numa_node;
  bool is_big_core;

  constexpr CpuTopology() noexcept : cpu_id(0), core_id(0), cluster_id(0), numa_node(0), is_big_core(false) {}

  constexpr CpuTopology(u32 cpu, u32 core, u32 cluster, u32 numa, bool big) noexcept
      : cpu_id(cpu), core_id(core), cluster_id(cluster), numa_node(numa), is_big_core(big) {}
};

// Load balancer class
class LoadBalancer {
private:
  containers::PerCpuData<LoadBalanceStats> stats_;
  CpuTopology topology_[MAX_CPUS];

  BalancePolicy policy_;
  u32 imbalance_threshold_;
  u64 migration_cost_;
  u64 last_balance_time_;
  u64 balance_interval_;

  containers::PerCpuWorkQueue<Thread *, 64> migration_queue_;

public:
  LoadBalancer() noexcept
      : stats_{}, topology_{}, policy_(BalancePolicy::Conservative), imbalance_threshold_(25), migration_cost_(10000),
        last_balance_time_(0), balance_interval_(4000000) {
    for (u32 i = 0; i < MAX_CPUS; ++i) {
      topology_[i] = CpuTopology(i, i, 0, 0, true);
    }
  }

  bool idle_balance(u32 cpu, CfsScheduler &scheduler) noexcept {
    if (cpu >= MAX_CPUS)
      return false;

    auto &local_stats = stats_.get_cpu(cpu);
    local_stats.idle_balance_count++;

    u32 busiest_cpu = find_busiest_cpu(cpu, scheduler);
    if (busiest_cpu == cpu || busiest_cpu >= MAX_CPUS) {
      return false;
    }

    return steal_task(cpu, busiest_cpu, scheduler);
  }

  void periodic_balance(u64 current_time, CfsScheduler &scheduler) noexcept {
    if (current_time - last_balance_time_ < balance_interval_) {
      return;
    }

    last_balance_time_ = current_time;

    for (u32 cpu = 0; cpu < MAX_CPUS; ++cpu) {
      u32 load = scheduler.get_cpu_load(cpu);
      u32 nr_running = scheduler.get_cpu_nr_running(cpu);

      if (nr_running > 2 && load > 80) {
        u32 target_cpu = find_least_loaded_cpu(scheduler);
        if (target_cpu != cpu && target_cpu < MAX_CPUS) {
          migrate_task(cpu, target_cpu, scheduler);
        }
      }
    }
  }

  [[nodiscard]] u32 select_cpu_for_task(Thread *thread, CfsScheduler &scheduler) noexcept {
    if (thread == nullptr)
      return 0;

    [[maybe_unused]] u32 current_cpu = current_cpu_id();
    u32 prev_cpu = thread->cpu;

    if (has_cpu_affinity(thread, prev_cpu)) {
      u32 prev_load = scheduler.get_cpu_load(prev_cpu);
      if (prev_load < 70) {
        return prev_cpu;
      }
    }

    return find_best_cpu_for_task(thread, scheduler);
  }

  bool migrate_task(u32 src_cpu, u32 dst_cpu, CfsScheduler &scheduler) noexcept {
    if (src_cpu >= MAX_CPUS || dst_cpu >= MAX_CPUS || src_cpu == dst_cpu) {
      return false;
    }

    Thread *task = select_migration_candidate(src_cpu, scheduler);
    if (task == nullptr) {
      return false;
    }

    scheduler.dequeue_task(task);
    scheduler.enqueue_task(task, dst_cpu);

    auto &src_stats = stats_.get_cpu(src_cpu);
    src_stats.migrations_count++;

    // Notify target CPU so it picks up the migrated task promptly
    send_reschedule_ipi(dst_cpu);
    return true;
  }

  [[nodiscard]] LoadBalanceStats get_stats(u32 cpu) const noexcept {
    if (cpu >= MAX_CPUS)
      return LoadBalanceStats{};
    return stats_.get_cpu(cpu);
  }

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

  void set_policy(BalancePolicy policy) noexcept {
    policy_ = policy;

    switch (policy) {
    case BalancePolicy::Conservative:
      imbalance_threshold_ = 25;
      balance_interval_ = 8000000;
      break;
    case BalancePolicy::Aggressive:
      imbalance_threshold_ = 15;
      balance_interval_ = 2000000;
      break;
    case BalancePolicy::NUMA_Aware:
      imbalance_threshold_ = 20;
      balance_interval_ = 4000000;
      break;
    default:
      imbalance_threshold_ = 25;
      balance_interval_ = 8000000;
      break;
    }
  }

private:
  [[nodiscard]] u32 find_busiest_cpu(u32 current_cpu, CfsScheduler &scheduler) const noexcept {
    u32 busiest_cpu = current_cpu;
    u32 max_load = scheduler.get_cpu_load(current_cpu);

    for (u32 cpu = 0; cpu < MAX_CPUS; ++cpu) {
      if (cpu == current_cpu)
        continue;

      u32 load = scheduler.get_cpu_load(cpu);
      u32 nr_running = scheduler.get_cpu_nr_running(cpu);

      if (load > max_load && nr_running > 1) {
        max_load = load;
        busiest_cpu = cpu;
      }
    }

    u32 current_load = scheduler.get_cpu_load(current_cpu);
    if (max_load - current_load < imbalance_threshold_) {
      return current_cpu;
    }

    return busiest_cpu;
  }

  [[nodiscard]] u32 find_least_loaded_cpu(CfsScheduler &scheduler) const noexcept {
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

  [[nodiscard]] u32 find_best_cpu_for_task(Thread *thread, CfsScheduler &scheduler) const noexcept {
    u32 best_cpu = 0;
    u32 min_load = static_cast<u32>(-1);

    for (u32 cpu = 0; cpu < MAX_CPUS; ++cpu) {
      if (!has_cpu_affinity(thread, cpu)) {
        continue;
      }

      u32 load = scheduler.get_cpu_load(cpu);

      if (policy_ == BalancePolicy::NUMA_Aware) {
        u32 thread_numa = get_thread_numa_node(thread);
        u32 cpu_numa = topology_[cpu].numa_node;

        if (thread_numa != cpu_numa) {
          load += 20;
        }
      }

      if (load < min_load) {
        min_load = load;
        best_cpu = cpu;
      }
    }

    return best_cpu;
  }

  bool steal_task(u32 dst_cpu, u32 src_cpu, CfsScheduler &scheduler) noexcept {
    auto &stats = stats_.get_cpu(dst_cpu);
    stats.steal_attempts++;

    Thread *task = select_migration_candidate(src_cpu, scheduler);
    if (task == nullptr) {
      return false;
    }

    u64 expected_benefit = calculate_migration_benefit(task, src_cpu, dst_cpu, scheduler);
    if (expected_benefit < migration_cost_) {
      return false;
    }

    scheduler.dequeue_task(task);
    scheduler.enqueue_task(task, dst_cpu);

    stats.steal_success++;
    stats.migrations_count++;

    // Notify destination CPU (ourselves if idle-balancing, or another CPU)
    send_reschedule_ipi(dst_cpu);
    return true;
  }

  [[nodiscard]] Thread *select_migration_candidate(u32 cpu, CfsScheduler &scheduler) const noexcept {
    // Pick the highest-vruntime (least-deserving) runnable task on source CPU.
    // This preserves CFS fairness: we migrate the task that has consumed
    // the most CPU, not the one most in need of CPU time.
    // Only candidates with nr_running > 1 on source are eligible
    // (we never steal the last runnable task).
    if (scheduler.get_cpu_nr_running(cpu) <= 1)
      return nullptr;
    return scheduler.pick_last_task(cpu);
  }

  [[nodiscard]] u64 calculate_migration_benefit(Thread *thread, u32 src_cpu, u32 dst_cpu,
                                                CfsScheduler &scheduler) const noexcept {
    if (thread == nullptr)
      return 0;

    u32 src_load = scheduler.get_cpu_load(src_cpu);
    u32 dst_load = scheduler.get_cpu_load(dst_cpu);

    return (src_load > dst_load) ? (src_load - dst_load) * 1000 : 0;
  }

  [[nodiscard]] bool has_cpu_affinity(Thread *thread, u32 cpu) const noexcept {
    if (!thread || cpu >= MAX_CPUS)
      return false;
    return (thread->cpu_affinity_mask & (1u << cpu)) != 0;
  }

  [[nodiscard]] u32 get_thread_numa_node([[maybe_unused]] Thread *thread) const noexcept { return 0; }

  [[nodiscard]] static u32 current_cpu_id() noexcept { return arch::get_current_cpu_id(); }
};

/// Global load balancer instance (initialized alongside scheduler)
extern LoadBalancer *g_load_balancer;

/// Try idle-balance: steal tasks from busiest CPU into the idle CPU.
/// Call from idle paths when no local tasks are available.
inline void try_idle_balance(u32 cpu) noexcept {
  if (g_load_balancer && g_scheduler)
    g_load_balancer->idle_balance(cpu, *g_scheduler);
}

} // namespace moss::kernel::process
