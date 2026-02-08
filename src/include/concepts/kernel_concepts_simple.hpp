#pragma once

/**
 * @file kernel_concepts_simple.hpp
 * @brief MOSS 微内核 C++23 concepts 定义（能编译版本）
 * @author MOSS Kernel Team
 * @version C++23
 *
 * 提供内核安全、高性能的类型约束和接口定义。
 * 使用kernel_std.hpp中实际存在的类型特征。
 */

#include "../types.hpp"

namespace moss::concepts {

using namespace moss::kernel;

/**
 * @brief 内核安全类型概念（简化版）
 *
 * 基于大小检查，避免std依赖
 */
template<typename T>
concept KernelSafe = sizeof(T) > 0;

/**
 * @brief 原子操作兼容概念
 *
 * 基于大小检查，避免使用不存在的type traits
 */
template<typename T>
concept AtomicCompatible = (sizeof(T) == 1 || sizeof(T) == 2 ||
                            sizeof(T) == 4 || sizeof(T) == 8);

/**
 * @brief 页面对齐概念
 */
template<typename T>
concept PageAligned = alignof(T) >= PAGE_SIZE;

/**
 * @brief 缓存行对齐概念
 */
template<typename T>
concept CacheAligned = alignof(T) >= CACHE_LINE_SIZE;

/**
 * @brief 整数类型概念（简化版）
 */
template<typename T>
concept IntegerType = requires {
    sizeof(T) >= sizeof(u8) && sizeof(T) <= sizeof(u64);
};

/**
 * @brief 2的幂概念
 */
template<auto N>
concept PowerOfTwo = (N > 0) && ((N & (N - 1)) == 0);

/**
 * @brief 有效容量概念
 */
template<usize Capacity>
concept ValidCapacity = PowerOfTwo<Capacity>;

/**
 * @brief 大小限制概念
 */
template<typename T, usize MaxSize>
concept SizeLimit = sizeof(T) <= MaxSize;

/**
 * @brief 无锁元素概念（简化版）
 */
template<typename T>
concept LockFreeElement = KernelSafe<T> &&
                          AtomicCompatible<T> &&
                          SizeLimit<T, 64>;

} // namespace moss::concepts