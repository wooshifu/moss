// MOSS Process Module - Partition: scheduler
// CFS Scheduler, Idle Task, CfsRunqueue, CfsScheduler

module;

// Architecture detection
#include "arch_detect.h"

// Assembly interop declarations (global module fragment)
extern "C" void switch_to_user(void* context, unsigned long long user_stack);
extern "C" void early_debug_print(const char* message) noexcept;
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
extern "C" void context_switch(void* prev_context, void* next_context);
#endif

export module moss.process:scheduler;

import :types;

import moss.std;
import moss.types;
import moss.result;
import moss.smart_ptr;
import moss.arch;
import moss.containers;
import moss.mm;
import moss.interrupts;
import moss.platform;
import moss.hal.intc;
import moss.hal.timer;
import moss.timer;
import moss.logging;

// ============================================================================
// cfs_scheduler.hpp - CFS (Completely Fair Scheduler)
// ============================================================================
export namespace moss::kernel::process {

namespace log = moss::kernel::logging;

// ---------------------------------------------------------------------------
// Direct-UART helpers (bypass ring buffer to avoid IRQ livelock / lock
// contention that makes klog unusable from ISR and idle-loop contexts).
// ---------------------------------------------------------------------------

// Append decimal representation of v to buf at pos (no NUL terminator).
inline void fmt_u64(char* buf, u32& pos, u32 cap, u64 v) noexcept {
  char tmp[20];
  u32 len = 0;
  if (v == 0) { tmp[len++] = '0'; }
  else { while (v > 0 && len < 20) { tmp[len++] = static_cast<char>('0' + v % 10); v /= 10; } }
  for (u32 i = len; i > 0 && pos < cap - 1; --i) buf[pos++] = tmp[i - 1];
}

inline void fmt_str(char* buf, u32& pos, u32 cap, const char* s) noexcept {
  while (*s != '\0' && pos < cap - 1) buf[pos++] = *s++;
}

/// Print idle-heartbeat via direct UART (no locks, no ring buffer).
inline void idle_heartbeat_print(const char* tag, u32 cpu, u64 uptime_ms) noexcept {
  char buf[96];
  u32 pos = 0;
  constexpr u32 CAP = sizeof(buf);
  fmt_str(buf, pos, CAP, "[idle] ");
  fmt_str(buf, pos, CAP, tag);
  fmt_str(buf, pos, CAP, " CPU");
  fmt_u64(buf, pos, CAP, cpu);
  fmt_str(buf, pos, CAP, " heartbeat: uptime_ms=");
  fmt_u64(buf, pos, CAP, uptime_ms);
  fmt_str(buf, pos, CAP, "\n");
  buf[pos] = '\0';
  early_debug_print(buf);
}

// CFS scheduling parameters
namespace CfsParams {
inline constexpr u64 SCHED_LATENCY_NS = 6000000;  // 6ms
inline constexpr u64 MIN_GRANULARITY_NS = 750000;  // 0.75ms
inline constexpr u32 SCHED_NR_LATENCY = 8;

// nice-to-weight mapping table (similar to Linux kernel)
inline constexpr u32 NICE_TO_WEIGHT[] = {
    /* -20 */ 88761, 71755, 56483, 46273, 36291,
    /* -15 */ 29154, 23254, 18705, 14949, 11916,
    /* -10 */ 9548,  7620,  6100,  4904,  3906,
    /*  -5 */ 3121,  2501,  1991,  1586,  1277,
    /*   0 */ 1024,  820,   655,   526,   423,
    /*   5 */ 335,   272,   215,   172,   137,
    /*  10 */ 110,   87,    70,    56,    45,
    /*  15 */ 36,    29,    23,    18,    15,
};

inline constexpr u32 nice_to_weight_index(i32 nice) {
  return static_cast<u32>(nice + 20);
}

inline constexpr u32 nice_to_weight(i32 nice) {
  u32 index = nice_to_weight_index(nice);
  return (index < 40) ? NICE_TO_WEIGHT[index] : 1;
}

inline constexpr u64 sched_slice(u32 weight, u32 total_weight) {
  if (total_weight == 0)
    return MIN_GRANULARITY_NS;

  u64 slice = (SCHED_LATENCY_NS * weight) / total_weight;
  return (slice < MIN_GRANULARITY_NS) ? MIN_GRANULARITY_NS : slice;
}
} // namespace CfsParams

// Red-black tree node (simplified implementation)
template <typename T> struct RbNode {
  T *data;
  RbNode *left;
  RbNode *right;
  RbNode *parent;
  bool red;

  constexpr RbNode() noexcept
      : data(nullptr), left(nullptr), right(nullptr), parent(nullptr),
        red(true) {}

  constexpr RbNode(T *d) noexcept
      : data(d), left(nullptr), right(nullptr), parent(nullptr), red(true) {}
};

// ============================================================================
// idle_process.hpp - Idle task management
// ============================================================================

// Linux-style idle task class
// Each CPU has an independent idle task that runs when no other tasks are available
class IdleTask : public Thread {
public:
    explicit IdleTask(u32 cpu_id) noexcept;

    ~IdleTask() noexcept = default;

    // Non-copyable, non-movable
    IdleTask(const IdleTask&) = delete;
    IdleTask& operator=(const IdleTask&) = delete;
    IdleTask(IdleTask&&) = delete;
    IdleTask& operator=(IdleTask&&) = delete;

    [[noreturn]] void run() noexcept;

    u32 get_cpu_id() const noexcept { return cpu_id_; }

    u64 get_idle_time_ns() const noexcept { return idle_time_ns_; }

