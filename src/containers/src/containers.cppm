// MOSS Containers Module - High-Performance Kernel Data Structures
// Provides lock-free queues, atomic types, per-CPU data, RCU structures,
// slab allocator, and related utilities for kernel use.

module;

// Global module fragment: arch detection and extern "C" declarations.
#include "arch_detect.h"

// C-linkage shim for PageFrameAllocator (defined in page_alloc_shim.cpp).
// Returns 0 on failure, otherwise the allocated physical address.
extern "C" unsigned long long
moss_slab_alloc_pages(unsigned long long order) noexcept;
// Returns 0 on success, non-zero on failure.
extern "C" int moss_slab_free_pages(unsigned long long addr,
                                    unsigned long long order) noexcept;

export module moss.containers;

import moss.std;
import moss.types;
import moss.result;
import moss.arch;

// ============================================================================
// Atomic types
// ============================================================================
export namespace moss::kernel::containers {

// Re-export MemoryOrder from moss.std for convenience
using MemoryOrder = moss::MemoryOrder;

// Convert MemoryOrder to GCC/Clang builtin atomic memory model
constexpr int to_builtin_order(MemoryOrder order) noexcept {
  switch (order) {
  case MemoryOrder::Relaxed:
    return __ATOMIC_RELAXED;
  case MemoryOrder::Consume:
    return __ATOMIC_CONSUME;
  case MemoryOrder::Acquire:
    return __ATOMIC_ACQUIRE;
  case MemoryOrder::Release:
    return __ATOMIC_RELEASE;
  case MemoryOrder::AcqRel:
    return __ATOMIC_ACQ_REL;
  case MemoryOrder::SeqCst:
    return __ATOMIC_SEQ_CST;
  default:
    return __ATOMIC_SEQ_CST;
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

  AtomicPtr(AtomicPtr &&other) noexcept {
    ptr_ = other.exchange(nullptr, MemoryOrder::AcqRel);
  }

  AtomicPtr &operator=(AtomicPtr &&other) noexcept {
    if (this != &other) {
      T *old_ptr = other.exchange(nullptr, MemoryOrder::AcqRel);
      store(old_ptr, MemoryOrder::Release);
    }
    return *this;
  }

  [[nodiscard]] T *
  load(MemoryOrder order = MemoryOrder::SeqCst) const noexcept {
    return __atomic_load_n(&ptr_, to_builtin_order(order));
  }

  void store(T *desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
    __atomic_store_n(&ptr_, desired, to_builtin_order(order));
  }

  [[nodiscard]] T *
  exchange(T *desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
    return __atomic_exchange_n(&ptr_, desired, to_builtin_order(order));
  }

  [[nodiscard]] bool
  compare_exchange_weak(T *&expected, T *desired,
                        MemoryOrder success = MemoryOrder::SeqCst,
                        MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
    return __atomic_compare_exchange_n(&ptr_, &expected, desired, true,
                                       to_builtin_order(success),
                                       to_builtin_order(failure));
  }

  [[nodiscard]] bool
  compare_exchange_strong(T *&expected, T *desired,
                          MemoryOrder success = MemoryOrder::SeqCst,
                          MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
    return __atomic_compare_exchange_n(&ptr_, &expected, desired, false,
                                       to_builtin_order(success),
                                       to_builtin_order(failure));
  }

  [[nodiscard]] T *operator->() const noexcept {
    return load(MemoryOrder::Acquire);
  }

  [[nodiscard]] T &operator*() const noexcept {
    return *load(MemoryOrder::Acquire);
  }

  [[nodiscard]] operator T *() const noexcept {
    return load(MemoryOrder::Acquire);
  }

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

  [[nodiscard]] T
  load(MemoryOrder order = MemoryOrder::SeqCst) const noexcept {
    return __atomic_load_n(&value_, to_builtin_order(order));
  }

  void store(T desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
    __atomic_store_n(&value_, desired, to_builtin_order(order));
  }

  [[nodiscard]] T
  fetch_add(T arg, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
    return __atomic_fetch_add(&value_, arg, to_builtin_order(order));
  }

  [[nodiscard]] T
  fetch_sub(T arg, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
    return __atomic_fetch_sub(&value_, arg, to_builtin_order(order));
  }

  [[nodiscard]] bool
  compare_exchange_weak(T &expected, T desired,
                        MemoryOrder success = MemoryOrder::SeqCst,
                        MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
    return __atomic_compare_exchange_n(&value_, &expected, desired, true,
                                       to_builtin_order(success),
                                       to_builtin_order(failure));
  }

  [[nodiscard]] T operator++() noexcept {
    return fetch_add(1, MemoryOrder::SeqCst) + 1;
  }
  [[nodiscard]] T operator--() noexcept {
    return fetch_sub(1, MemoryOrder::SeqCst) - 1;
  }
  [[nodiscard]] T operator++(int) noexcept {
    return fetch_add(1, MemoryOrder::SeqCst);
  }
  [[nodiscard]] T operator--(int) noexcept {
    return fetch_sub(1, MemoryOrder::SeqCst);
  }

  [[nodiscard]] operator T() const noexcept {
    return load(MemoryOrder::Acquire);
  }
};

// Common atomic type aliases
using AtomicU32 = AtomicCounter<u32>;
using AtomicU64 = AtomicCounter<u64>;
using AtomicSize = AtomicCounter<moss::kernel::usize>;
using AtomicBool = AtomicCounter<bool>;

// Cache-line aligned atomic type
template <typename T>
struct alignas(moss::kernel::CACHE_LINE_SIZE) CacheAlignedAtomic {
  AtomicCounter<T> value;

  constexpr CacheAlignedAtomic() noexcept = default;
  constexpr CacheAlignedAtomic(T initial) noexcept : value(initial) {}

  [[nodiscard]] T
  load(MemoryOrder order = MemoryOrder::SeqCst) const noexcept {
    return value.load(order);
  }

  void store(T desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
    value.store(desired, order);
  }

  [[nodiscard]] T operator++() noexcept { return ++value; }
  [[nodiscard]] T operator--() noexcept { return --value; }
  [[nodiscard]] T operator++(int) noexcept { return value++; }
  [[nodiscard]] T operator--(int) noexcept { return value--; }

  [[nodiscard]] operator T() const noexcept { return value; }
};

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
    u32 my_ticket = next_ticket_.fetch_add(1, MemoryOrder::Acquire);
    while (now_serving_.load(MemoryOrder::Acquire) != my_ticket) {
      moss::kernel::arch::cpu_yield();  // WFE on ARM64
    }
  }

