#pragma once

// 多架构原子操作和内存屏障支持
// 使用C++20的atomic库和架构特定的指令

#include "../include/types.hpp"
#include "../include/arch/arch_abstraction.hpp"
#include "../include/moss_std.hpp"

// 在freestanding环境中，我们需要自己实现原子操作而不是依赖<atomic>

namespace moss::kernel::containers {

// 使用moss_std.hpp中定义的MemoryOrder
using MemoryOrder = moss::MemoryOrder;

// 原子指针类型 - freestanding环境简化实现
template<typename T>
class AtomicPtr {
private:
    volatile T* ptr_;

public:
    constexpr AtomicPtr() noexcept : ptr_(nullptr) {}
    constexpr AtomicPtr(T* p) noexcept : ptr_(p) {}

    // 禁用拷贝，允许移动
    AtomicPtr(const AtomicPtr&) = delete;
    AtomicPtr& operator=(const AtomicPtr&) = delete;

    AtomicPtr(AtomicPtr&& other) noexcept {
        ptr_ = other.ptr_;
        other.ptr_ = nullptr;
    }

    AtomicPtr& operator=(AtomicPtr&& other) noexcept {
        if (this != &other) {
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    // 原子加载 (简化实现)
    [[nodiscard]] T* load(MemoryOrder order = MemoryOrder::SeqCst) const noexcept {
        (void)order;  // 忽略内存顺序参数
        return const_cast<T*>(ptr_);
    }

    // 原子存储 (简化实现)
    void store(T* desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
        (void)order;  // 忽略内存顺序参数
        ptr_ = desired;
    }

    // 原子交换 (简化实现)
    [[nodiscard]] T* exchange(T* desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
        (void)order;  // 忽略内存顺序参数
        T* old = const_cast<T*>(ptr_);
        ptr_ = desired;
        return old;
    }

    // CAS操作 (简化实现)
    [[nodiscard]] bool compare_exchange_weak(T*& expected, T* desired,
                                             MemoryOrder success = MemoryOrder::SeqCst,
                                             MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
        (void)success; (void)failure;  // 忽略内存顺序参数
        if (ptr_ == expected) {
            ptr_ = desired;
            return true;
        } else {
            expected = const_cast<T*>(ptr_);
            return false;
        }
    }

    [[nodiscard]] bool compare_exchange_strong(T*& expected, T* desired,
                                               MemoryOrder success = MemoryOrder::SeqCst,
                                               MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
        // 在简化实现中，strong 和 weak 版本相同
        return compare_exchange_weak(expected, desired, success, failure);
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

// 原子计数器 - freestanding环境简化实现
template<typename T>
class AtomicCounter {
private:
    volatile T value_;

public:
    constexpr AtomicCounter() noexcept : value_(0) {}
    constexpr AtomicCounter(T initial) noexcept : value_(initial) {}

    // 禁用拷贝
    AtomicCounter(const AtomicCounter&) = delete;
    AtomicCounter& operator=(const AtomicCounter&) = delete;

    // 原子加载 (简化实现)
    [[nodiscard]] T load(MemoryOrder order = MemoryOrder::SeqCst) const noexcept {
        (void)order;  // 忽略内存顺序参数
        return value_;
    }

    // 原子存储 (简化实现)
    void store(T desired, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
        (void)order;  // 忽略内存顺序参数
        value_ = desired;
    }

    // 原子递增 (简化实现)
    [[nodiscard]] T fetch_add(T arg, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
        (void)order;  // 忽略内存顺序参数
        T old = value_;
        value_ = old + arg;
        return old;
    }

    // 原子递减 (简化实现)
    [[nodiscard]] T fetch_sub(T arg, MemoryOrder order = MemoryOrder::SeqCst) noexcept {
        (void)order;  // 忽略内存顺序参数
        T old = value_;
        value_ = old - arg;
        return old;
    }

    // 原子比较和交换 (简化实现)
    [[nodiscard]] bool compare_exchange_weak(T& expected, T desired,
                                           MemoryOrder success = MemoryOrder::SeqCst,
                                           MemoryOrder failure = MemoryOrder::SeqCst) noexcept {
        (void)success; (void)failure;  // 忽略内存顺序参数
        if (value_ == expected) {
            value_ = desired;
            return true;
        } else {
            expected = value_;
            return false;
        }
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

// 内存屏障函数 - 使用架构抽象层
using moss::kernel::arch::memory_barrier;
using moss::kernel::arch::read_barrier;
using moss::kernel::arch::write_barrier;
using moss::kernel::arch::instruction_barrier;

// CPU缓存行对齐的原子类型
template<typename T>
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
template<typename T>
class PerCpuCounter {
private:
    static constexpr moss::kernel::usize MAX_CPUS_PADDED = ((moss::kernel::MAX_CPUS + moss::kernel::CACHE_LINE_SIZE - 1) / moss::kernel::CACHE_LINE_SIZE) * moss::kernel::CACHE_LINE_SIZE;
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