    void reset_idle_time() noexcept { idle_time_ns_ = 0; }

private:
    u32 cpu_id_;
    u64 idle_time_ns_;
    [[maybe_unused]] u64 last_idle_start_;
};

// Linux-style do_idle function
[[noreturn]] void do_idle(u32 cpu_id) noexcept;

// Create idle task for specified CPU
IdleTask* create_idle_task(u32 cpu_id) noexcept;

// Global idle task management
extern moss::kernel::containers::PerCpuData<IdleTask*> g_idle_tasks;

// Get idle task for specified CPU
inline IdleTask* get_idle_task(u32 cpu_id) noexcept {
    if (cpu_id >= moss::kernel::MAX_CPUS) {
        return nullptr;
    }
    return g_idle_tasks.get_cpu(cpu_id);
}

// Set idle task for specified CPU
inline bool set_idle_task(u32 cpu_id, IdleTask* idle_task) noexcept {
    if (cpu_id >= moss::kernel::MAX_CPUS) {
        return false;
    }
    g_idle_tasks.get_cpu(cpu_id) = idle_task;
    return true;
}

// Check if specified CPU is running idle task
bool is_cpu_idle(u32 cpu_id) noexcept;

// Wake up CPU from idle state (for IPI mechanism)
void wakeup_idle_cpu(u32 cpu_id) noexcept;

/// Send a reschedule IPI (SGI 0) to the target CPU.
/// This wakes the target from WFI and causes it to re-examine its runqueue.
/// Used by load balancer after migrating tasks to an idle or less-loaded CPU.
inline void send_reschedule_ipi(u32 target_cpu) noexcept {
    if (target_cpu >= MAX_CPUS || target_cpu == arch::get_current_cpu_id())
        return;
    u32 target_mask = 1U << target_cpu;
    VirtAddr dist_base = platform::intc_dist_base();
    VirtAddr cpu_base = platform::intc_cpu_base();
    (void)hal::intc::send_sgi(dist_base, cpu_base, 0, target_mask);  // SGI 0 = Reschedule
}

// CFS run queue (red-black tree implementation)
class CfsRunqueue {
private:
  mutable containers::IrqSpinLock lock_;
  RbNode<Thread> *rb_root_;
  RbNode<Thread> *rb_leftmost_;

  u32 nr_running_;
  u64 min_vruntime_;
  u64 total_weight_;

  u64 load_sum_;
  u64 util_sum_;
  u32 load_avg_;
  u32 util_avg_;

  static constexpr usize MAX_NODES = 1024;
  RbNode<Thread> node_pool_[MAX_NODES];
  usize next_fresh_index_;      // Next unused slot in node_pool_
  RbNode<Thread> *free_list_;   // Singly-linked free list (reuses `left` ptr)

public:
  constexpr CfsRunqueue() noexcept
      : rb_root_(nullptr), rb_leftmost_(nullptr), nr_running_(0),
        min_vruntime_(0), total_weight_(0), load_sum_(0), util_sum_(0),
        load_avg_(0), util_avg_(0), next_fresh_index_(0), free_list_(nullptr) {}

  void enqueue_task(Thread *thread) noexcept {
    if (thread == nullptr)
      return;
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);

    if (thread->se.vruntime == 0 && thread->tid < 1001) {
      thread->se.vruntime = calc_initial_vruntime();
    }

    RbNode<Thread> *node = allocate_node(thread);
    if (node != nullptr) {
      rb_insert(node);
      nr_running_++;
      total_weight_ += thread->se.weight;

      update_load_stats(thread, true);
    }
  }

  void dequeue_task(Thread *thread) noexcept {
    if (thread == nullptr)
      return;
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);

    static u64 dequeue_count = 0;
    dequeue_count++;
    if (dequeue_count % 1000000 == 0 || (thread->tid >= 1001 && dequeue_count % 50000 == 0)) {
      log::klog::debug("dequeue_task: TID={} vruntime={} queue_size={}", static_cast<u32>(thread->tid), thread->se.vruntime, nr_running_);
    }

    RbNode<Thread> *node = find_node(thread);
    if (node != nullptr) {
      rb_remove(node);
      deallocate_node(node);
      nr_running_--;
      total_weight_ -= thread->se.weight;

      update_load_stats(thread, false);
    }
  }

  [[nodiscard]] Thread *pick_next_task() noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    if (rb_leftmost_ == nullptr) {
      return nullptr;
    }

    Thread *next = rb_leftmost_->data;
    if (next == nullptr) {
      return nullptr;
    }

    static u64 pick_debug = 0;
    pick_debug++;
    if (pick_debug % 1000000 == 0) {
      log::klog::debug("pick_next_task: selected TID={} vruntime={} nr_running={} min_vruntime={}", static_cast<u32>(next->tid), next->se.vruntime, nr_running_, min_vruntime_);
    }

    // Linux CFS: only select the task, do not remove it
    min_vruntime_ = moss::max(min_vruntime_, next->se.vruntime);

    return next;
  }

  void update_curr_task(Thread *current, u64 delta_exec) noexcept {
    if (current == nullptr)
      return;
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);

    u64 old_vruntime = current->se.vruntime;

    current->se.sum_exec_runtime += delta_exec;

    u64 weighted_delta = calc_delta_fair(delta_exec, current);
    current->se.vruntime += weighted_delta;

    u64 vruntime_diff = current->se.vruntime - old_vruntime;
    if (vruntime_diff > 5000) {
      RbNode<Thread>* node = find_node(current);
      if (node != nullptr) {
        rb_remove(node);
        rb_insert(node);

        log::klog::debug("task vruntime rebalance: TID={} old={} new={}", static_cast<u32>(current->tid), old_vruntime, current->se.vruntime);
      }
    }

    min_vruntime_ = kernel_max(min_vruntime_, current->se.vruntime);

    if (should_preempt_unlocked(current)) {
      // Set reschedule flag (in actual implementation)
    }

    update_load_tracking(current, delta_exec);
  }

  // Public version: acquires lock for external callers
  [[nodiscard]] bool should_preempt(Thread *current) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    return should_preempt_unlocked(current);
  }

private:
  // Internal version: no lock, called from within already-locked methods
  [[nodiscard]] bool should_preempt_unlocked(Thread *current) const noexcept {
    if (current == nullptr || rb_leftmost_ == nullptr) {
      return false;
    }

    Thread *leftmost = rb_leftmost_->data;
    if (leftmost == nullptr || leftmost == current) {
      return false;
    }

    u64 ideal_runtime = CfsParams::sched_slice(current->se.weight,
                                               static_cast<u32>(total_weight_));
    u64 delta_exec =
        current->se.sum_exec_runtime - current->se.prev_sum_exec_runtime;

    return delta_exec > ideal_runtime;
  }

public:

  [[nodiscard]] u32 nr_running() const noexcept { return nr_running_; }
  [[nodiscard]] u64 min_vruntime() const noexcept { return min_vruntime_; }
  [[nodiscard]] u64 total_weight() const noexcept { return total_weight_; }
  [[nodiscard]] u32 load_avg() const noexcept { return load_avg_; }
  [[nodiscard]] u32 util_avg() const noexcept { return util_avg_; }

  void dump_runqueue() const noexcept {
    // Debug output placeholder
  }

