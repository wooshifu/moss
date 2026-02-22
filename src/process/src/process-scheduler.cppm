// MOSS Process Module - Partition: scheduler
// CFS Scheduler, Idle Task, CfsRunqueue, CfsScheduler

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
import moss.abi;
import moss.platform;
import moss.hal.intc;
import moss.hal.timer;
import moss.timer;
import moss.logging;

// Assembly/entry symbols from moss.abi — bring into scope for this partition
using moss::abi::context_switch;
using moss::abi::switch_to_user;
using moss::abi::entry::early_debug_print;
#if defined(MOSS_ARCH_ARM64)
using moss::abi::arm64::user_eret_trampoline;
#endif

// ============================================================================
// cfs_scheduler.hpp - CFS (Completely Fair Scheduler)
// ============================================================================
export namespace moss::kernel::process {

namespace log = moss::kernel::logging;

// CFS scheduling parameters
namespace cfs_params {
inline constexpr u64 SCHED_LATENCY_NS = 6000000;  // 6ms
inline constexpr u64 MIN_GRANULARITY_NS = 750000; // 0.75ms
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

inline constexpr u32 nice_to_weight_index(i32 nice) { return static_cast<u32>(nice + 20); }

inline constexpr u32 nice_to_weight(i32 nice) {
  u32 index = nice_to_weight_index(nice);
  return (index < 40) ? NICE_TO_WEIGHT[index] : 1;
}

// Adaptive scheduling period: when nr_running exceeds SCHED_NR_LATENCY,
// grow linearly to avoid excessively short time slices.
inline constexpr u64 sched_period(u32 nr_running) {
  if (nr_running > SCHED_NR_LATENCY) {
    return static_cast<u64>(nr_running) * MIN_GRANULARITY_NS;
  }
  return SCHED_LATENCY_NS;
}

inline constexpr u64 sched_slice(u32 weight, u32 total_weight, u32 nr_running = SCHED_NR_LATENCY) {
  if (total_weight == 0) {
    return MIN_GRANULARITY_NS;
  }

  u64 period = sched_period(nr_running);
  u64 slice = (period * weight) / total_weight;
  return (slice < MIN_GRANULARITY_NS) ? MIN_GRANULARITY_NS : slice;
}
} // namespace cfs_params

// Red-black tree node (simplified implementation)
template <typename T> struct RbNode {
  T *data;
  RbNode *left;
  RbNode *right;
  RbNode *parent;
  bool red;

  constexpr RbNode() noexcept : data(nullptr), left(nullptr), right(nullptr), parent(nullptr), red(true) {}

  constexpr RbNode(T *d) noexcept : data(d), left(nullptr), right(nullptr), parent(nullptr), red(true) {}
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
  IdleTask(const IdleTask &) = delete;
  IdleTask &operator=(const IdleTask &) = delete;
  IdleTask(IdleTask &&) = delete;
  IdleTask &operator=(IdleTask &&) = delete;

  [[noreturn]] void run() const noexcept;

  u32 get_cpu_id() const noexcept { return cpu_id_; }

  u64 get_idle_time_ns() const noexcept { return idle_time_ns_; }

  // Snapshot idle time including in-progress idle period (for topinfo)
  u64 snapshot_idle_time_ns(u64 now_ns) const noexcept {
    u64 total = idle_time_ns_;
    u64 start = last_idle_start_;
    if (start != 0 && now_ns > start) {
      total += (now_ns - start);
    }
    return total;
  }

  void accumulate_idle_time(u64 delta_ns) noexcept { idle_time_ns_ += delta_ns; }

  // Mark idle entry — called when CPU enters idle (before WFI)
  void enter_idle(u64 now_ns) noexcept { last_idle_start_ = now_ns; }

  // Mark idle exit — called when CPU exits idle (WFI returned)
  void exit_idle(u64 now_ns) noexcept {
    if (last_idle_start_ != 0 && now_ns > last_idle_start_) {
      idle_time_ns_ += (now_ns - last_idle_start_);
    }
    last_idle_start_ = 0;
  }

  void reset_idle_time() noexcept { idle_time_ns_ = 0; }

private:
  u32 cpu_id_;
  u64 idle_time_ns_;
  u64 last_idle_start_;
};

// Linux-style do_idle function
[[noreturn]] void do_idle(u32 cpu_id) noexcept;

// Create idle task for specified CPU
IdleTask *create_idle_task(u32 cpu_id) noexcept;

// Global idle task management
extern moss::kernel::containers::PerCpuData<IdleTask *> g_idle_tasks;

// Get idle task for specified CPU
inline IdleTask *get_idle_task(u32 cpu_id) noexcept {
  if (cpu_id >= moss::kernel::MAX_CPUS) {
    return nullptr;
  }
  return g_idle_tasks.get_cpu(cpu_id);
}

// Set idle task for specified CPU
inline bool set_idle_task(u32 cpu_id, IdleTask *idle_task) noexcept {
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
  if (target_cpu >= MAX_CPUS || target_cpu == arch::get_current_cpu_id()) {
    return;
  }
  u32 target_mask = 1U << target_cpu;
  VirtAddr dist_base = platform::intc_dist_base();
  VirtAddr cpu_base = platform::intc_cpu_base();
  (void)hal::intc::send_sgi(dist_base, cpu_base, 0, target_mask); // SGI 0 = Reschedule
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
  usize next_fresh_index_;    // Next unused slot in node_pool_
  RbNode<Thread> *free_list_; // Singly-linked free list (reuses `left` ptr)

public:
  constexpr CfsRunqueue() noexcept
      : rb_root_(nullptr), rb_leftmost_(nullptr), nr_running_(0), min_vruntime_(0), total_weight_(0), load_sum_(0),
        util_sum_(0), load_avg_(0), util_avg_(0), next_fresh_index_(0), free_list_(nullptr) {}

  void enqueue_task(Thread *thread) noexcept {
    if (thread == nullptr) {
      return;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);

    if (thread->se.vruntime == 0) {
      thread->se.vruntime = calc_initial_vruntime();
    }

    RbNode<Thread> *node = allocate_node(thread);
    if (node == nullptr) {
      log::klog::error("CfsRunqueue: node pool exhausted, TID={}", static_cast<u32>(thread->tid));
      return;
    }

    thread->rq_node = static_cast<void *>(node);
    rb_insert(node);
    nr_running_++;
    total_weight_ += thread->se.weight;

    update_load_stats(thread, true);
  }

  void dequeue_task(Thread *thread) noexcept {
    if (thread == nullptr) {
      return;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);

    auto *node = static_cast<RbNode<Thread> *>(thread->rq_node);
    if (node == nullptr) {
      return; // not in this queue
    }

    thread->rq_node = nullptr;
    rb_remove(node);
    deallocate_node(node);
    nr_running_--;
    total_weight_ -= thread->se.weight;

    update_load_stats(thread, false);
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
      log::klog::debug("pick_next_task: selected TID={} vruntime={} nr_running={} min_vruntime={}",
                       static_cast<u32>(next->tid), next->se.vruntime, nr_running_, min_vruntime_);
    }

    // Linux CFS: only select the task, do not remove it
    min_vruntime_ = moss::max(min_vruntime_, next->se.vruntime);

    return next;
  }

