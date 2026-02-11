#pragma once

// MOSS 混合内核的统一标准库支持
// 包含所有必要的标准库功能，避免重定义问题

// 防止系统头文件的包含
#ifndef MOSS_KERNEL_STD_H
#define MOSS_KERNEL_STD_H

// 基础常量定义（避免包含系统头文件）
#ifndef UINT32_MAX
#define UINT32_MAX (4294967295U)
#endif

#ifndef UINT64_MAX
#define UINT64_MAX (18446744073709551615ULL)
#endif

namespace std {

/// integral_constant
template <typename _Tp, _Tp __v> struct integral_constant {
  static constexpr _Tp value = __v;
  typedef _Tp value_type;
  typedef integral_constant<_Tp, __v> type;
  constexpr operator value_type() const noexcept { return value; }
  constexpr value_type operator()() const noexcept { return value; }
};

template <typename _Tp, _Tp __v>
constexpr _Tp integral_constant<_Tp, __v>::value;

/// The type used as a compile-time boolean with true value.
typedef integral_constant<bool, true> true_type;

/// The type used as a compile-time boolean with false value.
typedef integral_constant<bool, false> false_type;

template <bool __v> using bool_constant = integral_constant<bool, __v>;

/// remove_reference
template <typename _Tp> struct remove_reference {
  typedef _Tp type;
};

template <typename _Tp> struct remove_reference<_Tp &> {
  typedef _Tp type;
};

template <typename _Tp> struct remove_reference<_Tp &&> {
  typedef _Tp type;
};

/// Alias template for remove_reference
template <typename _Tp>
using remove_reference_t = typename remove_reference<_Tp>::type;

/// declval - 用于 noexcept 表达式中，只声明不定义
template <typename _Tp>
typename remove_reference<_Tp>::type &&declval() noexcept;

// 简化的 nothrow 类型特征 - 在内核环境中我们假设大多数操作是 nothrow 的
template <typename _Tp, typename... _Args>
struct is_nothrow_constructible : true_type {};

template <typename _Tp> struct is_nothrow_default_constructible : true_type {};

template <typename _Tp> struct is_nothrow_copy_constructible : true_type {};

template <typename _Tp> struct is_nothrow_move_constructible : true_type {};

template <typename _Tp> struct is_nothrow_copy_assignable : true_type {};

template <typename _Tp> struct is_nothrow_move_assignable : true_type {};

// 变量模板版本（C++17 风格）
template <typename _Tp, typename... _Args>
inline constexpr bool is_nothrow_constructible_v =
    is_nothrow_constructible<_Tp, _Args...>::value;

template <typename _Tp>
inline constexpr bool is_nothrow_default_constructible_v =
    is_nothrow_default_constructible<_Tp>::value;

template <typename _Tp>
inline constexpr bool is_nothrow_copy_constructible_v =
    is_nothrow_copy_constructible<_Tp>::value;

template <typename _Tp>
inline constexpr bool is_nothrow_move_constructible_v =
    is_nothrow_move_constructible<_Tp>::value;

template <typename _Tp>
inline constexpr bool is_nothrow_copy_assignable_v =
    is_nothrow_copy_assignable<_Tp>::value;

template <typename _Tp>
inline constexpr bool is_nothrow_move_assignable_v =
    is_nothrow_move_assignable<_Tp>::value;

// 用于 static_assert 的 is_lvalue_reference（简化版本）
template <typename _Tp> struct is_lvalue_reference : false_type {};

template <typename _Tp> struct is_lvalue_reference<_Tp &> : true_type {};

template <typename _Tp>
inline constexpr bool is_lvalue_reference_v = is_lvalue_reference<_Tp>::value;

/**
 *  @brief  Forward an lvalue.
 *  @return The parameter cast to the specified type.
 *
 *  This function is used to implement "perfect forwarding".
 */
template <typename _Tp>
constexpr _Tp &&
forward(typename std::remove_reference<_Tp>::type &__t) noexcept {
  return static_cast<_Tp &&>(__t);
}

/**
 *  @brief  Forward an rvalue.
 *  @return The parameter cast to the specified type.
 *
 *  This function is used to implement "perfect forwarding".
 */
template <typename _Tp>
constexpr _Tp &&
forward(typename std::remove_reference<_Tp>::type &&__t) noexcept {
  // 在内核环境中，我们简化这个检查
  return static_cast<_Tp &&>(__t);
}

/**
 *  @brief  Convert a value to an rvalue.
 *  @param  __t  A thing of arbitrary type.
 *  @return The parameter cast to an rvalue-reference to allow moving it.
 */
template <typename _Tp>
constexpr typename std::remove_reference<_Tp>::type &&move(_Tp &&__t) noexcept {
  return static_cast<typename std::remove_reference<_Tp>::type &&>(__t);
}

} // namespace std

#endif // MOSS_KERNEL_STD_H