private:
  [[nodiscard]] u64 calc_initial_vruntime() const noexcept {
    return 100;
  }

  [[nodiscard]] u64 calc_delta_fair(u64 delta_exec,
                                    Thread *thread) const noexcept {
    if (thread->se.weight == 0)
      return delta_exec;

    return (delta_exec * CfsParams::NICE_TO_WEIGHT[20]) / thread->se.weight;
  }

  void update_load_tracking(Thread *thread, u64 delta_exec) noexcept {
    if (thread == nullptr)
      return;

    [[maybe_unused]] constexpr u64 LOAD_AVG_PERIOD = 32;
    constexpr u64 LOAD_AVG_MAX = 47742;

    thread->se.load_sum += delta_exec;
    thread->se.util_sum += delta_exec;

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

  void update_load_stats(Thread *thread, bool add) noexcept {
    if (thread == nullptr)
      return;

    if (add) {
      load_sum_ += thread->se.load_avg;
      util_sum_ += thread->se.util_avg;
    } else {
      load_sum_ = (load_sum_ > thread->se.load_avg)
                      ? (load_sum_ - thread->se.load_avg)
                      : 0;
      util_sum_ = (util_sum_ > thread->se.util_avg)
                      ? (util_sum_ - thread->se.util_avg)
                      : 0;
    }

    load_avg_ =
        static_cast<u32>((nr_running_ > 0) ? (load_sum_ / nr_running_) : 0);
    util_avg_ =
        static_cast<u32>((nr_running_ > 0) ? (util_sum_ / nr_running_) : 0);
  }

  void rb_insert(RbNode<Thread> *node) noexcept {
    if (node == nullptr || node->data == nullptr)
      return;

    RbNode<Thread> **new_node = &rb_root_;
    RbNode<Thread> *parent = nullptr;
    u64 vruntime = node->data->se.vruntime;

    while (*new_node != nullptr) {
      parent = *new_node;

      if (vruntime < parent->data->se.vruntime) {
        new_node = &parent->left;
      } else {
        new_node = &parent->right;
      }
    }

    *new_node = node;
    node->parent = parent;

    bool should_update_leftmost = false;

    if (rb_leftmost_ == nullptr) {
      should_update_leftmost = true;
    } else if (vruntime < rb_leftmost_->data->se.vruntime) {
      should_update_leftmost = true;
    } else if (vruntime == rb_leftmost_->data->se.vruntime) {
      if (node->data->tid < rb_leftmost_->data->tid) {
        should_update_leftmost = true;
      }
    }

    if (should_update_leftmost) {
      rb_leftmost_ = node;
    }

    rb_insert_fixup(node);
  }

  void rb_remove(RbNode<Thread> *node) noexcept {
    if (node == nullptr) return;

    bool was_leftmost = (node == rb_leftmost_);

    if (was_leftmost) {
      RbNode<Thread>* new_leftmost = rb_next(node);

      if (new_leftmost == nullptr) {
        new_leftmost = find_tree_minimum(rb_root_);
      }

      rb_leftmost_ = new_leftmost;
    }

    rb_delete_node(node);

    if (rb_root_ != nullptr && rb_leftmost_ == nullptr) {
      rb_leftmost_ = find_tree_minimum(rb_root_);
    }

    #ifdef DEBUG
    if (!verify_tree_consistency()) {
      log::klog::error("tree consistency check failed!");
    }
    #endif
  }

  [[nodiscard]] RbNode<Thread>* find_tree_minimum(RbNode<Thread>* root) const noexcept {
    if (root == nullptr) return nullptr;

    while (root->left != nullptr) {
      root = root->left;
    }
    return root;
  }

  [[nodiscard]] bool verify_tree_consistency() const noexcept {
    if (rb_root_ == nullptr) {
      return rb_leftmost_ == nullptr;
    }

    RbNode<Thread>* actual_min = find_tree_minimum(rb_root_);
    if (rb_leftmost_ != actual_min) {
      return false;
    }

    u32 actual_count = count_tree_nodes(rb_root_);
    if (actual_count != nr_running_) {
      return false;
    }

    return true;
  }

  [[nodiscard]] u32 count_tree_nodes(RbNode<Thread>* node) const noexcept {
    if (node == nullptr) return 0;
    return 1 + count_tree_nodes(node->left) + count_tree_nodes(node->right);
  }

  [[nodiscard]] RbNode<Thread> *find_node(Thread *thread) const noexcept {
    return find_node_linear(rb_root_, thread);
  }

  [[nodiscard]] RbNode<Thread> *find_node_linear(RbNode<Thread> *node, Thread *thread) const noexcept {
    if (node == nullptr) {
      return nullptr;
    }

    if (node->data == thread) {
      return node;
    }

    RbNode<Thread> *left_result = find_node_linear(node->left, thread);
    if (left_result != nullptr) {
      return left_result;
    }

    return find_node_linear(node->right, thread);
  }

  [[nodiscard]] RbNode<Thread> *rb_next(RbNode<Thread> *node) const noexcept {
    if (node == nullptr)
      return nullptr;

    if (node->right != nullptr) {
      node = node->right;
      while (node->left != nullptr) {
        node = node->left;
      }
      return node;
    }

    RbNode<Thread> *parent = node->parent;
    while (parent != nullptr && node == parent->right) {
      node = parent;
      parent = parent->parent;
    }

    return parent;
  }

  void rb_insert_fixup(RbNode<Thread> *node) noexcept {
    if (node != nullptr && node->parent == nullptr) {
      node->red = false;
    }
  }

  void rb_delete_node(RbNode<Thread> *node) noexcept {
    if (node == nullptr) return;

    RbNode<Thread>* replacement = nullptr;
    RbNode<Thread>* original_parent = node->parent;
    bool original_red = node->red;

    // Case 1: Leaf node
    if (node->left == nullptr && node->right == nullptr) {
      replacement = nullptr;
      replace_node_in_parent(node, nullptr);
    }
    // Case 2: Only right child
    else if (node->left == nullptr) {
      replacement = node->right;
      replace_node_in_parent(node, node->right);
      node->right->parent = original_parent;
    }
    // Case 3: Only left child
    else if (node->right == nullptr) {
      replacement = node->left;
      replace_node_in_parent(node, node->left);
      node->left->parent = original_parent;
    }
    // Case 4: Two children - find inorder successor
    else {
      RbNode<Thread>* successor = tree_minimum(node->right);
      original_red = successor->red;
      replacement = successor->right;

      if (successor->parent != node) {
        replace_node_in_parent(successor, successor->right);
        if (successor->right) {
          successor->right->parent = successor->parent;
        }

        successor->right = node->right;
        successor->right->parent = successor;
      } else {
        if (replacement) {
          replacement->parent = successor;
        }
      }

      replace_node_in_parent(node, successor);
      successor->left = node->left;
      successor->left->parent = successor;
      successor->red = node->red;
    }

    if (!original_red && replacement != nullptr) {
      rb_delete_fixup(replacement);
    }
  }

  void replace_node_in_parent(RbNode<Thread>* old_node, RbNode<Thread>* new_node) noexcept {
    if (old_node->parent == nullptr) {
      rb_root_ = new_node;
    } else if (old_node == old_node->parent->left) {
      old_node->parent->left = new_node;
    } else {
      old_node->parent->right = new_node;
    }
  }

  [[nodiscard]] RbNode<Thread>* tree_minimum(RbNode<Thread>* node) const noexcept {
    if (node == nullptr) return nullptr;

    while (node->left != nullptr) {
      node = node->left;
    }
    return node;
  }

  void rb_delete_fixup(RbNode<Thread>* node) noexcept {
    while (node != rb_root_ && node != nullptr && !node->red) {
      if (node == node->parent->left) {
        break;
      } else {
        break;
      }
    }

    if (node != nullptr) {
      node->red = false;
    }

    if (rb_root_ != nullptr) {
      rb_root_->red = false;
    }
  }

  [[nodiscard]] RbNode<Thread> *allocate_node(Thread *thread) noexcept {
    RbNode<Thread> *node = nullptr;

    // First try the free list (recycled nodes)
    if (free_list_ != nullptr) {
      node = free_list_;
      free_list_ = free_list_->left; // left used as next pointer
    } else if (next_fresh_index_ < MAX_NODES) {
      // Fall back to fresh pool allocation
      node = &node_pool_[next_fresh_index_++];
    } else {
      return nullptr; // Pool exhausted
    }

    node->data = thread;
    node->left = nullptr;
    node->right = nullptr;
    node->parent = nullptr;
    node->red = true;

    return node;
  }

  void deallocate_node(RbNode<Thread> *node) noexcept {
    if (node == nullptr) return;

    // Return node to free list for reuse
    node->data = nullptr;
    node->right = nullptr;
    node->parent = nullptr;
    node->red = false;
    node->left = free_list_; // Use left as next pointer
    free_list_ = node;
  }

  template <typename T>
  constexpr const T &kernel_max(const T &a, const T &b) noexcept {
    return (a < b) ? b : a;
  }
};