  void update_curr_task(Thread *current, u64 delta_exec) noexcept {
    if (current == nullptr) {
      return;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);

    current->se.sum_exec_runtime += delta_exec;

    u64 weighted_delta = calc_delta_fair(delta_exec, current);
    current->se.vruntime += weighted_delta;

    // Re-position the node in the tree if vruntime changed
    auto *node = static_cast<RbNode<Thread> *>(current->rq_node);
    if (node != nullptr) {
      rb_remove(node);
      rb_insert(node);
    }

    // min_vruntime: max(current, min(curr, leftmost)) — monotonically increasing
    u64 vmin = current->se.vruntime;
    if (rb_leftmost_ != nullptr) {
      vmin = kernel_min(vmin, rb_leftmost_->data->se.vruntime);
    }
    min_vruntime_ = kernel_max(min_vruntime_, vmin);

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

    u64 ideal_runtime = cfs_params::sched_slice(current->se.weight, static_cast<u32>(total_weight_), nr_running_);
    u64 delta_exec = current->se.sum_exec_runtime - current->se.prev_sum_exec_runtime;

    if (delta_exec > ideal_runtime) {
      return true;
    }

    // vruntime-based preemption: if leftmost has much lower vruntime, preempt.
    // WAKEUP_GRANULARITY prevents excessive switching on tiny vruntime deltas.
    constexpr u64 WAKEUP_GRANULARITY_NS = 1000000; // 1ms
    return current->se.vruntime > leftmost->se.vruntime + WAKEUP_GRANULARITY_NS;
  }

public:
  // Pick the highest-vruntime (rightmost) task — used by load balancer
  // to select migration candidates (migrate the least-deserving task).
  [[nodiscard]] Thread *pick_last_task() noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    if (rb_root_ == nullptr) {
      return nullptr;
    }
    RbNode<Thread> *node = rb_root_;
    while (node->right != nullptr) {
      node = node->right;
    }
    return node->data;
  }

  [[nodiscard]] u32 nr_running() const noexcept { return nr_running_; }
  [[nodiscard]] u64 min_vruntime() const noexcept { return min_vruntime_; }
  [[nodiscard]] u64 total_weight() const noexcept { return total_weight_; }
  [[nodiscard]] u32 load_avg() const noexcept { return load_avg_; }
  [[nodiscard]] u32 util_avg() const noexcept { return util_avg_; }

  // Set initial vruntime for a newly enqueued task.
  // is_fork=true: slight penalty so parent runs first (returns child PID).
  // is_fork=false (wakeup): slight bonus to reduce wakeup latency.
  void place_entity(Thread *thread, bool is_fork) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    u64 vruntime = min_vruntime_;
    if (is_fork) {
      // Fork: penalty → parent runs first
      vruntime += cfs_params::SCHED_LATENCY_NS / 2;
    } else {
      // Wakeup: bonus → reduce wakeup latency
      u64 thresh = cfs_params::SCHED_LATENCY_NS / 2;
      vruntime = (vruntime > thresh) ? (vruntime - thresh) : 0;
    }
    thread->se.vruntime = vruntime;
  }

  void dump_runqueue() const noexcept {
    // Debug output placeholder
  }

