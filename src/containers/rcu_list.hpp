#pragma once

// RCU (Read-Copy-Update) 保护的数据结构
// 实现高性能的并发读取，延迟释放机制

#include "atomic_types.hpp"
#include "../include/types.hpp"
#include "../include/result.hpp"

// 包含统一的内核标准库支持
// Removed kernel_std.hpp include to avoid conflicts

namespace moss::kernel::containers {

// RCU读取侧临界区保护
class RcuReadLock {
private:
    static thread_local u32 read_depth_;

public:
    RcuReadLock() noexcept {
        enter_read_side();
    }

    ~RcuReadLock() noexcept {
        exit_read_side();
    }

    // 禁用拷贝和移动
    NON_COPYABLE_NON_MOVABLE(RcuReadLock)

private:
    static void enter_read_side() noexcept {
        ++read_depth_;
        // 内存屏障确保读取操作不会重排到锁之前
        read_barrier();
    }

    static void exit_read_side() noexcept {
        // 内存屏障确保读取操作不会重排到锁之后
        read_barrier();
        --read_depth_;
    }

public:
    static bool in_read_side() noexcept {
        return read_depth_ > 0;
    }
};

thread_local u32 RcuReadLock::read_depth_ = 0;

// RCU保护的指针
template<typename T>
class RcuPtr {
private:
    AtomicPtr<T> ptr_;

public:
    constexpr RcuPtr() noexcept : ptr_(nullptr) {}
    constexpr RcuPtr(T* p) noexcept : ptr_(p) {}

    // 禁用拷贝，允许移动
    RcuPtr(const RcuPtr&) = delete;
    RcuPtr& operator=(const RcuPtr&) = delete;

    RcuPtr(RcuPtr&& other) noexcept : ptr_(other.ptr_.exchange(nullptr)) {}
    RcuPtr& operator=(RcuPtr&& other) noexcept {
        if (this != &other) {
            T* old_ptr = ptr_.exchange(other.ptr_.exchange(nullptr));
            schedule_rcu_callback([old_ptr]() {
                delete old_ptr;
            });
        }
        return *this;
    }

    // RCU保护的读取
    [[nodiscard]] T* load_rcu() const noexcept {
        // 在调试模式下检查RCU读取侧临界区
        #ifndef NDEBUG
        if (!RcuReadLock::in_read_side()) {
            // 在实际内核中，这里应该触发panic或警告
            #if defined(MOSS_ARCH_ARM64)
                asm volatile("brk #1");  // ARM64断点指令
            #elif defined(MOSS_ARCH_X86_64)
                asm volatile("int3");    // x86_64断点指令
            #elif defined(MOSS_ARCH_RISCV)
                asm volatile("ebreak");  // RISC-V断点指令
            #else
                // 通用断点 - 进入无限循环
                while(1) {}
            #endif
        }
        #endif
        return ptr_.load(MemoryOrder::Consume);
    }

    // RCU更新（只能在写入侧调用）
    void store_rcu(T* new_ptr) noexcept {
        T* old_ptr = ptr_.exchange(new_ptr, MemoryOrder::Release);
        if (old_ptr != nullptr) {
            schedule_rcu_callback([old_ptr]() {
                delete old_ptr;
            });
        }
    }

    // CAS更新
    [[nodiscard]] bool compare_exchange_rcu(T*& expected, T* desired) noexcept {
        if (ptr_.compare_exchange_weak(expected, desired, MemoryOrder::Release, MemoryOrder::Consume)) {
            if (expected != nullptr) {
                schedule_rcu_callback([expected]() {
                    delete expected;
                });
            }
            return true;
        }
        return false;
    }

    // 交换并返回旧值（用于clear()等操作）
    [[nodiscard]] T* exchange(T* new_ptr, MemoryOrder order = MemoryOrder::AcqRel) noexcept {
        return ptr_.exchange(new_ptr, order);
    }