// CFS scheduler class
class CfsScheduler {
private:
  containers::PerCpuData<CfsRunqueue> runqueues_;
  containers::PerCpuData<IdleTask*> idle_tasks_;

  containers::PerCpuAtomicCounter<u64> total_switches_;
  containers::PerCpuAtomicCounter<u64> total_preemptions_;

  // Timer-driven scheduling tick
  timer::HrTimer sched_tick_;
  u64 tick_count_{0};

public:
  constexpr CfsScheduler() noexcept : idle_tasks_{nullptr} {}

  void enqueue_task(Thread *thread, u32 cpu) noexcept {
    if (thread == nullptr || cpu >= MAX_CPUS)
      return;

    runqueues_.get_cpu(cpu).enqueue_task(thread);
    thread->cpu = cpu;
    thread->state = ProcessState::Ready;
  }

  void dequeue_task(Thread *thread) noexcept {
    if (thread == nullptr)
      return;

    u32 cpu = thread->cpu;
    if (cpu < MAX_CPUS) {
      runqueues_.get_cpu(cpu).dequeue_task(thread);
    }
  }

  [[nodiscard]] Thread *pick_next_task(u32 cpu) noexcept {
    if (cpu >= MAX_CPUS)
      return nullptr;

    return runqueues_.get_cpu(cpu).pick_next_task();
  }

  inline void set_idle_task(u32 cpu_id, IdleTask* idle_task) noexcept {
    if (cpu_id >= MAX_CPUS)
      return;

    idle_tasks_.get_cpu(cpu_id) = idle_task;

    if (idle_task) {
      log::klog::info("set idle task CPU{}: TID={}", cpu_id, static_cast<u32>(idle_task->get_cpu_id()));
    } else {
      log::klog::info("set idle task CPU{}: TID=NULL", cpu_id);
    }
  }

  [[nodiscard]] IdleTask* get_idle_task(u32 cpu_id) const noexcept {
    if (cpu_id >= MAX_CPUS)
      return nullptr;
    return idle_tasks_.get_cpu(cpu_id);
  }

  [[nodiscard]] bool has_runnable_tasks(u32 cpu_id) const noexcept {
    if (cpu_id >= MAX_CPUS)
      return false;
    return runqueues_.get_cpu(cpu_id).nr_running() > 0;
  }

private:
  void execute_task_simplified(Thread* task, [[maybe_unused]] u32 cpu_id) noexcept {
    if (task == nullptr) return;

    // Simulate a realistic time slice so vruntime advances fairly.
    // Without this, fake test tasks accumulate negligible vruntime
    // and starve real user tasks (CFS always picks lowest vruntime).
    constexpr u64 SIMULATED_SLICE_NS = 6000000;  // 6ms = sched latency
    update_current(task, SIMULATED_SLICE_NS);

    set_current_task(task);
    record_context_switch();
  }

  void run_idle_task_simplified(IdleTask* idle_task, u32 cpu_id) noexcept {
    if (idle_task == nullptr) return;

    mark_cpu_idle(cpu_id, true);
    idle_task_loop(cpu_id);
    mark_cpu_idle(cpu_id, false);
  }

  void idle_task_loop([[maybe_unused]] u32 cpu_id) noexcept {
    arch::cpu_idle_once();
  }

  void mark_cpu_idle(u32 cpu_id, bool is_idle) noexcept {
    static bool cpu_idle_status[MAX_CPUS] = {false};
    if (cpu_id < MAX_CPUS) {
      cpu_idle_status[cpu_id] = is_idle;
    }
  }

public:
  [[noreturn]] inline void cpu_startup_entry(u32 cpu_id) noexcept {
    log::klog::info("CPU{}: per-CPU scheduling loop started", cpu_id);

    if (cpu_id >= MAX_CPUS) {
      log::klog::error("CPU{}: invalid CPU ID", cpu_id);
      while (true) {
        idle_task_loop(cpu_id);
      }
    }

    IdleTask* idle_task = get_idle_task(cpu_id);
    if (idle_task == nullptr) {
      log::klog::warn("CPU{}: idle task not set, creating default", cpu_id);

      idle_task = create_idle_task(cpu_id);
      if (idle_task) {
        set_idle_task(cpu_id, idle_task);
      }
    }

    log::klog::info("CPU{}: entering per-CPU scheduling loop", cpu_id);

    u32 idle_cycles = 0;
    u32 active_cycles = 0;
    constexpr u32 LOG_INTERVAL = 2000000;

    while (true) {
      Thread* next_task = nullptr;

      if (has_runnable_tasks(cpu_id)) {
        next_task = pick_next_task(cpu_id);

        if (next_task != nullptr) {
          active_cycles++;

          if (active_cycles % LOG_INTERVAL == 1) {
            log::klog::debug("[CPU{}][TID={}] task running", cpu_id, static_cast<u32>(next_task->tid));
          }

          // User tasks (e.g. init/shell) need a real context switch —
          // switch_to_user/eret for first entry, context_switch for
          // subsequent dispatches.  execute_task_simplified only fakes
          // the accounting without actually giving the CPU to the task.
          if (next_task->is_user_task) {
            dequeue_task(next_task);
            context_switch_to_task(next_task);
            // Returns here when this CPU's bootstrap context is restored
            // after the user task is preempted by a timer IRQ.
          } else {
            execute_task_simplified(next_task, cpu_id);
          }
        }
      }

      if (next_task == nullptr) {
        idle_cycles++;

        if (idle_cycles % LOG_INTERVAL == 1) {
          log::klog::debug("[CPU{}] entering idle", cpu_id);
        }

        if (idle_task != nullptr) {
          run_idle_task_simplified(idle_task, cpu_id);
        } else {
          idle_task_loop(cpu_id);
        }
      }

      u32 total_cycles = active_cycles + idle_cycles;
      if (total_cycles > 0 && total_cycles % (LOG_INTERVAL * 5) == 0) {
        log::klog::info("[CPU{}] stats: active={} idle={} tasks={}", cpu_id, active_cycles, idle_cycles, get_cpu_nr_running(cpu_id));
      }
    }
  }

