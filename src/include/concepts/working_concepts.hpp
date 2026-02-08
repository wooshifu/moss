#pragma once

/**
 * @file working_concepts.hpp
 * @brief 实际可编译的MOSS内核concepts
 * @author MOSS Kernel Team
 * @version C++23
 *
 * 这个版本避开了所有编译问题，提供实用的concepts定义
 */

namespace moss::concepts {

// 基础类型定义（避免依赖问题）
using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using usize = unsigned long;

// 常量定义
constexpr usize PAGE_SIZE = 4096;
constexpr usize CACHE_LINE_SIZE = 64;

/**
 * @brief 实用的内核concepts
 */

// 基于大小的类型检查
template<typename T>
concept SmallType = sizeof(T) <= 64;

template<typename T>
concept AtomicSize = (sizeof(T) == 1 || sizeof(T) == 2 ||
                      sizeof(T) == 4 || sizeof(T) == 8);

// 对齐检查
template<typename T>
concept CacheAligned = alignof(T) >= CACHE_LINE_SIZE;

template<typename T>
concept PageAligned = alignof(T) >= PAGE_SIZE;

// 数学概念
template<auto N>
concept PowerOfTwo = (N > 0) && ((N & (N - 1)) == 0);

template<usize Cap>
concept ValidCapacity = PowerOfTwo<Cap> && (Cap <= 65536);

// 组合概念
template<typename T>
concept KernelSafeType = SmallType<T> && AtomicSize<T>;

template<typename T>
concept QueueElement = KernelSafeType<T>;

/**
 * @brief 使用concepts的实际队列实现
 */
template<QueueElement T, usize Capacity>
    requires ValidCapacity<Capacity>
class KernelQueue {
private:
    T buffer_[Capacity];
    usize head_{0};
    usize tail_{0};

public:
    constexpr KernelQueue() = default;

    bool enqueue(const T& item) noexcept {
        usize next = (tail_ + 1) % Capacity;
        if (next == head_) return false;

        buffer_[tail_] = item;
        tail_ = next;
        return true;
    }

    bool dequeue(T& item) noexcept {
        if (head_ == tail_) return false;

        item = buffer_[head_];
        head_ = (head_ + 1) % Capacity;
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_ == tail_;
    }

    [[nodiscard]] static constexpr usize capacity() noexcept {
        return Capacity;
    }
};

/**
 * @brief 智能指针concepts（简化版）
 */
template<typename T>
concept PtrCompatible = sizeof(T) > 0; // 简单的存在性检查

template<PtrCompatible T>
class SimpleUniquePtr {
private:
    T* ptr_;

public:
    explicit SimpleUniquePtr(T* p = nullptr) noexcept : ptr_(p) {}
    ~SimpleUniquePtr() { delete ptr_; }

    // 移动语义
    SimpleUniquePtr(SimpleUniquePtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    SimpleUniquePtr& operator=(SimpleUniquePtr&& other) noexcept {
        if (this != &other) {
            delete ptr_;
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    // 禁用复制
    SimpleUniquePtr(const SimpleUniquePtr&) = delete;
    SimpleUniquePtr& operator=(const SimpleUniquePtr&) = delete;

    T* get() const noexcept { return ptr_; }
    T& operator*() const { return *ptr_; }
    T* operator->() const noexcept { return ptr_; }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }
};

/**
 * @brief 编译期验证
 */
namespace tests {
    static_assert(SmallType<u32>);
    static_assert(AtomicSize<u64>);
    static_assert(PowerOfTwo<256>);
    static_assert(ValidCapacity<1024>);
    static_assert(KernelSafeType<u32>);
    static_assert(QueueElement<u32>);

    // 这些应该失败（注释掉避免编译错误）
    // static_assert(PowerOfTwo<3>);
    // static_assert(ValidCapacity<3>);
}

} // namespace moss::concepts