    // 原子加载（用于next指针等）
    [[nodiscard]] T* load(MemoryOrder order = MemoryOrder::Acquire) const noexcept {
        return ptr_.load(order);
    }

private:
    // RCU回调调度（简化实现）
    template<typename Func>
    static void schedule_rcu_callback(Func&& callback) noexcept {
        // 在实际实现中，这里会加入到RCU回调队列中
        // 在安静期（quiescent state）结束后执行
        // 目前简化为立即执行（不安全，仅用于原型）
        callback();
    }
};

// RCU保护的链表节点
template<typename T>
struct RcuListNode {
    RcuPtr<RcuListNode<T>> next;
    T data;

    template<typename... Args>
#if MOSS_HAS_STD_UTILITY
    constexpr RcuListNode(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
        : next(nullptr), data(std::forward<Args>(args)...) {}
#else
    constexpr RcuListNode(Args&&... args) noexcept
        : next(nullptr), data(static_cast<Args&&>(args)...) {}
#endif
};

// RCU保护的链表
template<typename T>
class RcuList {
private:
    RcuPtr<RcuListNode<T>> head_;
    AtomicCounter<usize> size_;

public:
    constexpr RcuList() noexcept : head_(nullptr), size_(0) {}

    ~RcuList() noexcept {
        // 清空链表（需要确保没有读取者）
        clear();
    }

    // 禁用拷贝和移动
    NON_COPYABLE_NON_MOVABLE(RcuList)

    // 在头部插入元素（写入操作）
    template<typename... Args>
    void push_front(Args&&... args) {
#if MOSS_HAS_STD_UTILITY
        auto new_node = new RcuListNode<T>(std::forward<Args>(args)...);
#else
        auto new_node = new RcuListNode<T>(static_cast<Args&&>(args)...);
#endif

        RcuListNode<T>* old_head = head_.load(MemoryOrder::Relaxed);
        do {
            new_node->next.store_rcu(old_head);
        } while (!head_.compare_exchange_rcu(old_head, new_node));

        (void)size_.fetch_add(1, MemoryOrder::Relaxed);
    }

