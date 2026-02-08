#pragma once

/**
 * @file container_concepts.hpp
 * @brief MOSS 容器类型 C++23 concepts 定义（工作版本）
 * @author MOSS Kernel Team
 * @version C++23
 *
 * 为无锁队列、原子类型、分配器等容器定义类型约束。
 * 专门针对高性能内核容器的要求。避免std依赖以确保编译成功。
 */

#include "kernel_concepts.hpp"

namespace moss::concepts {

using namespace moss::kernel;

// 常量定义
constexpr usize MAX_ELEMENT_SIZE = 64;
constexpr usize MAX_RCU_ELEMENT_SIZE = 1024;
constexpr usize MAX_QUEUE_CAPACITY = 65536;
constexpr usize MIN_QUEUE_CAPACITY = 2;
constexpr usize MAX_SLAB_OBJECT_SIZE = 4096;
constexpr usize MIN_SLAB_OBJECT_SIZE = 8;
constexpr usize MAX_ALIGNMENT = 64;

/**
 * @brief 无锁队列元素概念
 *
 * 基于大小的无锁元素检查，避免std依赖
 */
template<typename T>
concept LockFreeElement = KernelSafe<T> &&
                          (sizeof(T) <= MAX_ELEMENT_SIZE);

/**
 * @brief SPSC队列元素概念
 *
 * 单生产者-单消费者队列的元素要求
 */
template<typename T>
concept SPSCQueueElement = LockFreeElement<T> &&
                           AtomicCompatible<T>;

/**
 * @brief MPSC队列元素概念
 *
 * 多生产者-单消费者队列的要求
 */
template<typename T>
concept MPSCQueueElement = SPSCQueueElement<T>;

/**
 * @brief 有效队列容量概念
 *
 * 队列容量检查，基于2的幂和范围
 */
template<usize Capacity>
concept ValidQueueCapacity = PowerOfTwo<Capacity> &&
                             (Capacity >= MIN_QUEUE_CAPACITY) &&
                             (Capacity <= MAX_QUEUE_CAPACITY);

/**
 * @brief SPSC队列约束概念
 *
 * 组合概念，检查SPSC队列的完整约束
 */
template<typename T, usize Capacity>
concept ValidSPSCQueue = SPSCQueueElement<T> &&
                         ValidQueueCapacity<Capacity>;

/**
 * @brief 原子计数器类型概念
 *
 * 原子计数器的类型要求
 */
template<typename T>
concept CounterType = IntegerType<T> &&
                      AtomicCompatible<T>;

/**
 * @brief RCU元素概念
 *
 * RCU（Read-Copy-Update）数据结构的元素要求，基于大小检查
 */
template<typename T>
concept RCUElement = KernelSafe<T> &&
                     (sizeof(T) <= MAX_RCU_ELEMENT_SIZE);

/**
 * @brief 哈希表键概念
 *
 * 哈希表键的基本要求，基于接口检查
 */
template<typename K>
concept HashKey = KernelSafe<K> &&
                  requires(K a, K b) {
                      { a == b };
                  };

/**
 * @brief 哈希表值概念
 *
 * 哈希表值的要求
 */
template<typename V>
concept HashValue = RCUElement<V>;

/**
 * @brief Slab对象概念
 *
 * Slab分配器对象的要求，基于大小和对齐检查
 */
template<typename T>
concept SlabObject = KernelSafe<T> &&
                     (alignof(T) <= MAX_ALIGNMENT) &&
                     (sizeof(T) >= MIN_SLAB_OBJECT_SIZE) &&
                     (sizeof(T) <= MAX_SLAB_OBJECT_SIZE);

/**
 * @brief Per-CPU数据概念
 *
 * Per-CPU数据结构的要求
 */
template<typename T>
concept PerCpuData = KernelSafe<T> &&
                     CacheAligned<T>;

/**
 * @brief 工作项概念
 *
 * 工作队列项的要求，基于接口检查
 */
template<typename W>
concept WorkItem = KernelSafe<W> &&
                   requires(W w) {
                       w.execute();
                   };

/**
 * @brief 内存对象概念
 *
 * 通用内存对象的要求
 */
template<typename T>
concept MemoryObject = KernelSafe<T> &&
                       (alignof(T) <= PAGE_SIZE);

/**
 * @brief 容器分配器概念
 *
 * 容器使用的分配器接口，基于接口检查
 */
template<typename Alloc, typename T>
concept ContainerAllocator = KernelSafe<Alloc> &&
                             requires(Alloc a, T* ptr, usize count) {
                                 a.allocate(count);
                                 a.deallocate(ptr, count);
                             };

/**
 * @brief 线程安全容器概念
 *
 * 线程安全容器的基本要求，基于接口检查
 */
template<typename Container>
concept ThreadSafeContainer = KernelSafe<Container> &&
                              requires(Container c) {
                                  c.size();
                                  c.empty();
                              };

/**
 * @brief 可池化对象概念
 *
 * 对象池中对象的要求
 */
template<typename T>
concept PoolableObject = KernelSafe<T> &&
                         (sizeof(T) <= MAX_ELEMENT_SIZE);

/**
 * @brief MPMC队列元素概念
 *
 * 多生产者-多消费者队列的元素要求
 */
template<typename T>
concept MPMCQueueElement = SPSCQueueElement<T>;

} // namespace moss::concepts