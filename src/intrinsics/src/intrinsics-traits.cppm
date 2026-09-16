// -*- C++ -*- (Tell editors this is C++26 module code)
//
// Copyright 2025 Mercedes-Benz Tech Innovation GmbH
// SPDX-License-Identifier: MIT
//
// MOSS Kernel - Intrinsics Module: Type Traits
//
// This partition provides compile-time type introspection using Clang intrinsics.
// Efficient alternative to standard library type traits (no template instantiation overhead).

export module moss.intrinsics:traits;

/// Type traits using Clang intrinsics.
/// These compile to direct compiler queries with zero runtime cost.
export namespace moss::intrinsics::traits {

// ============================================================================
// Type Properties
// ============================================================================

/// Check exact type identity, including cv-qualifiers.
template <typename T, typename U> inline constexpr bool is_same_v = __is_same(T, U);

/// Check if type is const-qualified.
template <typename T> inline constexpr bool is_const_v = __is_const(T);

/// Check if type is volatile-qualified.
template <typename T> inline constexpr bool is_volatile_v = __is_volatile(T);

/// Check if type is void.
template <typename T> inline constexpr bool is_void_v = __is_void(T);

/// Check if type is an integral type (int, char, bool, etc.).
template <typename T> inline constexpr bool is_integral_v = __is_integral(T);

/// Check if type is a floating point type (float, double, long double).
template <typename T> inline constexpr bool is_floating_point_v = __is_floating_point(T);

/// Check if type is an array type (T[] or T[N]).
template <typename T> inline constexpr bool is_array_v = __is_array(T);

/// Check if type is a pointer type (T*).
template <typename T> inline constexpr bool is_pointer_v = __is_pointer(T);

/// Check if type is an lvalue reference (T&).
template <typename T> inline constexpr bool is_lvalue_reference_v = __is_lvalue_reference(T);

/// Check if type is an rvalue reference (T&&).
template <typename T> inline constexpr bool is_rvalue_reference_v = __is_rvalue_reference(T);

/// Check if type is any reference (T& or T&&).
template <typename T> inline constexpr bool is_reference_v = __is_reference(T);

/// Check if type is a function type.
template <typename T> inline constexpr bool is_function_v = __is_function(T);

// ============================================================================
// Constructibility and Assignability
// ============================================================================

/// Check if type T can be constructed from Args... without throwing.
template <typename T, typename... Args>
inline constexpr bool is_nothrow_constructible_v = __is_nothrow_constructible(T, Args...);

/// Check if type T can be assigned from type U without throwing.
template <typename T, typename U> inline constexpr bool is_nothrow_assignable_v = __is_nothrow_assignable(T, U);

// ============================================================================
// Type Transformations
// ============================================================================

/// Remove const qualifier from type.
/// Example: remove_const_t<const int> -> int
template <typename T> using remove_const_t = __remove_const(T);

/// Remove volatile qualifier from type.
/// Example: remove_volatile_t<volatile int> -> int
template <typename T> using remove_volatile_t = __remove_volatile(T);

/// Remove both const and volatile qualifiers from type.
/// Example: remove_cv_t<const volatile int> -> int
template <typename T> using remove_cv_t = __remove_cv(T);

/// Remove reference from type.
/// Example: remove_reference_t<int&> -> int
template <typename T> using remove_reference_t = __remove_reference_t(T);

/// Remove pointer from type.
/// Example: remove_pointer_t<int*> -> int
template <typename T> using remove_pointer_t = __remove_pointer(T);

} // namespace moss::intrinsics::traits
