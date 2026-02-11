#pragma once

// 高性能硬件原子操作实现 - 基于编译器内建函数
// 提供真正的多架构硬件级原子操作支持

#include "../include/arch/arch_abstraction.hpp"
#include "../include/moss_std.hpp"
#include "../include/types.hpp"

namespace moss::kernel::containers {

// 使用moss_std.hpp中定义的MemoryOrder
using MemoryOrder = moss::MemoryOrder;

// 将MemoryOrder转换为GCC/Clang内建原子函数的内存模型
constexpr int to_builtin_order(MemoryOrder order) noexcept {
  switch (order) {
    case MemoryOrder::Relaxed: return __ATOMIC_RELAXED;
    case MemoryOrder::Consume: return __ATOMIC_CONSUME;
    case MemoryOrder::Acquire: return __ATOMIC_ACQUIRE;
    case MemoryOrder::Release: return __ATOMIC_RELEASE;
    case MemoryOrder::AcqRel:  return __ATOMIC_ACQ_REL;
    case MemoryOrder::SeqCst:  return __ATOMIC_SEQ_CST;
    default:                   return __ATOMIC_SEQ_CST;
  }
}

// 高性能原子指针类型 - 基于编译器内建函数
template <typename T> class AtomicPtr {
private:
  T* volatile ptr_;

public:
  constexpr AtomicPtr() noexcept : ptr_(nullptr) {}
  constexpr AtomicPtr(T *p) noexcept : ptr_(p) {}

  // 禁用拷贝，允许移动
  AtomicPtr(const AtomicPtr &) = delete;
  AtomicPtr &operator=(const AtomicPtr &) = delete;

  AtomicPtr(AtomicPtr &&other) noexcept {
    ptr_ = other.exchange(nullptr, MemoryOrder::AcqRel);
  }

  AtomicPtr &operator=(AtomicPtr &&other) noexcept {
    if (this != &other) {
      T* old_ptr = other.exchange(nullptr, MemoryOrder::AcqRel);
      store(old_ptr, MemoryOrder::Release);
    }
    return *this;
  }

  // 硬件级原子加载
  [[nodiscard]] T* load(MemoryOrder order = MemoryOrder::SeqCst) const noexcept {
#if defined(__clang__) || defined(__GNUC__)
    return __atomic_load_n(&ptr_, to_builtin_order(order));
#else
    // Fallback for unsupported compilers
    (void)order;
    memory_barrier();
    return const_cast<T*>(ptr_);
#endif
  }

  // 硬件级原子存储
  void store(T* desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    __atomic_store_n(&ptr_, desired, to_builtin_order(order));
#else
    // Fallback for unsupported compilers
    (void)order;
    ptr_ = desired;
    memory_barrier();
#endif
  }

  // 硬件级原子交换
  [[nodiscard]] T* exchange(T* desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    return __atomic_exchange_n(&ptr_, desired, to_builtin_order(order));
#else
    // Fallback for unsupported compilers
    (void)order;
    memory_barrier();
    T* old = const_cast<T*>(ptr_);
    ptr_ = desired;
    memory_barrier();
    return old;
#endif
  }

  // 硬件级CAS操作 - weak版本（推荐）
  [[nodiscard]] bool compare_exchange_weak(T*& expected, T* desired,
                                          MemoryOrder success = MemoryOrder::SeqCst,
                                          MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    return __atomic_compare_exchange_n(&ptr_, &expected, desired, true,
                                       to_builtin_order(success), to_builtin_order(failure));
#else
    // Fallback for unsupported compilers
    (void)success; (void)failure;
    memory_barrier();
    if (ptr_ == expected) {
      ptr_ = desired;
      memory_barrier();
      return true;
    } else {
      expected = const_cast<T*>(ptr_);
      return false;
    }
#endif
  }

  // 硬件级CAS操作 - strong版本
  [[nodiscard]] bool compare_exchange_strong(T*& expected, T* desired,
                                            MemoryOrder success = MemoryOrder::SeqCst,
                                            MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    return __atomic_compare_exchange_n(&ptr_, &expected, desired, false,
                                       to_builtin_order(success), to_builtin_order(failure));
#else
    // Strong和weak在fallback中相同
    return compare_exchange_weak(expected, desired, success, failure);
#endif
  }

  // 便利操作符
  [[nodiscard]] T* operator->() const noexcept {
    return load(MemoryOrder::Acquire);
  }

  [[nodiscard]] T& operator*() const noexcept {
    return *load(MemoryOrder::Acquire);
  }

  [[nodiscard]] operator T*() const noexcept {
    return load(MemoryOrder::Acquire);
  }

  AtomicPtr& operator=(T* desired) noexcept {
    store(desired, MemoryOrder::Release);
    return *this;
  }
};

// 高性能原子计数器 - 基于编译器内建函数
template <typename T> class AtomicCounter {
private:
  volatile T value_;

public:
  constexpr AtomicCounter() noexcept : value_(0) {}
  constexpr AtomicCounter(T initial) noexcept : value_(initial) {}

  // 禁用拷贝
  AtomicCounter(const AtomicCounter &) = delete;
  AtomicCounter &operator=(const AtomicCounter &) = delete;

  // 硬件级原子加载
  [[nodiscard]] T load(MemoryOrder order = MemoryOrder::SeqCst) const noexcept {
#if defined(__clang__) || defined(__GNUC__)
    return __atomic_load_n(&value_, to_builtin_order(order));
#else
    // Fallback for unsupported compilers
    (void)order;
    memory_barrier();
    return value_;
#endif
  }

  // 硬件级原子存储
  void store(T desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    __atomic_store_n(&value_, desired, to_builtin_order(order));
#else
    // Fallback for unsupported compilers
    (void)order;
    value_ = desired;
    memory_barrier();
#endif
  }

  // 硬件级原子递增
  [[nodiscard]] T fetch_add(T arg, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    return __atomic_fetch_add(&value_, arg, to_builtin_order(order));
#else
    // Fallback for unsupported compilers
    (void)order;
    memory_barrier();
    T old = value_;
    value_ = old + arg;
    memory_barrier();
    return old;
#endif
  }

  // 硬件级原子递减
  [[nodiscard]] T fetch_sub(T arg, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    return __atomic_fetch_sub(&value_, arg, to_builtin_order(order));
#else
    // Fallback for unsupported compilers
    (void)order;
    memory_barrier();
    T old = value_;
    value_ = old - arg;
    memory_barrier();
    return old;
#endif
  }

  // 硬件级原子比较和交换
  [[nodiscard]] bool compare_exchange_weak(T& expected, T desired,
                                          MemoryOrder success = MemoryOrder::SeqCst,
                                          MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    return __atomic_compare_exchange_n(&value_, &expected, desired, true,
                                       to_builtin_order(success), to_builtin_order(failure));
#else
    // Fallback for unsupported compilers
    (void)success; (void)failure;
    memory_barrier();
    if (value_ == expected) {
      value_ = desired;
      memory_barrier();
      return true;
    } else {
      expected = value_;
      return false;
    }
#endif
  }

  // 前缀递增/递减
  [[nodiscard]] T operator++() noexcept {
    return fetch_add(1, MemoryOrder::SeqCst) + 1;
  }

  [[nodiscard]] T operator--() noexcept {
    return fetch_sub(1, MemoryOrder::SeqCst) - 1;
  }

  // 后缀递增/递减
  [[nodiscard]] T operator++(int) noexcept {
    return fetch_add(1, MemoryOrder::SeqCst);
  }

  [[nodiscard]] T operator--(int) noexcept {
    return fetch_sub(1, MemoryOrder::SeqCst);
  }

  // 转换操作符
  [[nodiscard]] operator T() const noexcept {
    return load(MemoryOrder::Acquire);
  }
};

// 类型别名
using AtomicU32 = AtomicCounter<moss::kernel::u32>;
using AtomicU64 = AtomicCounter<moss::kernel::u64>;
using AtomicSize = AtomicCounter<moss::kernel::usize>;
using AtomicBool = AtomicCounter<bool>;

// 内存屏障函数 - 使用架构抽象层
using moss::kernel::arch::instruction_barrier;
using moss::kernel::arch::memory_barrier;
using moss::kernel::arch::read_barrier;
using moss::kernel::arch::write_barrier;

// CPU缓存行对齐的原子类型
template <typename T>
struct alignas(moss::kernel::CACHE_LINE_SIZE) CacheAlignedAtomic {
  AtomicCounter<T> value;

