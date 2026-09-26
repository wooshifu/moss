// MOSS Process Module - Partition: scheduler
// CFS Scheduler, Idle Task, CfsRunqueue, CfsScheduler

export module moss.process:scheduler;

import :types;
import :signal;

import moss.intrinsics;
import moss.std;
import moss.types;
import moss.result;
import moss.smart_ptr;
import moss.arch;
import moss.containers;
import moss.mm;
import moss.hal.mmu;
import moss.interrupts;
import moss.abi;
import moss.platform;
import moss.hal.intc;
import moss.hal.timer;
import moss.timer;
import moss.logging;

// Validation observes the real selection-to-dispatch boundary; production is a no-op.
extern "C" void moss_validation_dispatch_selected() noexcept;

// Assembly/entry symbols from moss.abi — bring into scope for this partition
using moss::abi::context_switch;
using moss::abi::switch_to_user;
using moss::abi::entry::early_debug_print;
#if defined(MOSS_ARCH_ARM64)
using moss::abi::arm64::user_eret_trampoline;
#elif defined(MOSS_ARCH_X64)
using moss::abi::x64::user_iret_trampoline;
#elif defined(MOSS_ARCH_RISCV64)
using moss::abi::riscv64::user_sret_trampoline;
#endif

// ============================================================================
// cfs_scheduler.hpp - CFS (Completely Fair Scheduler)
// ============================================================================
export namespace moss::kernel::process {

namespace log = moss::kernel::logging;
namespace containers = moss::kernel::containers;

// CFS scheduling parameters
namespace cfs_params {
// Default latency/quantum budgets trade switch frequency against response
// time; their exact 6 ms/0.75 ms tuning evidence is not recorded. Eight is
// their ratio, so periods grow once equal shares would fall below the quantum.
inline constexpr u64 SCHED_LATENCY_NS = 6000000;  // 6ms
inline constexpr u64 MIN_GRANULARITY_NS = 750000; // 0.75ms
inline constexpr u32 SCHED_NR_LATENCY = 8;

// Linux-style nice scale: 40 entries cover -20..19; index 20 (nice 0) is the
// neutral weight 1024 used to normalize vruntime. Adjacent weights differ by
// roughly 1.25 so nice changes affect proportional CPU shares, not fixed slices.
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

constexpr u32 nice_to_weight_index(i32 nice) { return static_cast<u32>(nice + 20); }

constexpr u32 nice_to_weight(i32 nice) {
  // Invalid nice values use the smallest positive fallback, avoiding a zero
  // denominator in fairness math. Valid values index the -20..19 public range.
  u32 index = nice_to_weight_index(nice);
  return (index < 40) ? NICE_TO_WEIGHT[index] : 1;
}

// Adaptive scheduling period: when nr_running exceeds SCHED_NR_LATENCY,
// grow linearly to avoid excessively short time slices.
constexpr u64 sched_period(u32 nr_running) {
  if (nr_running > SCHED_NR_LATENCY) {
    return static_cast<u64>(nr_running) * MIN_GRANULARITY_NS;
  }
  return SCHED_LATENCY_NS;
}

constexpr u64 sched_slice(u32 weight, u32 total_weight, u32 nr_running = SCHED_NR_LATENCY) {
  if (total_weight == 0) {
    return MIN_GRANULARITY_NS;
  }

  u64 period = sched_period(nr_running);
  u64 slice = (period * weight) / total_weight;
  return (slice < MIN_GRANULARITY_NS) ? MIN_GRANULARITY_NS : slice;
}
} // namespace cfs_params

// ============================================================================
// RT (Real-Time) run queue — priority-ordered FIFO/RR queue
//
// Design: simple array of linked-list heads, one per RT priority level (1-99).
// pick_next scans from highest to lowest priority (O(1) with bitmap).
// SCHED_FIFO tasks run until block/yield; SCHED_RR tasks rotate after
// their time slice expires (100ms default, matching Linux).
// ============================================================================
class RtRunqueue {
private:
  mutable containers::IrqSpinLock lock_;

  // Per-priority FIFO lists (index 0 unused; priorities 1-99).
  // Each slot is the head of a singly-linked list using rt_next_.
  static constexpr u32 NUM_PRIORITIES = 100;
  Thread *heads_[NUM_PRIORITIES]{};
  Thread *tails_[NUM_PRIORITIES]{};

  // Bitmap: bit N set iff heads_[N] != nullptr (fast highest-prio scan).
  // Two 64-bit words cover priorities 0-127 (we use 0-99).
  u64 bitmap_[2]{};

  u32 nr_running_{0};

  void set_bit(u32 prio) noexcept {
    if (prio < 64) {
      bitmap_[0] |= (1ULL << prio);
    } else {
      bitmap_[1] |= (1ULL << (prio - 64));
    }
  }

  void clear_bit(u32 prio) noexcept {
    if (prio < 64) {
      bitmap_[0] &= ~(1ULL << prio);
    } else {
      bitmap_[1] &= ~(1ULL << (prio - 64));
    }
  }

  // Find highest set bit across both words (highest priority with tasks).
  // Returns 0 if no bits set (priority 0 is unused).
  [[nodiscard]] u32 find_highest() const noexcept {
    // clzll counts within a 64-bit word, so 63-clz is its highest bit index;
    // add 64 only for the second word to recover the RT priority.
    // Check high word first (priorities 64-99)
    if (bitmap_[1] != 0) {
      return 64 + 63 - static_cast<u32>(intrinsics::bitops::clzll(bitmap_[1]));
    }
    if (bitmap_[0] != 0) {
      return 63 - static_cast<u32>(intrinsics::bitops::clzll(bitmap_[0]));
    }
    return 0;
  }

public:
  constexpr RtRunqueue() noexcept = default;

  // Enqueue an RT task at the tail of its priority list (FIFO order).
  void enqueue_task(Thread *thread) noexcept {
    if (thread == nullptr) {
      return;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);

    u32 prio = thread->effective_rt_priority();
    if (prio == 0 || prio >= NUM_PRIORITIES) {
      prio = priority::DEFAULT_RT_PRIORITY;
    }

    thread->rt_next_ = nullptr;

    if (tails_[prio] != nullptr) {
      tails_[prio]->rt_next_ = thread;
    } else {
      heads_[prio] = thread;
    }
    tails_[prio] = thread;
    set_bit(prio);
    thread->rt_on_rq = true;
    nr_running_++;
  }

  // Dequeue a specific RT task from its priority list.
  void dequeue_task(Thread *thread) noexcept {
    if (thread == nullptr) {
      return;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);

    u32 prio = thread->effective_rt_priority();
    if (prio == 0 || prio >= NUM_PRIORITIES) {
      prio = priority::DEFAULT_RT_PRIORITY;
    }

    // Scan list to find and remove thread
    Thread *prev = nullptr;
    Thread *cur = heads_[prio];
    while (cur != nullptr) {
      if (cur == thread) {
        if (prev != nullptr) {
          prev->rt_next_ = cur->rt_next_;
        } else {
          heads_[prio] = cur->rt_next_;
        }
        if (tails_[prio] == cur) {
          tails_[prio] = prev;
        }
        cur->rt_next_ = nullptr;
        cur->rt_on_rq = false;
        nr_running_--;
        if (heads_[prio] == nullptr) {
          clear_bit(prio);
        }
        return;
      }
      prev = cur;
      cur = cur->rt_next_;
    }
  }

  // Pick the highest-priority task (peek, does not dequeue).
  [[nodiscard]] Thread *pick_next_task() noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    u32 prio = find_highest();
    if (prio == 0) {
      return nullptr;
    }
    return heads_[prio];
  }

  // SCHED_RR: move the head of a priority list to the tail (round-robin).
  void requeue_task(Thread *thread) noexcept {
    if (thread == nullptr) {
      return;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);

    u32 prio = thread->effective_rt_priority();
    if (prio == 0 || prio >= NUM_PRIORITIES) {
      return;
    }

    // Only requeue if this task is the head (it should be, since it's running)
    if (heads_[prio] != thread) {
      return;
    }

    // Single task — nothing to rotate
    if (heads_[prio] == tails_[prio]) {
      return;
    }

    // Move head to tail
    heads_[prio] = thread->rt_next_;
    thread->rt_next_ = nullptr;
    tails_[prio]->rt_next_ = thread;
    tails_[prio] = thread;
  }

  [[nodiscard]] u32 nr_running() const noexcept { return nr_running_; }