  // State transition management
  void task_blocked(Thread* task) noexcept {
    if (task == nullptr) return;

    ProcessState old_state = task->state;
    task->state = ProcessState::Blocked;

    if (old_state == ProcessState::Running || old_state == ProcessState::Ready) {
      dequeue_task(task);
      log::klog::info("task blocked and dequeued: TID={}", static_cast<u32>(task->tid));
    }
  }

  void task_wakeup(Thread* task, u32 target_cpu) noexcept {
    if (task == nullptr || task->state != ProcessState::Blocked) return;

    task->state = ProcessState::Ready;
    enqueue_task(task, target_cpu);

    log::klog::info("task wakeup and enqueued: TID={} CPU={}", static_cast<u32>(task->tid), target_cpu);
  }

  void task_terminate(Thread* task) noexcept {
    if (task == nullptr) return;

    ProcessState old_state = task->state;
    task->state = ProcessState::Terminated;

    if (old_state == ProcessState::Running || old_state == ProcessState::Ready) {
      dequeue_task(task);
      log::klog::info("task terminated and dequeued: TID={}", static_cast<u32>(task->tid));
    }
  }

  [[nodiscard]] bool should_preempt(Thread* current, u32 cpu) noexcept {
    if (current == nullptr || current->state != ProcessState::Running) {
      return true;
    }

    Thread* leftmost = pick_next_task(cpu);
    if (leftmost == nullptr || leftmost == current) {
      return false;
    }

    const u64 preempt_threshold = CfsParams::SCHED_LATENCY_NS / 2;
    u64 vruntime_diff = current->se.vruntime - leftmost->se.vruntime;

    return vruntime_diff > preempt_threshold;
  }

  void transition_task_state(Thread* task, ProcessState new_state) noexcept {
    if (task == nullptr) return;

    ProcessState old_state = task->state;

    bool valid_transition = false;
    switch (old_state) {
      case ProcessState::Created:
        valid_transition = (new_state == ProcessState::Ready);
        break;
      case ProcessState::Ready:
        valid_transition = (new_state == ProcessState::Running ||
                          new_state == ProcessState::Blocked ||
                          new_state == ProcessState::Terminated);
        break;
      case ProcessState::Running:
        valid_transition = (new_state == ProcessState::Ready ||
                          new_state == ProcessState::Blocked ||
                          new_state == ProcessState::Terminated);
        break;
      case ProcessState::Blocked:
        valid_transition = (new_state == ProcessState::Ready ||
                          new_state == ProcessState::Terminated);
        break;
      case ProcessState::Terminated:
        valid_transition = (new_state == ProcessState::Zombie);
        break;
      case ProcessState::Zombie:
        valid_transition = false;
        break;
      default:
        valid_transition = false;
    }

    if (valid_transition) {
      task->state = new_state;
    } else {
      log::klog::warn("invalid state transition: {} -> {}", static_cast<u32>(static_cast<u8>(old_state)), static_cast<u32>(static_cast<u8>(new_state)));
    }
  }

  void update_current(Thread *current, u64 delta_exec) noexcept {
    if (current == nullptr)
      return;

    u32 cpu = current->cpu;
    if (cpu < MAX_CPUS) {
      runqueues_.get_cpu(cpu).update_curr_task(current, delta_exec);
    }
  }

  [[nodiscard]] bool should_preempt_current(Thread *current) noexcept {
    if (current == nullptr)
      return false;

    u32 cpu = current->cpu;
    if (cpu >= MAX_CPUS)
      return false;

    return runqueues_.get_cpu(cpu).should_preempt(current);
  }

  [[nodiscard]] u32 get_cpu_load(u32 cpu) const noexcept {
    if (cpu >= MAX_CPUS)
      return 0;
    return runqueues_.get_cpu(cpu).load_avg();
  }

  [[nodiscard]] u32 get_cpu_nr_running(u32 cpu) const noexcept {
    if (cpu >= MAX_CPUS)
      return 0;
    return runqueues_.get_cpu(cpu).nr_running();
  }

  [[nodiscard]] u64 total_context_switches() const noexcept {
    return total_switches_.load_total();
  }

  [[nodiscard]] u64 total_preemptions() const noexcept {
    return total_preemptions_.load_total();
  }

  void dump_runqueue(u32 cpu) const noexcept {
    if (cpu < MAX_CPUS) {
      runqueues_.get_cpu(cpu).dump_runqueue();
    }
  }

  void record_context_switch() noexcept {
    (void)total_switches_.fetch_add_local(1);
  }

  void record_preemption() noexcept {
    (void)total_preemptions_.fetch_add_local(1);
  }

  // Create 20 test tasks to verify scheduler
  void create_test_task() noexcept {
    early_debug_print("[sched] creating 20 test tasks...\n");

    bool found_user_task = false;
    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
      Thread* current_task = current_running_tasks_[cpu];
      if (current_task != nullptr && current_task->tid == 1000) {
        current_task->se.vruntime = 100;
        found_user_task = true;
        break;
      }
    }

    (void)found_user_task;

    // 32KB per stack — IRQ handling (irq_trampoline 272B + scheduler_tick
    // + RB-tree ops + logging) runs on the interrupted task's stack.
    alignas(16) static char test_task_stacks[20][32768];
    static Thread* test_threads[20];

    u32 created_tasks = 0;
    u32 failed_tasks = 0;

