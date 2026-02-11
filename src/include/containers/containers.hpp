#pragma once

// 高性能内核容器库 - 综合头文件
// 提供无锁、高性能的数据结构

#include "containers/atomic_types.hpp"
#include "containers/lockfree_queue.hpp"
#include "containers/per_cpu_data.hpp"
#include "containers/rcu_list.hpp"
#include "containers/slab_allocator.hpp"
#include "../moss_std.hpp" // 包含裸机环境基础定义
#include "config/config.h" // 包含配置系统定义的宏

// 避免包含有冲突的kernel_std.hpp

namespace moss::kernel::containers {

// Optional类型实现
template <typename T> class Optional {
private:
  alignas(T) u8 storage_[sizeof(T)];
  bool has_value_;

public:
  // 构造函数
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

  // 拷贝构造
  Optional(const Optional &other) noexcept(
      moss::is_nothrow_copy_constructible_v<T>)
      : has_value_(other.has_value_) {
    if (has_value_) {
      new (storage_) T(other.value());
    }
  }

  // 移动构造
  Optional(Optional &&other) noexcept(moss::is_nothrow_move_constructible_v<T>)
      : has_value_(other.has_value_) {
    if (has_value_) {
      new (storage_) T(moss::move(other.value()));
      other.reset();
    }
  }

  // 析构函数
  ~Optional() noexcept { reset(); }

  // 赋值操作符
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

  Optional &
  operator=(Optional &&other) noexcept(moss::is_nothrow_move_assignable_v<T>) {
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

  // 检查是否有值
  [[nodiscard]] constexpr bool has_value() const noexcept { return has_value_; }

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return has_value_;
  }

  // 访问值
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

  // 操作符重载
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

  // 重置
  void reset() noexcept {
    if (has_value_) {
      value().~T();
      has_value_ = false;
    }
  }

  // 就地构造
  template <typename... Args>
  T &emplace(Args &&...args) noexcept(
      moss::is_nothrow_constructible_v<T, Args...>) {
    reset();
    new (storage_) T(moss::forward<Args>(args)...);
    has_value_ = true;
    return value();
  }
};

// 容器库初始化
class ContainerLibrary {
private:
  static inline bool initialized_ = false;
  static inline SlabAllocator *slab_allocator_ = nullptr;

public:
  // 初始化容器库
  static bool initialize() noexcept {
    if (initialized_) {
      return true;
    }

    // 初始化Slab分配器
    slab_allocator_ = new SlabAllocator();
    if (slab_allocator_ == nullptr) {
      return false;
    }

    // 设置全局分配器指针
    g_slab_allocator = slab_allocator_;

    initialized_ = true;
    return true;
  }

  // 清理容器库
  static void cleanup() noexcept {
    if (initialized_) {
      delete slab_allocator_;
      slab_allocator_ = nullptr;
      g_slab_allocator = nullptr;
      initialized_ = false;
    }
  }

  // 获取分配器统计信息
  static void print_statistics() noexcept {
    if (slab_allocator_ != nullptr) {
      slab_allocator_->get_statistics();
    }
  }

