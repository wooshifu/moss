#pragma once

// 无锁队列实现 - 高性能的SPSC和MPSC队列
// 专为内核环境优化，避免动态内存分配

#include "atomic_types.hpp"
#include "../include/types.hpp"
#include "../include/result.hpp"

// 包含统一的内核标准库支持
// Removed kernel_std.hpp include to avoid conflicts

// 包含concepts约束
#include "../include/concepts/container_concepts.hpp"

namespace moss::kernel::containers {

// 队列节点基类（侵入式设计）
template<typename T>
struct QueueNode {
    AtomicPtr<QueueNode<T>> next;
    T data;

    template<typename... Args>
    constexpr QueueNode(Args&&... args) noexcept
        : next(nullptr), data(move(args)...) {}
};

// SPSC (Single Producer Single Consumer) 无锁队列
// 使用环形缓冲区实现，性能最优
template<typename T, moss::kernel::usize Capacity>
class SPSCQueue {
private:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr moss::kernel::usize MASK = Capacity - 1;

    // 使用缓存行对齐避免false sharing
    alignas(moss::kernel::CACHE_LINE_SIZE) CacheAlignedAtomic<moss::kernel::usize> head_{0};
    alignas(moss::kernel::CACHE_LINE_SIZE) CacheAlignedAtomic<moss::kernel::usize> tail_{0};

    // 数据数组，每个元素缓存行对齐
    alignas(moss::kernel::CACHE_LINE_SIZE) T data_[Capacity];

public:
    constexpr SPSCQueue() noexcept = default;

    // 禁用拷贝和移动
    NON_COPYABLE_NON_MOVABLE(SPSCQueue)

    // 生产者接口：入队（非阻塞）
    template<typename U>
    [[nodiscard]] bool try_enqueue(U&& item) noexcept {
        const moss::kernel::usize current_tail = tail_.load(MemoryOrder::Relaxed);
        const moss::kernel::usize next_tail = (current_tail + 1) & MASK;

        // 检查队列是否已满（与head相撞）
        if (next_tail == head_.load(MemoryOrder::Acquire)) {
            return false;  // 队列满
        }

        // 写入数据
        data_[current_tail] = moss::forward<U>(item);

        // 更新tail，使用release语义确保数据写入可见
        tail_.store(next_tail, MemoryOrder::Release);

        return true;
    }

    // 消费者接口：出队（非阻塞）
    [[nodiscard]] bool try_dequeue(T& result) noexcept {
        const moss::kernel::usize current_head = head_.load(MemoryOrder::Relaxed);

        // 检查队列是否为空
        if (current_head == tail_.load(MemoryOrder::Acquire)) {
            return false;  // 队列空
        }

        // 读取数据
        result = moss::move(data_[current_head]);

        // 更新head，使用release语义
        head_.store((current_head + 1) & MASK, MemoryOrder::Release);

        return true;
    }

    // 队列状态查询
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

    [[nodiscard]] static constexpr moss::kernel::usize capacity() noexcept {
        return Capacity - 1;  // 实际容量减1（用于区分满和空）
    }
};

// MPSC (Multiple Producer Single Consumer) 无锁队列
// 使用链表实现，支持多个生产者
template<typename T>
class MPSCQueue {
private:
    // 头节点（消费者操作）
    alignas(moss::kernel::CACHE_LINE_SIZE) AtomicPtr<QueueNode<T>> head_;

    // 尾节点（生产者操作）
    alignas(moss::kernel::CACHE_LINE_SIZE) AtomicPtr<QueueNode<T>> tail_;

    // 虚拟头节点，简化算法实现
    QueueNode<T> stub_;

public:
    MPSCQueue() noexcept : head_(&stub_), tail_(&stub_) {
        stub_.next.store(nullptr, MemoryOrder::Relaxed);
    }

    // 禁用拷贝和移动
    NON_COPYABLE_NON_MOVABLE(MPSCQueue)

    // 生产者接口：入队（多线程安全）
    void enqueue(QueueNode<T>* node) noexcept {
        node->next.store(nullptr, MemoryOrder::Relaxed);

        // 原子地将节点链接到队列尾部
        QueueNode<T>* prev_tail = tail_.exchange(node, MemoryOrder::AcqRel);
        prev_tail->next.store(node, MemoryOrder::Release);
    }

    // 消费者接口：出队（单线程，非阻塞）
    [[nodiscard]] QueueNode<T>* try_dequeue() noexcept {
        QueueNode<T>* head = head_.load(MemoryOrder::Relaxed);
        QueueNode<T>* next = head->next.load(MemoryOrder::Acquire);

        if (next == nullptr) {
            return nullptr;  // 队列空
        }

        // 移动头指针
        head_.store(next, MemoryOrder::Relaxed);

        return next;
    }