  // Highest RT priority among queued tasks (0 = none).
  [[nodiscard]] u32 highest_priority() const noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    return find_highest();
  }
};

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
  if (cpu_id >= g_num_cpus) {
    return nullptr;
  }
  return g_idle_tasks.get_cpu(cpu_id);
}

// Set idle task for specified CPU
inline bool set_idle_task(u32 cpu_id, IdleTask *idle_task) noexcept {
  if (cpu_id >= g_num_cpus) {
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
  if (target_cpu >= g_num_cpus || target_cpu == arch::get_current_cpu_id()) {
    return;
  }
  // GICv2 SGIR only supports 8-bit CPU target mask (CPUs 0-7).
  // GICv3 uses ICC_SGI1R_EL1 with 16-bit TargetList (CPUs 0-15 in Aff0).
  // This check is ARM64-only; x64 (APIC) and RISC-V 64 (PLIC/CLINT) have
  // different IPI mechanisms without this 8-core limitation.
#if defined(MOSS_ARCH_ARM64)
  if (hal::intc::g_gic_version != hal::intc::GicVersion::GICv3 && target_cpu >= 8) {
    return;
  }
#endif
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

  // Obtain the embedded RbNode view from a Thread's SchedEntity.
  // The se.rb_* fields ARE the node; we reinterpret their address
  // as RbNode<Thread>* since the layout matches exactly.
  static RbNode<Thread> *thread_to_node(Thread *t) noexcept {
    // SchedEntity::rb_data is the first of the 5 embedded RB fields and
    // has the same layout as RbNode<Thread> (data, left, right, parent, red).
    return reinterpret_cast<RbNode<Thread> *>(&t->se.rb_data);
  }

public:
  constexpr CfsRunqueue() noexcept
      : rb_root_(nullptr), rb_leftmost_(nullptr), nr_running_(0), min_vruntime_(0), total_weight_(0), load_sum_(0),
        util_sum_(0), load_avg_(0), util_avg_(0) {}

  void enqueue_task(Thread *thread) noexcept {
    if (thread == nullptr) {
      return;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);

    if (thread->se.vruntime == 0) {
      thread->se.vruntime = calc_initial_vruntime();
    }

    // Use the RbNode embedded in thread->se (no pool allocation needed).
    RbNode<Thread> *node = thread_to_node(thread);
    node->data = thread;
    node->left = nullptr;
    node->right = nullptr;
    node->parent = nullptr;
    node->red = true;
    thread->se.rb_on_rq = true;

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

    if (!thread->se.rb_on_rq) {
      return; // not in this queue
    }

    RbNode<Thread> *node = thread_to_node(thread);
    rb_remove(node);
    thread->se.rb_on_rq = false;
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
    // Sample one selection per million calls to bound hot-path debug output;
    // the exact logging interval has no recorded measurement behind it.
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
    if (current->se.rb_on_rq) {
      auto *node = thread_to_node(current);
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
    // The exact 1 ms threshold is a policy default with no recorded calibration.
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

  // The scheduler holds its transition lock until this candidate is removed.
  // Ready can still describe the source CPU's unsaved continuation, so CPU
  // ownership, rather than state alone, determines whether it may move.
  [[nodiscard]] Thread *pick_migration_task(u32 destination, Thread *current) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    auto *node = rb_root_;
    if (!node) {
      return nullptr;
    }
    while (node->right) {
      node = node->right;
    }
    while (node) {
      auto *task = node->data;
      if (task && task != current && task->state == ProcessState::Ready && task->cpu_affinity_mask.test(destination)) {
        return task;
      }
      // Walk predecessors so a pinned or executing rightmost task does not
      // prevent another eligible task from leaving the source queue.
      if (node->left) {
        node = node->left;
        while (node->right) {
          node = node->right;
        }
      } else {
        auto *parent = node->parent;
        while (parent && node == parent->left) {
          node = parent;
          parent = parent->parent;
        }
        node = parent;
      }
    }
    return nullptr;
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
    // Half the target latency (3 ms) bounds the fork penalty/wakeup credit.
    // The choice of one-half is not backed by recorded workload measurements.
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

    // Index 20 is nice 0: normalize charged execution to neutral-weight units.
    // Divide before multiplying so intermediates fit whenever the final u64
    // delta fits. The remainder is below a u32 weight, so scaling it by the
    // neutral 1024 weight fits u64 without a freestanding 128-bit division.
    const u64 weight = thread->se.weight;
    const u64 neutral_weight = cfs_params::NICE_TO_WEIGHT[20];
    const u64 quotient = delta_exec / weight;
    const u64 remainder = delta_exec % weight;
    return quotient * neutral_weight + (remainder * neutral_weight) / weight;
  }

  // ── PELT (Per-Entity Load Tracking) ─────────────────────────────────
  //
  // Linux documents 1024 us periods and y^32 = 1/2. Moss uses exact us,
  // rather than Linux's fast 1024 ns approximation, and tracks charged
  // execution here; this is not blocked/runnable wall-clock accounting.
  // Reference: kernel/sched/pelt.c and Documentation/scheduler/sched-pelt.c
  // in https://github.com/torvalds/linux.
  static constexpr u64 PELT_PERIOD_US = 1024;
  static constexpr u64 PELT_NS_PER_US = 1000; // Exact ns-to-us unit conversion.
  static constexpr u64 PELT_PERIOD_NS = PELT_PERIOD_US * PELT_NS_PER_US;
  static constexpr u32 PELT_HALF_LIFE_PERIODS = 32;
  // Q32 coefficients store 32 fractional bits in u32; full utilization uses
  // Linux's 2^10 capacity scale, independent of a task's nice weight.
  static constexpr u32 PELT_Q32_SHIFT = 32;
  static constexpr u64 PELT_UTIL_SCALE = 1024;

  // floor((2^32-1) * 2^(-n/32)), n=0..31, from the upstream generator.
  // Exact 32-period halves use shifts; a zero remainder returns the sum
  // directly, avoiding entry zero's fractional approximation.
  static constexpr u32 PELT_YN_Q32[] = {
      // clang-format off
      0xffffffffU, 0xfa83b2daU, 0xf5257d14U, 0xefe4b99aU,
      0xeac0c6e6U, 0xe5b906e6U, 0xe0ccdeebU, 0xdbfbb796U,
      0xd744fcc9U, 0xd2a81d91U, 0xce248c14U, 0xc9b9bd85U,
      0xc5672a10U, 0xc12c4cc9U, 0xbd08a39eU, 0xb8fbaf46U,
      0xb504f333U, 0xb123f581U, 0xad583ee9U, 0xa9a15ab4U,
      0xa5fed6a9U, 0xa2704302U, 0x9ef5325fU, 0x9b8d39b9U,
      0x9837f050U, 0x94f4efa8U, 0x91c3d373U, 0x8ea4398aU,
      0x8b95c1e3U, 0x88980e80U, 0x85aac367U, 0x82cd8698U,
      // clang-format on
  };
  static_assert(sizeof(PELT_YN_Q32) / sizeof(PELT_YN_Q32[0]) == PELT_HALF_LIFE_PERIODS);
  static constexpr u64 PELT_Y_Q32 = PELT_YN_Q32[1];

  // Upstream's 47742 is an integer fixed point, not the real geometric sum.
  // Derive it from the same scalar so coefficients and normalization cannot
  // drift: S_next = floor(S*y) + 1024, starting with one complete period.
  static constexpr u64 LOAD_AVG_MAX = []() constexpr {
    u64 sum = PELT_PERIOD_US;
    for (;;) {
      const u64 next = ((sum * PELT_Y_Q32) >> PELT_Q32_SHIFT) + PELT_PERIOD_US;
      if (next == sum) {
        return sum;
      }
      sum = next;
    }
  }();
  static_assert(LOAD_AVG_MAX == 47742);

  [[nodiscard]] static u64 pelt_decay_sum(u64 sum, u64 periods) noexcept {
    // A u64 has no bits left after 64 halvings; guard the shift width.
    const u64 halves = periods / PELT_HALF_LIFE_PERIODS;
    if (halves >= 64) {
      return 0;
    }
    sum >>= halves;
    const u32 remainder = static_cast<u32>(periods % PELT_HALF_LIFE_PERIODS);
    if (remainder == 0) {
      return sum;
    }
    // Sums are bounded by LOAD_AVG_MAX, so the Q32 product fits in u64.
    return (sum * PELT_YN_Q32[remainder]) >> PELT_Q32_SHIFT;
  }

  void update_load_tracking(Thread *thread, u64 delta_exec_ns) noexcept {
    if (thread == nullptr || delta_exec_ns == 0) {
      return;
    }

    // update_curr_task already advanced this persistent execution counter.
    // Convert the endpoints, retaining both sub-us time and period phase
    // across calls; converting/rounding each delta changes the sampling rate.
    const u64 end_us = thread->se.sum_exec_runtime / PELT_NS_PER_US;
    const u64 start_us = (thread->se.sum_exec_runtime - delta_exec_ns) / PELT_NS_PER_US;
    const u64 delta_us = end_us - start_us;
    if (delta_us == 0) {
      return;
    }
    const u64 start_phase = start_us % PELT_PERIOD_US;
    const u64 end_phase = end_us % PELT_PERIOD_US;
    const u64 periods = (start_phase + delta_us) / PELT_PERIOD_US;

    u64 contribution = delta_us;
    if (periods != 0) {
      thread->se.load_sum = pelt_decay_sum(thread->se.load_sum, periods);
      thread->se.util_sum = pelt_decay_sum(thread->se.util_sum, periods);
      // Complete the old partial period, add the intervening full periods,
      // then add the undecayed current tail. Use the integer fixed-point
      // identity for full periods instead of a separate scalar series.
      contribution = pelt_decay_sum(PELT_PERIOD_US - start_phase, periods) + LOAD_AVG_MAX -
                     pelt_decay_sum(LOAD_AVG_MAX, periods) - PELT_PERIOD_US + end_phase;
    }
    thread->se.load_sum += contribution;
    thread->se.util_sum += contribution;

    // The peak sum depends on the elapsed part of the current period.
    // Including that phase prevents false idle time and load oscillations.
    const u64 divider = LOAD_AVG_MAX - PELT_PERIOD_US + end_phase;
    thread->se.load_sum = kernel_min(thread->se.load_sum, divider);
    thread->se.util_sum = kernel_min(thread->se.util_sum, divider);
    thread->se.load_avg = (thread->se.load_sum * thread->se.weight) / divider;
    thread->se.util_avg = (thread->se.util_sum * PELT_UTIL_SCALE) / divider;
  }

  // Per-runqueue load aggregation: sum of per-entity averages.
  // Updated on enqueue (+) and dequeue (-) for O(1) rq-level load.
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

    // Per-rq averages: arithmetic mean of entity averages
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

    if (rb_leftmost_ == nullptr || vruntime < rb_leftmost_->data->se.vruntime ||
        (vruntime == rb_leftmost_->data->se.vruntime && node->data->tid < rb_leftmost_->data->tid)) {
      should_update_leftmost = true;
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
      // The removed node's thread->se.rb_on_rq is cleared by the caller.
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

  template <typename T> constexpr const T &kernel_max(const T &a, const T &b) noexcept { return (a < b) ? b : a; }

  template <typename T> constexpr const T &kernel_min(const T &a, const T &b) noexcept { return (a < b) ? a : b; }
};

// BSP scheduling readiness flag — secondary CPUs idle-wait until BSP
// finishes creating and dispatching the init process.  Without this gate,
// secondary CPUs' scheduler_tick() and load balancer can steal/dispatch
// init before BSP's start_scheduling() dispatches it, causing a
// double-dispatch race (two CPUs execute the same Thread simultaneously).
inline containers::AtomicBool g_bsp_scheduling_ready{};

// CFS scheduler class (also dispatches RT tasks)
class CfsScheduler {
private:
  // Join the CFS/RT queue locks for publication, dispatch and CPU transfer.
  // Acquire this before a queue lock, and never hold it across context_switch:
  // another CPU must not claim a peeked task before its local dequeue, or see
  // a linked node with the old CPU/state. The shared short transition favors
  // a single ownership boundary over independently locked peek/remove calls.
  inline static containers::IrqSpinLock task_transition_lock_{};
  // IPC dependency changes may arrive from timer and peer-exit callbacks.
  // Always acquire this before task_transition_lock_ when both are needed.
  // ponytail: one global lock and O(chain + donors) recompute; split it only
  // if measured call-chain latency or contention requires finer locking.
  inline static containers::IrqSpinLock ipc_priority_lock_{};
  inline static u64 ipc_cycle_epoch_{};
  containers::PerCpuData<CfsRunqueue> runqueues_;
  containers::PerCpuData<RtRunqueue> rt_runqueues_;
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

  // Init task pointer — set by Kernel::create_init_process() so
  // start_scheduling() can dispatch it directly instead of searching
  // by hardcoded TID (which breaks when idle tasks consume TIDs first).
  Thread *init_task_{nullptr};

public:
  void set_balance_callback(void (*cb)(u64, CfsScheduler *)) noexcept { balance_callback_ = cb; }
  constexpr CfsScheduler() noexcept : idle_tasks_{nullptr} {}

  /// Register the init task for direct dispatch by start_scheduling().
  /// Called after enqueue_task() so the task is already in the runqueue.
  void set_init_task(Thread *task) noexcept { init_task_ = task; }

  void enqueue_task(Thread *thread, u32 cpu) noexcept {
    if (thread == nullptr || cpu >= g_num_cpus) {
      return;
    }
    {
      containers::LockGuard<containers::IrqSpinLock> guard(task_transition_lock_);
      thread->state = ProcessState::Ready;
      enqueue_task_unlocked(thread, cpu);
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

    containers::LockGuard<containers::IrqSpinLock> guard(task_transition_lock_);
    dequeue_task_unlocked(thread);
  }

  // Pick the next task to run — RT tasks always take precedence over CFS.
  [[nodiscard]] Thread *pick_next_task(u32 cpu) noexcept {
    if (cpu >= g_num_cpus) {
      return nullptr;
    }

    containers::LockGuard<containers::IrqSpinLock> guard(task_transition_lock_);
    return pick_next_task_unlocked(cpu);
  }

  // Dispatch consumes the selection under the same lock used by migration.
  // A returned task is no longer visible to a remote CPU's ready-queue scan.
  [[nodiscard]] Thread *take_next_task(u32 cpu) noexcept {
    if (cpu >= g_num_cpus) {
      return nullptr;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(task_transition_lock_);
    auto *task = pick_next_task_unlocked(cpu);
    if (task) {
      dequeue_task_unlocked(task);
    }
    return task;
  }

  bool migrate_ready_task(u32 source, u32 destination) noexcept {
    if (source >= g_num_cpus || destination >= g_num_cpus || source == destination) {
      return false;
    }
    {
      containers::LockGuard<containers::IrqSpinLock> guard(task_transition_lock_);
      // Retain the last queued task, matching the load balancer's policy.
      if (get_cpu_nr_running(source) <= 1) {
        return false;
      }
      auto *task = runqueues_.get_cpu(source).pick_migration_task(destination, get_current_task_on_cpu(source));
      if (!task) {
        return false;
      }
      dequeue_task_unlocked(task);
      // Moving a Ready node changes placement, not its state. Do not overwrite
      // a concurrent stop/termination while publishing it on the target queue.
      enqueue_task_unlocked(task, destination);
    }
    send_reschedule_ipi(destination);
    return true;
  }

  // Observe the highest-vruntime task. Migration must use migrate_ready_task
  // to join candidate selection with removal and target publication.
  [[nodiscard]] Thread *pick_last_task(u32 cpu) noexcept {
    if (cpu >= g_num_cpus) {
      return nullptr;
    }
    return runqueues_.get_cpu(cpu).pick_last_task();
  }

  void set_idle_task(u32 cpu_id, IdleTask *idle_task) noexcept {
    if (cpu_id >= g_num_cpus) {
      return;
    }

    idle_tasks_.get_cpu(cpu_id) = idle_task;

    if (idle_task) {
      log::klog::info("set idle task CPU{}: TID={}", cpu_id, static_cast<u32>(idle_task->tid));
    } else {
      log::klog::info("set idle task CPU{}: TID=NULL", cpu_id);
    }
  }

  [[nodiscard]] IdleTask *get_idle_task(u32 cpu_id) const noexcept {
    if (cpu_id >= g_num_cpus) {
      return nullptr;
    }
    return idle_tasks_.get_cpu(cpu_id);
  }

  [[nodiscard]] bool has_runnable_tasks(u32 cpu_id) const noexcept {
    if (cpu_id >= g_num_cpus) {
      return false;
    }
    return rt_runqueues_.get_cpu(cpu_id).nr_running() > 0 || runqueues_.get_cpu(cpu_id).nr_running() > 0;
  }

  [[nodiscard]] bool begin_ipc_call(PriorityDonation *donation, Thread *caller, u64 *deadline_ns = nullptr) noexcept {
    if (!donation || !caller) {
      return false;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(ipc_priority_lock_);
    if (caller->ipc_wait) {
      return false;
    }
    u64 effective_deadline = deadline_ns ? *deadline_ns : 0;
    // A service thread may hold several replies. Its next call inherits the
    // earliest active caller deadline, captured with the donor-list lock.
    for (auto *active = caller->ipc_donors; active; active = active->next) {
      if (active->deadline_ns && (!effective_deadline || active->deadline_ns < effective_deadline)) {
        effective_deadline = active->deadline_ns;
      }
    }
    donation->deadline_ns = effective_deadline;
    if (deadline_ns) {
      *deadline_ns = effective_deadline;
    }
    donation->caller = caller;
    donation->server = nullptr;
    donation->next = nullptr;
    caller->ipc_wait = donation;
    caller->ipc_scheduler = this;
    return true;
  }

  void bind_ipc_server(PriorityDonation *donation, Thread *server) noexcept {
    if (!donation) {
      return;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(ipc_priority_lock_);
    if (!donation->caller || donation->caller->ipc_wait != donation) {
      return;
    }
    bind_ipc_server_locked(donation, server);
  }

  [[nodiscard]] bool rebind_ipc_server(PriorityDonation *donation, Thread *expected, Thread *server) noexcept {
    if (!donation) {
      return false;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(ipc_priority_lock_);
    if (!donation->caller || donation->caller->ipc_wait != donation || donation->server != expected) {
      return false;
    }
    bind_ipc_server_locked(donation, server);
    return true;
  }

  void end_ipc_call(PriorityDonation *donation) noexcept {
    if (!donation) {
      return;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(ipc_priority_lock_);
    Thread *caller = donation->caller;
    Thread *server = donation->server;
    if (caller && caller->ipc_wait == donation) {
      caller->ipc_wait = nullptr;
    }
    if (server) {
      unlink_ipc_donor(server, donation);
    }
    donation->caller = nullptr;
    donation->server = nullptr;
    donation->next = nullptr;
    donation->deadline_ns = 0;
    if (caller && !caller->ipc_wait && !caller->ipc_donors) {
      caller->ipc_scheduler = nullptr;
    }
    if (server) {
      recompute_ipc_priority(server);
      if (!server->ipc_wait && !server->ipc_donors) {
        server->ipc_scheduler = nullptr;
      }
    }
  }

  void forget_ipc_thread(Thread *thread) noexcept {
    if (!thread) {
      return;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(ipc_priority_lock_);
    Thread *server = thread->ipc_wait ? thread->ipc_wait->server : nullptr;
    if (thread->ipc_wait) {
      if (server) {
        unlink_ipc_donor(server, thread->ipc_wait);
      }
      thread->ipc_wait->caller = nullptr;
      thread->ipc_wait->server = nullptr;
      thread->ipc_wait->next = nullptr;
      thread->ipc_wait = nullptr;
    }
    auto *donation = thread->ipc_donors;
    thread->ipc_donors = nullptr;
    while (donation) {
      auto *next = donation->next;
      donation->server = nullptr;
      donation->next = nullptr;
      donation = next;
    }
    thread->inherited_rt_priority.store(0);
    thread->inherited_cfs_nice.store(priority::NO_INHERITED_NICE);
    thread->ipc_scheduler = nullptr;
    if (server && server != thread) {
      recompute_ipc_priority(server);
      if (!server->ipc_wait && !server->ipc_donors) {
        server->ipc_scheduler = nullptr;
      }
    }
  }

  void set_base_nice(Thread *thread, i32 nice) noexcept {
    if (!thread || nice < priority::MIN_NICE || nice > priority::MAX_NICE) {
      return;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(ipc_priority_lock_);
    const i32 old_effective = thread->effective_cfs_nice();
    u32 cpu;
    {
      containers::LockGuard<containers::IrqSpinLock> queue_guard(task_transition_lock_);
      cpu = thread->cpu;
      const bool queued = thread->se.rb_on_rq || thread->rt_on_rq;
      if (queued) {
        dequeue_task_unlocked(thread);
      }
      thread->se.nice.store(nice);
      const u32 weight = cfs_params::nice_to_weight(thread->effective_cfs_nice());
      thread->se.weight.store(weight);
      thread->se.load_weight.store(weight);
      if (queued) {
        if (thread->effective_rt_priority() == 0 && thread->effective_cfs_nice() < old_effective) {
          runqueues_.get_cpu(cpu).place_entity(thread, false);
        }
        enqueue_task_unlocked(thread, cpu);
      }
      if (thread->state == ProcessState::Running && thread->effective_cfs_nice() > old_effective) {
        thread->need_resched = true;
      }
    }
    if (auto *server = ipc_wait_target(thread)) {
      recompute_ipc_priority(server);
    }
    if (cpu < g_num_cpus && thread->effective_cfs_nice() != old_effective) {
      send_reschedule_ipi(cpu);
    }
  }

  // Place entity vruntime for fork or wakeup (before enqueue)
  void place_entity(Thread *thread, u32 cpu, bool is_fork) noexcept {
    if (cpu >= g_num_cpus || !thread) {
      return;
    }
    runqueues_.get_cpu(cpu).place_entity(thread, is_fork);
  }

  // Get min_vruntime for a specific CPU's runqueue
  [[nodiscard]] u64 get_cpu_min_vruntime(u32 cpu) const noexcept {
    if (cpu >= g_num_cpus) {
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
    if (cpu >= g_num_cpus) {
      return;
    }
    u64 min_vr = runqueues_.get_cpu(cpu).min_vruntime();
    if (curr->se.vruntime > min_vr) {
      curr->se.vruntime = min_vr;
    }
    // Polling can reset CFS fairness, but its elapsed CPU time still belongs
    // to this thread before the next scheduling interval begins.
    (void)curr->se.charge_runtime(get_current_time());
  }

private:
  // The caller holds ipc_priority_lock_ through list repair and recomputation.
  void bind_ipc_server_locked(PriorityDonation *donation, Thread *server) noexcept {
    if (donation->server == server) {
      return;
    }
    Thread *old_server = donation->server;
    if (old_server) {
      unlink_ipc_donor(old_server, donation);
    }
    donation->server = server;
    donation->next = nullptr;
    if (server) {
      donation->next = server->ipc_donors;
      server->ipc_donors = donation;
      server->ipc_scheduler = this;
    }
    if (old_server) {
      recompute_ipc_priority(old_server);
      if (!old_server->ipc_wait && !old_server->ipc_donors) {
        old_server->ipc_scheduler = nullptr;
      }
    }
    if (server) {
      recompute_ipc_priority(server);
    }
  }

  [[nodiscard]] static Thread *ipc_wait_target(Thread *thread) noexcept {
    return thread && thread->ipc_wait ? thread->ipc_wait->server : nullptr;
  }

  static void unlink_ipc_donor(Thread *server, PriorityDonation *donation) noexcept {
    for (auto **link = &server->ipc_donors; *link; link = &(*link)->next) {
      if (*link == donation) {
        *link = donation->next;
        return;
      }
    }
  }

  [[nodiscard]] static Thread *ipc_cycle_entry(Thread *start) noexcept {
    Thread *slow = start;
    Thread *fast = start;
    do {
      slow = ipc_wait_target(slow);
      fast = ipc_wait_target(ipc_wait_target(fast));
    } while (slow && fast && slow != fast);
    if (!slow || !fast) {
      return nullptr;
    }
    slow = start;
    while (slow != fast) {
      slow = ipc_wait_target(slow);
      fast = ipc_wait_target(fast);
    }
    return slow;
  }

  void set_inherited_priority(Thread *thread, u32 inherited_rt, i32 inherited_nice) noexcept {
    if (thread->inherited_rt_priority.load() == inherited_rt && thread->inherited_cfs_nice.load() == inherited_nice) {
      return;
    }
    const u32 old_rt = thread->effective_rt_priority();
    const i32 old_nice = thread->effective_cfs_nice();
    const u32 base_rt = thread->sched_class == SchedClass::RealTime ? thread->rt.priority : 0;
    const u32 new_rt = base_rt > inherited_rt ? base_rt : inherited_rt;
    const i32 base_nice = thread->se.nice.load();
    const i32 new_nice = base_nice < inherited_nice ? base_nice : inherited_nice;
    if (old_rt == new_rt && old_nice == new_nice) {
      thread->inherited_rt_priority.store(inherited_rt);
      thread->inherited_cfs_nice.store(inherited_nice);
      return;
    }
    u32 cpu;
    {
      containers::LockGuard<containers::IrqSpinLock> guard(task_transition_lock_);
      cpu = thread->cpu;
      // A selected task is Ready but no longer queued until bootstrap dispatch.
      const bool queued = thread->se.rb_on_rq || thread->rt_on_rq;
      if (queued) {
        dequeue_task_unlocked(thread);
      }
      thread->inherited_rt_priority.store(inherited_rt);
      thread->inherited_cfs_nice.store(inherited_nice);
      const u32 weight = cfs_params::nice_to_weight(thread->effective_cfs_nice());
      thread->se.weight.store(weight);
      thread->se.load_weight.store(weight);
      if (queued) {
        if (thread->effective_rt_priority() == 0 && thread->effective_cfs_nice() < old_nice) {
          runqueues_.get_cpu(cpu).place_entity(thread, false);
        }
        enqueue_task_unlocked(thread, cpu);
      }
      if (thread->state == ProcessState::Running &&
          (old_rt > thread->effective_rt_priority() ||
           (old_rt == thread->effective_rt_priority() && old_nice < thread->effective_cfs_nice()))) {
        thread->need_resched = true;
      }
    }
    if ((old_rt != thread->effective_rt_priority() || old_nice != thread->effective_cfs_nice()) && cpu < g_num_cpus) {
      send_reschedule_ipi(cpu);
    }
  }

  void recompute_ipc_priority(Thread *start) noexcept {
    Thread *cycle = ipc_cycle_entry(start);
    for (Thread *thread = start; thread && thread != cycle; thread = ipc_wait_target(thread)) {
      u32 inherited_rt = 0;
      i32 inherited_nice = priority::NO_INHERITED_NICE;
      for (auto *donation = thread->ipc_donors; donation; donation = donation->next) {
        if (donation->caller) {
          if (donation->caller->effective_rt_priority() > inherited_rt) {
            inherited_rt = donation->caller->effective_rt_priority();
          }
          if (donation->caller->effective_cfs_nice() < inherited_nice) {
            inherited_nice = donation->caller->effective_cfs_nice();
          }
        }
      }
      set_inherited_priority(thread, inherited_rt, inherited_nice);
    }
    if (!cycle) {
      return;
    }

    // A cycle must not retain a priority after its last external donor exits.
    // Rebuild its common maximum from base priorities and outside callers.
    const u64 epoch = ++ipc_cycle_epoch_;
    for (Thread *thread = cycle;;) {
      thread->ipc_cycle_epoch = epoch;
      thread = ipc_wait_target(thread);
      if (thread == cycle) {
        break;
      }
    }
    u32 highest_rt = 0;
    i32 highest_nice = priority::NO_INHERITED_NICE;
    for (Thread *thread = cycle;;) {
      const u32 base = thread->sched_class == SchedClass::RealTime ? thread->rt.priority : 0;
      if (base > highest_rt) {
        highest_rt = base;
      }
      if (thread->se.nice.load() < highest_nice) {
        highest_nice = thread->se.nice.load();
      }
      for (auto *donation = thread->ipc_donors; donation; donation = donation->next) {
        if (donation->caller && donation->caller->ipc_cycle_epoch != epoch) {
          if (donation->caller->effective_rt_priority() > highest_rt) {
            highest_rt = donation->caller->effective_rt_priority();
          }
          if (donation->caller->effective_cfs_nice() < highest_nice) {
            highest_nice = donation->caller->effective_cfs_nice();
          }
        }
      }
      thread = ipc_wait_target(thread);
      if (thread == cycle) {
        break;
      }
    }
    for (Thread *thread = cycle;;) {
      set_inherited_priority(thread, highest_rt, highest_nice);
      thread = ipc_wait_target(thread);
      if (thread == cycle) {
        break;
      }
    }
  }

  [[nodiscard]] Thread *pick_next_task_unlocked(u32 cpu) noexcept {
    // RT class has strict priority over CFS (like Linux).
    if (auto *task = rt_runqueues_.get_cpu(cpu).pick_next_task()) {
      return task;
    }
    return runqueues_.get_cpu(cpu).pick_next_task();
  }

  void dequeue_task_unlocked(Thread *thread) noexcept {
    const u32 cpu = thread->cpu;
    if (cpu >= g_num_cpus) {
      return;
    }
    if (thread->effective_rt_priority() != 0) {
      rt_runqueues_.get_cpu(cpu).dequeue_task(thread);
    } else {
      runqueues_.get_cpu(cpu).dequeue_task(thread);
    }
  }

  void enqueue_task_unlocked(Thread *thread, u32 cpu) noexcept {
    // The caller publishes any state change under the transition lock. Set
    // placement before linking the node so a target CPU cannot observe an
    // intrusive node with the old CPU after acquiring that same lock.
    thread->cpu = cpu;
    if (thread->effective_rt_priority() != 0) {
      rt_runqueues_.get_cpu(cpu).enqueue_task(thread);
    } else {
      runqueues_.get_cpu(cpu).enqueue_task(thread);
    }

    // RT preempts CFS/lower-priority RT; CFS preempts a higher vruntime.
    if (auto *current = get_current_task_on_cpu(cpu)) {
      if (thread->effective_rt_priority() != 0) {
        if (current->effective_rt_priority() == 0 ||
            thread->effective_rt_priority() > current->effective_rt_priority()) {
          current->need_resched = true;
        }
      } else if (current->effective_rt_priority() == 0 && thread->se.vruntime < current->se.vruntime) {
        current->need_resched = true;
      }
    }
  }

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
    static containers::PerCpuData<bool> cpu_idle_status{};
    cpu_idle_status.get_cpu(cpu_id) = is_idle;
  }

public:
  [[noreturn]] void cpu_startup_entry(u32 cpu_id) noexcept {
    log::klog::info("CPU{}: per-CPU scheduling loop started", cpu_id);

    if (cpu_id >= g_num_cpus) {
      log::klog::error("CPU{}: invalid CPU ID", cpu_id);
      while (true) {
        idle_task_loop(cpu_id);
      }
    }

    // Idle tasks are pre-created by BSP in Kernel::initialize_scheduler()
    // before secondary CPUs are activated.  This avoids concurrent `new`
    // from 16 CPUs causing heap corruption.
    IdleTask *idle_task = get_idle_task(cpu_id);
    if (idle_task == nullptr) {
      log::klog::error("CPU{}: idle task not pre-created by BSP!", cpu_id);
    }

    // Secondary CPUs: wait for BSP to finish creating and dispatching
    // the init process.  Without this gate, scheduler_tick() and the
    // load balancer can steal/dispatch init before BSP dispatches it,
    // leading to double-dispatch (two CPUs executing the same Thread).
    // BSP (cpu 0) skips this — it sets the flag in start_scheduling().
    if (cpu_id != 0) {
      while (!g_bsp_scheduling_ready.load(containers::MemoryOrder::Acquire)) {
        arch::cpu_idle_once();
      }
    }

    log::klog::info("CPU{}: entering per-CPU scheduling loop", cpu_id);

    u32 idle_cycles = 0;
    u32 active_cycles = 0;
    // Sample task/idle loop logging every two million iterations; aggregate
    // stats use five times this interval. Exact diagnostic cadences are unrecorded.
    constexpr u32 LOG_INTERVAL = 2000000;

    while (true) {
      // A timer IRQ must not consume a selected task before this loop switches
      // to it. Idle enables IRQs while waiting; tasks restore their own state.
      arch::disable_interrupts();
      Thread *next_task = nullptr;

      if (has_runnable_tasks(cpu_id)) {
        next_task = take_next_task(cpu_id);

        if (next_task != nullptr) {
          active_cycles++;

          if (active_cycles % LOG_INTERVAL == 1) {
            log::klog::debug("[CPU{}][TID={}] task running", cpu_id, static_cast<u32>(next_task->tid));
          }

#if defined(MOSS_ARCH_X64)
          // A LAPIC one-shot can expire while masked in idle. Unmasking it does
          // not reload the count, so arm the secondary CPU before user dispatch.
          // CPU 0's timer is programmed by the shared HrTimer queue instead.
          if (cpu_id != 0) {
            const auto &clock = timer::TimerSubsystem::instance().clocksource();
            hal::timer::set_compare(hal::timer::read_counter() + clock.ns_to_cycles(cfs_params::SCHED_LATENCY_NS));
          }
#endif
          // Selection already removed the task before exposing its context
          // to the dispatch path; a remote migration cannot claim it now.
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
  //
  // task_block: transition to Sleeping (interruptible) by default.
  // Callers that need uninterruptible sleep should set DiskSleep explicitly
  // before calling dequeue_task().
  void task_blocked(Thread *task) noexcept {
    if (task == nullptr) {
      return;
    }

    ProcessState old_state = task->state;
    task->state = ProcessState::Sleeping;

    if (old_state == ProcessState::Running || old_state == ProcessState::Ready) {
      dequeue_task(task);
      log::klog::info("task blocked and dequeued: TID={}", static_cast<u32>(task->tid));
    }
  }

  void task_wakeup(Thread *task, u32 target_cpu) noexcept {
    if (task == nullptr || target_cpu >= g_num_cpus) {
      return;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(task->sleep_lock);

    // A prepared sleeper is still executing until bootstrap acknowledges its
    // saved context. Remember early wakeups without publishing it to any CPU.
    u32 handoff = task->sleep_handoff.load();
    while (handoff != 0) {
      if (handoff == 2 || task->sleep_handoff.compare_exchange_weak(handoff, 2)) {
        return;
      }
    }

    // A task may have excluded its old CPU before blocking. All wake sources
    // must select an allowed CPU, not just the load balancer's migrations.
    if (!task->cpu_affinity_mask.test(target_cpu)) {
      target_cpu = 0;
      while (target_cpu < g_num_cpus && !task->cpu_affinity_mask.test(target_cpu)) {
        ++target_cpu;
      }
      if (target_cpu == g_num_cpus) {
        return;
      }
    }

    // Several child exits, signals or timer callbacks may wake the same task.
    // Only the winner of the blocked -> Ready transition owns enqueueing it.
    ProcessState expected = task->state.load();
    do {
      if (!is_blocked_state(expected)) {
        return;
      }
    } while (!task->state.compare_exchange_weak(expected, ProcessState::Ready));
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
      valid_transition =
          (new_state == ProcessState::Running || is_blocked_state(new_state) || new_state == ProcessState::Terminated);
      break;
    case ProcessState::Running:
      valid_transition =
          (new_state == ProcessState::Ready || is_blocked_state(new_state) || new_state == ProcessState::Terminated);
      break;
    case ProcessState::Sleeping:
    case ProcessState::DiskSleep:
    case ProcessState::Stopped:
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
    if (cpu < g_num_cpus) {
      runqueues_.get_cpu(cpu).update_curr_task(current, delta_exec);
    }
  }

  [[nodiscard]] bool should_preempt_current(Thread *current) noexcept {
    if (current == nullptr) {
      return false;
    }

    u32 cpu = current->cpu;
    if (cpu >= g_num_cpus) {
      return false;
    }

    return runqueues_.get_cpu(cpu).should_preempt(current);
  }

  [[nodiscard]] u32 get_cpu_load(u32 cpu) const noexcept {
    if (cpu >= g_num_cpus) {
      return 0;
    }
    return runqueues_.get_cpu(cpu).load_avg();
  }

  [[nodiscard]] u32 get_cpu_nr_running(u32 cpu) const noexcept {
    if (cpu >= g_num_cpus) {
      return 0;
    }
    return rt_runqueues_.get_cpu(cpu).nr_running() + runqueues_.get_cpu(cpu).nr_running();
  }

  [[nodiscard]] u64 total_context_switches() const noexcept { return total_switches_.load_total(); }

  [[nodiscard]] u64 total_preemptions() const noexcept { return total_preemptions_.load_total(); }

  void dump_runqueue(u32 cpu) const noexcept {
    if (cpu < g_num_cpus) {
      runqueues_.get_cpu(cpu).dump_runqueue();
    }
  }

  void record_context_switch() noexcept { (void)total_switches_.fetch_add_local(1); }

  void record_preemption() noexcept { (void)total_preemptions_.fetch_add_local(1); }

private:
  static containers::PerCpuData<Thread *> current_running_tasks_;

  // Per-CPU bootstrap context — used as "prev" save target when there is
  // no current task (e.g. schedule_after_exit or first dispatch).
  // context_switch() saves the caller's registers here; the new task
  // starts on its own stack.  Restoring bootstrap returns to the caller.
  static containers::PerCpuData<CpuContext> bootstrap_contexts_;
  // Each installed user root owns its tables, backing and ASID independently
  // of Process publication. Only this CPU, with IRQs masked, accesses its slot.
  static containers::PerCpuData<shared_ptr<AddressSpace>> active_address_spaces_;
  // Keep the exiting task's owner alive until we are back on the scheduler stack.
  containers::PerCpuData<shared_ptr<Process>> exiting_processes_;
  containers::PerCpuData<Thread *> sleeping_tasks_{};

  // Legacy one-page (4 KiB) exit-stack declaration. schedule_after_exit now
  // restores bootstrap_contexts_ to leave the dying task's stack before its
  // owner can be reclaimed; it does not use ExitStack. No stack-usage evidence
  // for this retained size is recorded.
  static constexpr usize EXIT_STACK_SIZE = 4096;

public:
  struct alignas(16) ExitStack {
    u8 data[EXIT_STACK_SIZE];
    u8 *end() noexcept { return data + EXIT_STACK_SIZE; }
    const u8 *end() const noexcept { return data + EXIT_STACK_SIZE; }
  };

  static CpuContext &bootstrap_context(u32 cpu) noexcept { return bootstrap_contexts_.get_cpu(cpu); }

  // Pin next before the hardware write and retire the old pin afterward;
  // an empty owner selects the kernel root. This may reclaim a complete tree
  // and synchronously shoot down TLBs: do not call while holding VM locks.
  // Restores the caller's IRQ state; it does not rebind another CPU or task.
  static void use_address_space(shared_ptr<AddressSpace> next) noexcept;
  // An owning snapshot of this CPU's installed user root, empty on bootstrap.
  // Keep IRQs masked if the caller also needs the hardware root to stay fixed.
  [[nodiscard]] static shared_ptr<AddressSpace> active_address_space() noexcept;
  static void use_kernel_address_space() noexcept { use_address_space({}); }

  // Caller masks IRQs. Bootstrap must never borrow a task's page tables:
  // that task can resume on another CPU and free them while this CPU idles.
  static void switch_to_bootstrap(CpuContext &previous) noexcept {
    use_kernel_address_space();
    // Keep the outgoing owner visible until bootstrap resumes after the
    // assembly save. Clearing it here would permit migrating a Ready task
    // whose continuation is still executing on this CPU.
    if (auto *outgoing = get_current_task()) {
      // Capture the final sub-tick interval on sleep, yield, exit and migration.
      (void)outgoing->se.charge_runtime(get_current_time());
    }
    context_switch(&previous, &bootstrap_contexts_.get_local());
  }

  static void set_current_task(Thread *task) noexcept {
    intrinsics::atomic::store(&current_running_tasks_.get_local(), task, intrinsics::atomic::memory_order::release);
  }

  static Thread *get_current_task() noexcept {
    // Spinlock preemption hooks call this lookup, so it cannot acquire another
    // spinlock. Atomic access also makes remote ownership checks race-free.
    return intrinsics::atomic::load(&current_running_tasks_.get_local(), intrinsics::atomic::memory_order::acquire);
  }

  // Get the currently running task on a specific CPU (for topinfo)
  static Thread *get_current_task_on_cpu(u32 cpu) noexcept {
    if (cpu >= g_num_cpus) {
      return nullptr;
    }
    return intrinsics::atomic::load(&current_running_tasks_.get_cpu(cpu), intrinsics::atomic::memory_order::acquire);
  }

  // Caller holds its event lock with IRQs masked. It must register its waiter
  // before unlocking and committing, so condition changes cannot be lost.
  Thread *prepare_sleep() noexcept {
    auto *task = get_current_task();
    if (!task || arch::interrupts_enabled()) {
      return nullptr;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(task->sleep_lock);
    task->sleep_handoff.store(1);
    task->wake_cpu = get_current_cpu_id();
    task->state = ProcessState::Sleeping;
    dequeue_task(task);
    return task;
  }

  void commit_sleep() noexcept {
    auto *task = get_current_task();
    const auto cpu = get_current_cpu_id();
    sleeping_tasks_.get_cpu(cpu) = task;
    switch_to_bootstrap(task->context);
  }

  void stop_current(u32 signo) noexcept {
    auto *task = get_current_task();
    if (!task || arch::interrupts_enabled()) {
      return;
    }
    {
      containers::LockGuard<containers::IrqSpinLock> guard(task->sleep_lock);
      // SIGCONT resumes even when masked. If it arrived before this handoff,
      // do not suspend after its wake attempt has already observed Running.
      const u64 resume = sig::sigmask(sig::SIGCONT) | sig::sigmask(sig::SIGKILL);
      if ((task->pending_signals & resume) != 0) {
        return;
      }
      task->sleep_handoff.store(1);
      task->wake_cpu = get_current_cpu_id();
      task->state = ProcessState::Stopped;
      task->job_stopped = true;
      // abi-bits/wait.h recognizes the low byte 0x7f as a stopped child.
      task->publish_wait_status((signo << 8) | 0x7f);
      dequeue_task(task);
    }
    notify_parent_job_status(task);
    // The same bootstrap acknowledgement as an interruptible sleep prevents
    // a concurrent CONT/KILL from dispatching an unsaved kernel continuation.
    commit_sleep();
  }

  // Move the calling thread only after its continuation has been saved on the
  // source CPU. Publishing it directly on a remote runqueue would let that CPU
  // restore the same context while context_switch() is still writing it here.
  // Reusing the sleep handoff makes bootstrap perform the first safe publish;
  // the call returns only when the thread is dispatched on destination.
  [[nodiscard]] bool migrate_current(u32 destination) noexcept {
    auto *task = get_current_task();
    const u32 source = get_current_cpu_id();
    if (!task || arch::interrupts_enabled() || source >= g_num_cpus || destination >= g_num_cpus) {
      return false;
    }
    if (source == destination) {
      return true;
    }

    {
      containers::LockGuard<containers::IrqSpinLock> guard(task->sleep_lock);
      if (task->state != ProcessState::Running || !task->cpu_affinity_mask.test(destination)) {
        return false;
      }
      // Handoff value 2 records that bootstrap must wake the saved task
      // immediately; no timer or external wake source is required.
      task->sleep_handoff.store(2);
      task->wake_cpu = destination;
      task->state = ProcessState::Sleeping;
      dequeue_task(task);
    }

    sleeping_tasks_.get_cpu(source) = task;
    switch_to_bootstrap(task->context);
    return true;
  }

private:
  static containers::PerCpuData<ExitStack> exit_stacks_;

  void reschedule_current(Thread *current, u32 cpu) noexcept {
    {
      containers::LockGuard<containers::IrqSpinLock> guard(task_transition_lock_);
      current->state = ProcessState::Ready;
      enqueue_task_unlocked(current, cpu);
      if (pick_next_task_unlocked(cpu) == current) {
        // Self-selection must remove the node before the next yield inserts
        // it again, but needs no assembly save or ownership transfer.
        dequeue_task_unlocked(current);
        current->state = ProcessState::Running;
        current->need_resched = false;
        return;
      }
    }
    // Bootstrap acknowledges the saved continuation before publishing another
    // owner. Direct task-to-task switching publishes next before saving current,
    // which would let a remote balancer move current during the assembly save.
    switch_to_bootstrap(current->context);
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

public:
  void scheduler_tick() noexcept {
    // Suppress scheduling activity until BSP finishes init dispatch.
    // Without this, secondary CPUs' timer IRQs trigger load balancing
    // and task dispatch that race with BSP's start_scheduling().
    if (!g_bsp_scheduling_ready.load(containers::MemoryOrder::Acquire)) {
      return;
    }

    tick_count_++;
    u32 cpu = get_current_cpu_id();

    // Amortize balance scans over eight 6 ms ticks (~48 ms); the choice of
    // eight ticks has no recorded workload calibration.
    if (tick_count_ % 8 == 0 && balance_callback_) {
      balance_callback_(get_current_time(), this);
    }

    Thread *curr = get_current_task();

    if (curr == nullptr) {
      Thread *next = take_next_task(cpu);
      if (next != nullptr) {
        context_switch_to_task(next);
      }
      return;
    }

    if (curr->state != ProcessState::Running) {
      return;
    }

    // Preemption guard: if the task holds a spinlock (preempt_count > 0),
    // do NOT context-switch.  Just mark need_resched and return; the
    // actual switch happens when preempt_enable() drops the count to 0.
    // We still update vruntime/time-slice accounting below so CFS and
    // RR accounting stay accurate even during non-preemptible sections.
    bool preempt_blocked = (curr->preempt_count > 0);

    u64 now = get_current_time();
    const u64 actual_delta = curr->se.charge_runtime(now);
    // A non-advancing timestamp still advances CFS fairness by a 1 us
    // fallback. The exact choice is unrecorded; it is not actual CPU time.
    u64 delta = actual_delta ? actual_delta : 1000;

    // ---- RT scheduling tick ----
    if (curr->effective_rt_priority() != 0) {
      // Check if a higher-priority RT task arrived
      u32 hp = rt_runqueues_.get_cpu(cpu).highest_priority();
      bool need_preempt = (hp > curr->effective_rt_priority());

      // SCHED_RR: decrement time slice, rotate on expiry
      if (!need_preempt && curr->sched_policy == SchedPolicy::RR) {
        if (actual_delta >= curr->rt.time_slice_remaining) {
          curr->rt.time_slice_remaining = rt_params::RR_TIMESLICE_NS;
          need_preempt = true; // time slice expired → rotate
        } else {
          curr->rt.time_slice_remaining -= actual_delta;
        }
      }
      // SCHED_FIFO: no time slice — only preempted by higher priority

      if (need_preempt) {
        if (curr->state == ProcessState::Terminated) {
          return;
        }
        curr->need_resched = true;
        if (preempt_blocked) {
          return; // defer switch until preempt_enable()
        }
        curr->state = ProcessState::Ready;
        record_preemption();
        reschedule_current(curr, cpu);
      }
      return;
    }

    // ---- CFS scheduling tick ----
    // If an RT task is waiting, preempt CFS immediately
    if (rt_runqueues_.get_cpu(cpu).nr_running() > 0) {
      curr->need_resched = true;
      update_current(curr, delta);
      if (preempt_blocked) {
        return; // defer until preempt_enable()
      }
      curr->state = ProcessState::Ready;
      record_preemption();
      reschedule_current(curr, cpu);
      return;
    }

    // Cap delta to one scheduling period.  scheduler_tick() fires every
    // SCHED_LATENCY_NS (6ms); a delta much larger than that means the task
    // was in a kernel path that masked IRQ (e.g. console_read polling for
    // keyboard input).  Charge at most one tick's worth of vruntime so the
    // task is not starved by CFS after the masked period ends. Twice the period
    // is the cutoff; the exact factor has no recorded calibration.
    if (delta > cfs_params::SCHED_LATENCY_NS * 2) {
      delta = cfs_params::SCHED_LATENCY_NS;
    }
    update_current(curr, delta);

    // Check if a higher-priority task is waiting (CFS: lower vruntime)
    if (should_preempt_current(curr)) {
      // Guard: task may have been marked Terminated by sys_exit
      // between our state==Running check above and here.
      if (curr->state == ProcessState::Terminated) {
        return;
      }

      curr->need_resched = true;

      if (preempt_blocked) {
        return; // defer until preempt_enable()
      }

      // Reset time-slice accounting so curr gets a fresh slice next time
      curr->se.prev_sum_exec_runtime = curr->se.sum_exec_runtime;
      curr->state = ProcessState::Ready;
      record_preemption();

      reschedule_current(curr, cpu);
    }
  }

private:
  // Move start_scheduling to public as well

public:
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
      if (!sched_tick_.start_relative(cfs_params::SCHED_LATENCY_NS)) {
        arch::kernel_panic("cannot arm scheduler tick");
      }

      early_debug_print("[sched] tick armed, entering idle loop\n");

      // Before entering idle, dispatch the init user process directly.
      // init_task_ is set by Kernel::create_init_process() after enqueue.
      {
        Thread *init_task = init_task_;
        init_task_ = nullptr; // consumed

        if (init_task != nullptr) {
          early_debug_print("[sched] dispatching init process\n");
          dequeue_task(init_task);
          // Signal secondary CPUs BEFORE dispatching so they start
          // processing tasks while init is running on BSP.
          g_bsp_scheduling_ready.store(true, containers::MemoryOrder::Release);
          context_switch_to_task(init_task);
          // Returns here when init is preempted by timer IRQ.
          // scheduler_tick already re-enqueued init; proceed to idle loop.
        } else {
          early_debug_print("[sched] WARN: init task not registered\n");
          // Unblock secondary CPUs even when init is missing so they
          // don't spin forever.
          g_bsp_scheduling_ready.store(true, containers::MemoryOrder::Release);
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

private:
  // Legacy busy-wait scheduling loop (fallback when timer is unavailable)
  [[noreturn]] void fallback_busy_wait_scheduling(u32 current_cpu) noexcept {
    while (true) {
      current_cpu = CfsScheduler::get_current_cpu_id();

      Thread *next_task = take_next_task(current_cpu);
      if (next_task != nullptr) {
        execute_task_simplified(next_task, current_cpu);
        enqueue_task(next_task, current_cpu);
      } else {
        idle_task_loop(current_cpu);
      }
    }
  }

  void context_switch_to_task(Thread *task) noexcept {
    if (task == nullptr) {
      return;
    }
    moss_validation_dispatch_selected();

    // Guard: never switch to a terminated task (e.g. sys_exit race)
    if (task->state == ProcessState::Terminated) {
      return;
    }

    // Task identity, initial context, address space and entry stack must be
    // published as one local IRQ-masked transition. In particular, RV64's
    // nonzero sscratch would otherwise make an S-mode IRQ use (and overwrite)
    // the incoming task's initial user context as its trap frame.
    const bool restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();

    // Dispatch runs on bootstrap: every outgoing task returns there before
    // the next selection. No task continuation is replaced before its save.
    CfsScheduler::set_current_task(task);
    task->state = ProcessState::Running;
    task->se.exec_start = get_current_time(); // Start both CPU-time and CFS accounting at dispatch.
    record_context_switch();

    const bool first_user_entry = task->needs_initial_eret;
    if (first_user_entry) {
      // First entry into user space.  We MUST go through context_switch
      // (not direct switch_to_user) so the caller's bootstrap context is
      // properly saved.  Without this, waitpid's context_switch back to
      // bootstrap would restore an all-zero CpuContext and hang.
      //
      // Strategy: prepare task->context as a *kernel* context whose
      // return address points to a user-mode trampoline.  The trampoline
      // reads user PC/SP from callee-saved registers and enters user mode.
      // context_switch saves bootstrap, restores this prepared context,
      // and `ret` jumps to the trampoline.
      task->needs_initial_eret = false;

#if defined(MOSS_ARCH_ARM64)
      // Keep the user context separate from the kernel trampoline context.
      // Mask 15 rounds the byte address to the 16-byte ABI stack alignment;
      // x19 carries the saved-context pointer and x30 is the trampoline LR.
      {
        u64 saved_address = (task->kernel_stack_top() - sizeof(CpuContext)) & ~15ULL;
        auto *saved = reinterpret_cast<CpuContext *>(saved_address);
        *saved = task->context;
        task->context = CpuContext{};
        task->context.x[19] = saved_address;
        u64 trampoline_addr = reinterpret_cast<u64>(&user_eret_trampoline);
        task->context.x[30] = trampoline_addr;
        task->context.pc = trampoline_addr;
        task->context.sp = saved_address;
      }

#elif defined(MOSS_ARCH_X64)
      // Keep a complete user context above the initial kernel stack frame;
      // masking 15 supplies the 16-byte alignment required by FXSAVE64.
      {
        u64 saved_address = (task->kernel_stack_top() - sizeof(CpuContext)) & ~15ULL;
        auto *saved = reinterpret_cast<CpuContext *>(saved_address);
        *saved = task->context;
        task->context = CpuContext{};
        task->context.rbx = saved_address;
        u64 trampoline_addr = reinterpret_cast<u64>(&user_iret_trampoline);
        task->context.pc = trampoline_addr;
        task->context.sp = saved_address;
      }

#elif defined(MOSS_ARCH_RISCV64)
      // The top 16 bytes remain reserved for the CPU identity on trap entry:
      // one 8-byte hart value plus padding preserves 16-byte stack alignment.
      // s2/s3 (x18/x19) carry context/identity pointers to user_sret_trampoline.
      {
        u64 saved_address = (task->kernel_stack_top() - 16 - sizeof(CpuContext)) & ~15ULL;
        auto *saved = reinterpret_cast<CpuContext *>(saved_address);
        *saved = task->context;
        task->context = CpuContext{};
        task->context.x[18] = saved_address;
        task->context.x[19] = task->kernel_stack_top() - 16;
        u64 trampoline_addr = reinterpret_cast<u64>(&user_sret_trampoline);
        task->context.pc = trampoline_addr;
        task->context.x[1] = trampoline_addr; // ra = ret target
        task->context.sp = saved_address;
      }
#endif
      // Fall through to the normal context_switch path below.
    }
    {
      // Save the live scheduler stack for every task departure to restore.
      CpuContext *prev_ctx = &bootstrap_contexts_.get_local();

      // For user tasks being re-dispatched after preemption:
      // Set page table base to this process's page tables BEFORE context_switch.
      if (task->is_user_task) {
        auto proc = g_process_manager ? g_process_manager->find_process(task->owner_pid) : shared_ptr<Process>{};
        auto as = proc ? proc->address_space() : shared_ptr<AddressSpace>{};
        if (as && as->pgd_phys != 0) {
          use_address_space(moss::move(as));
        }
        // A newly populated executable page may have been written on a
        // different CPU. Synchronize this CPU before its first user fetch.
        if (first_user_entry) {
          arch::invalidate_icache();
        }

#if defined(MOSS_ARCH_ARM64)
        // Set TPIDR_EL1 for per-thread kernel stack.
        if (task->kernel_stack_base != 0) {
          u64 kstack_top = task->kernel_stack_top();
          asm volatile("msr tpidr_el1, %0" ::"r"(kstack_top));
        }
#elif defined(MOSS_ARCH_RISCV64)
        // Set sscratch to per-thread kernel stack top.
        // On trap from U-mode, the entry code swaps sp↔sscratch to get kernel stack.
        if (task->kernel_stack_base != 0) {
          u64 kstack_top = task->kernel_stack_top();
          arch::set_user_kernel_stack(kstack_top);
        }
#elif defined(MOSS_ARCH_X64)
        // Update TSS RSP0 so hardware interrupts from ring 3 use this task's kernel stack.
        // Also update the SYSCALL kernel stack global for syscall_entry_point.
        if (task->kernel_stack_base != 0) {
          u64 kstack_top = task->kernel_stack_top();
          moss::abi::x64::set_kernel_stack(kstack_top);
        }
#endif
      }

      context_switch(prev_ctx, &task->context);
      // Bootstrap has resumed after the task's assembly save. Release the
      // outgoing owner only now; a Ready node may then migrate safely.
      CfsScheduler::set_current_task(nullptr);
      if (auto *sleeper = sleeping_tasks_.get_local()) {
        sleeping_tasks_.get_local() = nullptr;
        // The continuation is now saved; release publication to a waking CPU.
        if (sleeper->sleep_handoff.exchange(0) == 2) {
          task_wakeup(sleeper, sleeper->wake_cpu);
        }
      }
      exiting_processes_.get_local().reset();
      // An IRQ caller must finish restoring its trap frame with IRQs masked.
      // Ordinary scheduler callers regain their original enabled state.
      if (restore_irqs) {
        arch::enable_interrupts();
      }
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

  // CFS periods, load-balance intervals and CPU runtime are nanoseconds;
  // cntvct/rdtsc/rdtime are raw ticks with different frequencies.
  [[nodiscard]] static u64 get_current_time() noexcept { return timer::TimerSubsystem::instance().now_ns(); }

public:
  // Called after a process exits (sys_exit).  Picks the next runnable task
  // and switches to it.  Never returns to the caller because the exited
  // task's context is no longer valid.
  [[noreturn]] void schedule_after_exit(shared_ptr<Process> process) noexcept {
    arch::disable_interrupts();
    u32 cpu = get_current_cpu_id();
    exiting_processes_.get_cpu(cpu) = moss::move(process);
    // The bootstrap context already owns a live scheduler stack. Do not
    // overwrite it with a dying task, or change SP inside a C++ frame.
    CpuContext discarded{};
    switch_to_bootstrap(discarded);
    arch::kernel_panic("terminated task resumed");
  }
};

// Global CFS scheduler instance
extern CfsScheduler *g_scheduler;

// Secondary CPU scheduling entry point -- called from boot_impl.cpp
[[noreturn]] void secondary_cpu_schedule_loop(u32 cpu_id) noexcept;

// current_thread / current_process implementation (needs CfsScheduler to be defined)
inline Thread *current_thread() noexcept { return CfsScheduler::get_current_task(); }
inline shared_ptr<Process> current_process() noexcept {
  Thread *t = current_thread();
  if (!t || !g_process_manager) {
    return {};
  }
  return g_process_manager->find_process(t->owner_pid);
}

} // namespace moss::kernel::process
