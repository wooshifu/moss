#pragma once

// Per-CPU数据结构实现
// 避免缓存行冲突，提高多核性能

#include "atomic_types.hpp"
#include "../include/types.hpp"

// 包含统一的内核标准库支持
#include "../include/kernel_std.hpp"

namespace moss::kernel::containers {

// 前向声明
template<typename T, usize Capacity>
class SPSCQueue;

// Per-CPU数据访问器
template<typename T>
class PerCpuData {
private:
    // 每个CPU的数据，缓存行对齐
    alignas(CACHE_LINE_SIZE) T data_[MAX_CPUS];

public:
    // 默认构造
#if MOSS_HAS_STD_UTILITY_PERCPU
    constexpr PerCpuData() noexcept(std::is_nothrow_default_constructible_v<T>)
        : data_{} {}
#else
    constexpr PerCpuData() noexcept
        : data_{} {}
#endif

    // 统一值构造
    template<typename... Args>
#if MOSS_HAS_STD_UTILITY_PERCPU
    explicit PerCpuData(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
#else
    explicit PerCpuData(Args&&... args) noexcept {
#endif
        for (usize i = 0; i < MAX_CPUS; ++i) {
            new (&data_[i]) T(args...);
        }
    }

    // 禁用拷贝，允许移动
    PerCpuData(const PerCpuData&) = delete;
    PerCpuData& operator=(const PerCpuData&) = delete;

#if MOSS_HAS_STD_UTILITY_PERCPU
    PerCpuData(PerCpuData&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        for (usize i = 0; i < MAX_CPUS; ++i) {
            data_[i] = std::move(other.data_[i]);
        }
    }
#else
    PerCpuData(PerCpuData&& other) noexcept {
        for (usize i = 0; i < MAX_CPUS; ++i) {
            data_[i] = static_cast<T&&>(other.data_[i]);
        }
    }
#endif

#if MOSS_HAS_STD_UTILITY_PERCPU
    PerCpuData& operator=(PerCpuData&& other) noexcept(std::is_nothrow_move_assignable_v<T>) {
        if (this != &other) {
            for (usize i = 0; i < MAX_CPUS; ++i) {
                data_[i] = std::move(other.data_[i]);
            }
        }
        return *this;
    }
#else
    PerCpuData& operator=(PerCpuData&& other) noexcept {
        if (this != &other) {
            for (usize i = 0; i < MAX_CPUS; ++i) {
                data_[i] = static_cast<T&&>(other.data_[i]);
            }
        }
        return *this;
    }
#endif

    // 获取当前CPU的数据
    [[nodiscard]] T& get_local() noexcept {
        return data_[get_current_cpu_id()];
    }

    [[nodiscard]] const T& get_local() const noexcept {
        return data_[get_current_cpu_id()];
    }

    // 获取指定CPU的数据
    [[nodiscard]] T& get_cpu(usize cpu_id) noexcept {
        return data_[cpu_id % MAX_CPUS];
    }

    [[nodiscard]] const T& get_cpu(usize cpu_id) const noexcept {
        return data_[cpu_id % MAX_CPUS];
    }

    // 对所有CPU数据应用函数
    template<typename Func>
    void for_each_cpu(Func&& func) {
        for (usize i = 0; i < MAX_CPUS; ++i) {
            func(i, data_[i]);
        }
    }

    template<typename Func>
    void for_each_cpu(Func&& func) const {
        for (usize i = 0; i < MAX_CPUS; ++i) {
            func(i, data_[i]);
        }
    }

    // 聚合所有CPU的数据
    template<typename Func, typename Result = T>
    [[nodiscard]] Result fold(Func&& func, Result initial = Result{}) const {
        Result result = initial;
        for (usize i = 0; i < MAX_CPUS; ++i) {
            result = func(result, data_[i]);
        }
        return result;
    }

    // 获取所有CPU数据的总和（要求T支持+=操作）
#if MOSS_HAS_STD_UTILITY_PERCPU
    [[nodiscard]] T sum() const noexcept(noexcept(std::declval<T&>() += std::declval<const T&>())) {
#else
    [[nodiscard]] T sum() const noexcept {
#endif
        T total{};
        for (usize i = 0; i < MAX_CPUS; ++i) {
            total += data_[i];
        }
        return total;
    }

private:
    // 获取当前CPU ID
    [[nodiscard]] static usize get_current_cpu_id() noexcept {
        #if defined(MOSS_ARCH_ARM64)
            u64 mpidr;
            asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
            return static_cast<usize>(mpidr & 0xFF) % MAX_CPUS;
        #elif defined(MOSS_ARCH_X86_64)
            // x86_64: 简化实现，返回CPU 0
            // 实际应使用APIC ID或其他机制
            return 0;
        #elif defined(MOSS_ARCH_RISCV)
            // RISC-V: 读取hart ID
            u64 hart_id;
            asm volatile("csrr %0, mhartid" : "=r"(hart_id));
            return static_cast<usize>(hart_id) % MAX_CPUS;
        #else
            return 0; // 回退实现
        #endif
    }
};

// Per-CPU原子计数器
template<typename T>
class PerCpuAtomicCounter {
private:
    PerCpuData<CacheAlignedAtomic<T>> counters_;

public:
    constexpr PerCpuAtomicCounter() noexcept = default;

    // 在当前CPU上递增
    [[nodiscard]] T fetch_add_local(T value = 1) noexcept {
        return counters_.get_local().value.fetch_add(value, MemoryOrder::Relaxed);
    }

    // 在当前CPU上递减
    [[nodiscard]] T fetch_sub_local(T value = 1) noexcept {
        return counters_.get_local().value.fetch_sub(value, MemoryOrder::Relaxed);
    }

    // 获取当前CPU的值
    [[nodiscard]] T load_local() const noexcept {
        return counters_.get_local().value.load(MemoryOrder::Relaxed);
    }

    // 获取所有CPU的总计值
    [[nodiscard]] T load_total() const noexcept {
        T total = 0;
        counters_.for_each_cpu([&total](usize, const auto& counter) {
            total += counter.value.load(MemoryOrder::Relaxed);
        });
        return total;
    }

    // 重置所有计数器
    void reset_all() noexcept {
        counters_.for_each_cpu([](usize, auto& counter) {
            counter.value.store(0, MemoryOrder::Relaxed);
        });
    }

    // 操作符重载
    [[nodiscard]] T operator++() noexcept {
        return fetch_add_local() + 1;
    }

    [[nodiscard]] T operator++(int) noexcept {
        return fetch_add_local();
    }

    [[nodiscard]] T operator--() noexcept {
        return fetch_sub_local() - 1;
    }

    [[nodiscard]] T operator--(int) noexcept {
        return fetch_sub_local();
    }

    PerCpuAtomicCounter& operator+=(T value) noexcept {
        fetch_add_local(value);
        return *this;
    }

    PerCpuAtomicCounter& operator-=(T value) noexcept {
        fetch_sub_local(value);
        return *this;
    }

    // 转换为总值
    [[nodiscard]] operator T() const noexcept {
        return load_total();
    }
};

// Per-CPU工作队列
template<typename T, usize QueueSize = 256>
class PerCpuWorkQueue {
private:
    PerCpuData<SPSCQueue<T, QueueSize>> queues_;

public:
    constexpr PerCpuWorkQueue() noexcept = default;

    // 向当前CPU的队列添加工作
    [[nodiscard]] bool enqueue_local(const T& item) noexcept {
        return queues_.get_local().try_enqueue(item);
    }

    [[nodiscard]] bool enqueue_local(T&& item) noexcept {
#if MOSS_HAS_STD_UTILITY_PERCPU
        return queues_.get_local().try_enqueue(std::move(item));
#else
        return queues_.get_local().try_enqueue(static_cast<T&&>(item));
#endif
    }

    // 从当前CPU的队列取出工作
    [[nodiscard]] bool dequeue_local(T& result) noexcept {
        return queues_.get_local().try_dequeue(result);
    }

    // 向指定CPU的队列添加工作
    [[nodiscard]] bool enqueue_to_cpu(usize cpu_id, const T& item) noexcept {
        return queues_.get_cpu(cpu_id).try_enqueue(item);
    }

    [[nodiscard]] bool enqueue_to_cpu(usize cpu_id, T&& item) noexcept {
#if MOSS_HAS_STD_UTILITY_PERCPU
        return queues_.get_cpu(cpu_id).try_enqueue(std::move(item));
#else
        return queues_.get_cpu(cpu_id).try_enqueue(static_cast<T&&>(item));
#endif
    }

    // 从指定CPU的队列取出工作
    [[nodiscard]] bool dequeue_from_cpu(usize cpu_id, T& result) noexcept {
        return queues_.get_cpu(cpu_id).try_dequeue(result);
    }

    // 工作窃取：从其他CPU的队列窃取工作
    [[nodiscard]] bool steal_work(T& result) noexcept {
        usize current_cpu = get_current_cpu_id();

        // 从下一个CPU开始，避免窃取自己的工作
        for (usize i = 1; i < MAX_CPUS; ++i) {
            usize target_cpu = (current_cpu + i) % MAX_CPUS;
            if (queues_.get_cpu(target_cpu).try_dequeue(result)) {
                return true;
            }
        }

        return false;
    }

    // 检查当前CPU队列是否为空
    [[nodiscard]] bool empty_local() const noexcept {
        return queues_.get_local().empty();
    }

    // 检查所有队列是否都为空
    [[nodiscard]] bool empty_all() const noexcept {
        bool all_empty = true;
        queues_.for_each_cpu([&all_empty](usize, const auto& queue) {
            if (!queue.empty()) {
                all_empty = false;
            }
        });
        return all_empty;
    }

    // 获取所有队列的近似总大小
    [[nodiscard]] usize approximate_total_size() const noexcept {
        usize total_size = 0;
        queues_.for_each_cpu([&total_size](usize, const auto& queue) {
            total_size += queue.approximate_size();
        });
        return total_size;
    }

private:
    [[nodiscard]] static usize get_current_cpu_id() noexcept {
        u64 mpidr;
        asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        return static_cast<usize>(mpidr & 0xFF) % MAX_CPUS;
    }
};

// 简化的Per-CPU RCU回调系统
struct RcuCallback {
    void (*callback)();
    u64 grace_period;

    RcuCallback() noexcept : callback(nullptr), grace_period(0) {}
    RcuCallback(void (*func)(), u64 gp) noexcept : callback(func), grace_period(gp) {}
};

class PerCpuRcuCallbacks {
private:
    // 简化：使用固定大小的数组而不是队列
    static constexpr usize MAX_CALLBACKS = 1024;

    struct CallbackArray {
        RcuCallback callbacks[MAX_CALLBACKS];
        AtomicCounter<usize> head{0};
        AtomicCounter<usize> tail{0};

        [[nodiscard]] bool enqueue(const RcuCallback& cb) noexcept {
            usize current_tail = tail.load(MemoryOrder::Relaxed);
            usize next_tail = (current_tail + 1) % MAX_CALLBACKS;

            if (next_tail == head.load(MemoryOrder::Acquire)) {
                return false;  // 队列满
            }

            callbacks[current_tail] = cb;
            tail.store(next_tail, MemoryOrder::Release);
            return true;
        }

        [[nodiscard]] bool dequeue(RcuCallback& cb) noexcept {
            usize current_head = head.load(MemoryOrder::Relaxed);

            if (current_head == tail.load(MemoryOrder::Acquire)) {
                return false;  // 队列空
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

    // 调度RCU回调
    [[nodiscard]] bool schedule_callback(void (*func)()) noexcept {
        if (func == nullptr) {
            return false;
        }

        u64 current_gp = grace_period_counter_.load_total();
        RcuCallback callback(func, current_gp + 1);

        return callback_arrays_.get_local().enqueue(callback);
    }

    // 处理到期的RCU回调
    void process_callbacks() noexcept {
        u64 current_gp = grace_period_counter_.load_total();
        auto& local_array = callback_arrays_.get_local();

        RcuCallback callback;
        while (local_array.dequeue(callback)) {
            if (callback.grace_period <= current_gp) {
                // 安静期已过，执行回调
                if (callback.callback != nullptr) {
                    callback.callback();
                }
            } else {
                // 重新入队等待下一次
                (void)local_array.enqueue(callback);
                break;  // 避免无限循环
            }
        }
    }

    // 推进安静期
    void advance_grace_period() noexcept {
        (void)grace_period_counter_.fetch_add_local(1);
    }
};

// 类型别名
using PerCpuU32Counter = PerCpuAtomicCounter<u32>;
using PerCpuU64Counter = PerCpuAtomicCounter<u64>;
using PerCpuUSizeCounter = PerCpuAtomicCounter<usize>;

using ProcessWorkQueue = PerCpuWorkQueue<ProcessId, 128>;
using InterruptWorkQueue = PerCpuWorkQueue<InterruptId, 64>;

} // namespace moss::kernel::containers