    for (u32 i = 0; i < 20; i++) {
      u32 tid = 1001 + i;

      test_threads[i] = new Thread(tid, 1);
      if (test_threads[i] == nullptr) {
        failed_tasks++;
        continue;
      }

      test_threads[i]->stack_base = reinterpret_cast<VirtAddr>(test_task_stacks[i]);
      test_threads[i]->stack_size = sizeof(test_task_stacks[i]);

      test_threads[i]->context.sp = reinterpret_cast<u64>(test_task_stacks[i] + sizeof(test_task_stacks[i]) - 16);
      test_threads[i]->context.pc = reinterpret_cast<u64>(&test_task_entry);
#if defined(MOSS_ARCH_ARM64)
      test_threads[i]->context.x[30] = reinterpret_cast<u64>(&test_task_entry);  // LR = entry for context_switch ret
#endif
      test_threads[i]->context.pstate = 0x00000000;  // DAIF=0: all interrupts unmasked

      test_threads[i]->sched_class = process::SchedClass::Normal;

      test_threads[i]->se.nice = 0;
      test_threads[i]->se.weight = CfsParams::nice_to_weight(0);

      test_threads[i]->se.vruntime = static_cast<u64>(i);

      u32 target_cpu = i % MAX_CPUS;
      enqueue_task(test_threads[i], target_cpu);

      (void)tid;
      created_tasks++;
    }

    // Summary via direct UART (one message instead of many klog calls)
    char msg[80];
    u32 pos = 0;
    constexpr u32 CAP = sizeof(msg);
    fmt_str(msg, pos, CAP, "[sched] test tasks: created=");
    fmt_u64(msg, pos, CAP, created_tasks);
    fmt_str(msg, pos, CAP, " failed=");
    fmt_u64(msg, pos, CAP, failed_tasks);
    fmt_str(msg, pos, CAP, "\n");
    msg[pos] = '\0';
    early_debug_print(msg);
  }

  void verify_task_diversity() noexcept {
    // Quick sanity check: count per-CPU queues via direct UART.
    u32 total = 0;
    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++)
      total += get_cpu_nr_running(cpu);

    char msg[64];
    u32 pos = 0;
    constexpr u32 CAP = sizeof(msg);
    fmt_str(msg, pos, CAP, "[sched] total queued tasks: ");
    fmt_u64(msg, pos, CAP, total);
    fmt_str(msg, pos, CAP, "\n");
    msg[pos] = '\0';
    early_debug_print(msg);
  }

private:
  static Thread* current_running_tasks_[MAX_CPUS];

  // Per-CPU bootstrap context — used as "prev" save target when there is
  // no current task (e.g. schedule_after_exit or first dispatch).
  // context_switch() saves the caller's registers here; the new task
  // starts on its own stack.  Restoring bootstrap returns to the caller.
  static CpuContext bootstrap_contexts_[MAX_CPUS];