  void unlock() noexcept {
    (void)now_serving_.fetch_add(1, MemoryOrder::Release);
    // ARM64: store-release generates implicit SEV to wake WFE waiters
  }

  [[nodiscard]] bool try_lock() noexcept {
    u32 current = now_serving_.load(MemoryOrder::Acquire);
    u32 next = current;
    return next_ticket_.compare_exchange_weak(next, current + 1,
                                              MemoryOrder::Acquire,
                                              MemoryOrder::Relaxed);
  }

  [[nodiscard]] bool is_locked() const noexcept {
    return next_ticket_.load(MemoryOrder::Relaxed) !=
           now_serving_.load(MemoryOrder::Relaxed);
  }
};

// IRQ-safe spinlock variant: disables IRQs while held.
// Prevents deadlock when IRQ handler also takes the same lock.
class IrqSpinLock {
private:
  TicketSpinLock inner_;

public:
  constexpr IrqSpinLock() noexcept = default;

  IrqSpinLock(const IrqSpinLock &) = delete;
  IrqSpinLock &operator=(const IrqSpinLock &) = delete;

  void lock() noexcept {
    moss::kernel::arch::disable_interrupts();
    inner_.lock();
  }

  void unlock() noexcept {
    inner_.unlock();
    moss::kernel::arch::enable_interrupts();
  }

  [[nodiscard]] bool try_lock() noexcept {
    moss::kernel::arch::disable_interrupts();
    if (inner_.try_lock()) {
      return true;
    }
    moss::kernel::arch::enable_interrupts();
    return false;
  }
};

// RAII lock guard
template <typename LockType>
class LockGuard {
private:
  LockType &lock_;

public:
  explicit LockGuard(LockType &lock) noexcept : lock_(lock) {
    lock_.lock();
  }
  ~LockGuard() noexcept {
    lock_.unlock();
  }

  LockGuard(const LockGuard &) = delete;
  LockGuard &operator=(const LockGuard &) = delete;
};

