#pragma once

/**
 * @file concepts_test.hpp
 * @brief MOSS C++23 concepts 测试和使用示例
 * @author MOSS Kernel Team
 * @version C++23
 *
 * 验证和演示concepts的正确性和用法。
 */

#include "kernel_concepts.hpp"
#include "container_concepts.hpp"
#include "memory_concepts.hpp"
#include "concurrency_concepts.hpp"

namespace moss::concepts::test {

using namespace moss::kernel;

/**
 * @brief 编译时concepts测试
 *
 * 使用static_assert来验证concepts的正确性。
 * 这些测试在编译时执行，不产生运行时代码。
 */
namespace compile_time_tests {

    // 测试基础类型是否满足KernelSafe概念
    static_assert(KernelSafe<u32>);
    static_assert(KernelSafe<u64>);
    static_assert(KernelSafe<ProcessId>);
    static_assert(KernelSafe<ThreadId>);

    // 测试原子兼容性
    static_assert(AtomicCompatible<u32>);
    static_assert(AtomicCompatible<u64>);
    static_assert(AtomicCompatible<u8>);
    // static_assert(!AtomicCompatible<u128>); // u128 not available

    // 测试2的幂概念
    static_assert(PowerOfTwo<1>);
    static_assert(PowerOfTwo<2>);
    static_assert(PowerOfTwo<4>);
    static_assert(PowerOfTwo<256>);
    static_assert(PowerOfTwo<1024>);
    static_assert(!PowerOfTwo<3>);
    static_assert(!PowerOfTwo<5>);
    static_assert(!PowerOfTwo<0>);

    // 测试容量概念
    static_assert(ValidCapacity<2>);
    static_assert(ValidCapacity<256>);
    static_assert(ValidCapacity<1024>);
    static_assert(!ValidCapacity<3>);

    // 测试队列容量概念
    static_assert(ValidQueueCapacity<256>);
    static_assert(ValidQueueCapacity<1024>);
    static_assert(!ValidQueueCapacity<3>);

    // 测试SPSC队列元素概念
    static_assert(SPSCQueueElement<u32>);
    static_assert(SPSCQueueElement<ProcessId>);
    static_assert(LockFreeElement<ThreadId>);

    // 测试整数类型概念
    static_assert(IntegerType<u32>);
    static_assert(IntegerType<i64>);
    static_assert(IntegerType<usize>);

    // 测试地址类型概念（基于大小）
    static_assert(AddressType<PhysAddr>);
    static_assert(AddressType<VirtAddr>);

} // namespace compile_time_tests

/**
 * @brief 概念使用示例
 *
 * 展示如何在实际代码中使用concepts来约束模板参数。
 */
namespace usage_examples {

    /**
     * @brief 使用concepts约束的函数模板
     *
     * 这个函数只接受满足KernelSafe和AtomicCompatible的类型。
     */
    template<typename T>
        requires KernelSafe<T> && AtomicCompatible<T>
    T atomic_increment(T& value) noexcept {
        // 在实际实现中，这里会使用原子操作
        return ++value;
    }

    /**
     * @brief 使用concepts约束的类模板
     *
     * 这个容器只能存储满足特定要求的元素。
     */
    template<SPSCQueueElement T, usize Capacity>
        requires ValidQueueCapacity<Capacity>
    class SafeQueue {
    private:
        T buffer_[Capacity];
        usize head_ = 0;
        usize tail_ = 0;

    public:
        bool try_push(const T& item) noexcept {
            // 简化的实现
            if ((tail_ + 1) % Capacity == head_) {
                return false; // 队列满
            }
            buffer_[tail_] = item;
            tail_ = (tail_ + 1) % Capacity;
            return true;
        }

        bool try_pop(T& item) noexcept {
            if (head_ == tail_) {
                return false; // 队列空
            }
            item = buffer_[head_];
            head_ = (head_ + 1) % Capacity;
            return true;
        }

        [[nodiscard]] static constexpr usize capacity() noexcept {
            return Capacity;
        }

        [[nodiscard]] bool empty() const noexcept {
            return head_ == tail_;
        }
    };

    /**
     * @brief 使用多个concepts的组合约束
     */
    template<typename T>
        requires KernelSafe<T> && AtomicCompatible<T> && IntegerType<T>
    class AtomicCounter {
    private:
        T value_;

    public:
        explicit AtomicCounter(T initial = T{}) noexcept : value_(initial) {}

        T load() const noexcept {
            return value_;
        }

        void store(T new_value) noexcept {
            value_ = new_value;
        }

        T fetch_add(T increment) noexcept {
            T old_value = value_;
            value_ += increment;
            return old_value;
        }
    };

    /**
     * @brief 概念特化：为不同类型提供不同实现
     */
    template<typename T>
    constexpr const char* get_type_category() noexcept {
        if constexpr (KernelSafe<T> && AtomicCompatible<T>) {
            return "Kernel-safe atomic type";
        } else if constexpr (KernelSafe<T>) {
            return "Kernel-safe type";
        } else {
            return "Generic type";
        }
    }

    /**
     * @brief SFINAE 替代品：使用requires表达式
     */
    template<typename T>
    auto process_if_lockfree_element(T&& item) noexcept -> void
        requires LockFreeElement<T>
    {
        // 只有满足LockFreeElement的类型才能调用这个函数
        // 编译时错误信息会清楚地指出类型约束
        static_cast<void>(item); // 避免未使用变量警告
    }

} // namespace usage_examples

/**
 * @brief 概念错误演示
 *
 * 这些函数展示了当类型不满足concepts时会发生什么。
 * 注释掉的代码会导致编译错误，并提供清晰的错误信息。
 */
namespace error_examples {

    // 这些调用会产生编译错误，因为类型不满足要求：

    void demonstrate_concept_violations() {
        // 错误：3不是2的幂
        // constexpr bool bad_capacity = ValidQueueCapacity<3>;

        // 错误：float可能不满足AtomicCompatible（取决于实现）
        // usage_examples::AtomicCounter<float> bad_counter;

        // 错误：大型结构体不满足LockFreeElement的大小要求
        struct LargeStruct { char data[1000]; };
        // static_assert(LockFreeElement<LargeStruct>); // 会失败
    }

} // namespace error_examples

} // namespace moss::concepts::test