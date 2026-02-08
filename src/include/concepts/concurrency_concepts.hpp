#pragma once

/**
 * @file concurrency_concepts.hpp
 * @brief MOSS 并发和同步 C++23 concepts 定义
 * @author MOSS Kernel Team
 * @version C++23
 *
 * 为原子操作、锁、线程同步等定义类型约束。
 * 专注于无锁编程和高性能并发。
 */

#include "kernel_concepts.hpp"

namespace moss::concepts {

using namespace moss::kernel;

/**
 * @brief 原子操作兼容类型概念
 *
 * 要求类型可以用于std::atomic：
 * - 平凡可复制
 * - 适合硬件原子操作的大小
 * - 内核安全
 */
template<typename T>
concept AtomicType = AtomicCompatible<T> && KernelSafe<T>;

/**
 * @brief 自旋锁概念
 *
 * 自旋锁的基本要求：
 * - 内核安全
 * - 支持基本的锁操作
 * - 无阻塞（适合内核使用）
 */
template<typename SpinLock>
concept Spinlock = KernelSafe<SpinLock> &&
                   requires(SpinLock lock) {
                       lock.lock();
                       lock.unlock();
                       lock.try_lock();
                       lock.is_locked();
                   };

/**
 * @brief 互斥锁概念
 *
 * 互斥锁的要求：
 * - 支持基本的锁操作
 * - 支持超时锁定（可选）
 */
template<typename Mutex>
concept MutexLike = KernelSafe<Mutex> &&
                    requires(Mutex m) {
                        m.lock();
                        m.unlock();
                        m.try_lock();
                    };

/**
 * @brief 读写锁概念
 *
 * 读写锁支持读写分离：
 * - 支持读锁和写锁
 * - 多个读者，单个写者
 */
template<typename RWLock>
concept ReadWriteLock = KernelSafe<RWLock> &&
                        requires(RWLock rwlock) {
                            rwlock.read_lock();
                            rwlock.read_unlock();
                            rwlock.write_lock();
                            rwlock.write_unlock();
                            rwlock.try_read_lock();
                            rwlock.try_write_lock();
                        };

/**
 * @brief 无锁队列概念
 *
 * 无锁队列的要求：
 * - 内核安全
 * - 支持无锁的入队和出队操作
 * - 返回操作是否成功
 */
template<typename LockFreeQueue>
concept LockFreeQueueLike = KernelSafe<LockFreeQueue> &&
                            requires(LockFreeQueue q) {
                                q.try_enqueue(typename LockFreeQueue::value_type{});
                                q.try_dequeue();
                                q.empty();
                                q.size();
                            };

/**
 * @brief 信号量概念
 *
 * 信号量的基本操作：
 * - 获取和释放资源
 * - 查询可用资源数量
 */
template<typename Semaphore>
concept SemaphoreLike = KernelSafe<Semaphore> &&
                        requires(Semaphore sem) {
                            sem.acquire();
                            sem.release();
                            sem.try_acquire();
                            sem.available();
                        };

/**
 * @brief 条件变量概念
 *
 * 条件变量用于线程同步：
 * - 支持等待和通知
 * - 与互斥锁配合使用
 */
template<typename CondVar>
concept ConditionVariable = KernelSafe<CondVar> &&
                            requires(CondVar cv) {
                                cv.wait();
                                cv.notify_one();
                                cv.notify_all();
                            };

/**
 * @brief 内存序概念
 *
 * 内存序枚举的要求，基于大小检查，避免std依赖
 */
template<typename MemOrder>
concept MemoryOrderEnum = KernelSafe<MemOrder> &&
                          (sizeof(MemOrder) <= sizeof(u32));

/**
 * @brief 原子计数器概念
 *
 * 原子计数器的特殊要求：
 * - 支持原子的增减操作
 * - 支持比较和交换操作
 * - 返回操作后的值
 */
template<typename AtomicCounter>
concept AtomicCounterLike = KernelSafe<AtomicCounter> &&
                            requires(AtomicCounter counter, typename AtomicCounter::value_type val) {
                                counter.load();
                                counter.store(val);
                                counter.fetch_add(val);
                                counter.fetch_sub(val);
                                counter.compare_exchange_weak(val, val);
                                counter.compare_exchange_strong(val, val);
                            };

/**
 * @brief RCU（Read-Copy-Update）概念
 *
 * RCU同步机制：
 * - 支持读取临界区
 * - 支持同步等待
 * - 支持数据更新
 */
template<typename RCU>
concept ReadCopyUpdate = KernelSafe<RCU> &&
                         requires(RCU rcu) {
                             rcu.read_lock();
                             rcu.read_unlock();
                             rcu.synchronize();
                             rcu.call_rcu();
                         };

/**
 * @brief 锁保护概念
 *
 * RAII锁保护的要求：
 * - 构造时自动加锁
 * - 析构时自动解锁
 * - 不可复制和移动
 */
template<typename LockGuard>
concept LockGuardLike = KernelSafe<LockGuard> &&
                        NonCopyable<LockGuard> &&
                        NonMovable<LockGuard>;

/**
 * @brief 原子指针概念
 *
 * 原子指针的要求：
 * - 指针类型的原子操作
 * - 支持CAS操作
 * - 支持指针算术（可选）
 */
template<typename AtomicPtr>
concept AtomicPointer = KernelSafe<AtomicPtr> &&
                        requires(AtomicPtr aptr, typename AtomicPtr::pointer_type ptr) {
                            aptr.load();
                            aptr.store(ptr);
                            aptr.exchange(ptr);
                            aptr.compare_exchange_weak(ptr, ptr);
                            aptr.compare_exchange_strong(ptr, ptr);
                        };

/**
 * @brief 等待队列概念
 *
 * 内核等待队列：
 * - 支持任务等待和唤醒
 * - 支持条件等待
 */
template<typename WaitQueue>
concept WaitQueueLike = KernelSafe<WaitQueue> &&
                        requires(WaitQueue wq) {
                            wq.wait();
                            wq.wake_up();
                            wq.wake_up_all();
                            wq.empty();
                        };

/**
 * @brief 工作队列概念
 *
 * 异步工作队列：
 * - 支持任务提交
 * - 支持任务执行
 * - 支持队列管理
 */
template<typename WorkQueue>
concept WorkQueueLike = KernelSafe<WorkQueue> &&
                        requires(WorkQueue wq) {
                            wq.submit_work();
                            wq.flush();
                            wq.size();
                            wq.empty();
                        };

/**
 * @brief 内存屏障操作概念
 *
 * 内存屏障的具体操作：
 * - 编译器屏障
 * - CPU内存屏障
 * - 不同类型的屏障
 */
template<typename MemBarrier>
concept MemoryBarrierOps = KernelSafe<MemBarrier> &&
                           requires(MemBarrier mb) {
                               mb.compiler_barrier();
                               mb.memory_barrier();
                               mb.read_barrier();
                               mb.write_barrier();
                               mb.acquire_barrier();
                               mb.release_barrier();
                           };

/**
 * @brief Per-CPU变量概念
 *
 * Per-CPU数据的要求：
 * - 每个CPU有独立的副本
 * - 支持当前CPU访问
 * - 支持跨CPU访问（谨慎使用）
 */
template<typename PerCpuVar>
concept PerCpuVariable = KernelSafe<PerCpuVar> &&
                         requires(PerCpuVar var, u32 cpu_id) {
                             var.get();
                             var.get_cpu(cpu_id);
                             var.set(typename PerCpuVar::value_type{});
                         };

} // namespace moss::concepts