public:
  static CpuContext& bootstrap_context(u32 cpu) noexcept {
    return bootstrap_contexts_[cpu % MAX_CPUS];
  }

  static void set_current_task(Thread* task) noexcept {
    u32 cpu = CfsScheduler::get_current_cpu_id();
    if (cpu < MAX_CPUS) {
      current_running_tasks_[cpu] = task;
    }
  }

  static Thread* get_current_task() noexcept {
    u32 cpu = CfsScheduler::get_current_cpu_id();
    return (cpu < MAX_CPUS) ? current_running_tasks_[cpu] : nullptr;
  }

  [[noreturn]] static void test_task_entry() noexcept {
    // Enable IRQs: context_switch does NOT restore DAIF, so when a fresh
    // task is first-started from inside an IRQ handler (DAIF.I=1), we
    // arrive here with interrupts masked.  Unmask IRQs so the timer can
    // preempt us.
#if defined(MOSS_ARCH_ARM64)
    asm volatile("msr daifclr, #2" ::: "memory");
#endif

    Thread* current_task = get_current_task();
    u32 task_tid = (current_task != nullptr) ? static_cast<u32>(current_task->tid) : 0;
    i32 task_nice = (current_task != nullptr) ? current_task->se.nice : 0;
    u32 task_weight = (current_task != nullptr) ? current_task->se.weight : 1024;

    (void)task_tid;
    (void)task_nice;
    (void)task_weight;

    while (true) {
      if (task_nice < 0) {
        for (volatile int work = 0; work < 1000; work = work + 1) {
        }
      } else if (task_nice > 5) {
        for (volatile int work = 0; work < 100; work = work + 1) {
        }
      }

      yield_cpu();
    }
  }

  static void yield_cpu() noexcept {
    arch::cpu_yield();

    for (volatile int i = 0; i < 10000; i = i + 1) {
    }
  }

  // ---- GIC timer IRQ handler ----
  // Bridges the hardware interrupt from GIC to the TimerSubsystem.
  // GIC calls this when timer IRQ fires; we ack the hardware timer
  // and dispatch all expired HrTimer callbacks (including sched_tick_).
  static void timer_irq_handler(u32 /*irq*/, void* /*context*/) noexcept {
    hal::timer::ack_interrupt();
    timer::TimerSubsystem::instance().handle_interrupt();
  }

  // ---- Timer-driven scheduler tick callback ----
  // Called from timer interrupt context every SCHED_LATENCY_NS (6ms).
  // Performs one scheduling round: pick next task, update vruntime,
  // context-switch if needed.
  static void scheduler_tick_callback(void* data) noexcept {
    auto* sched = static_cast<CfsScheduler*>(data);
    sched->scheduler_tick();
  }

  void scheduler_tick() noexcept {
    tick_count_++;
    u32 cpu = get_current_cpu_id();
    Thread *curr = get_current_task();

    if (curr == nullptr) {
      Thread *next = pick_next_task(cpu);
      if (next != nullptr) {
        dequeue_task(next);
        context_switch_to_task(next);
      }
      return;
    }

    // Update vruntime for the currently running task
    if (curr->state == ProcessState::Running) {
      u64 now = get_current_time();
      u64 delta = (now > curr->se.exec_start) ? (now - curr->se.exec_start) : 1000;
      curr->se.exec_start = now;
      update_current(curr, delta);

      // Check if a higher-priority task is waiting (CFS: lower vruntime)
      if (should_preempt_current(curr)) {
        // Guard: task may have been marked Terminated by sys_exit
        // between our state==Running check above and here.
        if (curr->state == ProcessState::Terminated) return;

        // Reset time-slice accounting so curr gets a fresh slice next time
        curr->se.prev_sum_exec_runtime = curr->se.sum_exec_runtime;
        curr->state = ProcessState::Ready;
        record_preemption();

        // Re-enqueue current task, pick the next one
        enqueue_task(curr, cpu);
        Thread *next = pick_next_task(cpu);
        if (next != nullptr && next != curr) {
          dequeue_task(next);
          context_switch_to_task(next);
          // Returns here when curr is scheduled again.
        }
      }
    }

  }

  [[noreturn]] void start_scheduling() noexcept {
    // Use direct UART for all boot-path messages to avoid ring-buffer
    // lock contention with secondary CPUs (IRQ livelock root cause).
    early_debug_print("[sched] CFS scheduler starting\n");

    u32 current_cpu = CfsScheduler::get_current_cpu_id();

    // Create synthetic test tasks
    early_debug_print("[sched] creating test tasks...\n");
    create_test_task();
    early_debug_print("[sched] test tasks created\n");

    verify_task_diversity();

    // Arm the periodic scheduler tick timer
    if (timer::TimerSubsystem::instance().is_initialized()) {
      // Step 1: Register timer IRQ handler with GIC
      u32 timer_irq = hal::timer::irq_number();
      early_debug_print("[sched] registering timer IRQ handler\n");

      if (interrupts::g_gic) {
        auto reg_result = interrupts::g_gic->register_interrupt(
            timer_irq, timer_irq_handler, nullptr, "sched_timer");
        if (!reg_result) {
          early_debug_print("[sched] WARN: failed to register timer IRQ\n");
        } else {
          auto en_result = interrupts::g_gic->enable_interrupt(timer_irq);
          if (!en_result) {
            early_debug_print("[sched] WARN: failed to enable timer IRQ\n");
          } else {
            early_debug_print("[sched] timer IRQ registered and enabled\n");
          }
        }
      } else {
        early_debug_print("[sched] WARN: GIC not available\n");
      }

      // Step 2: Arm the periodic scheduler tick HrTimer
      early_debug_print("[sched] arming scheduler tick timer (6ms period)\n");

      sched_tick_.init(timer::TimerMode::Periodic,
                       scheduler_tick_callback, this);
      sched_tick_.start_relative(CfsParams::SCHED_LATENCY_NS);

      early_debug_print("[sched] tick armed, entering idle loop\n");

      // Before entering idle, dispatch the init user process (TID=1000).
      // We explicitly search for it because test tasks may have lower
      // vruntime values and would otherwise be selected first by CFS.
      {
        // Scan CPU 0's runqueue for TID=1000
        Thread *init_task = nullptr;
        for (u32 cpu = 0; cpu < MAX_CPUS && init_task == nullptr; cpu++) {
          // Try picking tasks from this CPU until we find TID=1000 or exhaust
          constexpr u32 MAX_SCAN = 32;
          Thread *stash[MAX_SCAN];
          u32 stash_count = 0;

          for (u32 s = 0; s < MAX_SCAN; s++) {
            Thread *t = pick_next_task(cpu);
            if (t == nullptr) break;
            dequeue_task(t);
            if (t->tid == 1000) {
              init_task = t;
              break;
            }
            stash[stash_count++] = t;
          }
          // Re-enqueue any tasks we pulled out
          for (u32 s = 0; s < stash_count; s++) {
            enqueue_task(stash[s], cpu);
          }
        }

        if (init_task != nullptr) {
          early_debug_print("[sched] dispatching init TID=1000\n");
          context_switch_to_task(init_task);
          // switch_to_user does eret and never returns for user tasks.
          // If we somehow get here (shouldn't), re-enqueue.
          enqueue_task(init_task, current_cpu);
        } else {
          early_debug_print("[sched] WARN: init TID=1000 not found\n");
        }
      }

      // BSP enters the same per-CPU scheduling loop as secondary CPUs.
      // This ensures that user tasks re-enqueued after preemption (e.g. init/shell)
      // are properly dispatched via context_switch_to_task, not just
      // execute_task_simplified.
      early_debug_print("[BSP] entering scheduling loop\n");
      cpu_startup_entry(current_cpu);
    }

    // Fallback: if timer is not available, use the legacy busy-wait loop
    early_debug_print("[sched] WARN: timer unavailable, fallback busy-wait\n");
    fallback_busy_wait_scheduling(current_cpu);
  }

  // Legacy busy-wait scheduling loop (fallback when timer is unavailable)
  [[noreturn]] void fallback_busy_wait_scheduling(u32 current_cpu) noexcept {
    u32 active_cycles = 0;
    constexpr u32 LOG_INTERVAL = 1000000;

    while (true) {
      current_cpu = CfsScheduler::get_current_cpu_id();

      Thread *next_task = pick_next_task(current_cpu);
      if (next_task != nullptr) {
        dequeue_task(next_task);
        active_cycles++;

        if (next_task->tid >= 1001 && next_task->tid <= 1020) {
          u64 start_time = CfsScheduler::get_current_time();
          i32 nice = next_task->se.nice;
          u32 work_amount = 1000;
          if (nice < 0) work_amount = 1500;
          else if (nice > 5) work_amount = 500;

          for (volatile u32 work = 0; work < work_amount; work = work + 1) {}

          u64 end_time = CfsScheduler::get_current_time();
          u64 delta_exec = (end_time > start_time) ? (end_time - start_time) : 1000;
          update_current(next_task, delta_exec);
          enqueue_task(next_task, current_cpu);
        }

        if (active_cycles == 1 || (active_cycles % LOG_INTERVAL == 0)) {
          log::klog::info("CPU{}: task TID={} (active: {})", current_cpu, next_task->tid, active_cycles);
        }

        CfsScheduler::set_current_task(next_task);
        record_context_switch();
      } else {
        idle_task_loop(current_cpu);
      }
    }
  }

