#pragma once

// 多架构原子操作和内存屏障支持
// 使用C++20的atomic库和架构特定的指令

#include "../include/types.hpp"
#include "../include/arch/arch_abstraction.hpp"

// 条件包含atomic头文件
#if __has_include(<atomic>)
#include <atomic>
#define MOSS_HAS_STD_ATOMIC 1
#else
#define MOSS_HAS_STD_ATOMIC 0
#endif

namespace moss::kernel::containers {

// 内存排序标签 - 多架构支持
enum class MemoryOrder : int {
#if MOSS_HAS_STD_ATOMIC
    Relaxed = static_cast<int>(std::memory_order_relaxed),
    Consume = static_cast<int>(std::memory_order_consume),
    Acquire = static_cast<int>(std::memory_order_acquire),
    Release = static_cast<int>(std::memory_order_release),
    AcqRel = static_cast<int>(std::memory_order_acq_rel),
    SeqCst = static_cast<int>(std::memory_order_seq_cst)
#else
    // Fallback values when std::atomic is not available
    Relaxed = 0,
    Consume = 1,
    Acquire = 2,
    Release = 3,
    AcqRel = 4,
    SeqCst = 5
#endif
};

// 原子指针类型 - 多架构优化
template<typename T>
class AtomicPtr {
private:
#if MOSS_HAS_STD_ATOMIC
    std::atomic<T*> ptr_;
#else
    // 简化实现：当没有标准库atomic时使用volatile指针
    // 注意：这不是真正的原子操作，仅用于编译兼容性
    volatile T* ptr_;
#endif

public:
    constexpr AtomicPtr() noexcept : ptr_(nullptr) {}
    constexpr AtomicPtr(T* p) noexcept : ptr_(p) {}

    // 禁用拷贝，允许移动
    AtomicPtr(const AtomicPtr&) = delete;
    AtomicPtr& operator=(const AtomicPtr&) = delete;

    AtomicPtr(AtomicPtr&& other) noexcept {
#if MOSS_HAS_STD_ATOMIC
        ptr_ = other.ptr_.exchange(nullptr);
#else
        ptr_ = other.ptr_;
        other.ptr_ = nullptr;
#endif
    }

    AtomicPtr& operator=(AtomicPtr&& other) noexcept {
        if (this != &other) {
#if MOSS_HAS_STD_ATOMIC
            ptr_.store(other.ptr_.exchange(nullptr));
#else
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
#endif
        }
        return *this;
    }

    // 原子加载
    [[nodiscard]] T* load(MemoryOrder order = MemoryOrder::SeqCst) const noexcept {
#if MOSS_HAS_STD_ATOMIC
        return ptr_.load(static_cast<std::memory_order>(order));
#else
        (void)order;  // 忽略内存顺序参数
        return const_cast<T*>(ptr_);
#endif
    }

    // 原子存储
    void store(T* desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
#if MOSS_HAS_STD_ATOMIC
        ptr_.store(desired, static_cast<std::memory_order>(order));
#else
        (void)order;  // 忽略内存顺序参数
        ptr_ = desired;
#endif
    }

    // 原子交换
    [[nodiscard]] T* exchange(T* desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
#if MOSS_HAS_STD_ATOMIC
        return ptr_.exchange(desired, static_cast<std::memory_order>(order));
#else
        (void)order;  // 忽略内存顺序参数
        T* old = const_cast<T*>(ptr_);
        ptr_ = desired;
        return old;
#endif
    }

    // CAS操作
    [[nodiscard]] bool compare_exchange_weak(T*& expected, T* desired,
                                             MemoryOrder success = MemoryOrder::SeqCst,
                                             MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
#if MOSS_HAS_STD_ATOMIC
        return ptr_.compare_exchange_weak(expected, desired,
                                          static_cast<std::memory_order>(success),
                                          static_cast<std::memory_order>(failure));
#else
        (void)success; (void)failure;  // 忽略内存顺序参数
        if (ptr_ == expected) {
            ptr_ = desired;
            return true;
        } else {
            expected = const_cast<T*>(ptr_);
            return false;
        }
#endif
    }