// Per-CPU counter (avoids cache line contention)
template <typename T> class PerCpuCounter {
private:
  CacheAlignedAtomic<T> counters_[moss::kernel::MAX_CPUS];

public:
  constexpr PerCpuCounter() noexcept = default;

  [[nodiscard]] AtomicCounter<T> &get_local() noexcept {
    u32 cpu_id = moss::kernel::arch::get_current_cpu_id();
    return counters_[cpu_id % moss::kernel::MAX_CPUS].value;
  }

  [[nodiscard]] T get_total() const noexcept {
    T total = 0;
    for (moss::kernel::usize i = 0; i < moss::kernel::MAX_CPUS; ++i) {
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

  template <typename... Args>
  constexpr QueueNode(Args &&...args) noexcept
      : next(nullptr), data(moss::move(args)...) {}
};

// SPSC (Single Producer Single Consumer) lock-free queue
template <typename T, moss::kernel::usize Capacity> class SPSCQueue {
private:
  static_assert((Capacity & (Capacity - 1)) == 0,
                "Capacity must be power of 2");
  static constexpr moss::kernel::usize MASK = Capacity - 1;

  alignas(moss::kernel::CACHE_LINE_SIZE)
      CacheAlignedAtomic<moss::kernel::usize> head_{0};
  alignas(moss::kernel::CACHE_LINE_SIZE)
      CacheAlignedAtomic<moss::kernel::usize> tail_{0};

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
    return head_.load(MemoryOrder::Acquire) ==
           tail_.load(MemoryOrder::Acquire);
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

  [[nodiscard]] static constexpr moss::kernel::usize capacity() noexcept {
    return Capacity - 1;
  }
};

// MPSC (Multiple Producer Single Consumer) lock-free queue
template <typename T> class MPSCQueue {
private:
  alignas(moss::kernel::CACHE_LINE_SIZE) AtomicPtr<QueueNode<T>> head_;
  alignas(moss::kernel::CACHE_LINE_SIZE) AtomicPtr<QueueNode<T>> tail_;
  QueueNode<T> stub_;

public:
  MPSCQueue() noexcept : head_(&stub_), tail_(&stub_) {
    stub_.next.store(nullptr, MemoryOrder::Relaxed);
  }

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
    constexpr PoolNode(Args &&...args) noexcept(
        moss::is_nothrow_constructible_v<T, Args...>)
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

  [[nodiscard]] QueueNode<T> *allocate() noexcept {
    return free_list_.try_dequeue();
  }

  void deallocate(QueueNode<T> *node) noexcept {
    if (node != nullptr) {
      node->~QueueNode<T>();
      new (node) QueueNode<T>();
      free_list_.enqueue(node);
    }
  }

  [[nodiscard]] bool has_available() const noexcept {
    return !free_list_.empty();
  }

  [[nodiscard]] static constexpr moss::kernel::usize pool_size() noexcept {
    return PoolSize;
  }
};

// MPMC (Multiple Producer Multiple Consumer) queue
template <typename T, moss::kernel::usize NumConsumers,
          moss::kernel::usize QueueCapacity>
  requires(QueueCapacity > 0 && (QueueCapacity & (QueueCapacity - 1)) == 0) &&
          (NumConsumers > 0)
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
    const moss::kernel::usize start_idx =
        round_robin_counter_.fetch_add(1, MemoryOrder::Relaxed) % NumConsumers;

    for (moss::kernel::usize i = 0; i < NumConsumers; ++i) {
      moss::kernel::usize queue_idx = (start_idx + i) % NumConsumers;
      if (queues_[queue_idx].try_enqueue(moss::forward<U>(item))) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool try_dequeue(moss::kernel::usize consumer_id,
                                 T &result) noexcept {
    if (consumer_id >= NumConsumers) {
      return false;
    }
    return queues_[consumer_id].try_dequeue(result);
  }

  [[nodiscard]] bool try_dequeue_any(T &result) noexcept {
    const moss::kernel::usize start_idx =
        moss::kernel::arch::get_current_cpu_id() % NumConsumers;

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

// Per-CPU data accessor
template <typename T> class PerCpuData {
private:
  alignas(moss::kernel::CACHE_LINE_SIZE) T data_[moss::kernel::MAX_CPUS];

public:
  constexpr PerCpuData() noexcept : data_{} {}

  template <typename... Args> explicit PerCpuData(Args &&...args) noexcept {
    for (moss::kernel::usize i = 0; i < moss::kernel::MAX_CPUS; ++i) {
      new (&data_[i]) T(args...);
    }
  }

  PerCpuData(const PerCpuData &) = delete;
  PerCpuData &operator=(const PerCpuData &) = delete;

  PerCpuData(PerCpuData &&other) noexcept {
    for (moss::kernel::usize i = 0; i < moss::kernel::MAX_CPUS; ++i) {
      data_[i] = static_cast<T &&>(other.data_[i]);
    }
  }

  PerCpuData &operator=(PerCpuData &&other) noexcept {
    if (this != &other) {
      for (moss::kernel::usize i = 0; i < moss::kernel::MAX_CPUS; ++i) {
        data_[i] = static_cast<T &&>(other.data_[i]);
      }
    }
    return *this;
  }

  [[nodiscard]] T &get_local() noexcept { return data_[get_current_cpu_id()]; }

  [[nodiscard]] const T &get_local() const noexcept {
    return data_[get_current_cpu_id()];
  }

  [[nodiscard]] T &get_cpu(moss::kernel::usize cpu_id) noexcept {
    return data_[cpu_id % MAX_CPUS];
  }

  [[nodiscard]] const T &get_cpu(moss::kernel::usize cpu_id) const noexcept {
    return data_[cpu_id % MAX_CPUS];
  }

  template <typename Func> void for_each_cpu(Func &&func) {
    for (moss::kernel::usize i = 0; i < MAX_CPUS; ++i) {
      func(i, data_[i]);
    }
  }

  template <typename Func> void for_each_cpu(Func &&func) const {
    for (moss::kernel::usize i = 0; i < MAX_CPUS; ++i) {
      func(i, data_[i]);
    }
  }

  template <typename Func, typename Result = T>
  [[nodiscard]] Result fold(Func &&func, Result initial = Result{}) const {
    Result result = initial;
    for (moss::kernel::usize i = 0; i < MAX_CPUS; ++i) {
      result = func(result, data_[i]);
    }
    return result;
  }

  [[nodiscard]] T sum() const noexcept {
    T total{};
    for (moss::kernel::usize i = 0; i < MAX_CPUS; ++i) {
      total += data_[i];
    }
    return total;
  }

private:
  [[nodiscard]] static moss::kernel::usize get_current_cpu_id() noexcept {
    return static_cast<moss::kernel::usize>(
        moss::kernel::arch::get_current_cpu_id());
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

  [[nodiscard]] T load_local() const noexcept {
    return counters_.get_local().value.load(MemoryOrder::Relaxed);
  }

  [[nodiscard]] T load_total() const noexcept {
    T total = 0;
    counters_.for_each_cpu([&total](moss::kernel::usize, const auto &counter) {
      total += counter.value.load(MemoryOrder::Relaxed);
    });
    return total;
  }

  void reset_all() noexcept {
    counters_.for_each_cpu([](moss::kernel::usize, auto &counter) {
      counter.value.store(0, MemoryOrder::Relaxed);
    });
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
template <typename T, moss::kernel::usize QueueSize = 256>
class PerCpuWorkQueue {
private:
  PerCpuData<T> data_;

public:
  constexpr PerCpuWorkQueue() noexcept = default;

  [[nodiscard]] bool enqueue_local(const T &item) noexcept {
    return data_.get_local().try_enqueue(item);
  }

  [[nodiscard]] bool enqueue_local(T &&item) noexcept {
    return data_.get_local().try_enqueue(static_cast<T &&>(item));
  }

  [[nodiscard]] bool dequeue_local(T &result) noexcept {
    return data_.get_local().try_dequeue(result);
  }

  [[nodiscard]] bool enqueue_to_cpu(moss::kernel::usize cpu_id,
                                    const T &item) noexcept {
    return data_.get_cpu(cpu_id).try_enqueue(item);
  }

  [[nodiscard]] bool enqueue_to_cpu(moss::kernel::usize cpu_id,
                                    T &&item) noexcept {
    return data_.get_cpu(cpu_id).try_enqueue(static_cast<T &&>(item));
  }

  [[nodiscard]] bool dequeue_from_cpu(moss::kernel::usize cpu_id,
                                      T &result) noexcept {
    return data_.get_cpu(cpu_id).try_dequeue(result);
  }

  [[nodiscard]] bool steal_work(T &result) noexcept {
    moss::kernel::usize current_cpu = static_cast<moss::kernel::usize>(
        moss::kernel::arch::get_current_cpu_id());

    for (moss::kernel::usize i = 1; i < MAX_CPUS; ++i) {
      moss::kernel::usize target_cpu = (current_cpu + i) % MAX_CPUS;
      if (data_.get_cpu(target_cpu).try_dequeue(result)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool empty_local() const noexcept {
    return data_.get_local().empty();
  }

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
        [&total_size](moss::kernel::usize, const auto &queue) {
          total_size += queue.approximate_size();
        });
    return total_size;
  }
};

// Simplified Per-CPU RCU callback system
struct RcuCallback {
  void (*callback)();
  u64 grace_period;

  RcuCallback() noexcept : callback(nullptr), grace_period(0) {}
  RcuCallback(void (*func)(), u64 gp) noexcept
      : callback(func), grace_period(gp) {}
};

class PerCpuRcuCallbacks {
private:
  static constexpr moss::kernel::usize MAX_CALLBACKS = 1024;

  struct CallbackArray {
    RcuCallback callbacks[MAX_CALLBACKS];
    AtomicCounter<moss::kernel::usize> head{0};
    AtomicCounter<moss::kernel::usize> tail{0};

    [[nodiscard]] bool enqueue(const RcuCallback &cb) noexcept {
      moss::kernel::usize current_tail = tail.load(MemoryOrder::Relaxed);
      moss::kernel::usize next_tail = (current_tail + 1) % MAX_CALLBACKS;

      if (next_tail == head.load(MemoryOrder::Acquire)) {
        return false;
      }

      callbacks[current_tail] = cb;
      tail.store(next_tail, MemoryOrder::Release);
      return true;
    }

    [[nodiscard]] bool dequeue(RcuCallback &cb) noexcept {
      moss::kernel::usize current_head = head.load(MemoryOrder::Relaxed);

      if (current_head == tail.load(MemoryOrder::Acquire)) {
        return false;
      }

      cb = callbacks[current_head];
      head.store((current_head + 1) % MAX_CALLBACKS, MemoryOrder::Release);
      return true;
    }
  };

  PerCpuData<CallbackArray> callback_arrays_;
  PerCpuAtomicCounter<u64> grace_period_counter_;

public:
  constexpr PerCpuRcuCallbacks() noexcept = default;

  [[nodiscard]] bool schedule_callback(void (*func)()) noexcept {
    if (func == nullptr) {
      return false;
    }

    u64 current_gp = grace_period_counter_.load_total();
    RcuCallback callback(func, current_gp + 1);
    return callback_arrays_.get_local().enqueue(callback);
  }

  void process_callbacks() noexcept {
    u64 current_gp = grace_period_counter_.load_total();
    auto &local_array = callback_arrays_.get_local();

    RcuCallback callback;
    while (local_array.dequeue(callback)) {
      if (callback.grace_period <= current_gp) {
        if (callback.callback != nullptr) {
          callback.callback();
        }
      } else {
        (void)local_array.enqueue(callback);
        break;
      }
    }
  }

  void advance_grace_period() noexcept {
    (void)grace_period_counter_.fetch_add_local(1);
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
// RCU (Read-Copy-Update) protected data structures
// ============================================================================
export namespace moss::kernel::containers {

// RCU read-side critical section guard
class RcuReadLock {
private:
  // Per-CPU read depth counter (replaces thread_local, unavailable in kernel)
  static inline PerCpuData<u32> read_depth_{};

public:
  RcuReadLock() noexcept { enter_read_side(); }
  ~RcuReadLock() noexcept { exit_read_side(); }

  RcuReadLock(const RcuReadLock &) = delete;
  RcuReadLock &operator=(const RcuReadLock &) = delete;
  RcuReadLock(RcuReadLock &&) = delete;
  RcuReadLock &operator=(RcuReadLock &&) = delete;

private:
  static void enter_read_side() noexcept {
    ++read_depth_.get_local();
    moss::kernel::arch::read_barrier();
  }

  static void exit_read_side() noexcept {
    moss::kernel::arch::read_barrier();
    --read_depth_.get_local();
  }

public:
  static bool in_read_side() noexcept { return read_depth_.get_local() > 0; }
};

// RCU-protected pointer
template <typename T> class RcuPtr {
private:
  moss::kernel::containers::AtomicPtr<T> ptr_;

public:
  constexpr RcuPtr() noexcept : ptr_(nullptr) {}
  constexpr RcuPtr(T *p) noexcept : ptr_(p) {}

  RcuPtr(const RcuPtr &) = delete;
  RcuPtr &operator=(const RcuPtr &) = delete;

  RcuPtr(RcuPtr &&other) noexcept : ptr_(other.ptr_.exchange(nullptr)) {}
  RcuPtr &operator=(RcuPtr &&other) noexcept {
    if (this != &other) {
      T *old_ptr = ptr_.exchange(other.ptr_.exchange(nullptr));
      schedule_rcu_callback([old_ptr]() { delete old_ptr; });
    }
    return *this;
  }

  [[nodiscard]] T *load_rcu() const noexcept {
#ifndef NDEBUG
    if (!RcuReadLock::in_read_side()) {
#if defined(MOSS_ARCH_ARM64)
      asm volatile("brk #1");
#elif defined(MOSS_ARCH_X86_64)
      asm volatile("int3");
#elif defined(MOSS_ARCH_RISCV)
      asm volatile("ebreak");
#else
      while (1) {
      }
#endif
    }
#endif
    return ptr_.load(MemoryOrder::Consume);
  }

  void store_rcu(T *new_ptr) noexcept {
    T *old_ptr = ptr_.exchange(new_ptr, MemoryOrder::Release);
    if (old_ptr != nullptr) {
      schedule_rcu_callback([old_ptr]() { delete old_ptr; });
    }
  }

  [[nodiscard]] bool compare_exchange_rcu(T *&expected, T *desired) noexcept {
    if (ptr_.compare_exchange_weak(expected, desired, MemoryOrder::Release,
                                   MemoryOrder::Consume)) {
      if (expected != nullptr) {
        schedule_rcu_callback([expected]() { delete expected; });
      }
      return true;
    }
    return false;
  }

  [[nodiscard]] T *exchange(T *new_ptr,
                            MemoryOrder order = MemoryOrder::AcqRel) noexcept {
    return ptr_.exchange(new_ptr, order);
  }

  [[nodiscard]] T *
  load(MemoryOrder order = MemoryOrder::Acquire) const noexcept {
    return ptr_.load(order);
  }

private:
  template <typename Func>
  static void schedule_rcu_callback(Func &&callback) noexcept {
    // Simplified: execute immediately (prototype only)
    callback();
  }
};

// RCU-protected linked list node
template <typename T> struct RcuListNode {
  RcuPtr<RcuListNode<T>> next;
  T data;

  template <typename... Args>
  constexpr RcuListNode(Args &&...args) noexcept(
      moss::is_nothrow_constructible_v<T, Args...>)
      : next(nullptr), data(moss::forward<Args>(args)...) {}
};

// RCU-protected linked list
template <typename T> class RcuList {
private:
  RcuPtr<RcuListNode<T>> head_;
  moss::kernel::containers::AtomicCounter<moss::kernel::usize> size_;

public:
  constexpr RcuList() noexcept : head_(nullptr), size_(0) {}

  ~RcuList() noexcept { clear(); }

  RcuList(const RcuList &) = delete;
  RcuList &operator=(const RcuList &) = delete;
  RcuList(RcuList &&) = delete;
  RcuList &operator=(RcuList &&) = delete;

  template <typename... Args> void push_front(Args &&...args) {
    auto new_node = new RcuListNode<T>(moss::forward<Args>(args)...);

    RcuListNode<T> *old_head = head_.load(MemoryOrder::Relaxed);
    do {
      new_node->next.store_rcu(old_head);
    } while (!head_.compare_exchange_rcu(old_head, new_node));

    (void)size_.fetch_add(1, MemoryOrder::Relaxed);
  }

  bool remove(const T &value) {
    RcuListNode<T> *prev = nullptr;
    RcuListNode<T> *current = head_.load_rcu();

    while (current != nullptr) {
      if (current->data == value) {
        RcuListNode<T> *next = current->next.load_rcu();

        if (prev == nullptr) {
          if (head_.compare_exchange_rcu(current, next)) {
            (void)size_.fetch_sub(1, MemoryOrder::Relaxed);
            return true;
          }
        } else {
          prev->next.store_rcu(next);
          (void)size_.fetch_sub(1, MemoryOrder::Relaxed);
          return true;
        }
      }
      prev = current;
      current = current->next.load_rcu();
    }
    return false;
  }

  template <typename Predicate>
  [[nodiscard]] const T *find_if(Predicate pred) const {
    RcuReadLock read_lock;

    RcuListNode<T> *current = head_.load_rcu();
    while (current != nullptr) {
      if (pred(current->data)) {
        return &current->data;
      }
      current = current->next.load_rcu();
    }
    return nullptr;
  }

  [[nodiscard]] const T *find(const T &value) const {
    return find_if([&value](const T &item) { return item == value; });
  }

  template <typename Func> void for_each(Func func) const {
    RcuReadLock read_lock;

    RcuListNode<T> *current = head_.load_rcu();
    while (current != nullptr) {
      func(current->data);
      current = current->next.load_rcu();
    }
  }

  [[nodiscard]] moss::kernel::usize size() const noexcept {
    return size_.load(MemoryOrder::Relaxed);
  }

  [[nodiscard]] bool empty() const noexcept {
    RcuReadLock read_lock;
    return head_.load_rcu() == nullptr;
  }

  void clear() {
    RcuListNode<T> *current = head_.exchange(nullptr, MemoryOrder::AcqRel);
    size_.store(0, MemoryOrder::Relaxed);

    while (current != nullptr) {
      RcuListNode<T> *next = current->next.load(MemoryOrder::Relaxed);
      schedule_rcu_callback([current]() { delete current; });
      current = next;
    }
  }

private:
  template <typename Func>
  static void schedule_rcu_callback(Func &&callback) noexcept {
    callback();
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
    void* thread;   // Actually Thread*, but opaque to avoid module cycle

    WaitQueueEntry() noexcept : thread(nullptr) {}
    explicit WaitQueueEntry(void* t) noexcept : thread(t) {}

    bool operator==(const WaitQueueEntry& other) const noexcept {
        return thread == other.thread;
    }
};

// WaitQueue: a list of threads waiting for an event.
// Data structure only — actual sleep/wake logic requires scheduler access,
// so it is implemented as bridge functions in the kernel module.
class WaitQueue {
private:
    RcuList<WaitQueueEntry> waiters_;

public:
    constexpr WaitQueue() noexcept = default;

    WaitQueue(const WaitQueue&) = delete;
    WaitQueue& operator=(const WaitQueue&) = delete;
    WaitQueue(WaitQueue&&) = delete;
    WaitQueue& operator=(WaitQueue&&) = delete;

    // Add a thread to the wait queue
    void add_waiter(void* thread) {
        waiters_.push_front(WaitQueueEntry(thread));
    }

    // Remove a specific thread from the wait queue
    void remove_waiter(void* thread) {
        RcuReadLock lock;
        waiters_.remove(WaitQueueEntry(thread));
    }

    // Iterate over all waiters and call func(void* thread) for each.
    // The kernel module uses this to set state=Ready and enqueue each thread.
    template <typename Func>
    void for_each_waiter(Func func) const {
        waiters_.for_each([&func](const WaitQueueEntry& entry) {
            func(entry.thread);
        });
    }

    // Check if any threads are waiting
    [[nodiscard]] bool has_waiters() const noexcept {
        return !waiters_.empty();
    }

    // Clear all waiters (used during teardown)
    void clear() {
        waiters_.clear();
    }
};

// RCU-protected hash map
template <typename Key, typename Value, moss::kernel::usize BucketCount = 256>
class RcuHashMap {
private:
  static_assert((BucketCount & (BucketCount - 1)) == 0,
                "BucketCount must be power of 2");
  static constexpr moss::kernel::usize BUCKET_MASK = BucketCount - 1;

  struct Entry {
    Key key;
    Value value;

    template <typename K, typename V>
    Entry(K &&k, V &&v) noexcept(
        moss::is_nothrow_constructible_v<Key, K &&> &&
        moss::is_nothrow_constructible_v<Value, V &&>)
        : key(moss::forward<K>(k)), value(moss::forward<V>(v)) {}

    bool operator==(const Entry &other) const noexcept {
      return key == other.key;
    }
  };

  RcuList<Entry> buckets_[BucketCount];
  moss::kernel::containers::AtomicCounter<moss::kernel::usize> size_;

public:
  constexpr RcuHashMap() noexcept : size_(0) {}

  template <typename K, typename V>
  void insert_or_update(K &&key, V &&value) {
    moss::kernel::usize bucket_idx = hash_key(key) & BUCKET_MASK;

    {
      RcuReadLock read_lock;
      const Entry *existing = buckets_[bucket_idx].find_if(
          [&key](const Entry &entry) { return entry.key == key; });

      if (existing != nullptr) {
        buckets_[bucket_idx].remove(*existing);
      }
    }

    buckets_[bucket_idx].push_front(static_cast<K &&>(key),
                                    static_cast<V &&>(value));
    (void)size_.fetch_add(1, MemoryOrder::Relaxed);
  }

  template <typename K> [[nodiscard]] const Value *find(const K &key) const {
    moss::kernel::usize bucket_idx = hash_key(key) & BUCKET_MASK;

    const Entry *entry = buckets_[bucket_idx].find_if(
        [&key](const Entry &e) { return e.key == key; });

    return entry ? &entry->value : nullptr;
  }

  template <typename K> bool remove(const K &key) {
    moss::kernel::usize bucket_idx = hash_key(key) & BUCKET_MASK;

    bool removed;
    {
      RcuReadLock read_lock;
      removed = buckets_[bucket_idx].remove(Entry{key, Value{}});
    }
    if (removed) {
      (void)size_.fetch_sub(1, MemoryOrder::Relaxed);
    }
    return removed;
  }

  [[nodiscard]] moss::kernel::usize size() const noexcept {
    return size_.load(MemoryOrder::Relaxed);
  }

  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

  template <typename Func> void for_each(Func &&func) const {
    for (moss::kernel::usize i = 0; i < BucketCount; ++i) {
      buckets_[i].for_each([&func](const Entry &entry) {
        struct KeyValue {
          const Key &key;
          const Value &value;
        };
        func(KeyValue{entry.key, entry.value});
      });
    }
  }

private:
  template <typename K>
  [[nodiscard]] static moss::kernel::usize hash_key(const K &key) noexcept {
    moss::kernel::usize hash = 2166136261u;
    const u8 *data = reinterpret_cast<const u8 *>(&key);
    for (moss::kernel::usize i = 0; i < sizeof(K); ++i) {
      hash ^= data[i];
      hash *= 16777619u;
    }
    return hash;
  }
};

// RCU type aliases
using ProcessList = RcuList<moss::kernel::ProcessId>;
using DeviceRegistry =
    RcuHashMap<moss::kernel::DeviceId, moss::kernel::VirtAddr>;

} // namespace moss::kernel::containers

// ============================================================================
// Slab memory allocator
// ============================================================================
export namespace moss::kernel::containers {

// Kernel utility function
template <typename T>
constexpr const T &kernel_max(const T &a, const T &b) noexcept {
  return (a < b) ? b : a;
}

// Slab allocator error codes
enum class SlabError : u32 {
  OutOfMemory = 1,
  InvalidSize = 2,
  DoubleFree = 3,
  CorruptedSlab = 4
};

// Slab result types
template <typename T> using SlabResult = moss::kernel::Result<T, SlabError>;
using SlabVoidResult = moss::kernel::Result<void, SlabError>;

// Memory alignment utilities
template <moss::kernel::usize Alignment>
constexpr moss::kernel::usize align_up(moss::kernel::usize value) noexcept {
  static_assert((Alignment & (Alignment - 1)) == 0,
                "Alignment must be power of 2");
  return (value + Alignment - 1) & ~(Alignment - 1);
}

constexpr bool is_aligned(moss::kernel::usize value,
                          moss::kernel::usize alignment) noexcept {
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

  SlabPage(void *mem, moss::kernel::usize obj_size,
           moss::kernel::usize obj_per_page) noexcept
      : memory(mem), object_size(obj_size), objects_per_page(obj_per_page),
        free_count(obj_per_page), free_list(nullptr), next(nullptr) {
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
  moss::kernel::containers::AtomicCounter<moss::kernel::usize>
      allocated_objects_;

public:
  SlabCache(moss::kernel::usize object_size,
            moss::kernel::usize alignment = alignof(void *)) noexcept
      : object_size_(object_size), object_alignment_(alignment),
        aligned_object_size_(align_up<alignof(void *)>(
            kernel_max<moss::kernel::usize>(object_size, sizeof(void *)))),
        objects_per_page_(moss::kernel::PAGE_SIZE / aligned_object_size_),
        full_pages_(nullptr), partial_pages_(nullptr), empty_pages_(nullptr),
        total_objects_(0), allocated_objects_(0) {}

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
    } while (!page->free_list.compare_exchange_weak(
        current_free, static_cast<u8 *>(ptr), moss::MemoryOrder::Release,
        moss::MemoryOrder::Relaxed));

    moss::kernel::usize new_free_count =
        page->free_count.fetch_add(1, moss::MemoryOrder::AcqRel) + 1;
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

  [[nodiscard]] moss::kernel::usize object_size() const noexcept {
    return object_size_;
  }

  // Returns utilization as a percentage (0-100)
  [[nodiscard]] moss::kernel::usize utilization() const noexcept {
    moss::kernel::usize total = total_objects();
    if (total == 0)
      return 0;
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
    SlabPage *page =
        empty_pages_.exchange(nullptr, moss::MemoryOrder::AcqRel);
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

    SlabPage *new_page =
        new SlabPage(page_memory, aligned_object_size_, objects_per_page_);
    (void)total_objects_.fetch_add(objects_per_page_,
                                   moss::MemoryOrder::Relaxed);

    move_page_to_partial(new_page);
    return allocate_from_page(new_page);
  }

  [[nodiscard]] SlabResult<void *>
  allocate_from_page(SlabPage *page) noexcept {
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
    } while (!page->free_list.compare_exchange_weak(
        current_free, next_free, moss::MemoryOrder::AcqRel,
        moss::MemoryOrder::Acquire));

    moss::kernel::usize new_free_count =
        page->free_count.fetch_sub(1, moss::MemoryOrder::AcqRel) - 1;
    (void)allocated_objects_.fetch_add(1, moss::MemoryOrder::Relaxed);

    if (new_free_count == 0) {
      move_page_from_partial_to_full(page);
    }

    return SlabResult<void *>{static_cast<void *>(current_free)};
  }

  [[nodiscard]] SlabPage *find_page_for_object(void *ptr) const noexcept {
    moss::kernel::usize ptr_addr = reinterpret_cast<moss::kernel::usize>(ptr);
    moss::kernel::usize page_addr = ptr_addr & ~(moss::kernel::PAGE_SIZE - 1);

    if (auto page = find_in_page_list(
            full_pages_.load(moss::MemoryOrder::Acquire), page_addr)) {
      return page;
    }
    if (auto page = find_in_page_list(
            partial_pages_.load(moss::MemoryOrder::Acquire), page_addr)) {
      return page;
    }
    return find_in_page_list(empty_pages_.load(moss::MemoryOrder::Acquire),
                             page_addr);
  }

  [[nodiscard]] SlabPage *
  find_in_page_list(SlabPage *head,
                    moss::kernel::usize page_addr) const noexcept {
    SlabPage *current = head;
    while (current != nullptr) {
      moss::kernel::usize current_page_addr =
          reinterpret_cast<moss::kernel::usize>(current->memory) &
          ~(moss::kernel::PAGE_SIZE - 1);
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
    } while (!partial_pages_.compare_exchange_weak(
        old_head, page, moss::MemoryOrder::Release,
        moss::MemoryOrder::Relaxed));
  }

  void move_page_to_full(SlabPage *page) noexcept {
    SlabPage *old_head = full_pages_.load(moss::MemoryOrder::Relaxed);
    do {
      page->next.store(old_head, moss::MemoryOrder::Relaxed);
    } while (!full_pages_.compare_exchange_weak(
        old_head, page, moss::MemoryOrder::Release,
        moss::MemoryOrder::Relaxed));
  }

  void move_page_to_empty(SlabPage *page) noexcept {
    SlabPage *old_head = empty_pages_.load(moss::MemoryOrder::Relaxed);
    do {
      page->next.store(old_head, moss::MemoryOrder::Relaxed);
    } while (!empty_pages_.compare_exchange_weak(
        old_head, page, moss::MemoryOrder::Release,
        moss::MemoryOrder::Relaxed));
  }

  void move_page_from_partial_to_full(SlabPage *page) noexcept {
    remove_page_from_list(partial_pages_, page);
    move_page_to_full(page);
  }

  void move_page_from_full_to_partial(SlabPage *page) noexcept {
    remove_page_from_list(full_pages_, page);
    move_page_to_partial(page);
  }

  void remove_page_from_list(
      [[maybe_unused]] moss::kernel::containers::AtomicPtr<SlabPage> &head,
      [[maybe_unused]] SlabPage *page) noexcept {
    // Simplified: rebuild list (actual implementation should be more efficient)
  }

  [[nodiscard]] void *allocate_page() noexcept {
    unsigned long long addr = moss_slab_alloc_pages(0);
    if (addr == 0) {
      return nullptr;
    }
    return reinterpret_cast<void *>(addr);
  }

  void free_page(void *ptr) noexcept {
    if (ptr == nullptr) {
      return;
    }
    (void)moss_slab_free_pages(reinterpret_cast<unsigned long long>(ptr), 0);
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

  [[nodiscard]] SlabResult<void *>
  allocate(moss::kernel::usize size) noexcept {
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

  [[nodiscard]] SlabVoidResult deallocate(void *ptr,
                                          moss::kernel::usize size) noexcept {
    if (ptr == nullptr) {
      return SlabVoidResult{};
    }

    moss::kernel::usize cache_index = find_cache_index(size);
    if (cache_index >= NUM_CACHES) {
      return SlabVoidResult{moss::kernel::Err<SlabError>(SlabError::InvalidSize)};
    }
    return caches_[cache_index]->deallocate(ptr);
  }

  template <typename T>
  [[nodiscard]] SlabVoidResult deallocate(T *ptr) noexcept {
    return deallocate(ptr, sizeof(T));
  }

  void get_statistics() const noexcept {
    for (moss::kernel::usize i = 0; i < NUM_CACHES; ++i) {
      if (caches_[i]->total_objects() > 0) {
        // In actual implementation, output to kernel log
      }
    }
  }

private:
  [[nodiscard]] moss::kernel::usize
  find_cache_index(moss::kernel::usize size) const noexcept {
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
template <typename T, typename... Args>
[[nodiscard]] SlabResult<T *> slab_new(Args &&...args) noexcept {
  auto ptr_result = g_slab_allocator->allocate<T>();
  if (!ptr_result) {
    return SlabResult<T *>{moss::kernel::Err<SlabError>(ptr_result.error())};
  }

  T *ptr = *ptr_result;
  new (ptr) T(moss::forward<Args>(args)...);
  return SlabResult<T *>{ptr};
}

template <typename T>
[[nodiscard]] SlabVoidResult slab_delete(T *ptr) noexcept {
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

  constexpr Optional(const T &value) noexcept(
      moss::is_nothrow_copy_constructible_v<T>)
      : has_value_(true) {
    new (storage_) T(value);
  }

  constexpr Optional(T &&value) noexcept(
      moss::is_nothrow_move_constructible_v<T>)
      : has_value_(true) {
    new (storage_) T(moss::move(value));
  }

  Optional(const Optional &other) noexcept(
      moss::is_nothrow_copy_constructible_v<T>)
      : has_value_(other.has_value_) {
    if (has_value_) {
      new (storage_) T(other.value());
    }
  }

  Optional(Optional &&other) noexcept(
      moss::is_nothrow_move_constructible_v<T>)
      : has_value_(other.has_value_) {
    if (has_value_) {
      new (storage_) T(moss::move(other.value()));
      other.reset();
    }
  }

  ~Optional() noexcept { reset(); }

  Optional &operator=(const Optional &other) noexcept(
      moss::is_nothrow_copy_assignable_v<T>) {
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

  Optional &operator=(Optional &&other) noexcept(
      moss::is_nothrow_move_assignable_v<T>) {
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

  [[nodiscard]] constexpr bool has_value() const noexcept {
    return has_value_;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return has_value_;
  }

  [[nodiscard]] constexpr T &value() & noexcept {
    return *reinterpret_cast<T *>(storage_);
  }

  [[nodiscard]] constexpr const T &value() const & noexcept {
    return *reinterpret_cast<const T *>(storage_);
  }

  [[nodiscard]] constexpr T &&value() && noexcept {
    return moss::move(*reinterpret_cast<T *>(storage_));
  }

  [[nodiscard]] constexpr const T &&value() const && noexcept {
    return moss::move(*reinterpret_cast<const T *>(storage_));
  }

  [[nodiscard]] constexpr T &operator*() & noexcept { return value(); }
  [[nodiscard]] constexpr const T &operator*() const & noexcept {
    return value();
  }
  [[nodiscard]] constexpr T &&operator*() && noexcept {
    return moss::move(value());
  }
  [[nodiscard]] constexpr const T &&operator*() const && noexcept {
    return moss::move(value());
  }

  [[nodiscard]] constexpr T *operator->() noexcept { return &value(); }
  [[nodiscard]] constexpr const T *operator->() const noexcept {
    return &value();
  }

  void reset() noexcept {
    if (has_value_) {
      value().~T();
      has_value_ = false;
    }
  }

  template <typename... Args>
  T &emplace(Args &&...args) noexcept(
      moss::is_nothrow_constructible_v<T, Args...>) {
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
using ProcessList = RcuList<ProcessId>;
using ProcessWorkQueue = PerCpuWorkQueue<ProcessId, 128>;
using ProcessCounter = PerCpuAtomicCounter<u64>;

using PageQueue = SPSCQueue<PhysAddr, 1024>;
using MemoryCounter = PerCpuAtomicCounter<usize>;

using InterruptQueue = MPSCQueue<u8>;
using DeviceRegistry = RcuHashMap<DeviceId, VirtAddr>;
using InterruptCounter = PerCpuAtomicCounter<u64>;

using MessageQueue = SPSCQueue<u64, 512>;
using EndpointRegistry = RcuHashMap<EndpointId, ProcessId>;

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
  static constexpr usize MAX_RCU_CALLBACKS = 1024;

  static constexpr bool ENABLE_STATISTICS = true;
  static constexpr bool ENABLE_DEBUG_CHECKS = false;
};

// Global slab allocator instance
SlabAllocator *g_slab_allocator = nullptr;

} // namespace moss::kernel::containers