    // 检查队列是否为空（近似）
    [[nodiscard]] bool empty() const noexcept {
        QueueNode<T>* head = head_.load(MemoryOrder::Acquire);
        QueueNode<T>* next = head->next.load(MemoryOrder::Acquire);
        return next == nullptr;
    }
};

// 固定大小的对象池（配合MPSC队列使用）
template<typename T, moss::kernel::usize PoolSize>
    requires (PoolSize > 0 && (PoolSize & (PoolSize - 1)) == 0)
class ObjectPool {
private:
    struct PoolNode : public QueueNode<T> {
        template<typename... Args>
        constexpr PoolNode(Args&&... args) noexcept(moss::is_nothrow_constructible_v<T, Args...>)
            : QueueNode<T>(moss::forward<Args>(args)...) {}
    };

    alignas(PAGE_SIZE) PoolNode pool_[PoolSize];
    MPSCQueue<T> free_list_;

public:
    constexpr ObjectPool() noexcept {
        // 初始化时将所有节点放入自由列表
        for (moss::kernel::usize i = 0; i < PoolSize; ++i) {
            free_list_.enqueue(&pool_[i]);
        }
    }

    // 分配一个对象
    [[nodiscard]] QueueNode<T>* allocate() noexcept {
        return free_list_.try_dequeue();
    }

    // 释放一个对象
    void deallocate(QueueNode<T>* node) noexcept {
        if (node != nullptr) {
            // 重置节点数据（可选）
            node->~QueueNode<T>();
            new (node) QueueNode<T>();

            free_list_.enqueue(node);
        }
    }

    // 池状态查询
    [[nodiscard]] bool has_available() const noexcept {
        return !free_list_.empty();
    }

    [[nodiscard]] static constexpr moss::kernel::usize pool_size() noexcept {
        return PoolSize;
    }
};

// MPMC (Multiple Producer Multiple Consumer) 队列
// 基于SPSC队列数组实现，每个消费者有专用队列
template<typename T, moss::kernel::usize NumConsumers, moss::kernel::usize QueueCapacity>
    requires (QueueCapacity > 0 && (QueueCapacity & (QueueCapacity - 1)) == 0) && (NumConsumers > 0)
class MPMCQueue {
private:
    SPSCQueue<T, QueueCapacity> queues_[NumConsumers];
    AtomicCounter<moss::kernel::usize> round_robin_counter_;

public:
    constexpr MPMCQueue() noexcept : round_robin_counter_(0) {}

    // 禁用拷贝和移动
    NON_COPYABLE_NON_MOVABLE(MPMCQueue)

    // 生产者接口：入队（轮询分发）
    template<typename U>
    [[nodiscard]] bool try_enqueue(U&& item) noexcept {
        const moss::kernel::usize start_idx = round_robin_counter_.fetch_add(1, MemoryOrder::Relaxed) % NumConsumers;

        // 尝试所有队列，从随机位置开始
        for (moss::kernel::usize i = 0; i < NumConsumers; ++i) {
            moss::kernel::usize queue_idx = (start_idx + i) % NumConsumers;
            if (queues_[queue_idx].try_enqueue(moss::forward<U>(item))) {
                return true;
            }
        }

        return false;  // 所有队列都满
    }

    // 消费者接口：出队（指定消费者索引）
    [[nodiscard]] bool try_dequeue(moss::kernel::usize consumer_id, T& result) noexcept {
        if (consumer_id >= NumConsumers) {
            return false;
        }

        return queues_[consumer_id].try_dequeue(result);
    }

    // 消费者接口：从任意队列出队（工作窃取）
    [[nodiscard]] bool try_dequeue_any(T& result) noexcept {
        const moss::kernel::usize start_idx = get_current_cpu_id() % NumConsumers;

        // 尝试所有队列
        for (moss::kernel::usize i = 0; i < NumConsumers; ++i) {
            moss::kernel::usize queue_idx = (start_idx + i) % NumConsumers;
            if (queues_[queue_idx].try_dequeue(result)) {
                return true;
            }
        }

        return false;  // 所有队列都空
    }

    // 队列状态
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

private:
    [[nodiscard]] static moss::kernel::u32 get_current_cpu_id() noexcept {
        moss::kernel::u64 mpidr;
        asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        return static_cast<moss::kernel::u32>(mpidr & 0xFFU);  // 返回CPU ID
    }
};

// 类型别名
using ProcessQueue = SPSCQueue<ProcessId, 256>;
using MessageQueue = MPSCQueue<u8>;  // 泛型消息
using WorkQueue = MPMCQueue<ProcessId, 4, 128>;  // 工作队列

} // namespace moss::kernel::containers