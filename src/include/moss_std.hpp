#pragma once

/**
 * @file moss_std.hpp
 * @brief 裸机环境标准库替代定义
 * 为Moss微内核提供基础类型和工具，替代标准库功能
 */

// ============================================================================
// 基础类型定义 (替代 cstddef)
// ============================================================================

// 基础整数类型
using size_t = unsigned long;
using ptrdiff_t = long;
using intptr_t = long;
using uintptr_t = unsigned long;

// nullptr 类型 (C++11)
using nullptr_t = decltype(nullptr);

// 最大对齐类型
using max_align_t = long double;

// ============================================================================
// 类型特征简化实现 (替代 type_traits)
// ============================================================================

namespace moss {

// 基础类型特征
template<typename T, T v>
struct integral_constant {
    static constexpr T value = v;
    using value_type = T;
    using type = integral_constant;
};

using true_type = integral_constant<bool, true>;
using false_type = integral_constant<bool, false>;

// 简化的 remove_reference
template<typename T> struct remove_reference { using type = T; };
template<typename T> struct remove_reference<T&> { using type = T; };
template<typename T> struct remove_reference<T&&> { using type = T; };
template<typename T> using remove_reference_t = typename remove_reference<T>::type;

// move 工具函数
template<typename T>
constexpr remove_reference_t<T>&& move(T&& t) noexcept {
    return static_cast<remove_reference_t<T>&&>(t);
}

// forward 工具函数
template<typename T>
constexpr T&& forward(remove_reference_t<T>& t) noexcept {
    return static_cast<T&&>(t);
}

template<typename T>
constexpr T&& forward(remove_reference_t<T>&& t) noexcept {
    static_assert(!__is_lvalue_reference(T), "Invalid forward of lvalue as rvalue");
    return static_cast<T&&>(t);
}

// 简化的 is_same
template<typename T, typename U>
struct is_same : false_type {};

template<typename T>
struct is_same<T, T> : true_type {};

template<typename T, typename U>
inline constexpr bool is_same_v = is_same<T, U>::value;

// 简化的 enable_if
template<bool B, typename T = void>
struct enable_if {};

template<typename T>
struct enable_if<true, T> { using type = T; };

template<bool B, typename T = void>
using enable_if_t = typename enable_if<B, T>::type;

// 简化的构造函数特征 (在裸机环境中假设是安全的)
template<typename T>
inline constexpr bool is_nothrow_copy_constructible_v = true;

template<typename T>
inline constexpr bool is_nothrow_move_constructible_v = true;

template<typename T>
inline constexpr bool is_nothrow_copy_assignable_v = true;

template<typename T>
inline constexpr bool is_nothrow_move_assignable_v = true;

template<typename T, typename... Args>
inline constexpr bool is_nothrow_constructible_v = true;

// ============================================================================
// 内存操作函数 (替代 cstring)
// ============================================================================

inline void* memset(void* ptr, int value, size_t num) {
    unsigned char* p = static_cast<unsigned char*>(ptr);
    for (size_t i = 0; i < num; ++i) {
        p[i] = static_cast<unsigned char>(value);
    }
    return ptr;
}

inline void* memcpy(void* dest, const void* src, size_t num) {
    unsigned char* d = static_cast<unsigned char*>(dest);
    const unsigned char* s = static_cast<const unsigned char*>(src);
    for (size_t i = 0; i < num; ++i) {
        d[i] = s[i];
    }
    return dest;
}

inline void* memmove(void* dest, const void* src, size_t num) {
    unsigned char* d = static_cast<unsigned char*>(dest);
    const unsigned char* s = static_cast<const unsigned char*>(src);

    if (d < s) {
        // 向前复制
        for (size_t i = 0; i < num; ++i) {
            d[i] = s[i];
        }
    } else if (d > s) {
        // 向后复制
        for (size_t i = num; i > 0; --i) {
            d[i-1] = s[i-1];
        }
    }
    return dest;
}

inline int memcmp(const void* ptr1, const void* ptr2, size_t num) {
    const unsigned char* p1 = static_cast<const unsigned char*>(ptr1);
    const unsigned char* p2 = static_cast<const unsigned char*>(ptr2);

    for (size_t i = 0; i < num; ++i) {
        if (p1[i] < p2[i]) return -1;
        if (p1[i] > p2[i]) return 1;
    }
    return 0;
}

// ============================================================================
// 常量定义 (替代 climits, cstdint)
// ============================================================================

// 整数类型的最大值
#define UINT8_MAX 255u
#define UINT16_MAX 65535u
#define UINT32_MAX 4294967295u
#define UINT64_MAX 18446744073709551615ull

#define INT8_MAX 127
#define INT16_MAX 32767
#define INT32_MAX 2147483647
#define INT64_MAX 9223372036854775807ll

#define INT8_MIN (-128)
#define INT16_MIN (-32768)
#define INT32_MIN (-2147483648)
#define INT64_MIN (-9223372036854775808ll)

// ============================================================================
// 原子操作支持 (替代 atomic)
// ============================================================================

// 内存顺序枚举
enum class memory_order {
    relaxed,
    acquire,
    release,
    acq_rel,
    seq_cst
};

// 简化的原子操作函数
inline void atomic_thread_fence(memory_order order = memory_order::seq_cst) {
    switch (order) {
        case memory_order::relaxed:
            break;
        case memory_order::acquire:
        case memory_order::release:
        case memory_order::acq_rel:
        case memory_order::seq_cst:
        default:
            __asm__ __volatile__("dmb sy" : : : "memory");
            break;
    }
}

// 内存顺序别名
static constexpr memory_order memory_order_relaxed = memory_order::relaxed;
static constexpr memory_order memory_order_acquire = memory_order::acquire;
static constexpr memory_order memory_order_release = memory_order::release;
static constexpr memory_order memory_order_acq_rel = memory_order::acq_rel;
static constexpr memory_order memory_order_seq_cst = memory_order::seq_cst;

// ============================================================================
// 断言支持 (替代 cassert)
// ============================================================================

// MOSS_ASSERT宏在config/config.h中定义，此处不再重复定义

} // namespace moss

// ============================================================================
// 简化的新删除操作符 (无堆分配) - 必须在全局命名空间
// ============================================================================

// placement new支持
inline void* operator new(size_t, void* ptr) noexcept { return ptr; }
inline void* operator new[](size_t, void* ptr) noexcept { return ptr; }
inline void operator delete(void*, void*) noexcept {}
inline void operator delete[](void*, void*) noexcept {}

// ============================================================================
// 全局命名空间别名，方便使用
// ============================================================================

// 将moss命名空间的内容引入全局空间，用于兼容
using moss::move;
using moss::forward;
using moss::true_type;
using moss::false_type;
using moss::is_same_v;
using moss::enable_if_t;
using moss::remove_reference_t;

// 内存函数
using moss::memset;
using moss::memcpy;
using moss::memmove;
using moss::memcmp;

// 原子操作
using moss::atomic_thread_fence;
using moss::memory_order;
using moss::memory_order_relaxed;
using moss::memory_order_acquire;
using moss::memory_order_release;
using moss::memory_order_acq_rel;
using moss::memory_order_seq_cst;