private:
  [[nodiscard]] u64 calc_initial_vruntime() const noexcept {
    // New tasks start from current queue watermark (min_vruntime),
    // minus half a scheduling period to give them a slight initial boost
    // (equivalent to Linux CFS place_entity semantics).
    u64 thresh = cfs_params::SCHED_LATENCY_NS / 2;
    return (min_vruntime_ > thresh) ? (min_vruntime_ - thresh) : min_vruntime_;
  }

  [[nodiscard]] u64 calc_delta_fair(u64 delta_exec, Thread *thread) const noexcept {
    if (thread->se.weight == 0) {
      return delta_exec;
    }

    return (delta_exec * cfs_params::NICE_TO_WEIGHT[20]) / thread->se.weight;
  }

  // Simplified PELT (Per-Entity Load Tracking) with geometric decay.
  // Fixed decay factor ~0.98 per tick (Q12 fixed-point, ~32ms half-life).
  void update_load_tracking(Thread *thread, u64 delta_exec) noexcept {
    if (thread == nullptr) {
      return;
    }

    constexpr u64 LOAD_AVG_MAX = 47742;
    // Decay factor ~0.98 in Q12 fixed-point: 0.98 * 4096 ≈ 4015
    constexpr u64 DECAY_FACTOR = 4015;

    // Decay existing sums
    thread->se.load_sum = (thread->se.load_sum * DECAY_FACTOR) >> 12;
    thread->se.util_sum = (thread->se.util_sum * DECAY_FACTOR) >> 12;

    // Accumulate new contribution
    thread->se.load_sum += delta_exec;
    thread->se.util_sum += delta_exec;

    // Cap to prevent unbounded growth
    if (thread->se.load_sum > LOAD_AVG_MAX) {
      thread->se.load_sum = LOAD_AVG_MAX;
    }
    if (thread->se.util_sum > LOAD_AVG_MAX) {
      thread->se.util_sum = LOAD_AVG_MAX;
    }

    // Derive averages
    thread->se.load_avg = thread->se.load_sum >> 10;
    thread->se.util_avg = thread->se.util_sum >> 10;
  }

  void update_load_stats(Thread *thread, bool add) noexcept {
    if (thread == nullptr) {
      return;
    }

    if (add) {
      load_sum_ += thread->se.load_avg;
      util_sum_ += thread->se.util_avg;
    } else {
      load_sum_ = (load_sum_ > thread->se.load_avg) ? (load_sum_ - thread->se.load_avg) : 0;
      util_sum_ = (util_sum_ > thread->se.util_avg) ? (util_sum_ - thread->se.util_avg) : 0;
    }

    load_avg_ = static_cast<u32>((nr_running_ > 0) ? (load_sum_ / nr_running_) : 0);
    util_avg_ = static_cast<u32>((nr_running_ > 0) ? (util_sum_ / nr_running_) : 0);
  }

  void rb_insert(RbNode<Thread> *node) noexcept {
    if (node == nullptr || node->data == nullptr) {
      return;
    }

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
    if (node == nullptr) {
      return;
    }

    // 1. Update leftmost cache BEFORE deletion (node's links still intact)
    if (node == rb_leftmost_) {
      rb_leftmost_ = rb_next(node);
    }

    // 2. Remove from tree (may trigger rotations)
    rb_delete_node(node);

    // 3. Safety: recalculate leftmost after rotations if needed
    if (rb_root_ != nullptr && rb_leftmost_ == nullptr) {
      rb_leftmost_ = find_tree_minimum(rb_root_);
    }
    if (rb_root_ == nullptr) {
      rb_leftmost_ = nullptr;
    }
  }

  [[nodiscard]] RbNode<Thread> *find_tree_minimum(RbNode<Thread> *root) const noexcept {
    if (root == nullptr) {
      return nullptr;
    }

    while (root->left != nullptr) {
      root = root->left;
    }
    return root;
  }

  [[nodiscard]] bool verify_tree_consistency() const noexcept {
    if (rb_root_ == nullptr) {
      return rb_leftmost_ == nullptr;
    }

    RbNode<Thread> *actual_min = find_tree_minimum(rb_root_);
    if (rb_leftmost_ != actual_min) {
      return false;
    }

    u32 actual_count = count_tree_nodes(rb_root_);
    return actual_count == nr_running_;
  }

  [[nodiscard]] u32 count_tree_nodes(RbNode<Thread> *node) const noexcept {
    if (node == nullptr) {
      return 0;
    }
    return 1 + count_tree_nodes(node->left) + count_tree_nodes(node->right);
  }

  [[nodiscard]] RbNode<Thread> *rb_next(RbNode<Thread> *node) const noexcept {
    if (node == nullptr) {
      return nullptr;
    }

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

  void rotate_left(RbNode<Thread> *x) noexcept {
    auto *y = x->right;
    x->right = y->left;
    if (y->left) {
      y->left->parent = x;
    }
    y->parent = x->parent;
    if (!x->parent) {
      rb_root_ = y;
    } else if (x == x->parent->left) {
      x->parent->left = y;
    } else {
      x->parent->right = y;
    }
    y->left = x;
    x->parent = y;
  }

  void rotate_right(RbNode<Thread> *x) noexcept {
    auto *y = x->left;
    x->left = y->right;
    if (y->right) {
      y->right->parent = x;
    }
    y->parent = x->parent;
    if (!x->parent) {
      rb_root_ = y;
    } else if (x == x->parent->right) {
      x->parent->right = y;
    } else {
      x->parent->left = y;
    }
    y->right = x;
    x->parent = y;
  }

  void rb_insert_fixup(RbNode<Thread> *z) noexcept {
    while (z->parent && z->parent->red) {
      if (z->parent->parent == nullptr) {
        break; // safety: no grandparent
      }

      if (z->parent == z->parent->parent->left) {
        auto *y = z->parent->parent->right; // uncle
        if (y && y->red) {
          // Case 1: uncle is red → recolor
          z->parent->red = false;
          y->red = false;
          z->parent->parent->red = true;
          z = z->parent->parent;
        } else {
          if (z == z->parent->right) {
            // Case 2: z is right child → left rotate
            z = z->parent;
            rotate_left(z);
          }
          // Case 3: z is left child → right rotate
          z->parent->red = false;
          z->parent->parent->red = true;
          rotate_right(z->parent->parent);
        }
      } else {
        // Mirror: parent is right child of grandparent
        auto *y = z->parent->parent->left; // uncle
        if (y && y->red) {
          z->parent->red = false;
          y->red = false;
          z->parent->parent->red = true;
          z = z->parent->parent;
        } else {
          if (z == z->parent->left) {
            z = z->parent;
            rotate_right(z);
          }
          z->parent->red = false;
          z->parent->parent->red = true;
          rotate_left(z->parent->parent);
        }
      }
    }
    rb_root_->red = false;
  }

  void rb_delete_node(RbNode<Thread> *node) noexcept {
    if (node == nullptr) {
      return;
    }

    RbNode<Thread> *x = nullptr;        // replacement child for fixup
    RbNode<Thread> *x_parent = nullptr; // x's parent (needed when x is null)
    bool original_red = node->red;

    if (node->left == nullptr) {
      // Case 1/2: no left child (includes leaf)
      x = node->right;
      x_parent = node->parent;
      replace_node_in_parent(node, node->right);
      if (node->right) {
        node->right->parent = node->parent;
      }
    } else if (node->right == nullptr) {
      // Case 3: only left child
      x = node->left;
      x_parent = node->parent;
      replace_node_in_parent(node, node->left);
      node->left->parent = node->parent;
    } else {
      // Case 4: two children — splice out inorder successor
      RbNode<Thread> *successor = tree_minimum(node->right);
      original_red = successor->red;
      x = successor->right;

      if (successor->parent == node) {
        x_parent = successor;
      } else {
        x_parent = successor->parent;
        replace_node_in_parent(successor, successor->right);
        if (successor->right) {
          successor->right->parent = successor->parent;
        }
        successor->right = node->right;
        successor->right->parent = successor;
      }

      replace_node_in_parent(node, successor);
      successor->left = node->left;
      successor->left->parent = successor;
      successor->parent = node->parent;
      successor->red = node->red;

      // Update successor's Thread back-pointer: successor node now holds
      // the position of 'node' in the tree, but its data (Thread*) still
      // points to the successor's original thread — that's correct.
      // The removed node's thread->rq_node is cleared by the caller.
    }

    if (!original_red) {
      rb_delete_fixup(x, x_parent);
    }
  }

  void replace_node_in_parent(RbNode<Thread> *old_node, RbNode<Thread> *new_node) noexcept {
    if (old_node->parent == nullptr) {
      rb_root_ = new_node;
    } else if (old_node == old_node->parent->left) {
      old_node->parent->left = new_node;
    } else {
      old_node->parent->right = new_node;
    }
  }

  [[nodiscard]] RbNode<Thread> *tree_minimum(RbNode<Thread> *node) const noexcept {
    if (node == nullptr) {
      return nullptr;
    }

    while (node->left != nullptr) {
      node = node->left;
    }
    return node;
  }

  // Two-parameter rb_delete_fixup: supports x == nullptr (NIL sentinel)
  // by tracking parent explicitly.  This is the standard CLRS algorithm
  // adapted for nullptr-as-NIL (no sentinel node).
  void rb_delete_fixup(RbNode<Thread> *x, RbNode<Thread> *x_parent) noexcept {
    while (x != rb_root_ && (x == nullptr || !x->red)) {
      if (x_parent == nullptr) {
        break;
      }

      if (x == x_parent->left) {
        auto *w = x_parent->right; // sibling
        if (w == nullptr) {
          break;
        }

        if (w->red) {
          // Case 1: sibling is red
          w->red = false;
          x_parent->red = true;
          rotate_left(x_parent);
          w = x_parent->right;
          if (w == nullptr) {
            break;
          }
        }
        bool left_black = (w->left == nullptr || !w->left->red);
        bool right_black = (w->right == nullptr || !w->right->red);
        if (left_black && right_black) {
          // Case 2: both nephews black
          w->red = true;
          x = x_parent;
          x_parent = x->parent;
        } else {
          if (right_black) {
            // Case 3: left nephew red, right nephew black
            if (w->left) {
              w->left->red = false;
            }
            w->red = true;
            rotate_right(w);
            w = x_parent->right;
            if (w == nullptr) {
              break;
            }
          }
          // Case 4: right nephew red
          w->red = x_parent->red;
          x_parent->red = false;
          if (w->right) {
            w->right->red = false;
          }
          rotate_left(x_parent);
          x = rb_root_; // terminate loop
        }
      } else {
        // Mirror: x is right child of x_parent
        auto *w = x_parent->left; // sibling
        if (w == nullptr) {
          break;
        }

        if (w->red) {
          w->red = false;
          x_parent->red = true;
          rotate_right(x_parent);
          w = x_parent->left;
          if (w == nullptr) {
            break;
          }
        }
        bool left_black = (w->left == nullptr || !w->left->red);
        bool right_black = (w->right == nullptr || !w->right->red);
        if (left_black && right_black) {
          w->red = true;
          x = x_parent;
          x_parent = x->parent;
        } else {
          if (left_black) {
            if (w->right) {
              w->right->red = false;
            }
            w->red = true;
            rotate_left(w);
            w = x_parent->left;
            if (w == nullptr) {
              break;
            }
          }
          w->red = x_parent->red;
          x_parent->red = false;
          if (w->left) {
            w->left->red = false;
          }
          rotate_right(x_parent);
          x = rb_root_;
        }
      }
    }

    if (x != nullptr) {
      x->red = false;
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
    if (node == nullptr) {
      return;
    }

    // Return node to free list for reuse
    node->data = nullptr;
    node->right = nullptr;
    node->parent = nullptr;
    node->red = false;
    node->left = free_list_; // Use left as next pointer
    free_list_ = node;
  }

  template <typename T> constexpr const T &kernel_max(const T &a, const T &b) noexcept { return (a < b) ? b : a; }

  template <typename T> constexpr const T &kernel_min(const T &a, const T &b) noexcept { return (a < b) ? a : b; }
};

// CFS scheduler class
class CfsScheduler {
private:
  containers::PerCpuData<CfsRunqueue> runqueues_;
  containers::PerCpuData<IdleTask *> idle_tasks_;

  containers::PerCpuAtomicCounter<u64> total_switches_;
  containers::PerCpuAtomicCounter<u64> total_preemptions_;

  // Timer-driven scheduling tick
  timer::HrTimer sched_tick_;
  u64 tick_count_{0};

  // Periodic load balance callback — set by process.cpp to avoid
  // circular partition dependency (scheduler → load_balancer).
  // Signature: void(u64 now, CfsScheduler& sched)
  void (*balance_callback_)(u64, CfsScheduler *){nullptr};

public:
  void set_balance_callback(void (*cb)(u64, CfsScheduler *)) noexcept { balance_callback_ = cb; }
  constexpr CfsScheduler() noexcept : idle_tasks_{nullptr} {}

  void enqueue_task(Thread *thread, u32 cpu) noexcept {
    if (thread == nullptr || cpu >= MAX_CPUS) {
      return;
    }

    runqueues_.get_cpu(cpu).enqueue_task(thread);
    thread->cpu = cpu;
    thread->state = ProcessState::Ready;

    // Wakeup preemption check: if the newly enqueued task has lower
    // vruntime than the current task on the target CPU, mark it for
    // rescheduling.
    Thread *curr = current_running_tasks_[cpu];
    if (curr != nullptr && thread->se.vruntime < curr->se.vruntime) {
      curr->need_resched = true;
    }

    // Wake target CPU if idle (tickless idle disables timer PPI,
    // so only SGI can break WFI).  send_reschedule_ipi() is a
    // no-op when target == current CPU.
    send_reschedule_ipi(cpu);
  }

  void dequeue_task(Thread *thread) noexcept {
    if (thread == nullptr) {
      return;
    }

    u32 cpu = thread->cpu;
    if (cpu < MAX_CPUS) {
      runqueues_.get_cpu(cpu).dequeue_task(thread);
    }
  }

  [[nodiscard]] Thread *pick_next_task(u32 cpu) noexcept {
    if (cpu >= MAX_CPUS) {
      return nullptr;
    }

    return runqueues_.get_cpu(cpu).pick_next_task();
  }

  // Pick highest-vruntime task from a CPU's runqueue (for load balancer).
  // Steals the least-deserving task (ran most), preserving CFS fairness.
  [[nodiscard]] Thread *pick_last_task(u32 cpu) noexcept {
    if (cpu >= MAX_CPUS) {
      return nullptr;
    }
    return runqueues_.get_cpu(cpu).pick_last_task();
  }

  inline void set_idle_task(u32 cpu_id, IdleTask *idle_task) noexcept {
    if (cpu_id >= MAX_CPUS) {
      return;
    }

    idle_tasks_.get_cpu(cpu_id) = idle_task;

    if (idle_task) {
      log::klog::info("set idle task CPU{}: TID={}", cpu_id, static_cast<u32>(idle_task->get_cpu_id()));
    } else {
      log::klog::info("set idle task CPU{}: TID=NULL", cpu_id);
    }
  }

  [[nodiscard]] IdleTask *get_idle_task(u32 cpu_id) const noexcept {
    if (cpu_id >= MAX_CPUS) {
      return nullptr;
    }
    return idle_tasks_.get_cpu(cpu_id);
  }

  [[nodiscard]] bool has_runnable_tasks(u32 cpu_id) const noexcept {
    if (cpu_id >= MAX_CPUS) {
      return false;
    }
    return runqueues_.get_cpu(cpu_id).nr_running() > 0;
  }

  // Place entity vruntime for fork or wakeup (before enqueue)
  void place_entity(Thread *thread, u32 cpu, bool is_fork) noexcept {
    if (cpu >= MAX_CPUS || !thread) {
      return;
    }
    runqueues_.get_cpu(cpu).place_entity(thread, is_fork);
  }

  // Get min_vruntime for a specific CPU's runqueue
  [[nodiscard]] u64 get_cpu_min_vruntime(u32 cpu) const noexcept {
    if (cpu >= MAX_CPUS) {
      return 0;
    }
    return runqueues_.get_cpu(cpu).min_vruntime();
  }

  // Reset the current task's vruntime to min_vruntime of its runqueue.
  // Called after an IO polling wait (e.g. console_read) so CFS does not
  // starve the task — equivalent to Linux place_entity() for waking tasks.
  void reset_current_to_min_vruntime() noexcept {
    auto *curr = get_current_task();
    if (!curr) {
      return;
    }
    u32 cpu = get_current_cpu_id();
    if (cpu >= MAX_CPUS) {
      return;
    }
    u64 min_vr = runqueues_.get_cpu(cpu).min_vruntime();
    if (curr->se.vruntime > min_vr) {
      curr->se.vruntime = min_vr;
    }
    curr->se.exec_start = get_current_time();
  }

private:
  void execute_task_simplified(Thread *task, [[maybe_unused]] u32 cpu_id) noexcept {
    if (task == nullptr) {
      return;
    }

    // Simulate a realistic time slice so vruntime advances fairly.
    // Without this, fake test tasks accumulate negligible vruntime
    // and starve real user tasks (CFS always picks lowest vruntime).
    constexpr u64 SIMULATED_SLICE_NS = 6000000; // 6ms = sched latency
    update_current(task, SIMULATED_SLICE_NS);

    set_current_task(task);
    record_context_switch();
  }

  void run_idle_task_simplified(IdleTask *idle_task, u32 cpu_id) noexcept {
    if (idle_task == nullptr) {
      return;
    }

    mark_cpu_idle(cpu_id, true);

    // Track idle time: mark entry before WFI so snapshot_idle_time_ns()
    // can include the in-progress idle period.  exit_idle() accumulates
    // the delta when WFI returns.
    idle_task->enter_idle(timer::TimerSubsystem::instance().now_ns());
    idle_task_loop(cpu_id);
    idle_task->exit_idle(timer::TimerSubsystem::instance().now_ns());

    mark_cpu_idle(cpu_id, false);
  }

  void idle_task_loop([[maybe_unused]] u32 cpu_id) noexcept { arch::cpu_idle_once(); }

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

    IdleTask *idle_task = get_idle_task(cpu_id);
    if (idle_task == nullptr) {
      log::klog::info("CPU{}: idle task not set, creating default", cpu_id);

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
      Thread *next_task = nullptr;

      if (has_runnable_tasks(cpu_id)) {
        next_task = pick_next_task(cpu_id);

        if (next_task != nullptr) {
          active_cycles++;

          if (active_cycles % LOG_INTERVAL == 1) {
            log::klog::debug("[CPU{}][TID={}] task running", cpu_id, static_cast<u32>(next_task->tid));
          }

          // All tasks use real context switch — dequeue from runqueue,
          // switch to the task's context (eret for first entry, or
          // context_switch for subsequent dispatches).
          dequeue_task(next_task);
          context_switch_to_task(next_task);
          // Returns here when this CPU's bootstrap context is restored
          // after the task is preempted by a timer IRQ.
        }
      }

      if (next_task == nullptr) {
        idle_cycles++;

        if (idle_cycles % LOG_INTERVAL == 1) {
          log::klog::debug("[CPU{}] entering idle", cpu_id);
        }

        // Idle balance is handled via reschedule IPI: when load_balancer
        // migrates a task to this CPU's runqueue, it sends SGI 0 which
        // breaks WFI below and lets us pick up the new task.

        // Tickless idle (NO_HZ_IDLE): disable per-CPU timer before WFI
        // so idle CPUs are not woken every 6ms by timer PPI (IRQ 27).
        // Only SGI 0 (reschedule IPI) can wake us — sent by enqueue_task()
        // when a task is placed on this CPU's runqueue.
        //
        // Exception: if there are pending HrTimers (e.g. nanosleep),
        // keep the timer enabled so the ISR can fire and wake blocked
        // threads.  reprogram_next() already set the compare register
        // to the earliest expiry.
        bool timer_disabled = false;
        if (!timer::TimerSubsystem::instance().has_pending_timers()) {
          hal::timer::disable();
          timer_disabled = true;
        }
        if (idle_task != nullptr) {
          run_idle_task_simplified(idle_task, cpu_id);
        } else {
          idle_task_loop(cpu_id);
        }
        if (timer_disabled) {
          hal::timer::enable();
        }
      }

      u32 total_cycles = active_cycles + idle_cycles;
      if (total_cycles > 0 && total_cycles % (LOG_INTERVAL * 5) == 0) {
        log::klog::info("[CPU{}] stats: active={} idle={} tasks={}", cpu_id, active_cycles, idle_cycles,
                        get_cpu_nr_running(cpu_id));
      }
    }
  }

  // State transition management
  void task_blocked(Thread *task) noexcept {
    if (task == nullptr) {
      return;
    }

    ProcessState old_state = task->state;
    task->state = ProcessState::Blocked;

    if (old_state == ProcessState::Running || old_state == ProcessState::Ready) {
      dequeue_task(task);
      log::klog::info("task blocked and dequeued: TID={}", static_cast<u32>(task->tid));
    }
  }

  void task_wakeup(Thread *task, u32 target_cpu) noexcept {
    if (task == nullptr || task->state != ProcessState::Blocked) {
      return;
    }

    task->state = ProcessState::Ready;
    // Place entity with wakeup bonus before enqueueing
    place_entity(task, target_cpu, /*is_fork=*/false);
    enqueue_task(task, target_cpu);

    log::klog::info("task wakeup and enqueued: TID={} CPU={}", static_cast<u32>(task->tid), target_cpu);
  }

  void task_terminate(Thread *task) noexcept {
    if (task == nullptr) {
      return;
    }

    ProcessState old_state = task->state;
    task->state = ProcessState::Terminated;

    if (old_state == ProcessState::Running || old_state == ProcessState::Ready) {
      dequeue_task(task);
      log::klog::info("task terminated and dequeued: TID={}", static_cast<u32>(task->tid));
    }
  }

  void transition_task_state(Thread *task, ProcessState new_state) noexcept {
    if (task == nullptr) {
      return;
    }

    ProcessState old_state = task->state;

    bool valid_transition = false;
    switch (old_state) {
    case ProcessState::Created:
      valid_transition = (new_state == ProcessState::Ready);
      break;
    case ProcessState::Ready:
      valid_transition = (new_state == ProcessState::Running || new_state == ProcessState::Blocked ||
                          new_state == ProcessState::Terminated);
      break;
    case ProcessState::Running:
      valid_transition = (new_state == ProcessState::Ready || new_state == ProcessState::Blocked ||
                          new_state == ProcessState::Terminated);
      break;
    case ProcessState::Blocked:
      valid_transition = (new_state == ProcessState::Ready || new_state == ProcessState::Terminated);
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
      log::klog::warn("invalid state transition: {} -> {}", static_cast<u32>(static_cast<u8>(old_state)),
                      static_cast<u32>(static_cast<u8>(new_state)));
    }
  }

  void update_current(Thread *current, u64 delta_exec) noexcept {
    if (current == nullptr) {
      return;
    }

    u32 cpu = current->cpu;
    if (cpu < MAX_CPUS) {
      runqueues_.get_cpu(cpu).update_curr_task(current, delta_exec);
    }
  }

  [[nodiscard]] bool should_preempt_current(Thread *current) noexcept {
    if (current == nullptr) {
      return false;
    }

    u32 cpu = current->cpu;
    if (cpu >= MAX_CPUS) {
      return false;
    }

    return runqueues_.get_cpu(cpu).should_preempt(current);
  }

  [[nodiscard]] u32 get_cpu_load(u32 cpu) const noexcept {
    if (cpu >= MAX_CPUS) {
      return 0;
    }
    return runqueues_.get_cpu(cpu).load_avg();
  }

  [[nodiscard]] u32 get_cpu_nr_running(u32 cpu) const noexcept {
    if (cpu >= MAX_CPUS) {
      return 0;
    }
    return runqueues_.get_cpu(cpu).nr_running();
  }

  [[nodiscard]] u64 total_context_switches() const noexcept { return total_switches_.load_total(); }

  [[nodiscard]] u64 total_preemptions() const noexcept { return total_preemptions_.load_total(); }

  void dump_runqueue(u32 cpu) const noexcept {
    if (cpu < MAX_CPUS) {
      runqueues_.get_cpu(cpu).dump_runqueue();
    }
  }

  void record_context_switch() noexcept { (void)total_switches_.fetch_add_local(1); }

  void record_preemption() noexcept { (void)total_preemptions_.fetch_add_local(1); }

private:
  static Thread *current_running_tasks_[MAX_CPUS];

  // Per-CPU bootstrap context — used as "prev" save target when there is
  // no current task (e.g. schedule_after_exit or first dispatch).
  // context_switch() saves the caller's registers here; the new task
  // starts on its own stack.  Restoring bootstrap returns to the caller.
  static CpuContext bootstrap_contexts_[MAX_CPUS];

  // Per-CPU exit stack — used by schedule_after_exit() to avoid running
  // on the exited process's kernel stack (which will be freed by waitpid).
  // Without this, bootstrap_contexts_[cpu].sp would point to the dead
  // task's kernel stack, creating a use-after-free when that stack is
  // reclaimed by a subsequent fork.
  static constexpr usize EXIT_STACK_SIZE = 4096; // 4KB per CPU is plenty
  alignas(16) static u8 exit_stacks_[MAX_CPUS][EXIT_STACK_SIZE];

public:
  static CpuContext &bootstrap_context(u32 cpu) noexcept { return bootstrap_contexts_[cpu % MAX_CPUS]; }

  static void set_current_task(Thread *task) noexcept {
    u32 cpu = CfsScheduler::get_current_cpu_id();
    if (cpu < MAX_CPUS) {
      current_running_tasks_[cpu] = task;
    }
  }

  static Thread *get_current_task() noexcept {
    u32 cpu = CfsScheduler::get_current_cpu_id();
    return (cpu < MAX_CPUS) ? current_running_tasks_[cpu] : nullptr;
  }

  // Get the currently running task on a specific CPU (for topinfo)
  static Thread *get_current_task_on_cpu(u32 cpu) noexcept {
    return (cpu < MAX_CPUS) ? current_running_tasks_[cpu] : nullptr;
  }

  // ---- GIC timer IRQ handler ----
  // Bridges the hardware interrupt from GIC to the TimerSubsystem.
  // GIC calls this when timer IRQ fires; we ack the hardware timer
  // and dispatch all expired HrTimer callbacks (including sched_tick_).
  static void timer_irq_handler(u32 /*irq*/, void * /*context*/) noexcept {
    hal::timer::ack_interrupt();
    timer::TimerSubsystem::instance().handle_interrupt();
  }

  // ---- Timer-driven scheduler tick callback ----
  // Called from timer interrupt context every SCHED_LATENCY_NS (6ms).
  // Performs one scheduling round: pick next task, update vruntime,
  // context-switch if needed.
  static void scheduler_tick_callback(void *data) noexcept {
    auto *sched = static_cast<CfsScheduler *>(data);
    sched->scheduler_tick();
  }

  void scheduler_tick() noexcept {
    tick_count_++;
    u32 cpu = get_current_cpu_id();

    // Periodic load balance: every 8 ticks (~48ms)
    if (tick_count_ % 8 == 0 && balance_callback_) {
      balance_callback_(get_current_time(), this);
    }

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
      // Cap delta to one scheduling period.  scheduler_tick() fires every
      // SCHED_LATENCY_NS (6ms); a delta much larger than that means the task
      // was in a kernel path that masked IRQ (e.g. console_read polling for
      // keyboard input).  Charge at most one tick's worth of vruntime so the
      // task is not starved by CFS after the masked period ends.
      if (delta > cfs_params::SCHED_LATENCY_NS * 2) {
        delta = cfs_params::SCHED_LATENCY_NS;
      }
      curr->se.exec_start = now;
      update_current(curr, delta);

      // Check if a higher-priority task is waiting (CFS: lower vruntime)
      if (should_preempt_current(curr)) {
        // Guard: task may have been marked Terminated by sys_exit
        // between our state==Running check above and here.
        if (curr->state == ProcessState::Terminated) {
          return;
        }

        curr->need_resched = true;

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

    // NOTE: Synthetic test tasks (create_test_task) have been removed.
    // They used TID 1001-1020 which conflicted with fork-allocated TIDs,
    // consumed scheduling bandwidth via execute_task_simplified, and caused
    // user process starvation after long IRQ-masked console_read periods.

    // Arm the periodic scheduler tick timer
    if (timer::TimerSubsystem::instance().is_initialized()) {
      // Step 1: Register timer IRQ handler with GIC
      u32 timer_irq = hal::timer::irq_number();
      early_debug_print("[sched] registering timer IRQ handler\n");

      if (interrupts::g_gic) {
        auto reg_result = interrupts::g_gic->register_interrupt(timer_irq, timer_irq_handler, nullptr, "sched_timer");
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

      sched_tick_.init(timer::TimerMode::Periodic, scheduler_tick_callback, this);
      sched_tick_.start_relative(cfs_params::SCHED_LATENCY_NS);

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
            if (t == nullptr) {
              break;
            }
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
          // Returns here when init is preempted by timer IRQ.
          // scheduler_tick already re-enqueued init; proceed to idle loop.
        } else {
          early_debug_print("[sched] WARN: init TID=1000 not found\n");
        }
      }

      // Raise log level before entering the scheduling loop so that
      // debug/info messages from the idle loop and scheduler tick
      // do not pollute user-visible UART output.
      log::set_log_level(log::LogLevel::Warn);

      // BSP enters the same per-CPU scheduling loop as secondary CPUs.
      // This ensures that user tasks re-enqueued after preemption (e.g. init/shell)
      // are properly dispatched via context_switch_to_task, not just
      // execute_task_simplified.
      cpu_startup_entry(current_cpu);
    }

    // Fallback: if timer is not available, use the legacy busy-wait loop
    early_debug_print("[sched] WARN: timer unavailable, fallback busy-wait\n");
    fallback_busy_wait_scheduling(current_cpu);
  }

  // Legacy busy-wait scheduling loop (fallback when timer is unavailable)
  [[noreturn]] void fallback_busy_wait_scheduling(u32 current_cpu) noexcept {
    while (true) {
      current_cpu = CfsScheduler::get_current_cpu_id();

      Thread *next_task = pick_next_task(current_cpu);
      if (next_task != nullptr) {
        dequeue_task(next_task);
        execute_task_simplified(next_task, current_cpu);
        enqueue_task(next_task, current_cpu);
      } else {
        idle_task_loop(current_cpu);
      }
    }
  }

private:
  void context_switch_to_task(Thread *task) noexcept {
    if (task == nullptr) {
      return;
    }

    // Guard: never switch to a terminated task (e.g. sys_exit race)
    if (task->state == ProcessState::Terminated) {
      return;
    }

#if defined(MOSS_ARCH_ARM64)
    // Save prev BEFORE updating current — context_switch needs it
    Thread *prev = get_current_task();
#endif

    CfsScheduler::set_current_task(task);
    task->state = ProcessState::Running;
    task->se.exec_start = get_current_time(); // Reset for vruntime accounting
    record_context_switch();

    if (task->needs_initial_eret) {
      // First entry into user space.  We MUST go through context_switch
      // (not direct switch_to_user) so the caller's bootstrap context is
      // properly saved.  Without this, waitpid's context_switch back to
      // bootstrap would restore an all-zero CpuContext and hang.
      //
      // Strategy: prepare task->context as a *kernel* context whose LR
      // (x[30]) points to user_eret_trampoline.  The trampoline reads
      // the saved user PC/SP from callee-saved registers and calls
      // switch_to_user.  context_switch saves bootstrap, restores this
      // prepared context, and `ret` jumps to the trampoline.
      task->needs_initial_eret = false;
#if defined(MOSS_ARCH_ARM64)
      // Stash user-mode entry point, stack, x0 and x1 in callee-saved regs.
      // context_switch preserves x19-x28, so these survive the switch.
      u64 user_pc = task->context.pc;
      u64 user_sp = task->context.sp;
      u64 user_x0 = task->context.x[0]; // fork: 0, execve: argc
      u64 user_x1 = task->context.x[1]; // fork: 0, execve: argv ptr

      // Determine if this is a fork child or execve new program.
      // Fork child: needs full parent register restore (x2-x18, x24-x29).
      // Execve: clean slate — zero all user-visible registers.
      bool is_fork = (user_x0 == 0);

      if (is_fork) {
        // Fork child: preserve parent's GP registers in context.
        // Only x[19]-x[24] and x[30] are overwritten for trampoline args.
        // Parent's original x[19]-x[24] are saved to the top of the
        // child's kernel stack so the trampoline can restore them.
        //
        // x[24] = non-zero flag → trampoline uses fork restore path
        //         (pointer to CpuContext for x2-x18, x25-x30 restore)
        //
        // Parent's x[19]-x[24] saved at [kernel_stack_top - 48]:
        //   [kstop - 48] = parent x19   [kstop - 40] = parent x20
        //   [kstop - 32] = parent x21   [kstop - 24] = parent x22
        //   [kstop - 16] = parent x23   [kstop -  8] = parent x24

        // Save parent's original x19-x24 to kernel stack top
        if (task->kernel_stack_base != 0) {
          u64 kstop = task->kernel_stack_top();
          auto *saved = reinterpret_cast<u64 *>(kstop - 48);
          saved[0] = task->context.x[19];
          saved[1] = task->context.x[20];
          saved[2] = task->context.x[21];
          saved[3] = task->context.x[22];
          saved[4] = task->context.x[23];
          saved[5] = task->context.x[24];
        }

        task->context.x[19] = reinterpret_cast<u64>(task);
        task->context.x[20] = user_pc;
        task->context.x[21] = user_sp;
        task->context.x[22] = user_x0; // 0
        task->context.x[23] = user_x1;
        task->context.x[24] = reinterpret_cast<u64>(&task->context);
        // x[25]-x[29] retain parent values — context_switch restores them.
      } else {
        // Execve: clean slate — zero all registers, set only trampoline args.
        task->context = CpuContext{};
        task->context.x[19] = reinterpret_cast<u64>(task);
        task->context.x[20] = user_pc;
        task->context.x[21] = user_sp;
        task->context.x[22] = user_x0;
        task->context.x[23] = user_x1;
        // x[24] = 0 → trampoline uses "clean" path (zero all regs)
      }

      u64 trampoline_addr = reinterpret_cast<u64>(&user_eret_trampoline);
      task->context.x[30] = trampoline_addr; // LR → ret target
      task->context.pc = trampoline_addr;
      // SP = kernel stack top (16-byte aligned)
      task->context.sp = task->kernel_stack_base != 0 ? task->kernel_stack_top() : 0;

      // Fall through to the normal context_switch path below, which will:
      //   1. Save bootstrap_contexts_[cpu] (or prev task)
      //   2. Restore this prepared context
      //   3. `ret` to user_eret_trampoline
#endif
    }
    {
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
          u64 ttbr0_val = proc->address_space()->pgd_phys | (static_cast<u64>(proc->address_space()->asid) << 48);
          asm volatile("msr ttbr0_el1, %0" ::"r"(ttbr0_val));
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
          asm volatile("msr tpidr_el1, %0" ::"r"(kstack_top));
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
      // Non-ARM64: no asm context_switch yet — just simulate execution
      execute_task_simplified(task, get_current_cpu_id());
#endif
    }
  }

  void check_need_resched() noexcept {
    Thread *curr = get_current_task();
    if (curr && curr->need_resched) {
      // Preemption pending — will be serviced at next safe preemption point
      // (IRQ return, syscall return).
    }
  }

  [[nodiscard]] static u32 get_current_cpu_id() noexcept { return arch::get_current_cpu_id(); }

  [[nodiscard]] static u64 get_current_time() noexcept { return arch::get_timestamp_counter(); }

public:
  // Called after a process exits (sys_exit).  Picks the next runnable task
  // and switches to it.  Never returns to the caller because the exited
  // task's context is no longer valid.
  [[noreturn]] void schedule_after_exit() noexcept {
    u32 cpu = get_current_cpu_id();

    // Clear current task — the old one is dead
    set_current_task(nullptr);

    // CRITICAL: Switch SP to a safe per-CPU exit stack BEFORE any
    // context_switch.  We are currently running on the exited process's
    // kernel stack, which will be freed by the parent's waitpid →
    // terminate_process → cleanup_threads → free_pages().
    // If we don't switch, context_switch saves this SP into
    // bootstrap_contexts_[cpu], and later restoration reads from
    // freed/reused memory → use-after-free crash.
#if defined(MOSS_ARCH_ARM64)
    {
      u64 exit_sp = reinterpret_cast<u64>(&exit_stacks_[cpu % MAX_CPUS][EXIT_STACK_SIZE]);
      asm volatile("mov sp, %0" ::"r"(exit_sp) : "memory");
    }
#endif

    // Ensure interrupts are enabled so timer ticks can fire and
    // re-enqueue tasks while we idle.
    arch::enable_interrupts();

    // Loop: find a runnable task, switch to it.  When bootstrap context
    // is restored (the task was preempted away or exited), try next.
    while (true) {
      Thread *next = pick_next_task(cpu);
      if (next != nullptr) {
        dequeue_task(next);
        context_switch_to_task(next);
        // context_switch returned — bootstrap context restored.
        // The task was preempted or exited.  Re-clear and retry.
        set_current_task(nullptr);
      } else {
        // No runnable tasks: tickless idle until reschedule IPI.
        // Only disable the timer if there are no pending HrTimers
        // (e.g. nanosleep).  If there are pending timers, keep the
        // timer enabled so the ISR can fire and wake blocked threads.
        bool timer_disabled = false;
        if (!timer::TimerSubsystem::instance().has_pending_timers()) {
          hal::timer::disable();
          timer_disabled = true;
        }
        arch::cpu_idle_once();
        if (timer_disabled) {
          hal::timer::enable();
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
inline Thread *current_thread() noexcept { return CfsScheduler::get_current_task(); }
inline Process *current_process() noexcept {
  Thread *t = current_thread();
  if (!t || !g_process_manager) {
    return nullptr;
  }
  return g_process_manager->find_process(t->owner_pid);
}

} // namespace moss::kernel::process
