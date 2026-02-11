#pragma once

/**
 * @file moss_std.hpp
 * @brief 裸机环境标准库替代定义 - 完全自实现
 * 为Moss混合内核提供基础类型和工具，不使用任何标准库头文件
 */

// ============================================================================
// 基础类型定义 (完全自定义，不使用任何标准库)
// ============================================================================

// 基础整数类型定义
using size_t = unsigned long;
using ptrdiff_t = long;
using intptr_t = long;
using uintptr_t = unsigned long;

// nullptr 类型
using nullptr_t = decltype(nullptr);

// 最大对齐类型
using max_align_t = long double;

// 内核专用类型别名
using u8  = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;

using i8  = signed char;
using i16 = signed short;
using i32 = signed int;
using i64 = signed long long;

using f32 = float;
using f64 = double;

// usize 和 isize 在 types.hpp 中根据架构定义
// using usize = size_t;
// using isize = ptrdiff_t;

// 物理和虚拟地址类型
using PhysAddr = u64;
using VirtAddr = u64;
using PageFrame = u64;

// 常用常量定义
static constexpr u8  UINT8_MAX  = 255u;
static constexpr u16 UINT16_MAX = 65535u;
static constexpr u32 UINT32_MAX = 4294967295u;
static constexpr u64 UINT64_MAX = 18446744073709551615ull;

// ============================================================================
// 自实现的标准库功能
// ============================================================================

namespace moss {

// ============================================================================
// 自实现的 type_traits
// ============================================================================

// remove_reference
template<typename T> struct remove_reference { using type = T; };
template<typename T> struct remove_reference<T&> { using type = T; };
template<typename T> struct remove_reference<T&&> { using type = T; };
template<typename T> using remove_reference_t = typename remove_reference<T>::type;

// is_same
template<typename T, typename U> struct is_same { static constexpr bool value = false; };
template<typename T> struct is_same<T, T> { static constexpr bool value = true; };
template<typename T, typename U> constexpr bool is_same_v = is_same<T, U>::value;

// is_array
template<typename T> struct is_array { static constexpr bool value = false; };
template<typename T> struct is_array<T[]> { static constexpr bool value = true; };
template<typename T, size_t N> struct is_array<T[N]> { static constexpr bool value = true; };
template<typename T> constexpr bool is_array_v = is_array<T>::value;

// is_void
template<typename T> struct is_void { static constexpr bool value = false; };
template<> struct is_void<void> { static constexpr bool value = true; };
template<> struct is_void<const void> { static constexpr bool value = true; };
template<> struct is_void<volatile void> { static constexpr bool value = true; };
template<> struct is_void<const volatile void> { static constexpr bool value = true; };
template<typename T> constexpr bool is_void_v = is_void<T>::value;

// is_function (简化版本)
template<typename T> struct is_function { static constexpr bool value = false; };
template<typename T> constexpr bool is_function_v = is_function<T>::value;

// 简化的 nothrow 特性（在freestanding环境中假设所有操作都是nothrow）
template<typename T> struct is_nothrow_copy_constructible { static constexpr bool value = true; };
template<typename T> struct is_nothrow_move_constructible { static constexpr bool value = true; };
template<typename T> struct is_nothrow_copy_assignable { static constexpr bool value = true; };
template<typename T> struct is_nothrow_move_assignable { static constexpr bool value = true; };
template<typename T, typename... Args> struct is_nothrow_constructible { static constexpr bool value = true; };
template<typename T, typename U> struct is_nothrow_assignable { static constexpr bool value = true; };

template<typename T> constexpr bool is_nothrow_copy_constructible_v = is_nothrow_copy_constructible<T>::value;
template<typename T> constexpr bool is_nothrow_move_constructible_v = is_nothrow_move_constructible<T>::value;
template<typename T> constexpr bool is_nothrow_copy_assignable_v = is_nothrow_copy_assignable<T>::value;
template<typename T> constexpr bool is_nothrow_move_assignable_v = is_nothrow_move_assignable<T>::value;
template<typename T, typename... Args> constexpr bool is_nothrow_constructible_v = is_nothrow_constructible<T, Args...>::value;
template<typename T, typename U> constexpr bool is_nothrow_assignable_v = is_nothrow_assignable<T, U>::value;

// ============================================================================
// 自实现的 utility 函数
// ============================================================================

// move实现
template<typename T>
constexpr remove_reference_t<T>&& move(T&& t) noexcept {
    return static_cast<remove_reference_t<T>&&>(t);
}

// forward实现
template<typename T>
constexpr T&& forward(remove_reference_t<T>& t) noexcept {
    return static_cast<T&&>(t);
}

template<typename T>
constexpr T&& forward(remove_reference_t<T>&& t) noexcept {
    return static_cast<T&&>(t);
}

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
        for (size_t i = 0; i < num; ++i) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = num; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

inline int memcmp(const void* s1, const void* s2, size_t num) {
    const unsigned char* p1 = static_cast<const unsigned char*>(s1);
    const unsigned char* p2 = static_cast<const unsigned char*>(s2);
    for (size_t i = 0; i < num; ++i) {
        if (p1[i] < p2[i]) return -1;
        if (p1[i] > p2[i]) return 1;
    }
    return 0;
}

// ============================================================================
// 原子内存顺序枚举
// ============================================================================

enum class MemoryOrder {
    Relaxed = 0,
    Consume = 1,
    Acquire = 2,
    Release = 3,
    AcqRel = 4,
    SeqCst = 5
};

// 兼容std::memory_order的常量
constexpr int memory_order_relaxed = static_cast<int>(MemoryOrder::Relaxed);
constexpr int memory_order_consume = static_cast<int>(MemoryOrder::Consume);
constexpr int memory_order_acquire = static_cast<int>(MemoryOrder::Acquire);
constexpr int memory_order_release = static_cast<int>(MemoryOrder::Release);
constexpr int memory_order_acq_rel = static_cast<int>(MemoryOrder::AcqRel);
constexpr int memory_order_seq_cst = static_cast<int>(MemoryOrder::SeqCst);

// 简化的原子内存屏障函数（在freestanding环境中为空实现）
inline void atomic_thread_fence(int /*order*/) noexcept {
    // 在实际的内核中，这里应该插入适当的内存屏障指令
    // 对于ARM64，这可能是 dmb sy 或类似指令
    asm volatile("dmb sy" ::: "memory");
}

} // namespace moss

// ============================================================================
// 全局操作符 new/delete (placement new)
// ============================================================================

// Placement new
inline void* operator new(size_t, void* ptr) noexcept {
    return ptr;
}

inline void* operator new[](size_t, void* ptr) noexcept {
    return ptr;
}

inline void operator delete(void*, void*) noexcept {}
inline void operator delete[](void*, void*) noexcept {}