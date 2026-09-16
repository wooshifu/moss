// MOSS Containers Module - High-Performance Kernel Data Structures
// Provides lock-free queues, atomic types, per-CPU data, locked owning containers,
// slab allocator, and related utilities for kernel use.

export module moss.containers;

import moss.intrinsics;
import moss.std;
import moss.types;
import moss.smart_ptr;
import moss.result;
import moss.arch;
import moss.abi;

// ============================================================================
// Atomic types
// ============================================================================
export namespace moss::kernel::containers {

// Re-export MemoryOrder from moss.std for convenience
using MemoryOrder = moss::MemoryOrder;

// Convert MemoryOrder to intrinsics::atomic::memory_order
constexpr intrinsics::atomic::memory_order to_intrinsics_order(MemoryOrder order) noexcept {
  switch (order) {
  case MemoryOrder::Relaxed:
    return intrinsics::atomic::memory_order::relaxed;
  case MemoryOrder::Consume:
    return intrinsics::atomic::memory_order::consume;
  case MemoryOrder::Acquire:
    return intrinsics::atomic::memory_order::acquire;
  case MemoryOrder::Release:
    return intrinsics::atomic::memory_order::release;
  case MemoryOrder::AcqRel:
    return intrinsics::atomic::memory_order::acq_rel;
  case MemoryOrder::SeqCst:
  default:
    return intrinsics::atomic::memory_order::seq_cst;
  }
}

// High-performance atomic pointer - based on compiler builtins
template <typename T> class AtomicPtr {
private:
  T *volatile ptr_;

public:
  constexpr AtomicPtr() noexcept : ptr_(nullptr) {}
  constexpr AtomicPtr(T *p) noexcept : ptr_(p) {}

  AtomicPtr(const AtomicPtr &) = delete;
  AtomicPtr &operator=(const AtomicPtr &) = delete;

  AtomicPtr(AtomicPtr &&other) noexcept { ptr_ = other.exchange(nullptr, MemoryOrder::AcqRel); }

  AtomicPtr &operator=(AtomicPtr &&other) noexcept {
    if (this != &other) {
      T *old_ptr = other.exchange(nullptr, MemoryOrder::AcqRel);
      store(old_ptr, MemoryOrder::Release);
    }
    return *this;
  }

  [[nodiscard]] T *load(MemoryOrder order = MemoryOrder::SeqCst) const noexcept {
    // Cast away volatile for intrinsics (atomic ops provide memory visibility)
    return intrinsics::atomic::load(const_cast<T *const *>(&ptr_), to_intrinsics_order(order));
  }

  void store(T *desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
    // Cast away volatile for intrinsics (atomic ops provide memory visibility)
    intrinsics::atomic::store(const_cast<T **>(&ptr_), desired, to_intrinsics_order(order));
  }

  [[nodiscard]] T *exchange(T *desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
    // Cast away volatile for intrinsics (atomic ops provide memory visibility)
    return intrinsics::atomic::exchange(const_cast<T **>(&ptr_), desired, to_intrinsics_order(order));
  }

  [[nodiscard]] bool compare_exchange_weak(T *&expected, T *desired, MemoryOrder success = MemoryOrder::SeqCst,
                                           MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
    // Cast away volatile for intrinsics (atomic ops provide memory visibility)
    return intrinsics::atomic::compare_exchange_weak(const_cast<T **>(&ptr_), &expected, desired,
                                                     to_intrinsics_order(success), to_intrinsics_order(failure));
  }

  [[nodiscard]] bool compare_exchange_strong(T *&expected, T *desired, MemoryOrder success = MemoryOrder::SeqCst,
                                             MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
    // Cast away volatile for intrinsics (atomic ops provide memory visibility)
    return intrinsics::atomic::compare_exchange_strong(const_cast<T **>(&ptr_), &expected, desired,
                                                       to_intrinsics_order(success), to_intrinsics_order(failure));
  }

  [[nodiscard]] T *operator->() const noexcept { return load(MemoryOrder::Acquire); }

  [[nodiscard]] T &operator*() const noexcept { return *load(MemoryOrder::Acquire); }

  [[nodiscard]] operator T *() const noexcept { return load(MemoryOrder::Acquire); }

  AtomicPtr &operator=(T *desired) noexcept {
    store(desired, MemoryOrder::Release);
    return *this;
  }
};

// High-performance atomic counter - based on compiler builtins
template <typename T> class AtomicCounter {
private:
  volatile T value_;

public:
  constexpr AtomicCounter() noexcept : value_(0) {}
  constexpr AtomicCounter(T initial) noexcept : value_(initial) {}

  AtomicCounter(const AtomicCounter &) = delete;
  AtomicCounter &operator=(const AtomicCounter &) = delete;

  [[nodiscard]] T load(MemoryOrder order = MemoryOrder::SeqCst) const noexcept {
    // Cast away volatile for intrinsics (atomic ops provide memory visibility)
    return intrinsics::atomic::load(const_cast<const T *>(&value_), to_intrinsics_order(order));
  }

  void store(T desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
    // Cast away volatile for intrinsics (atomic ops provide memory visibility)
    intrinsics::atomic::store(const_cast<T *>(&value_), desired, to_intrinsics_order(order));
  }

  [[nodiscard]] T fetch_add(T arg, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
    // Cast away volatile for intrinsics (atomic ops provide memory visibility)
    return intrinsics::atomic::fetch_add(const_cast<T *>(&value_), arg, to_intrinsics_order(order));
  }

  [[nodiscard]] T fetch_sub(T arg, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
    // Cast away volatile for intrinsics (atomic ops provide memory visibility)
    return intrinsics::atomic::fetch_sub(const_cast<T *>(&value_), arg, to_intrinsics_order(order));
  }

  [[nodiscard]] bool compare_exchange_weak(T &expected, T desired, MemoryOrder success = MemoryOrder::SeqCst,
                                           MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
    // Cast away volatile for intrinsics (atomic ops provide memory visibility)
    return intrinsics::atomic::compare_exchange_weak(const_cast<T *>(&value_), &expected, desired,
                                                     to_intrinsics_order(success), to_intrinsics_order(failure));
  }

  [[nodiscard]] T operator++() noexcept { return fetch_add(1, MemoryOrder::SeqCst) + 1; }
  [[nodiscard]] T operator--() noexcept { return fetch_sub(1, MemoryOrder::SeqCst) - 1; }
  [[nodiscard]] T operator++(int) noexcept { return fetch_add(1, MemoryOrder::SeqCst); }
  [[nodiscard]] T operator--(int) noexcept { return fetch_sub(1, MemoryOrder::SeqCst); }

  [[nodiscard]] operator T() const noexcept { return load(MemoryOrder::Acquire); }
};

// Common atomic type aliases
using AtomicU32 = AtomicCounter<u32>;
using AtomicU64 = AtomicCounter<u64>;
using AtomicSize = AtomicCounter<moss::kernel::usize>;
using AtomicBool = AtomicCounter<bool>;

// Cache-line aligned atomic type
template <typename T> struct alignas(moss::kernel::CACHE_LINE_SIZE) CacheAlignedAtomic {
  AtomicCounter<T> value;

  constexpr CacheAlignedAtomic() noexcept = default;
  constexpr CacheAlignedAtomic(T initial) noexcept : value(initial) {}

  [[nodiscard]] T load(MemoryOrder order = MemoryOrder::SeqCst) const noexcept { return value.load(order); }

  void store(T desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept { value.store(desired, order); }

  [[nodiscard]] T operator++() noexcept { return ++value; }
  [[nodiscard]] T operator--() noexcept { return --value; }
  [[nodiscard]] T operator++(int) noexcept { return value++; }
  [[nodiscard]] T operator--(int) noexcept { return value--; }

  [[nodiscard]] operator T() const noexcept { return value; }
};

// ============================================================================
// Preemption hooks — set by the process module once the scheduler is ready.
// SpinLock acquire/release calls these to bump the current thread's
// preempt_count, preventing the scheduler from context-switching while
// a lock is held.  nullptr until the process module registers them.
// ============================================================================
inline void (*g_preempt_disable_fn)() noexcept = nullptr;
inline void (*g_preempt_enable_fn)() noexcept = nullptr;

// ============================================================================
// Ticket SpinLock — fair, FIFO-ordered mutual exclusion
// ============================================================================

// ARM64-optimized ticket spinlock.
// Uses fetch_add for ticket acquisition (LDAXR/STLXR on ARM64).
// Waiters use cpu_yield() which maps to WFE on ARM64, woken by
// SEV generated implicitly by the store-release in unlock().
class TicketSpinLock {
private:
  AtomicU32 next_ticket_{0};
  AtomicU32 now_serving_{0};

public:
  constexpr TicketSpinLock() noexcept = default;

  TicketSpinLock(const TicketSpinLock &) = delete;
  TicketSpinLock &operator=(const TicketSpinLock &) = delete;

  void lock() noexcept {
    if (g_preempt_disable_fn) {
      g_preempt_disable_fn();
    }
    u32 my_ticket = next_ticket_.fetch_add(1, MemoryOrder::Acquire);
    while (now_serving_.load(MemoryOrder::Acquire) != my_ticket) {
      moss::kernel::arch::cpu_yield(); // WFE on ARM64
    }
  }

  void unlock() noexcept {
    (void)now_serving_.fetch_add(1, MemoryOrder::Release);
    // ARM64: store-release generates implicit SEV to wake WFE waiters
    if (g_preempt_enable_fn) {
      g_preempt_enable_fn();
    }
  }

  [[nodiscard]] bool try_lock() noexcept {
    u32 current = now_serving_.load(MemoryOrder::Acquire);
    u32 next = current;
    return next_ticket_.compare_exchange_weak(next, current + 1, MemoryOrder::Acquire, MemoryOrder::Relaxed);
  }

  [[nodiscard]] bool is_locked() const noexcept {
    return next_ticket_.load(MemoryOrder::Relaxed) != now_serving_.load(MemoryOrder::Relaxed);
  }
};

// IRQ-safe spinlock variant: saves/restores IRQ state while held.
// Prevents deadlock when IRQ handler also takes the same lock.
// Uses irqsave/irqrestore pattern: nested lock/unlock pairs correctly
// preserve the outer caller's interrupt state.
class IrqSpinLock {
private:
  TicketSpinLock inner_;
  bool saved_irq_state_{false};

public:
  constexpr IrqSpinLock() noexcept = default;

  IrqSpinLock(const IrqSpinLock &) = delete;
  IrqSpinLock &operator=(const IrqSpinLock &) = delete;

  void lock() noexcept {
    bool was_enabled = moss::kernel::arch::interrupts_enabled();
    moss::kernel::arch::disable_interrupts();
    inner_.lock();
    saved_irq_state_ = was_enabled;
  }

  void unlock() noexcept {
    bool restore = saved_irq_state_;
    inner_.unlock();
    if (restore) {
      moss::kernel::arch::enable_interrupts();
    }
  }

  [[nodiscard]] bool try_lock() noexcept {
    bool was_enabled = moss::kernel::arch::interrupts_enabled();
    moss::kernel::arch::disable_interrupts();
    if (inner_.try_lock()) {
      saved_irq_state_ = was_enabled;
      return true;
    }
    if (was_enabled) {
      moss::kernel::arch::enable_interrupts();
    }
    return false;
  }
};

// RAII lock guard
template <typename LockType> class LockGuard {
private:
  LockType &lock_;

public:
  explicit LockGuard(LockType &lock) noexcept : lock_(lock) { lock_.lock(); }
  ~LockGuard() noexcept { lock_.unlock(); }

  LockGuard(const LockGuard &) = delete;
  LockGuard &operator=(const LockGuard &) = delete;
};

// Per-CPU counter (avoids cache line contention)
// Two-phase design: boot buffer of BOOT_MAX_CPUS, expandable after MM init.
template <typename T> class PerCpuCounter {
private:
  CacheAlignedAtomic<T> boot_buf_[moss::kernel::BOOT_MAX_CPUS];
  CacheAlignedAtomic<T> *counters_{boot_buf_};
  u32 capacity_{moss::kernel::BOOT_MAX_CPUS};

public:
  constexpr PerCpuCounter() noexcept = default;

  // Expand to support more CPUs (call after MM init)
  bool expand(u32 num_cpus) noexcept {
    if (num_cpus <= capacity_) {
      return true;
    }
    auto *new_buf = new CacheAlignedAtomic<T>[num_cpus]();
    for (u32 i = 0; i < capacity_; ++i) {
      new_buf[i].value.store(counters_[i].load(MemoryOrder::Relaxed), MemoryOrder::Relaxed);
    }
    counters_ = new_buf;
    capacity_ = num_cpus;
    return true;
  }

  [[nodiscard]] AtomicCounter<T> &get_local() noexcept {
    u32 cpu_id = moss::kernel::arch::get_current_cpu_id();
    return counters_[cpu_id % capacity_].value;
  }

  [[nodiscard]] T get_total() const noexcept {
    T total = 0;
    u32 limit = moss::kernel::g_num_cpus < capacity_ ? moss::kernel::g_num_cpus : capacity_;
    for (u32 i = 0; i < limit; ++i) {
      total += counters_[i].load(MemoryOrder::Relaxed);
    }
    return total;
  }
};

} // namespace moss::kernel::containers

// ============================================================================
// Lock-free queues
// ============================================================================
export namespace moss::kernel::containers {

// Queue node base (intrusive design)
template <typename T> struct QueueNode {
  AtomicPtr<QueueNode<T>> next;
  T data;

  template <typename... Args> constexpr QueueNode(Args &&...args) noexcept : next(nullptr), data(moss::move(args)...) {}
};

// SPSC (Single Producer Single Consumer) lock-free queue
template <typename T, moss::kernel::usize Capacity> class SPSCQueue {
private:
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
  static constexpr moss::kernel::usize MASK = Capacity - 1;

  alignas(moss::kernel::CACHE_LINE_SIZE) CacheAlignedAtomic<moss::kernel::usize> head_{0};
  alignas(moss::kernel::CACHE_LINE_SIZE) CacheAlignedAtomic<moss::kernel::usize> tail_{0};

  alignas(moss::kernel::CACHE_LINE_SIZE) T data_[Capacity];

public:
  constexpr SPSCQueue() noexcept = default;

  SPSCQueue(const SPSCQueue &) = delete;
  SPSCQueue &operator=(const SPSCQueue &) = delete;
  SPSCQueue(SPSCQueue &&) = delete;
  SPSCQueue &operator=(SPSCQueue &&) = delete;

  template <typename U> [[nodiscard]] bool try_enqueue(U &&item) noexcept {
    const moss::kernel::usize current_tail = tail_.load(MemoryOrder::Relaxed);
    const moss::kernel::usize next_tail = (current_tail + 1) & MASK;

    if (next_tail == head_.load(MemoryOrder::Acquire)) {
      return false;
    }

    data_[current_tail] = moss::forward<U>(item);
    tail_.store(next_tail, MemoryOrder::Release);
    return true;
  }

  [[nodiscard]] bool try_dequeue(T &result) noexcept {
    const moss::kernel::usize current_head = head_.load(MemoryOrder::Relaxed);

    if (current_head == tail_.load(MemoryOrder::Acquire)) {
      return false;
    }

    result = moss::move(data_[current_head]);
    head_.store((current_head + 1) & MASK, MemoryOrder::Release);
    return true;
  }

  [[nodiscard]] bool empty() const noexcept {
    return head_.load(MemoryOrder::Acquire) == tail_.load(MemoryOrder::Acquire);
  }

  [[nodiscard]] bool full() const noexcept {
    const moss::kernel::usize current_tail = tail_.load(MemoryOrder::Acquire);
    const moss::kernel::usize next_tail = (current_tail + 1) & MASK;
    return next_tail == head_.load(MemoryOrder::Acquire);
  }

  [[nodiscard]] moss::kernel::usize approximate_size() const noexcept {
    const moss::kernel::usize current_head = head_.load(MemoryOrder::Acquire);
    const moss::kernel::usize current_tail = tail_.load(MemoryOrder::Acquire);
    return (current_tail - current_head) & MASK;
  }

  [[nodiscard]] static constexpr moss::kernel::usize capacity() noexcept { return Capacity - 1; }
};

// MPSC (Multiple Producer Single Consumer) lock-free queue
template <typename T> class MPSCQueue {
private:
  alignas(moss::kernel::CACHE_LINE_SIZE) AtomicPtr<QueueNode<T>> head_;
  alignas(moss::kernel::CACHE_LINE_SIZE) AtomicPtr<QueueNode<T>> tail_;
  QueueNode<T> stub_;

public:
  MPSCQueue() noexcept : head_(&stub_), tail_(&stub_) { stub_.next.store(nullptr, MemoryOrder::Relaxed); }

  MPSCQueue(const MPSCQueue &) = delete;
  MPSCQueue &operator=(const MPSCQueue &) = delete;
  MPSCQueue(MPSCQueue &&) = delete;
  MPSCQueue &operator=(MPSCQueue &&) = delete;

  void enqueue(QueueNode<T> *node) noexcept {
    node->next.store(nullptr, MemoryOrder::Relaxed);
    QueueNode<T> *prev_tail = tail_.exchange(node, MemoryOrder::AcqRel);
    prev_tail->next.store(node, MemoryOrder::Release);
  }

  [[nodiscard]] QueueNode<T> *try_dequeue() noexcept {
    QueueNode<T> *head = head_.load(MemoryOrder::Relaxed);
    QueueNode<T> *next = head->next.load(MemoryOrder::Acquire);

    if (next == nullptr) {
      return nullptr;
    }

    head_.store(next, MemoryOrder::Relaxed);
    return next;
  }

  [[nodiscard]] bool empty() const noexcept {
    QueueNode<T> *head = head_.load(MemoryOrder::Acquire);
    QueueNode<T> *next = head->next.load(MemoryOrder::Acquire);
    return next == nullptr;
  }
};

// Fixed-size object pool (for use with MPSC queue)
template <typename T, moss::kernel::usize PoolSize>
  requires(PoolSize > 0 && (PoolSize & (PoolSize - 1)) == 0)
class ObjectPool {
private:
  struct PoolNode : public QueueNode<T> {
    template <typename... Args>
    constexpr PoolNode(Args &&...args) noexcept(moss::is_nothrow_constructible_v<T, Args...>)
        : QueueNode<T>(moss::forward<Args>(args)...) {}
  };

