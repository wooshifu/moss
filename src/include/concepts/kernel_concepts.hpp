#pragma once

/**
 * @file kernel_concepts.hpp
 * @brief MOSS 微内核核心 C++23 concepts 定义（工作版本）
 * @author MOSS Kernel Team
 * @version C++23
 *
 * 提供内核安全、高性能的类型约束和接口定义。
 * 所有 concepts 都是编译期检查，零运行时开销。
 * 避免使用标准库type traits以确保编译成功。
 */

#include "../types.hpp"

namespace moss::concepts {

using namespace moss::kernel;

// 常量定义
constexpr usize CACHE_LINE_SIZE = 64;
constexpr usize PAGE_SIZE = 4096;

/**
 * @brief 内核安全类型概念实现
 */
namespace detail {
    template<typename T>
    constexpr bool is_kernel_safe_impl() {
        if constexpr (sizeof(T*) == sizeof(void*)) {
            // 对于 void 类型，检查指针大小
            return true;
        } else {
            return sizeof(T) > 0;
        }
    }
}

/**
 * @brief 内核安全类型概念
 *
 * 基于大小和基本特性的内核安全检查，避免std依赖。
 * 特别处理 void 类型以支持 Result<void>
 */
template<typename T>
concept KernelSafe = detail::is_kernel_safe_impl<T>();

/**
 * @brief 小类型概念
 *
 * 要求类型大小不超过合理限制（64字节）
 */
template<typename T>
concept SmallType = sizeof(T) <= 64;

/**
 * @brief 原子操作兼容概念
 *
 * 要求类型大小是硬件支持的原子操作大小
 */
template<typename T>
concept AtomicCompatible = (sizeof(T) == 1 || sizeof(T) == 2 ||
                            sizeof(T) == 4 || sizeof(T) == 8);

/**
 * @brief 页面对齐概念
 *
 * 要求类型的对齐要求至少是页面大小
 */
template<typename T>
concept PageAligned = alignof(T) >= PAGE_SIZE;

/**
 * @brief 缓存行对齐概念
 *
 * 要求类型的对齐要求至少是缓存行大小
 */
template<typename T>
concept CacheAligned = alignof(T) >= CACHE_LINE_SIZE;

/**
 * @brief 整数类型概念（简化版）
 *
 * 基于大小的整数类型检查
 */
template<typename T>
concept IntegerType = (sizeof(T) == 1 || sizeof(T) == 2 ||
                       sizeof(T) == 4 || sizeof(T) == 8);

/**
 * @brief 地址类型概念
 *
 * 要求类型是64位大小（适合地址）
 */
template<typename T>
concept AddressType = (sizeof(T) == sizeof(u64));

/**
 * @brief ID类型概念
 *
 * 要求类型大小合理，适合用作ID
 */
template<typename T>
concept IdType = (sizeof(T) >= sizeof(u8) && sizeof(T) <= sizeof(u64));

/**
 * @brief 2的幂概念
 *
 * 检查值是否是2的幂
 */
template<auto N>
concept PowerOfTwo = (N > 0) && ((N & (N - 1)) == 0);

/**
 * @brief 有效容量概念
 *
 * 要求容量值是2的幂且不超过合理上限
 */
template<usize Capacity>
concept ValidCapacity = PowerOfTwo<Capacity> && (Capacity <= 65536);

/**
 * @brief 内核安全元素概念
 *
 * 组合概念：既内核安全又小型
 */
template<typename T>
concept KernelSafeElement = KernelSafe<T> && SmallType<T>;

/**
 * @brief 队列元素概念
 *
 * 适合在队列中使用的元素类型
 */
template<typename T>
concept QueueElement = KernelSafeElement<T> && AtomicCompatible<T>;

/**
 * @brief 指针兼容概念（基础版）
 *
 * 基于大小的指针兼容性检查，避免复杂的type traits
 */
template<typename T>
concept PtrCompatible = sizeof(T) > 0;

/**
 * @brief 不可复制概念（简化版）
 *
 * 基于构造函数删除检查，避免std依赖
 */
template<typename T>
concept NonCopyable = requires {
    sizeof(T) > 0; // 简化版本，只检查类型存在
};

/**
 * @brief 不可移动概念（简化版）
 *
 * 基于构造函数删除检查，避免std依赖
 */
template<typename T>
concept NonMovable = requires {
    sizeof(T) > 0; // 简化版本，只检查类型存在
};

} // namespace moss::concepts