    // 删除指定值的元素（写入操作）
    bool remove(const T& value) {
        RcuListNode<T>* prev = nullptr;
        RcuListNode<T>* current = head_.load_rcu();

        while (current != nullptr) {
            if (current->data == value) {
                RcuListNode<T>* next = current->next.load_rcu();

                if (prev == nullptr) {
                    // 删除头节点
                    if (head_.compare_exchange_rcu(current, next)) {
                        (void)size_.fetch_sub(1, MemoryOrder::Relaxed);
                        return true;
                    }
                } else {
                    // 删除中间节点
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

    // RCU保护的查找操作（读取操作）
    template<typename Predicate>
    [[nodiscard]] const T* find_if(Predicate pred) const {
        RcuReadLock read_lock;  // 进入RCU读取侧临界区

        RcuListNode<T>* current = head_.load_rcu();
        while (current != nullptr) {
            if (pred(current->data)) {
                return &current->data;
            }
            current = current->next.load_rcu();
        }

        return nullptr;
    }

    // 查找特定值
    [[nodiscard]] const T* find(const T& value) const {
        return find_if([&value](const T& item) { return item == value; });
    }

    // RCU保护的遍历操作
    template<typename Func>
    void for_each(Func func) const {
        RcuReadLock read_lock;  // 进入RCU读取侧临界区

        RcuListNode<T>* current = head_.load_rcu();
        while (current != nullptr) {
            func(current->data);
            current = current->next.load_rcu();
        }
    }

    // 获取大小（近似值）
    [[nodiscard]] usize size() const noexcept {
        return size_.load(MemoryOrder::Relaxed);
    }

    // 检查是否为空
    [[nodiscard]] bool empty() const noexcept {
        RcuReadLock read_lock;
        return head_.load_rcu() == nullptr;
    }

    // 清空链表
    void clear() {
        RcuListNode<T>* current = head_.exchange(nullptr, MemoryOrder::AcqRel);
        size_.store(0, MemoryOrder::Relaxed);

        // 使用RCU机制延迟释放所有节点
        while (current != nullptr) {
            RcuListNode<T>* next = current->next.load(MemoryOrder::Relaxed);
            schedule_rcu_callback([current]() {
                delete current;
            });
            current = next;
        }
    }

private:
    // RCU回调调度
    template<typename Func>
    static void schedule_rcu_callback(Func&& callback) noexcept {
        // 简化实现：立即执行
        // 实际应该加入到RCU回调队列中
        callback();
    }
};

// RCU保护的哈希表（简化实现）
template<typename Key, typename Value, usize BucketCount = 256>
class RcuHashMap {
private:
    static_assert((BucketCount & (BucketCount - 1)) == 0, "BucketCount must be power of 2");
    static constexpr usize BUCKET_MASK = BucketCount - 1;

    struct Entry {
        Key key;
        Value value;

        template<typename K, typename V>
#if MOSS_HAS_STD_UTILITY
        Entry(K&& k, V&& v) noexcept(std::is_nothrow_constructible_v<Key, K&&> &&
                                     std::is_nothrow_constructible_v<Value, V&&>)
            : key(std::forward<K>(k)), value(std::forward<V>(v)) {}
#else
        Entry(K&& k, V&& v) noexcept
            : key(static_cast<K&&>(k)), value(static_cast<V&&>(v)) {}
#endif

        bool operator==(const Entry& other) const noexcept {
            return key == other.key;
        }
    };

    RcuList<Entry> buckets_[BucketCount];
    AtomicCounter<usize> size_;

public:
    constexpr RcuHashMap() noexcept : size_(0) {}

    // 插入或更新键值对
    template<typename K, typename V>
    void insert_or_update(K&& key, V&& value) {
        usize bucket_idx = hash_key(key) & BUCKET_MASK;

        // 尝试更新现有条目
        {
            RcuReadLock read_lock;
            const Entry* existing = buckets_[bucket_idx].find_if(
                [&key](const Entry& entry) { return entry.key == key; });

            if (existing != nullptr) {
                // 找到现有条目，需要更新
                buckets_[bucket_idx].remove(*existing);
            }
        }

        // 插入新条目
#if MOSS_HAS_STD_UTILITY
        buckets_[bucket_idx].push_front(std::forward<K>(key), std::forward<V>(value));
#else
        buckets_[bucket_idx].push_front(static_cast<K&&>(key), static_cast<V&&>(value));
#endif
        (void)size_.fetch_add(1, MemoryOrder::Relaxed);
    }

    // 查找值
    template<typename K>
    [[nodiscard]] const Value* find(const K& key) const {
        usize bucket_idx = hash_key(key) & BUCKET_MASK;

        const Entry* entry = buckets_[bucket_idx].find_if(
            [&key](const Entry& e) { return e.key == key; });

        return entry ? &entry->value : nullptr;
    }

    // 删除键
    template<typename K>
    bool remove(const K& key) {
        usize bucket_idx = hash_key(key) & BUCKET_MASK;

        bool removed = buckets_[bucket_idx].remove(Entry{key, Value{}});
        if (removed) {
            (void)size_.fetch_sub(1, MemoryOrder::Relaxed);
        }
        return removed;
    }

    // 获取大小
    [[nodiscard]] usize size() const noexcept {
        return size_.load(MemoryOrder::Relaxed);
    }

    // 检查是否为空
    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    // 遍历所有键值对
    template<typename Func>
    void for_each(Func&& func) const {
        for (usize i = 0; i < BucketCount; ++i) {
            buckets_[i].for_each([&func](const Entry& entry) {
                struct KeyValue {
                    const Key& key;
                    const Value& value;
                };
                func(KeyValue{entry.key, entry.value});
            });
        }
    }

private:
    // 简单的哈希函数
    template<typename K>
    [[nodiscard]] static usize hash_key(const K& key) noexcept {
        // 使用FNV-1a哈希算法的简化版本
        usize hash = 2166136261u;
        const u8* data = reinterpret_cast<const u8*>(&key);
        for (usize i = 0; i < sizeof(K); ++i) {
            hash ^= data[i];
            hash *= 16777619u;
        }
        return hash;
    }
};

// 类型别名
using ProcessList = RcuList<ProcessId>;
using DeviceRegistry = RcuHashMap<DeviceId, VirtAddr>;

} // namespace moss::kernel::containers