  alignas(PAGE_SIZE) PoolNode pool_[PoolSize];
  MPSCQueue<T> free_list_;

public:
  constexpr ObjectPool() noexcept {
    for (moss::kernel::usize i = 0; i < PoolSize; ++i) {
      free_list_.enqueue(&pool_[i]);
    }
  }

  [[nodiscard]] QueueNode<T> *allocate() noexcept { return free_list_.try_dequeue(); }

  void deallocate(QueueNode<T> *node) noexcept {
    if (node != nullptr) {
      node->~QueueNode<T>();
      new (node) QueueNode<T>();
      free_list_.enqueue(node);
    }
  }

  [[nodiscard]] bool has_available() const noexcept { return !free_list_.empty(); }

  [[nodiscard]] static constexpr moss::kernel::usize pool_size() noexcept { return PoolSize; }
};

// MPMC (Multiple Producer Multiple Consumer) queue
template <typename T, moss::kernel::usize NumConsumers, moss::kernel::usize QueueCapacity>
  requires(QueueCapacity > 0 && (QueueCapacity & (QueueCapacity - 1)) == 0) && (NumConsumers > 0)
class MPMCQueue {
private:
  SPSCQueue<T, QueueCapacity> queues_[NumConsumers];
  AtomicCounter<moss::kernel::usize> round_robin_counter_;

public:
  constexpr MPMCQueue() noexcept : round_robin_counter_(0) {}

