#pragma once

/**
 * @file test_concepts.hpp
 * @brief 独立的C++23 concepts测试（不依赖其他头文件）
 * @author MOSS Kernel Team
 * @version C++23
 *
 * 验证C++23 concepts功能的最小实现
 */

// 基础类型定义
using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using usize = unsigned long;

namespace moss::concepts::test {

/**
 * @brief 基础concepts定义
 */
template<typename T>
concept SmallType = sizeof(T) <= 8;

template<typename T>
concept IntegerLike = requires(T t) {
    t + t;
    t - t;
    t * t;
};

template<auto N>
concept PowerOfTwo = (N > 0) && ((N & (N - 1)) == 0);

template<usize Capacity>
concept ValidCapacity = PowerOfTwo<Capacity> && (Capacity <= 1024);

/**
 * @brief 使用concepts的模板类
 */
template<SmallType T, usize Capacity>
    requires ValidCapacity<Capacity>
class SimpleQueue {
private:
    T data_[Capacity];
    usize head_ = 0;
    usize tail_ = 0;

public:
    bool push(const T& item) {
        usize next_tail = (tail_ + 1) % Capacity;
        if (next_tail == head_) return false;

        data_[tail_] = item;
        tail_ = next_tail;
        return true;
    }

    bool pop(T& item) {
        if (head_ == tail_) return false;

        item = data_[head_];
        head_ = (head_ + 1) % Capacity;
        return true;
    }

    static constexpr usize capacity() { return Capacity; }
};

/**
 * @brief 编译期测试
 */
namespace compile_tests {
    // 这些应该编译通过
    static_assert(SmallType<u32>);
    static_assert(SmallType<u64>);
    static_assert(IntegerLike<u32>);
    static_assert(PowerOfTwo<1>);
    static_assert(PowerOfTwo<256>);
    static_assert(ValidCapacity<64>);

    // 这些应该编译失败
    // static_assert(PowerOfTwo<3>);     // 不是2的幂
    // static_assert(ValidCapacity<3>);  // 不是2的幂

    // 使用concepts的实例
    using TestQueue = SimpleQueue<u32, 64>;
    // using BadQueue = SimpleQueue<u32, 3>;  // 会编译失败
}

/**
 * @brief 函数concepts示例
 */
template<IntegerLike T>
constexpr T add(T a, T b) {
    return a + b;
}

template<SmallType T>
constexpr bool fits_in_cache_line(usize count) {
    return (sizeof(T) * count) <= 64;
}

} // namespace moss::concepts::test