  [[nodiscard]] static bool is_initialized() noexcept { return initialized_; }
};

// 便利的类型别名和常用实例化
namespace common_types {
// 进程相关数据结构
using ProcessQueue = SPSCQueue<ProcessId, 256>;
using ProcessList = RcuList<ProcessId>;
using ProcessWorkQueue = PerCpuWorkQueue<ProcessId, 128>;
using ProcessCounter = PerCpuAtomicCounter<u64>;

// 内存管理相关
using PageQueue = SPSCQueue<PhysAddr, 1024>;
using MemoryCounter = PerCpuAtomicCounter<usize>;

// 中断和设备管理
using InterruptQueue = MPSCQueue<u8>; // 泛型中断数据
using DeviceRegistry = RcuHashMap<DeviceId, VirtAddr>;
using InterruptCounter = PerCpuAtomicCounter<u64>;

// IPC相关
using MessageQueue = SPSCQueue<u64, 512>; // 泛型消息ID
using EndpointRegistry = RcuHashMap<EndpointId, ProcessId>;

// 系统统计结构
struct SystemStats {
  u64 context_switches = 0;
  u64 system_calls = 0;
  u64 page_faults = 0;
  u64 interrupts = 0;
};

using SystemCounters = PerCpuData<SystemStats>;
} // namespace common_types

// 性能测试和基准测试
class PerformanceBenchmark {
public:
  // SPSC队列性能测试
  template <typename T, usize Capacity>
  static void benchmark_spsc_queue() noexcept {
    SPSCQueue<T, Capacity> queue;

    constexpr usize OPERATIONS = 1000000;

    // 简单的性能测试（在实际内核中会更复杂）
    for (usize i = 0; i < OPERATIONS; ++i) {
      T item{};
      if (queue.try_enqueue(moss::move(item))) {
        T result;
        queue.try_dequeue(result);
      }
    }
  }

  // 原子操作性能测试
  static void benchmark_atomic_operations() noexcept {
    AtomicU64 counter{0};

    constexpr usize OPERATIONS = 10000000;

    for (usize i = 0; i < OPERATIONS; ++i) {
      (void)counter.fetch_add(1, MemoryOrder::Relaxed);
    }
  }

  // Per-CPU数据结构性能测试
  static void benchmark_per_cpu_counter() noexcept {
    PerCpuAtomicCounter<u64> counter;

    constexpr usize OPERATIONS = 1000000;

    for (usize i = 0; i < OPERATIONS; ++i) {
      (void)counter.fetch_add_local(1);
    }
  }
};

// 内存使用情况分析
class MemoryAnalyzer {
public:
  // 分析各种数据结构的内存占用
  static void analyze_memory_usage() noexcept {
    // SPSC队列内存占用
    constexpr usize spsc_256_size = sizeof(SPSCQueue<u64, 256>);
    constexpr usize spsc_1024_size = sizeof(SPSCQueue<u64, 1024>);

    // Per-CPU数据结构内存占用
    constexpr usize per_cpu_u64_size = sizeof(PerCpuData<u64>);
    constexpr usize per_cpu_counter_size = sizeof(PerCpuAtomicCounter<u64>);

    // RCU结构内存占用
    constexpr usize rcu_list_size = sizeof(RcuList<u64>);

    // Suppress unused variable warnings
    (void)spsc_256_size;
    (void)spsc_1024_size;
    (void)per_cpu_u64_size;
    (void)per_cpu_counter_size;
    (void)rcu_list_size;

    // 在实际内核中，这些信息会输出到内核日志
    // 目前只是编译时大小检查
    static_assert(spsc_256_size < PAGE_SIZE, "SPSC queue too large");
    static_assert(per_cpu_u64_size <= MAX_CPUS * CACHE_LINE_SIZE * 2,
                  "Per-CPU data reasonable size");
  }
};

// 容器库的配置和调优
struct ContainerConfig {
  // 队列大小配置
  static constexpr usize DEFAULT_PROCESS_QUEUE_SIZE = 256;
  static constexpr usize DEFAULT_MESSAGE_QUEUE_SIZE = 512;
  static constexpr usize DEFAULT_INTERRUPT_QUEUE_SIZE = 64;

  // Slab分配器配置
  static constexpr usize SLAB_MIN_OBJECT_SIZE = 8;
  static constexpr usize SLAB_MAX_OBJECT_SIZE = 4096;
  static constexpr usize SLAB_NUM_CACHES = 32;

  // Per-CPU配置
  static constexpr usize MAX_WORK_QUEUE_SIZE = 256;
  static constexpr usize MAX_RCU_CALLBACKS = 1024;

  // 性能调优选项
  static constexpr bool ENABLE_STATISTICS = true;
  static constexpr bool ENABLE_DEBUG_CHECKS = false; // 在发布版本中禁用
};

} // namespace moss::kernel::containers