  MPMCQueue(const MPMCQueue &) = delete;
  MPMCQueue &operator=(const MPMCQueue &) = delete;
  MPMCQueue(MPMCQueue &&) = delete;
  MPMCQueue &operator=(MPMCQueue &&) = delete;

  template <typename U> [[nodiscard]] bool try_enqueue(U &&item) noexcept {
    const moss::kernel::usize start_idx = round_robin_counter_.fetch_add(1, MemoryOrder::Relaxed) % NumConsumers;

    for (moss::kernel::usize i = 0; i < NumConsumers; ++i) {
      moss::kernel::usize queue_idx = (start_idx + i) % NumConsumers;
      if (queues_[queue_idx].try_enqueue(moss::forward<U>(item))) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool try_dequeue(moss::kernel::usize consumer_id, T &result) noexcept {
    if (consumer_id >= NumConsumers) {
      return false;
    }
    return queues_[consumer_id].try_dequeue(result);
  }

  [[nodiscard]] bool try_dequeue_any(T &result) noexcept {
    const moss::kernel::usize start_idx = moss::kernel::arch::get_current_cpu_id() % NumConsumers;

    for (moss::kernel::usize i = 0; i < NumConsumers; ++i) {
      moss::kernel::usize queue_idx = (start_idx + i) % NumConsumers;
      if (queues_[queue_idx].try_dequeue(result)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool empty() const noexcept {
    for (moss::kernel::usize i = 0; i < NumConsumers; ++i) {
      if (!queues_[i].empty()) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] moss::kernel::usize approximate_total_size() const noexcept {
    moss::kernel::usize total = 0;
    for (moss::kernel::usize i = 0; i < NumConsumers; ++i) {
      total += queues_[i].approximate_size();
    }
    return total;
  }
};

// Queue type aliases
using ProcessQueue = SPSCQueue<ProcessId, 256>;
using MessageQueue = MPSCQueue<u8>;
using WorkQueue = MPMCQueue<ProcessId, 4, 128>;

} // namespace moss::kernel::containers

// ============================================================================
// Per-CPU data structures
// ============================================================================
export namespace moss::kernel::containers {

// Per-CPU data accessor — each slot is cache-line aligned to prevent false sharing.
//
// Two-phase design for runtime-unlimited CPU count:
//   Phase 1 (boot): uses inline boot_buf_[BOOT_MAX_CPUS] — no heap needed.
//   Phase 2 (post-MM): expand(n) allocates n slots via operator new.
//
// Static/global instances start in Phase 1 and call expand() after MM init.
// Heap-allocated instances (inside new CfsScheduler, etc.) use the Args
// constructor which directly allocates for g_num_cpus.
template <typename T> class PerCpuData {
private:
  struct alignas(moss::kernel::CACHE_LINE_SIZE) PaddedSlot {
    T value;
    template <typename... Args>
    constexpr explicit PaddedSlot(Args &&...args) noexcept : value(moss::forward<Args>(args)...) {}
    constexpr PaddedSlot() noexcept : value{} {}
  };

  PaddedSlot boot_buf_[moss::kernel::BOOT_MAX_CPUS];
  // nullptr/0 denotes the inline boot buffer. Keeping the static initializer
  // all-zero lets global PerCpuData instances live in .bss instead of writing
  // the entire inline buffer to the kernel image just because it contains a
  // self-referential pointer.
  PaddedSlot *data_{nullptr};
  u32 capacity_{0};

public:
  constexpr PerCpuData() noexcept : boot_buf_{} {}

  // Fill all slots with the same value.
  // If g_num_cpus > BOOT_MAX_CPUS, dynamically allocates.
  template <typename... Args> explicit PerCpuData(Args &&...args) noexcept : boot_buf_{} {
    u32 target = moss::kernel::g_num_cpus;
    if (target > moss::kernel::BOOT_MAX_CPUS) {
      data_ = new PaddedSlot[target];
      capacity_ = target;
    }
    PaddedSlot *active_data = data();
    for (u32 i = 0; i < capacity(); ++i) {
      new (&active_data[i].value) T(args...);
    }
  }

  PerCpuData(const PerCpuData &) = delete;
  PerCpuData &operator=(const PerCpuData &) = delete;

  PerCpuData(PerCpuData &&other) noexcept : boot_buf_{} {
    if (other.data_ != nullptr) {
      // Other uses dynamic buffer — steal it
      data_ = other.data_;
      capacity_ = other.capacity_;
      other.data_ = nullptr;
      other.capacity_ = 0;
    } else {
      // Other uses boot buffer — copy element-wise
      for (u32 i = 0; i < moss::kernel::BOOT_MAX_CPUS; ++i) {
        boot_buf_[i].value = static_cast<T &&>(other.boot_buf_[i].value);
      }
    }
  }

  PerCpuData &operator=(PerCpuData &&other) noexcept {
    if (this != &other) {
      // Free our dynamic buffer if any
      if (data_ != nullptr) {
        delete[] data_;
      }
      data_ = nullptr;
      capacity_ = 0;
      if (other.data_ != nullptr) {
        data_ = other.data_;
        capacity_ = other.capacity_;
        other.data_ = nullptr;
        other.capacity_ = 0;
      } else {
        for (u32 i = 0; i < moss::kernel::BOOT_MAX_CPUS; ++i) {
          boot_buf_[i].value = static_cast<T &&>(other.boot_buf_[i].value);
        }
      }
    }
    return *this;
  }

  // Expand capacity to num_cpus (call after MM init).
  // Copies existing boot_buf_ data to the new buffer.
  bool expand(u32 num_cpus) noexcept {
    const u32 old_capacity = capacity();
    if (num_cpus <= old_capacity) {
      return true;
    }
    auto *new_buf = new PaddedSlot[num_cpus]();
    PaddedSlot *old_data = data();
    for (u32 i = 0; i < old_capacity; ++i) {
      new_buf[i].value = static_cast<T &&>(old_data[i].value);
    }
    if (data_ != nullptr) {
      delete[] data_;
    }
    data_ = new_buf;
    capacity_ = num_cpus;
    return true;
  }

  [[nodiscard]] u32 capacity() const noexcept { return capacity_ == 0 ? moss::kernel::BOOT_MAX_CPUS : capacity_; }

  [[nodiscard]] T &get_local() noexcept { return data()[get_current_cpu_id()].value; }

  [[nodiscard]] const T &get_local() const noexcept { return data()[get_current_cpu_id()].value; }

  [[nodiscard]] T &get_cpu(moss::kernel::usize cpu_id) noexcept { return data()[cpu_id % capacity()].value; }

  [[nodiscard]] const T &get_cpu(moss::kernel::usize cpu_id) const noexcept {
    return data()[cpu_id % capacity()].value;
  }

  template <typename Func> void for_each_cpu(Func &&func) {
    const u32 active_capacity = capacity();
    PaddedSlot *active_data = data();
    u32 limit = moss::kernel::g_num_cpus < active_capacity ? moss::kernel::g_num_cpus : active_capacity;
    for (u32 i = 0; i < limit; ++i) {
      func(i, active_data[i].value);
    }
  }

  template <typename Func> void for_each_cpu(Func &&func) const {
    const u32 active_capacity = capacity();
    const PaddedSlot *active_data = data();
    u32 limit = moss::kernel::g_num_cpus < active_capacity ? moss::kernel::g_num_cpus : active_capacity;
    for (u32 i = 0; i < limit; ++i) {
      func(i, active_data[i].value);
    }
  }

  template <typename Func, typename Result = T>
  [[nodiscard]] Result fold(Func &&func, Result initial = Result{}) const {
    Result result = initial;
    const u32 active_capacity = capacity();
    const PaddedSlot *active_data = data();
    u32 limit = moss::kernel::g_num_cpus < active_capacity ? moss::kernel::g_num_cpus : active_capacity;
    for (u32 i = 0; i < limit; ++i) {
      result = func(result, active_data[i].value);
    }
    return result;
  }

  [[nodiscard]] T sum() const noexcept {
    T total{};
    const u32 active_capacity = capacity();
    const PaddedSlot *active_data = data();
    u32 limit = moss::kernel::g_num_cpus < active_capacity ? moss::kernel::g_num_cpus : active_capacity;
    for (u32 i = 0; i < limit; ++i) {
      total += active_data[i].value;
    }
    return total;
  }

private:
  [[nodiscard]] PaddedSlot *data() noexcept { return data_ == nullptr ? boot_buf_ : data_; }

  [[nodiscard]] const PaddedSlot *data() const noexcept { return data_ == nullptr ? boot_buf_ : data_; }

  [[nodiscard]] static moss::kernel::usize get_current_cpu_id() noexcept {
    return static_cast<moss::kernel::usize>(moss::kernel::arch::get_current_cpu_id());
  }
};

// Per-CPU atomic counter
template <typename T> class PerCpuAtomicCounter {
private:
  PerCpuData<CacheAlignedAtomic<T>> counters_;

public:
  constexpr PerCpuAtomicCounter() noexcept = default;

  [[nodiscard]] T fetch_add_local(T value = 1) noexcept {
    return counters_.get_local().value.fetch_add(value, MemoryOrder::Relaxed);
  }

  [[nodiscard]] T fetch_sub_local(T value = 1) noexcept {
    return counters_.get_local().value.fetch_sub(value, MemoryOrder::Relaxed);
  }

  [[nodiscard]] T load_local() const noexcept { return counters_.get_local().value.load(MemoryOrder::Relaxed); }

  [[nodiscard]] T load_total() const noexcept {
    T total = 0;
    counters_.for_each_cpu(
        [&total](moss::kernel::usize, const auto &counter) { total += counter.value.load(MemoryOrder::Relaxed); });
    return total;
  }

  void reset_all() noexcept {
    counters_.for_each_cpu([](moss::kernel::usize, auto &counter) { counter.value.store(0, MemoryOrder::Relaxed); });
  }

  [[nodiscard]] T operator++() noexcept { return fetch_add_local() + 1; }
  [[nodiscard]] T operator++(int) noexcept { return fetch_add_local(); }
  [[nodiscard]] T operator--() noexcept { return fetch_sub_local() - 1; }
  [[nodiscard]] T operator--(int) noexcept { return fetch_sub_local(); }

  PerCpuAtomicCounter &operator+=(T value) noexcept {
    fetch_add_local(value);
    return *this;
  }

  PerCpuAtomicCounter &operator-=(T value) noexcept {
    fetch_sub_local(value);
    return *this;
  }

  [[nodiscard]] operator T() const noexcept { return load_total(); }
};

// Per-CPU work queue
template <typename T, moss::kernel::usize QueueSize = 256> class PerCpuWorkQueue {
private:
  PerCpuData<T> data_;

public:
  constexpr PerCpuWorkQueue() noexcept = default;

  [[nodiscard]] bool enqueue_local(const T &item) noexcept { return data_.get_local().try_enqueue(item); }

  [[nodiscard]] bool enqueue_local(T &&item) noexcept { return data_.get_local().try_enqueue(static_cast<T &&>(item)); }

  [[nodiscard]] bool dequeue_local(T &result) noexcept { return data_.get_local().try_dequeue(result); }

  [[nodiscard]] bool enqueue_to_cpu(moss::kernel::usize cpu_id, const T &item) noexcept {
    return data_.get_cpu(cpu_id).try_enqueue(item);
  }

  [[nodiscard]] bool enqueue_to_cpu(moss::kernel::usize cpu_id, T &&item) noexcept {
    return data_.get_cpu(cpu_id).try_enqueue(static_cast<T &&>(item));
  }

  [[nodiscard]] bool dequeue_from_cpu(moss::kernel::usize cpu_id, T &result) noexcept {
    return data_.get_cpu(cpu_id).try_dequeue(result);
  }

  [[nodiscard]] bool steal_work(T &result) noexcept {
    moss::kernel::usize current_cpu = static_cast<moss::kernel::usize>(moss::kernel::arch::get_current_cpu_id());
    u32 num_cpus = moss::kernel::g_num_cpus;
    for (u32 i = 1; i < num_cpus; ++i) {
      moss::kernel::usize target_cpu = (current_cpu + i) % num_cpus;
      if (data_.get_cpu(target_cpu).try_dequeue(result)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool empty_local() const noexcept { return data_.get_local().empty(); }

  [[nodiscard]] bool empty_all() const noexcept {
    bool all_empty = true;
    data_.for_each_cpu([&all_empty](moss::kernel::usize, const auto &queue) {
      if (!queue.empty()) {
        all_empty = false;
      }
    });
    return all_empty;
  }

  [[nodiscard]] moss::kernel::usize approximate_total_size() const noexcept {
    moss::kernel::usize total_size = 0;
    data_.for_each_cpu(
        [&total_size](moss::kernel::usize, const auto &queue) { total_size += queue.approximate_size(); });
    return total_size;
  }
};

// Per-CPU type aliases
using PerCpuU32Counter = PerCpuAtomicCounter<u32>;
using PerCpuU64Counter = PerCpuAtomicCounter<u64>;
using PerCpuUSizeCounter = PerCpuAtomicCounter<moss::kernel::usize>;

using ProcessWorkQueue = PerCpuWorkQueue<ProcessId, 128>;
using InterruptWorkQueue = PerCpuWorkQueue<InterruptId, 64>;

} // namespace moss::kernel::containers

// ============================================================================
// Lock-protected owning data structures
// ============================================================================
export namespace moss::kernel::containers {

// Lookup returns an independent value, never storage inside a node. For owned
// pointees use shared_ptr values; raw pointer values remain externally owned.
template <typename T> class Optional;

// ponytail: one IRQ-safe lock per container; split locks only after contention measurements.
// Scoped callbacks run under the lock: no blocking, reentry or escaping references.
// Destructors and snapshot callbacks run after unlocking. Destroy the container
// itself only after all operations on it have stopped.
template <typename T> class LockedList {
  struct Node {
    Node *next{nullptr};
    T data;
    template <typename... Args> explicit Node(Args &&...args) : data(moss::forward<Args>(args)...) {}
  };
  Node *head_{nullptr};
  usize size_{0};
  mutable IrqSpinLock lock_{};

  void publish(Node *node) {
    LockGuard<IrqSpinLock> guard(lock_);
    node->next = head_;
    head_ = node;
    ++size_;
  }

public:
  constexpr LockedList() noexcept = default;
  ~LockedList() { clear(); }
  LockedList(const LockedList &) = delete;
  LockedList &operator=(const LockedList &) = delete;

  template <typename... Args> void push_front(Args &&...args) { publish(new Node(moss::forward<Args>(args)...)); }

  template <typename... Args> [[nodiscard]] bool try_push_front(Args &&...args) {
    auto *storage = moss::abi::bridge::moss_heap_allocate(sizeof(Node), alignof(Node));
    if (!storage)
      return false;
    publish(new (storage) Node(moss::forward<Args>(args)...));
    return true;
  }

  // Test and publication are one transaction (e.g. rejecting overlapping VMAs).
  template <typename Predicate, typename... Args> bool push_front_unless(Predicate conflicts, Args &&...args) {
    auto *storage = moss::abi::bridge::moss_heap_allocate(sizeof(Node), alignof(Node));
    if (!storage)
      return false;
    auto node = unique_ptr<Node>(new (storage) Node(moss::forward<Args>(args)...));
    bool inserted = true;
    {
      LockGuard<IrqSpinLock> guard(lock_);
      for (auto *it = head_; it; it = it->next) {
        if (conflicts(static_cast<const T &>(it->data))) {
          inserted = false;
          break;
        }
      }
      if (inserted) {
        node->next = head_;
        head_ = node.release();
        ++size_;
      }
    }
    return inserted;
  }

  template <typename Predicate> bool remove_if(Predicate pred) {
    Node *removed = nullptr;
    {
      LockGuard<IrqSpinLock> guard(lock_);
      for (auto **link = &head_; *link; link = &(*link)->next) {
        if (pred(static_cast<const T &>((*link)->data))) {
          removed = *link;
          *link = removed->next;
          --size_;
          break;
        }
      }
    }
    delete removed;
    return removed != nullptr;
  }

  bool remove(const T &value) {
    return remove_if([&](const T &item) { return item == value; });
  }

  template <typename Predicate> [[nodiscard]] Optional<T> find_if(Predicate pred) const {
    LockGuard<IrqSpinLock> guard(lock_);
    for (auto *node = head_; node; node = node->next)
      if (pred(static_cast<const T &>(node->data)))
        return node->data;
    return {};
  }

  [[nodiscard]] Optional<T> find(const T &value) const {
    return find_if([&](const T &item) { return item == value; });
  }

  template <typename Predicate, typename Func> bool update_if(Predicate pred, Func update) {
    LockGuard<IrqSpinLock> guard(lock_);
    for (auto *node = head_; node; node = node->next) {
      if (pred(static_cast<const T &>(node->data))) {
        update(node->data);
        return true;
      }
    }
    return false;
  }

  template <typename Func> void for_each(Func func) const {
    LockGuard<IrqSpinLock> guard(lock_);
    for (auto *node = head_; node; node = node->next)
      func(static_cast<const T &>(node->data));
  }

  // O(n) temporary storage buys a stable iteration with no lock held by callers.
  template <typename Func> void for_each_snapshot(Func func) const {
    LockedList snapshot;
    {
      LockGuard<IrqSpinLock> guard(lock_);
      auto **tail = &snapshot.head_;
      for (auto *node = head_; node; node = node->next) {
        *tail = new Node(node->data);
        tail = &(*tail)->next;
        ++snapshot.size_;
      }
    }
    for (auto *node = snapshot.head_; node; node = node->next)
      func(static_cast<const T &>(node->data));
  }

  [[nodiscard]] usize size() const noexcept {
    LockGuard<IrqSpinLock> guard(lock_);
    return size_;
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

  void clear() {
    Node *nodes;
    {
      LockGuard<IrqSpinLock> guard(lock_);
      nodes = head_;
      head_ = nullptr;
      size_ = 0;
    }
    while (nodes) {
      auto *next = nodes->next;
      delete nodes;
      nodes = next;
    }
  }
};

// ============================================================================
// WaitQueue — generic blocking primitive for process synchronization
// ============================================================================

// WaitQueue entry: holds a raw pointer to a blocked thread.
// Thread* is an opaque pointer here — the actual Thread type is defined
// in moss.process. WaitQueue only stores/retrieves the pointer; the
// sleep()/wake logic that calls scheduler APIs lives in kernel module
// bridge functions.
struct WaitQueueEntry {
  void *thread;   // Actually Thread*, but opaque to avoid module cycle
  bool exclusive; // If true, wake_up wakes at most one such waiter

  WaitQueueEntry() noexcept : thread(nullptr), exclusive(false) {}
  explicit WaitQueueEntry(void *t, bool excl = false) noexcept : thread(t), exclusive(excl) {}

  bool operator==(const WaitQueueEntry &other) const noexcept { return thread == other.thread; }
};

// WaitQueue: a list of threads waiting for an event.
// Data structure only — actual sleep/wake logic requires scheduler access,
// so it is implemented as bridge functions in the kernel module.
class WaitQueue {
private:
  LockedList<WaitQueueEntry> waiters_;

public:
  constexpr WaitQueue() noexcept = default;

  WaitQueue(const WaitQueue &) = delete;
  WaitQueue &operator=(const WaitQueue &) = delete;
  WaitQueue(WaitQueue &&) = delete;
  WaitQueue &operator=(WaitQueue &&) = delete;

  // Add a thread to the wait queue (non-exclusive by default).
  void add_waiter(void *thread, bool exclusive = false) { waiters_.push_front(WaitQueueEntry(thread, exclusive)); }

  // Remove a specific thread from the wait queue
  void remove_waiter(void *thread) { waiters_.remove(WaitQueueEntry(thread)); }

  // Wake ALL waiters — iterates over every entry and calls func(void*).
  // This is the "thundering herd" path; prefer wake_one() when only
  // a single waiter should be woken (e.g. waitpid, accept).
  template <typename Func> void for_each_waiter(Func func) const {
    waiters_.for_each([&func](const WaitQueueEntry &entry) { func(entry.thread); });
  }

  // Wake at most one exclusive waiter + all non-exclusive waiters.
  // Matches Linux wake_up() semantics: non-exclusive waiters are
  // always woken; for exclusive waiters, only the first one is woken.
  // Returns the number of waiters woken.
  template <typename Func> moss::kernel::u32 wake_up(Func func) const {
    moss::kernel::u32 woken = 0;
    bool exclusive_woken = false;
    waiters_.for_each([&](const WaitQueueEntry &entry) {
      if (entry.exclusive && exclusive_woken) {
        return; // already woke one exclusive waiter
      }
      func(entry.thread);
      ++woken;
      if (entry.exclusive) {
        exclusive_woken = true;
      }
    });
    return woken;
  }

  // Wake exactly one waiter (the first in the list, regardless of flags).
  // Simpler than wake_up() for cases where exactly one consumer is needed.
  template <typename Func> bool wake_one(Func func) const {
    bool woken = false;
    waiters_.for_each([&](const WaitQueueEntry &entry) {
      if (!woken) {
        func(entry.thread);
        woken = true;
      }
    });
    return woken;
  }

  // Check if any threads are waiting
  [[nodiscard]] bool has_waiters() const noexcept { return !waiters_.empty(); }

  // Clear all waiters (used during teardown)
  void clear() { waiters_.clear(); }
};

// Node ownership, lookup copies and write serialization share one lock.
template <typename Key, typename Value, usize BucketCount = 256> class LockedHashMap {
  static_assert(BucketCount != 0 && (BucketCount & (BucketCount - 1)) == 0,
                "BucketCount must be a nonzero power of two");
  struct Entry {
    Key key;
    Value value;
    template <typename K, typename V> Entry(K &&k, V &&v) : key(moss::forward<K>(k)), value(moss::forward<V>(v)) {}
  };
  struct Node {
    Node *next{nullptr};
    Entry entry;
    template <typename K, typename V> Node(K &&k, V &&v) : entry(moss::forward<K>(k), moss::forward<V>(v)) {}
  };
  Node *buckets_[BucketCount]{};
  usize size_{0};
  mutable IrqSpinLock lock_{};

  template <typename K> static usize bucket(const K &key) noexcept {
    // Normalize lookup keys to the stored type before hashing their bytes.
    Key stored_key = static_cast<Key>(key);
    usize hash = 2166136261U;
    auto *data = reinterpret_cast<const u8 *>(&stored_key);
    for (usize i = 0; i < sizeof(Key); ++i) {
      hash ^= data[i];
      hash *= 16777619U;
    }
    return hash & (BucketCount - 1);
  }

  void publish(Node *node) {
    Node *old = nullptr;
    {
      LockGuard<IrqSpinLock> guard(lock_);
      auto **link = &buckets_[bucket(node->entry.key)];
      while (*link && !((*link)->entry.key == node->entry.key))
        link = &(*link)->next;
      old = *link;
      node->next = old ? old->next : nullptr;
      *link = node;
      if (!old)
        ++size_;
    }
    delete old;
  }

public:
  constexpr LockedHashMap() noexcept = default;
  ~LockedHashMap() { clear(); }
  LockedHashMap(const LockedHashMap &) = delete;
  LockedHashMap &operator=(const LockedHashMap &) = delete;

  template <typename K, typename V> void insert_or_update(K &&key, V &&value) {
    publish(new Node(moss::forward<K>(key), moss::forward<V>(value)));
  }

  template <typename K, typename V> [[nodiscard]] bool try_insert_or_update(K &&key, V &&value) {
    auto *storage = moss::abi::bridge::moss_heap_allocate(sizeof(Node), alignof(Node));
    if (!storage)
      return false;
    publish(new (storage) Node(moss::forward<K>(key), moss::forward<V>(value)));
    return true;
  }

  template <typename K> [[nodiscard]] Optional<Value> find(const K &key) const {
    LockGuard<IrqSpinLock> guard(lock_);
    for (auto *node = buckets_[bucket(key)]; node; node = node->next)
      if (node->entry.key == key)
        return node->entry.value;
    return {};
  }

  // Factory/destruction run outside the lock; concurrent creators share the winner.
  template <typename Factory> Value get_or_insert(const Key &key, Factory factory) {
    if (auto found = find(key))
      return *found;
    auto *candidate = new Node(key, factory());
    Value result;
    {
      LockGuard<IrqSpinLock> guard(lock_);
      auto **link = &buckets_[bucket(key)];
      while (*link && !((*link)->entry.key == key))
        link = &(*link)->next;
      if (!*link) {
        *link = candidate;
        candidate = nullptr;
        ++size_;
      }
      result = (*link)->entry.value;
    }
    delete candidate;
    return result;
  }

  template <typename K> [[nodiscard]] Optional<Value> extract(const K &key) {
    Node *node = nullptr;
    Optional<Value> result;
    {
      LockGuard<IrqSpinLock> guard(lock_);
      auto **link = &buckets_[bucket(key)];
      while (*link && !((*link)->entry.key == key))
        link = &(*link)->next;
      if (*link) {
        node = *link;
        *link = node->next;
        --size_;
        result.emplace(moss::move(node->entry.value));
      }
    }
    delete node;
    return result;
  }

  template <typename K> bool remove(const K &key) { return static_cast<bool>(extract(key)); }

  [[nodiscard]] usize size() const noexcept {
    LockGuard<IrqSpinLock> guard(lock_);
    return size_;
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

  // Scoped callback: no blocking, reentry or retaining references to entries.
  template <typename Func> void for_each(Func func) const {
    LockGuard<IrqSpinLock> guard(lock_);
    for (const auto *head : buckets_)
      for (auto *node = head; node; node = node->next)
        func(node->entry);
  }

  template <typename Func> void for_each_snapshot(Func func) const {
    Node *snapshot = nullptr;
    auto **tail = &snapshot;
    {
      LockGuard<IrqSpinLock> guard(lock_);
      for (const auto *head : buckets_)
        for (auto *node = head; node; node = node->next) {
          *tail = new Node(node->entry.key, node->entry.value);
          tail = &(*tail)->next;
        }
    }
    while (snapshot) {
      auto *next = snapshot->next;
      func(static_cast<const Entry &>(snapshot->entry));
      delete snapshot;
      snapshot = next;
    }
  }

  void clear() {
    Node *nodes = nullptr;
    {
      LockGuard<IrqSpinLock> guard(lock_);
      for (auto *&head : buckets_) {
        while (head) {
          auto *next = head->next;
          head->next = nodes;
          nodes = head;
          head = next;
        }
      }
      size_ = 0;
    }
    while (nodes) {
      auto *next = nodes->next;
      delete nodes;
      nodes = next;
    }
  }
};

using ProcessList = LockedList<moss::kernel::ProcessId>;
using DeviceRegistry = LockedHashMap<moss::kernel::DeviceId, moss::kernel::VirtAddr>;

} // namespace moss::kernel::containers

// ============================================================================
// Slab memory allocator
// ============================================================================
export namespace moss::kernel::containers {

// Kernel utility function
template <typename T> constexpr const T &kernel_max(const T &a, const T &b) noexcept { return (a < b) ? b : a; }

// Slab allocator error codes
enum class SlabError : u32 { OutOfMemory = 1, InvalidSize = 2, DoubleFree = 3, CorruptedSlab = 4 };

// Slab result types
template <typename T> using SlabResult = moss::kernel::Result<T, SlabError>;
using SlabVoidResult = moss::kernel::Result<void, SlabError>;

// Memory alignment utilities
template <moss::kernel::usize Alignment> constexpr moss::kernel::usize align_up(moss::kernel::usize value) noexcept {
  static_assert((Alignment & (Alignment - 1)) == 0, "Alignment must be power of 2");
  return (value + Alignment - 1) & ~(Alignment - 1);
}

constexpr bool is_aligned(moss::kernel::usize value, moss::kernel::usize alignment) noexcept {
  return (value & (alignment - 1)) == 0;
}

// Slab page structure
struct SlabPage {
  void *memory;
  moss::kernel::usize object_size;
  moss::kernel::usize objects_per_page;
  moss::kernel::containers::AtomicCounter<moss::kernel::usize> free_count;
  moss::kernel::containers::AtomicPtr<u8> free_list;
  moss::kernel::containers::AtomicPtr<SlabPage> next;

  SlabPage(void *mem, moss::kernel::usize obj_size, moss::kernel::usize obj_per_page) noexcept
      : memory(mem), object_size(obj_size), objects_per_page(obj_per_page), free_count(obj_per_page),
        free_list(nullptr), next(nullptr) {
    initialize_free_list();
  }

private:
  void initialize_free_list() noexcept {
    char *current = static_cast<char *>(memory);
    void *last_free = nullptr;

    for (moss::kernel::usize i = 0; i < objects_per_page; ++i) {
      void **obj_ptr = reinterpret_cast<void **>(current);
      *obj_ptr = last_free;
      last_free = current;
      current += object_size;
    }

    free_list.store(static_cast<u8 *>(last_free), moss::MemoryOrder::Release);
  }
};

// Single object-size slab cache
class SlabCache {
private:
  const moss::kernel::usize object_size_;
  [[maybe_unused]] const moss::kernel::usize object_alignment_;
  const moss::kernel::usize aligned_object_size_;
  const moss::kernel::usize objects_per_page_;

  moss::kernel::containers::AtomicPtr<SlabPage> full_pages_;
  moss::kernel::containers::AtomicPtr<SlabPage> partial_pages_;
  moss::kernel::containers::AtomicPtr<SlabPage> empty_pages_;

  moss::kernel::containers::AtomicCounter<moss::kernel::usize> total_objects_;
  moss::kernel::containers::AtomicCounter<moss::kernel::usize> allocated_objects_;

public:
  SlabCache(moss::kernel::usize object_size, moss::kernel::usize alignment = alignof(void *)) noexcept
      : object_size_(object_size), object_alignment_(alignment),
        aligned_object_size_(align_up<alignof(void *)>(kernel_max<moss::kernel::usize>(object_size, sizeof(void *)))),
        objects_per_page_(moss::kernel::PAGE_SIZE / aligned_object_size_), full_pages_(nullptr),
        partial_pages_(nullptr), empty_pages_(nullptr), total_objects_(0), allocated_objects_(0) {}

  ~SlabCache() noexcept {
    free_page_list(full_pages_.load(moss::MemoryOrder::Relaxed));
    free_page_list(partial_pages_.load(moss::MemoryOrder::Relaxed));
    free_page_list(empty_pages_.load(moss::MemoryOrder::Relaxed));
  }

  SlabCache(const SlabCache &) = delete;
  SlabCache &operator=(const SlabCache &) = delete;
  SlabCache(SlabCache &&) = delete;
  SlabCache &operator=(SlabCache &&) = delete;

  [[nodiscard]] SlabResult<void *> allocate() noexcept {
    if (auto result = allocate_from_partial()) {
      return result;
    }
    if (auto result = allocate_from_empty()) {
      return result;
    }
    return allocate_new_page();
  }

  [[nodiscard]] SlabVoidResult deallocate(void *ptr) noexcept {
    if (ptr == nullptr) {
      return SlabVoidResult{};
    }

    SlabPage *page = find_page_for_object(ptr);
    if (page == nullptr) {
      return SlabVoidResult{moss::kernel::Err<SlabError>(SlabError::CorruptedSlab)};
    }

    u8 *current_free = page->free_list.load(moss::MemoryOrder::Relaxed);
    void **obj_ptr = static_cast<void **>(ptr);

    do {
      *obj_ptr = current_free;
    } while (!page->free_list.compare_exchange_weak(current_free, static_cast<u8 *>(ptr), moss::MemoryOrder::Release,
                                                    moss::MemoryOrder::Relaxed));

    moss::kernel::usize new_free_count = page->free_count.fetch_add(1, moss::MemoryOrder::AcqRel) + 1;
    (void)allocated_objects_.fetch_sub(1, moss::MemoryOrder::Relaxed);

    if (new_free_count == page->objects_per_page) {
      move_page_to_empty(page);
    } else if (new_free_count == 1) {
      move_page_from_full_to_partial(page);
    }

    return SlabVoidResult{};
  }

  [[nodiscard]] moss::kernel::usize total_objects() const noexcept {
    return total_objects_.load(moss::MemoryOrder::Relaxed);
  }

  [[nodiscard]] moss::kernel::usize allocated_objects() const noexcept {
    return allocated_objects_.load(moss::MemoryOrder::Relaxed);
  }

  [[nodiscard]] moss::kernel::usize object_size() const noexcept { return object_size_; }

  // Returns utilization as a percentage (0-100)
  [[nodiscard]] moss::kernel::usize utilization() const noexcept {
    moss::kernel::usize total = total_objects();
    if (total == 0) {
      return 0;
    }
    return (allocated_objects() * 100) / total;
  }

private:
  [[nodiscard]] SlabResult<void *> allocate_from_partial() noexcept {
    SlabPage *page = partial_pages_.load(moss::MemoryOrder::Acquire);
    if (page == nullptr) {
      return SlabResult<void *>{moss::kernel::Err<SlabError>(SlabError::OutOfMemory)};
    }
    return allocate_from_page(page);
  }

  [[nodiscard]] SlabResult<void *> allocate_from_empty() noexcept {
    SlabPage *page = empty_pages_.exchange(nullptr, moss::MemoryOrder::AcqRel);
    if (page == nullptr) {
      return SlabResult<void *>{moss::kernel::Err<SlabError>(SlabError::OutOfMemory)};
    }
    move_page_to_partial(page);
    return allocate_from_page(page);
  }

  [[nodiscard]] SlabResult<void *> allocate_new_page() noexcept {
    void *page_memory = allocate_page();
    if (page_memory == nullptr) {
      return SlabResult<void *>{moss::kernel::Err<SlabError>(SlabError::OutOfMemory)};
    }

    SlabPage *new_page = new SlabPage(page_memory, aligned_object_size_, objects_per_page_);
    (void)total_objects_.fetch_add(objects_per_page_, moss::MemoryOrder::Relaxed);

    move_page_to_partial(new_page);
    return allocate_from_page(new_page);
  }

  [[nodiscard]] SlabResult<void *> allocate_from_page(SlabPage *page) noexcept {
    u8 *current_free = page->free_list.load(moss::MemoryOrder::Acquire);
    if (current_free == nullptr) {
      return SlabResult<void *>{moss::kernel::Err<SlabError>(SlabError::OutOfMemory)};
    }

    u8 *next_free;
    do {
      if (current_free == nullptr) {
        return SlabResult<void *>{moss::kernel::Err<SlabError>(SlabError::OutOfMemory)};
      }
      next_free = *reinterpret_cast<u8 **>(current_free);
    } while (!page->free_list.compare_exchange_weak(current_free, next_free, moss::MemoryOrder::AcqRel,
                                                    moss::MemoryOrder::Acquire));

    moss::kernel::usize new_free_count = page->free_count.fetch_sub(1, moss::MemoryOrder::AcqRel) - 1;
    (void)allocated_objects_.fetch_add(1, moss::MemoryOrder::Relaxed);

    if (new_free_count == 0) {
      move_page_from_partial_to_full(page);
    }

    return SlabResult<void *>{static_cast<void *>(current_free)};
  }

  [[nodiscard]] SlabPage *find_page_for_object(void *ptr) const noexcept {
    moss::kernel::usize ptr_addr = reinterpret_cast<moss::kernel::usize>(ptr);
    moss::kernel::usize page_addr = ptr_addr & ~(moss::kernel::PAGE_SIZE - 1);

    if (auto *page = find_in_page_list(full_pages_.load(moss::MemoryOrder::Acquire), page_addr)) {
      return page;
    }
    if (auto *page = find_in_page_list(partial_pages_.load(moss::MemoryOrder::Acquire), page_addr)) {
      return page;
    }
    return find_in_page_list(empty_pages_.load(moss::MemoryOrder::Acquire), page_addr);
  }

  [[nodiscard]] SlabPage *find_in_page_list(SlabPage *head, moss::kernel::usize page_addr) const noexcept {
    SlabPage *current = head;
    while (current != nullptr) {
      moss::kernel::usize current_page_addr =
          reinterpret_cast<moss::kernel::usize>(current->memory) & ~(moss::kernel::PAGE_SIZE - 1);
      if (current_page_addr == page_addr) {
        return current;
      }
      current = current->next.load(moss::MemoryOrder::Acquire);
    }
    return nullptr;
  }

  void move_page_to_partial(SlabPage *page) noexcept {
    SlabPage *old_head = partial_pages_.load(moss::MemoryOrder::Relaxed);
    do {
      page->next.store(old_head, moss::MemoryOrder::Relaxed);
    } while (
        !partial_pages_.compare_exchange_weak(old_head, page, moss::MemoryOrder::Release, moss::MemoryOrder::Relaxed));
  }

  void move_page_to_full(SlabPage *page) noexcept {
    SlabPage *old_head = full_pages_.load(moss::MemoryOrder::Relaxed);
    do {
      page->next.store(old_head, moss::MemoryOrder::Relaxed);
    } while (
        !full_pages_.compare_exchange_weak(old_head, page, moss::MemoryOrder::Release, moss::MemoryOrder::Relaxed));
  }

  void move_page_to_empty(SlabPage *page) noexcept {
    SlabPage *old_head = empty_pages_.load(moss::MemoryOrder::Relaxed);
    do {
      page->next.store(old_head, moss::MemoryOrder::Relaxed);
    } while (
        !empty_pages_.compare_exchange_weak(old_head, page, moss::MemoryOrder::Release, moss::MemoryOrder::Relaxed));
  }

  void move_page_from_partial_to_full(SlabPage *page) noexcept {
    remove_page_from_list(partial_pages_, page);
    move_page_to_full(page);
  }

  void move_page_from_full_to_partial(SlabPage *page) noexcept {
    remove_page_from_list(full_pages_, page);
    move_page_to_partial(page);
  }

  void remove_page_from_list(moss::kernel::containers::AtomicPtr<SlabPage> &head, SlabPage *page) noexcept {
    // CAS-based removal from singly-linked list with retry.
    // Retry loop handles concurrent modifications to head or prev->next.
    constexpr int MAX_RETRIES = 16;
    for (int retry = 0; retry < MAX_RETRIES; ++retry) {
      // Case 1: page is the head.
      SlabPage *expected = page;
      // Re-load page->next inside the retry loop to avoid stale values.
      SlabPage *page_next = page->next.load(moss::MemoryOrder::Acquire);
      if (head.compare_exchange_strong(expected, page_next, moss::MemoryOrder::AcqRel, moss::MemoryOrder::Acquire)) {
        page->next.store(nullptr, moss::MemoryOrder::Relaxed);
        return;
      }

      // Case 2: page is in the middle or tail — walk from head.
      SlabPage *prev = head.load(moss::MemoryOrder::Acquire);
      while (prev != nullptr) {
        SlabPage *curr = prev->next.load(moss::MemoryOrder::Acquire);
        if (curr == page) {
          // Re-load page->next right before CAS to minimise TOCTOU window.
          SlabPage *next = page->next.load(moss::MemoryOrder::Acquire);
          if (prev->next.compare_exchange_strong(curr, next, moss::MemoryOrder::AcqRel, moss::MemoryOrder::Acquire)) {
            page->next.store(nullptr, moss::MemoryOrder::Relaxed);
            return;
          }
          // CAS failed — restart from head for this retry.
          break;
        }
        prev = curr;
      }
      // Page not found or CAS failed — retry from scratch.
    }
    // Exhausted retries — page was likely already removed by a concurrent op.
  }

  [[nodiscard]] void *allocate_page() noexcept {
    unsigned long long addr = moss::abi::bridge::moss_slab_alloc_pages(0);
    if (addr == 0) {
      return nullptr;
    }
    return reinterpret_cast<void *>(addr);
  }

  void free_page(void *ptr) noexcept {
    if (ptr == nullptr) {
      return;
    }
    (void)moss::abi::bridge::moss_slab_free_pages(reinterpret_cast<unsigned long long>(ptr), 0);
  }

  void free_page_list(SlabPage *head) noexcept {
    while (head != nullptr) {
      SlabPage *next = head->next.load(moss::MemoryOrder::Relaxed);
      free_page(head->memory);
      delete head;
      head = next;
    }
  }
};

// Multi-size slab allocator
class SlabAllocator {
private:
  static constexpr moss::kernel::usize NUM_CACHES = 32;
  static constexpr moss::kernel::usize MIN_OBJECT_SIZE = 8;
  static constexpr moss::kernel::usize MAX_OBJECT_SIZE = 4096;

  SlabCache *caches_[NUM_CACHES];
  moss::kernel::usize cache_sizes_[NUM_CACHES];

public:
  SlabAllocator() noexcept {
    moss::kernel::usize size = MIN_OBJECT_SIZE;
    for (moss::kernel::usize i = 0; i < NUM_CACHES; ++i) {
      cache_sizes_[i] = size;
      caches_[i] = new SlabCache(size);
      size *= 2;
      if (size > MAX_OBJECT_SIZE) {
        size = MAX_OBJECT_SIZE;
      }
    }
  }

  ~SlabAllocator() noexcept {
    for (moss::kernel::usize i = 0; i < NUM_CACHES; ++i) {
      delete caches_[i];
    }
  }

  SlabAllocator(const SlabAllocator &) = delete;
  SlabAllocator &operator=(const SlabAllocator &) = delete;
  SlabAllocator(SlabAllocator &&) = delete;
  SlabAllocator &operator=(SlabAllocator &&) = delete;

  [[nodiscard]] SlabResult<void *> allocate(moss::kernel::usize size) noexcept {
    moss::kernel::usize cache_index = find_cache_index(size);
    if (cache_index >= NUM_CACHES) {
      return SlabResult<void *>{moss::kernel::Err<SlabError>(SlabError::InvalidSize)};
    }
    return caches_[cache_index]->allocate();
  }

  template <typename T> [[nodiscard]] SlabResult<T *> allocate() noexcept {
    auto result = allocate(sizeof(T));
    if (!result) {
      return SlabResult<T *>{moss::kernel::Err<SlabError>(result.error())};
    }
    return SlabResult<T *>{static_cast<T *>(*result)};
  }

  [[nodiscard]] SlabVoidResult deallocate(void *ptr, moss::kernel::usize size) noexcept {
    if (ptr == nullptr) {
      return SlabVoidResult{};
    }

    moss::kernel::usize cache_index = find_cache_index(size);
    if (cache_index >= NUM_CACHES) {
      return SlabVoidResult{moss::kernel::Err<SlabError>(SlabError::InvalidSize)};
    }
    return caches_[cache_index]->deallocate(ptr);
  }

  template <typename T> [[nodiscard]] SlabVoidResult deallocate(T *ptr) noexcept { return deallocate(ptr, sizeof(T)); }

  void get_statistics() const noexcept {
    for (moss::kernel::usize i = 0; i < NUM_CACHES; ++i) {
      if (caches_[i]->total_objects() > 0) {
        // In actual implementation, output to kernel log
      }
    }
  }

private:
  [[nodiscard]] moss::kernel::usize find_cache_index(moss::kernel::usize size) const noexcept {
    for (moss::kernel::usize i = 0; i < NUM_CACHES; ++i) {
      if (cache_sizes_[i] >= size) {
        return i;
      }
    }
    return NUM_CACHES;
  }
};

// Global slab allocator instance
extern SlabAllocator *g_slab_allocator;

// Convenience functions
template <typename T, typename... Args> [[nodiscard]] SlabResult<T *> slab_new(Args &&...args) noexcept {
  auto ptr_result = g_slab_allocator->allocate<T>();
  if (!ptr_result) {
    return SlabResult<T *>{moss::kernel::Err<SlabError>(ptr_result.error())};
  }

  T *ptr = *ptr_result;
  new (ptr) T(moss::forward<Args>(args)...);
  return SlabResult<T *>{ptr};
}

template <typename T> [[nodiscard]] SlabVoidResult slab_delete(T *ptr) noexcept {
  if (ptr != nullptr) {
    ptr->~T();
    return g_slab_allocator->deallocate(ptr);
  }
  return SlabVoidResult{};
}

} // namespace moss::kernel::containers

// ============================================================================
// Optional type and container library initialization
// ============================================================================
export namespace moss::kernel::containers {

// Optional type implementation
template <typename T> class Optional {
private:
  alignas(T) u8 storage_[sizeof(T)];
  bool has_value_;

public:
  constexpr Optional() noexcept : has_value_(false) {}

  constexpr Optional(const T &value) noexcept(moss::is_nothrow_copy_constructible_v<T>) : has_value_(true) {
    new (storage_) T(value);
  }

  constexpr Optional(T &&value) noexcept(moss::is_nothrow_move_constructible_v<T>) : has_value_(true) {
    new (storage_) T(moss::move(value));
  }

  Optional(const Optional &other) noexcept(moss::is_nothrow_copy_constructible_v<T>) : has_value_(other.has_value_) {
    if (has_value_) {
      new (storage_) T(other.value());
    }
  }

  Optional(Optional &&other) noexcept(moss::is_nothrow_move_constructible_v<T>) : has_value_(other.has_value_) {
    if (has_value_) {
      new (storage_) T(moss::move(other.value()));
      other.reset();
    }
  }

  ~Optional() noexcept { reset(); }

  Optional &operator=(const Optional &other) noexcept(moss::is_nothrow_copy_assignable_v<T>) {
    if (this != &other) {
      if (other.has_value_) {
        if (has_value_) {
          value() = other.value();
        } else {
          new (storage_) T(other.value());
          has_value_ = true;
        }
      } else {
        reset();
      }
    }
    return *this;
  }

  Optional &operator=(Optional &&other) noexcept(moss::is_nothrow_move_assignable_v<T>) {
    if (this != &other) {
      if (other.has_value_) {
        if (has_value_) {
          value() = moss::move(other.value());
        } else {
          new (storage_) T(moss::move(other.value()));
          has_value_ = true;
        }
        other.reset();
      } else {
        reset();
      }
    }
    return *this;
  }

  [[nodiscard]] constexpr bool has_value() const noexcept { return has_value_; }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value_; }

  [[nodiscard]] constexpr T &value() & noexcept { return *reinterpret_cast<T *>(storage_); }

  [[nodiscard]] constexpr const T &value() const & noexcept { return *reinterpret_cast<const T *>(storage_); }

  [[nodiscard]] constexpr T &&value() && noexcept { return moss::move(*reinterpret_cast<T *>(storage_)); }

  [[nodiscard]] constexpr const T &&value() const && noexcept {
    return moss::move(*reinterpret_cast<const T *>(storage_));
  }

  [[nodiscard]] constexpr T &operator*() & noexcept { return value(); }
  [[nodiscard]] constexpr const T &operator*() const & noexcept { return value(); }
  [[nodiscard]] constexpr T &&operator*() && noexcept { return moss::move(value()); }
  [[nodiscard]] constexpr const T &&operator*() const && noexcept { return moss::move(value()); }

  [[nodiscard]] constexpr T *operator->() noexcept { return &value(); }
  [[nodiscard]] constexpr const T *operator->() const noexcept { return &value(); }

  void reset() noexcept {
    if (has_value_) {
      value().~T();
      has_value_ = false;
    }
  }

  template <typename... Args> T &emplace(Args &&...args) noexcept(moss::is_nothrow_constructible_v<T, Args...>) {
    reset();
    new (storage_) T(moss::forward<Args>(args)...);
    has_value_ = true;
    return value();
  }
};

// Container library initialization
class ContainerLibrary {
private:
  static inline bool initialized_ = false;
  static inline SlabAllocator *slab_allocator_ = nullptr;

public:
  static bool initialize() noexcept {
    if (initialized_) {
      return true;
    }

    slab_allocator_ = new SlabAllocator();
    if (slab_allocator_ == nullptr) {
      return false;
    }

    g_slab_allocator = slab_allocator_;
    initialized_ = true;
    return true;
  }

  static void cleanup() noexcept {
    if (initialized_) {
      delete slab_allocator_;
      slab_allocator_ = nullptr;
      g_slab_allocator = nullptr;
      initialized_ = false;
    }
  }

  static void print_statistics() noexcept {
    if (slab_allocator_ != nullptr) {
      slab_allocator_->get_statistics();
    }
  }

  [[nodiscard]] static bool is_initialized() noexcept { return initialized_; }
};

// Common type aliases
namespace common_types {
using ProcessQueue = SPSCQueue<ProcessId, 256>;
using ProcessList = LockedList<ProcessId>;
using ProcessWorkQueue = PerCpuWorkQueue<ProcessId, 128>;
using ProcessCounter = PerCpuAtomicCounter<u64>;

using PageQueue = SPSCQueue<PhysAddr, 1024>;
using MemoryCounter = PerCpuAtomicCounter<usize>;

using InterruptQueue = MPSCQueue<u8>;
using DeviceRegistry = LockedHashMap<DeviceId, VirtAddr>;
using InterruptCounter = PerCpuAtomicCounter<u64>;

using MessageQueue = SPSCQueue<u64, 512>;
using EndpointRegistry = LockedHashMap<EndpointId, ProcessId>;

struct SystemStats {
  u64 context_switches = 0;
  u64 system_calls = 0;
  u64 page_faults = 0;
  u64 interrupts = 0;
};

using SystemCounters = PerCpuData<SystemStats>;
} // namespace common_types

// Container configuration
struct ContainerConfig {
  static constexpr usize DEFAULT_PROCESS_QUEUE_SIZE = 256;
  static constexpr usize DEFAULT_MESSAGE_QUEUE_SIZE = 512;
  static constexpr usize DEFAULT_INTERRUPT_QUEUE_SIZE = 64;

  static constexpr usize SLAB_MIN_OBJECT_SIZE = 8;
  static constexpr usize SLAB_MAX_OBJECT_SIZE = 4096;
  static constexpr usize SLAB_NUM_CACHES = 32;

  static constexpr usize MAX_WORK_QUEUE_SIZE = 256;

  static constexpr bool ENABLE_STATISTICS = true;
  static constexpr bool ENABLE_DEBUG_CHECKS = false;
};

// Global slab allocator instance
SlabAllocator *g_slab_allocator = nullptr;

} // namespace moss::kernel::containers