  constexpr CacheAlignedAtomic() noexcept = default;
  constexpr CacheAlignedAtomic(T initial) noexcept : value(initial) {}

  // 转发所有操作到内部原子类型
  [[nodiscard]] T load(MemoryOrder order = MemoryOrder::SeqCst) const noexcept {
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

// Per-CPU计数器（避免缓存行争用）
template <typename T> class PerCpuCounter {
private:
  CacheAlignedAtomic<T> counters_[moss::kernel::MAX_CPUS];

public:
  constexpr PerCpuCounter() noexcept = default;

  // 获取当前CPU的计数器
  [[nodiscard]] AtomicCounter<T>& get_local() noexcept {
    moss::kernel::u32 cpu_id = get_current_cpu_id();
    return counters_[cpu_id % moss::kernel::MAX_CPUS].value;
  }

  // 获取总计数（需要遍历所有CPU）
  [[nodiscard]] T get_total() const noexcept {
    T total = 0;
    for (moss::kernel::usize i = 0; i < moss::kernel::MAX_CPUS; ++i) {
      total += counters_[i].load(MemoryOrder::Relaxed);
    }
    return total;
  }

private:
  // 获取当前CPU ID的简单实现
  [[nodiscard]] static moss::kernel::u32 get_current_cpu_id() noexcept {
    moss::kernel::u64 mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return static_cast<moss::kernel::u32>(mpidr & 0xFF);
  }
};

} // namespace moss::kernel::containers