private:

  void context_switch_to_task(Thread *task) noexcept {
    if (task == nullptr)
      return;

    // Guard: never switch to a terminated task (e.g. sys_exit race)
    if (task->state == ProcessState::Terminated)
      return;

#if defined(MOSS_ARCH_ARM64)
    // Save prev BEFORE updating current — context_switch needs it
    Thread *prev = get_current_task();
#endif

    CfsScheduler::set_current_task(task);
    task->state = ProcessState::Running;
    task->se.exec_start = get_current_time();  // Reset for vruntime accounting
    record_context_switch();

    if (task->needs_initial_eret) {
      // First entry into user space — set up TTBR0 and eret to EL0.
      // Subsequent dispatches (after IRQ preemption) go through the normal
      // context_switch path; irq_trampoline's eret returns to EL0.
      //
      // CRITICAL: Disable IRQs for the entire sequence.  A timer IRQ between
      // set_current_task() and switch_to_user() would trigger scheduler_tick
      // which calls context_switch(&this_task->context, ...), overwriting
      // context.pc with a kernel LR.  switch_to_user's eret restores SPSR
      // with DAIF=0, so IRQs are re-enabled upon entering EL0.
      task->needs_initial_eret = false;
#if defined(MOSS_ARCH_ARM64)
      arch::disable_interrupts();

      Process *proc = g_process_manager ? g_process_manager->find_process(task->owner_pid) : nullptr;
      if (proc && proc->address_space() && proc->address_space()->pgd_phys != 0) {
        u64 ttbr0_val = proc->address_space()->pgd_phys
                      | (static_cast<u64>(proc->address_space()->asid) << 48);
        asm volatile("msr ttbr0_el1, %0" :: "r"(ttbr0_val));
        asm volatile("isb" ::: "memory");
      } else {
        log::klog::error("TTBR0 switch FAILED: proc={} as={} pgd={}",
                         proc != nullptr, proc ? (proc->address_space() != nullptr) : false,
                         proc && proc->address_space() ? proc->address_space()->pgd_phys : 0ULL);
      }

      // Set TPIDR_EL1 to the per-thread kernel stack top.
      // switch_to_user reads TPIDR_EL1 to set SP_EL1 before eret.
      // When the next exception from EL0 occurs, SP_EL1 will be this
      // thread's dedicated kernel stack — not the shared boot stack.
      if (task->kernel_stack_base != 0) {
        u64 kstack_top = task->kernel_stack_top();
        asm volatile("msr tpidr_el1, %0" :: "r"(kstack_top));
      }
#endif
      switch_to_user(&task->context, task->stack_base + task->stack_size - 16);
    } else {
#if defined(MOSS_ARCH_ARM64)
      // Kernel-to-kernel context switch via assembly.
      // When prev is null (e.g. schedule_after_exit) or self-switch,
      // use per-CPU bootstrap context as throwaway save target.
      // This ensures the new task starts on its OWN stack.
      CpuContext *prev_ctx;
      if (prev != nullptr && prev != task) {
        prev_ctx = &prev->context;
      } else {
        prev_ctx = &bootstrap_contexts_[get_current_cpu_id()];
      }

      // For user tasks being re-dispatched after preemption:
      // Set TTBR0 to this process's page tables BEFORE context_switch.
      // context_switch restores regs → ret into irq_trampoline → eret to EL0.
      // Without this, the user task would resume with the wrong (or kernel) page tables.
      if (task->is_user_task) {
        Process *proc = g_process_manager ? g_process_manager->find_process(task->owner_pid) : nullptr;
        if (proc && proc->address_space() && proc->address_space()->pgd_phys != 0) {
          u64 ttbr0_val = proc->address_space()->pgd_phys
                        | (static_cast<u64>(proc->address_space()->asid) << 48);
          asm volatile("msr ttbr0_el1, %0" :: "r"(ttbr0_val));
          asm volatile("isb" ::: "memory");
        }

        // Set TPIDR_EL1 for per-thread kernel stack.
        // After context_switch restores this task → ret into irq_trampoline
        // → irq_trampoline's add sp + eret → SP_EL1 = kernel_stack_top.
        // The TPIDR_EL1 value is not used by context_switch itself, but
        // will be read by switch_to_user on future first-entry paths or
        // by irq_trampoline exit to verify SP correctness.
        if (task->kernel_stack_base != 0) {
          u64 kstack_top = task->kernel_stack_top();
          asm volatile("msr tpidr_el1, %0" :: "r"(kstack_top));
        }
      }

      // Mask IRQs before context_switch.  context_switch does NOT
      // touch DAIF, so the new task inherits IRQ-masked state.
      // Fresh tasks explicitly unmask in test_task_entry().
      // Resumed tasks are inside irq_trampoline and eret restores
      // the pre-IRQ PSTATE (with DAIF=0).
      arch::disable_interrupts();
      context_switch(prev_ctx, &task->context);
      // Returns here when prev_ctx is scheduled again.
      // Re-enable IRQs for the returned-to context.
      arch::enable_interrupts();
#else
      // Non-ARM64: direct call (no asm context_switch yet)
      test_task_entry();
#endif
    }
  }

  void check_need_resched() noexcept {
    u32 current_cpu = CfsScheduler::get_current_cpu_id();

    if (runqueues_.get_cpu(current_cpu).nr_running() > 0) {
      // Other tasks waiting, may need preemption
    }
  }

  [[nodiscard]] static u32 get_current_cpu_id() noexcept {
    return arch::get_current_cpu_id();
  }

  [[nodiscard]] static u64 get_current_time() noexcept {
    return arch::get_timestamp_counter();
  }

public:
  // Called after a process exits (sys_exit).  Picks the next runnable task
  // and switches to it.  Never returns to the caller because the exited
  // task's context is no longer valid.
  [[noreturn]] void schedule_after_exit() noexcept {
    early_debug_print("[sched] schedule_after_exit() entered\n");
    u32 cpu = get_current_cpu_id();

    // Clear current task — the old one is dead
    set_current_task(nullptr);

    // Ensure interrupts are enabled so timer ticks can fire and
    // re-enqueue tasks while we idle.
    arch::enable_interrupts();

    // Loop: find a runnable task, switch to it.  When bootstrap context
    // is restored (the task was preempted away or exited), try next.
    u64 last_idle_log_ns = timer::TimerSubsystem::instance().now_ns();
    while (true) {
      Thread *next = pick_next_task(cpu);
      if (next != nullptr) {
        dequeue_task(next);
        early_debug_print("[sched] schedule_after_exit: dispatching task\n");
        context_switch_to_task(next);
        // context_switch returned — bootstrap context restored.
        // The task was preempted or exited.  Re-clear and retry.
        set_current_task(nullptr);
      } else {
        // No runnable tasks: idle until timer interrupt enqueues work
        arch::cpu_idle_once();
        u64 now = timer::TimerSubsystem::instance().now_ns();
        if (now - last_idle_log_ns >= 1000000000ULL) {
          last_idle_log_ns = now;
          idle_heartbeat_print("exit", cpu, now / 1000000);
        }
      }
    }
  }
};

// Global CFS scheduler instance
extern CfsScheduler *g_scheduler;

// Secondary CPU scheduling entry point -- called from boot_impl.cpp
[[noreturn]] void secondary_cpu_schedule_loop(u32 cpu_id) noexcept;

// current_thread / current_process implementation (needs CfsScheduler to be defined)
inline Thread *current_thread() noexcept {
  return CfsScheduler::get_current_task();
}
inline Process *current_process() noexcept {
  Thread *t = current_thread();
  if (!t || !g_process_manager) return nullptr;
  return g_process_manager->find_process(t->owner_pid);
}

} // namespace moss::kernel::process