    [[nodiscard]] bool compare_exchange_strong(T*& expected, T* desired,
                                               MemoryOrder success = MemoryOrder::SeqCst,
                                               MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
#if MOSS_HAS_STD_ATOMIC
        return ptr_.compare_exchange_strong(expected, desired,
                                            static_cast<std::memory_order>(success),
                                            static_cast<std::memory_order>(failure));
#else
        // 在 fallback 实现中，strong 和 weak 版本相同
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

// 原子计数器 - 多架构优化
template<typename T>
class AtomicCounter {
private:
#if MOSS_HAS_STD_ATOMIC
    std::atomic<T> value_;
#else
    // 简化实现：当没有标准库atomic时使用volatile值
    // 注意：这不是真正的原子操作，仅用于编译兼容性
    volatile T value_;
#endif

public:
    constexpr AtomicCounter() noexcept : value_(0) {}
    constexpr AtomicCounter(T initial) noexcept : value_(initial) {}

    // 禁用拷贝
    AtomicCounter(const AtomicCounter&) = delete;
    AtomicCounter& operator=(const AtomicCounter&) = delete;

    // 原子加载
    [[nodiscard]] T load(MemoryOrder order = MemoryOrder::SeqCst) const noexcept {
#if MOSS_HAS_STD_ATOMIC
        return value_.load(static_cast<std::memory_order>(order));
#else
        (void)order;  // 忽略内存顺序参数
        return value_;
#endif
    }

    // 原子存储
    void store(T desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
#if MOSS_HAS_STD_ATOMIC
        value_.store(desired, static_cast<std::memory_order>(order));
#else
        (void)order;  // 忽略内存顺序参数
        value_ = desired;
#endif
    }

    // 原子递增
    [[nodiscard]] T fetch_add(T arg, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
#if MOSS_HAS_STD_ATOMIC
        return value_.fetch_add(arg, static_cast<std::memory_order>(order));
#else
        (void)order;  // 忽略内存顺序参数
        T old = value_;
        value_ = old + arg;
        return old;
#endif
    }

    // 原子递减
    [[nodiscard]] T fetch_sub(T arg, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
#if MOSS_HAS_STD_ATOMIC
        return value_.fetch_sub(arg, static_cast<std::memory_order>(order));
#else
        (void)order;  // 忽略内存顺序参数
        T old = value_;
        value_ = old - arg;
        return old;
#endif
    }

    // 原子比较和交换
    [[nodiscard]] bool compare_exchange_weak(T& expected, T desired,
                                           MemoryOrder success = MemoryOrder::SeqCst,
                                           MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
#if MOSS_HAS_STD_ATOMIC
        return value_.compare_exchange_weak(expected, desired,
                                          static_cast<std::memory_order>(success),
                                          static_cast<std::memory_order>(failure));
#else
        (void)success; (void)failure;  // 忽略内存顺序参数
        if (value_ == expected) {
            value_ = desired;
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
using AtomicU32 = AtomicCounter<u32>;
using AtomicU64 = AtomicCounter<u64>;
using AtomicSize = AtomicCounter<usize>;

// 内存屏障函数 - 使用架构抽象层
using moss::kernel::arch::memory_barrier;
using moss::kernel::arch::read_barrier;
using moss::kernel::arch::write_barrier;
using moss::kernel::arch::instruction_barrier;

// CPU缓存行对齐的原子类型
template<typename T>
struct alignas(CACHE_LINE_SIZE) CacheAlignedAtomic {
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
template<typename T>
class PerCpuCounter {
private:
    static constexpr usize MAX_CPUS_PADDED = ((MAX_CPUS + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE) * CACHE_LINE_SIZE;
    CacheAlignedAtomic<T> counters_[MAX_CPUS];

public:
    constexpr PerCpuCounter() noexcept = default;

    // 获取当前CPU的计数器
    [[nodiscard]] AtomicCounter<T>& get_local() noexcept {
        u32 cpu_id = get_current_cpu_id();
        return counters_[cpu_id % MAX_CPUS].value;
    }

    // 获取总计数（需要遍历所有CPU）
    [[nodiscard]] T get_total() const noexcept {
        T total = 0;
        for (usize i = 0; i < MAX_CPUS; ++i) {
            total += counters_[i].load(MemoryOrder::Relaxed);
        }
        return total;
    }

private:
    // 获取当前CPU ID的简单实现
    [[nodiscard]] static u32 get_current_cpu_id() noexcept {
        u64 mpidr;
        asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        return static_cast<u32>(mpidr & 0xFF);
    }
};

} // namespace moss::kernel::containers