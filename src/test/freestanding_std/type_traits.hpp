#pragma once

/**
 * @file type_traits.hpp
 * @brief Complete type traits implementation for ut.hpp compatibility
 *
 * Self-contained type traits implementation that doesn't depend on kernel headers
 * to avoid PAGE_SIZE and other redefinition conflicts.
 */

namespace std {
    // ========================================================================
    // Basic integer types (compatible with moss::kernel types)
    // ========================================================================

    using u8 = unsigned char;
    using u16 = unsigned short;
    using u32 = unsigned int;
    using u64 = unsigned long long;
    using i8 = signed char;
    using i16 = signed short;
    using i32 = signed int;
    using i64 = signed long long;

    // Platform-specific size types
    #ifdef __LP64__
    using usize = unsigned long;
    using isize = signed long;
    #else
    using usize = unsigned long long;
    using isize = signed long long;
    #endif

    // Standard library size types
    using size_t = usize;
    using ptrdiff_t = isize;

    // ========================================================================
    // Fundamental type traits
    // ========================================================================

    // Basic true_type and false_type
    template<bool B>
    struct bool_constant {
        static constexpr bool value = B;
        using value_type = bool;
        using type = bool_constant;
        constexpr operator value_type() const noexcept { return value; }
        constexpr value_type operator()() const noexcept { return value; }
    };

    using true_type = bool_constant<true>;
    using false_type = bool_constant<false>;

    template<typename T, T v>
    struct integral_constant {
        static constexpr T value = v;
        using value_type = T;
        using type = integral_constant;
        constexpr operator value_type() const noexcept { return value; }
        constexpr value_type operator()() const noexcept { return value; }
    };

    // remove_reference
    template<typename T> struct remove_reference { using type = T; };
    template<typename T> struct remove_reference<T&> { using type = T; };
    template<typename T> struct remove_reference<T&&> { using type = T; };
    template<typename T> using remove_reference_t = typename remove_reference<T>::type;

    // add_rvalue_reference (needed for declval)
    template<typename T>
    struct add_rvalue_reference {
        using type = T&&;
    };

    template<>
    struct add_rvalue_reference<void> {
        using type = void;
    };

    template<>
    struct add_rvalue_reference<const void> {
        using type = const void;
    };

    template<>
    struct add_rvalue_reference<volatile void> {
        using type = volatile void;
    };

    template<>
    struct add_rvalue_reference<const volatile void> {
        using type = const volatile void;
    };

    template<typename T>
    using add_rvalue_reference_t = typename add_rvalue_reference<T>::type;

    // declval for SFINAE
    template<typename T>
    typename add_rvalue_reference<T>::type declval() noexcept;

    // ========================================================================
    // Type traits needed by ut.hpp
    // ========================================================================

    // is_same
    template<typename T, typename U>
    struct is_same : false_type {};

    template<typename T>
    struct is_same<T, T> : true_type {};

    template<typename T, typename U>
    inline constexpr bool is_same_v = is_same<T, U>::value;

    // is_convertible
    namespace detail {
        template<typename To>
        void test_convertible(To);

        template<typename From, typename To,
                 typename = decltype(test_convertible<To>(declval<From>()))>
        true_type test_is_convertible(int);

        template<typename From, typename To>
        false_type test_is_convertible(...);
    }

    template<typename From, typename To>
    struct is_convertible : decltype(detail::test_is_convertible<From, To>(0)) {};

    template<typename From, typename To>
    inline constexpr bool is_convertible_v = is_convertible<From, To>::value;

    // enable_if
    template<bool B, typename T = void>
    struct enable_if {};

    template<typename T>
    struct enable_if<true, T> {
        using type = T;
    };

    template<bool B, typename T = void>
    using enable_if_t = typename enable_if<B, T>::type;

    // conditional
    template<bool B, typename T, typename F>
    struct conditional {
        using type = T;
    };

    template<typename T, typename F>
    struct conditional<false, T, F> {
        using type = F;
    };

    template<bool B, typename T, typename F>
    using conditional_t = typename conditional<B, T, F>::type;

    // is_const
    template<typename T>
    struct is_const : false_type {};

    template<typename T>
    struct is_const<const T> : true_type {};

    template<typename T>
    inline constexpr bool is_const_v = is_const<T>::value;

    // is_volatile
    template<typename T>
    struct is_volatile : false_type {};

    template<typename T>
    struct is_volatile<volatile T> : true_type {};

    template<typename T>
    inline constexpr bool is_volatile_v = is_volatile<T>::value;

    // is_reference
    template<typename T>
    struct is_reference : false_type {};

    template<typename T>
    struct is_reference<T&> : true_type {};

    template<typename T>
    struct is_reference<T&&> : true_type {};

    template<typename T>
    inline constexpr bool is_reference_v = is_reference<T>::value;

    // remove_const
    template<typename T>
    struct remove_const {
        using type = T;
    };

    template<typename T>
    struct remove_const<const T> {
        using type = T;
    };

    template<typename T>
    using remove_const_t = typename remove_const<T>::type;

    // remove_volatile
    template<typename T>
    struct remove_volatile {
        using type = T;
    };

    template<typename T>
    struct remove_volatile<volatile T> {
        using type = T;
    };

    template<typename T>
    using remove_volatile_t = typename remove_volatile<T>::type;

    // remove_cv
    template<typename T>
    struct remove_cv {
        using type = typename remove_const<typename remove_volatile<T>::type>::type;
    };

    template<typename T>
    using remove_cv_t = typename remove_cv<T>::type;

    // is_void
    template<typename T>
    struct is_void : is_same<void, typename remove_cv<T>::type> {};

    template<typename T>
    inline constexpr bool is_void_v = is_void<T>::value;

    // is_integral
    template<typename T>
    struct is_integral : false_type {};

    // Specialize for integral types
    template<> struct is_integral<bool> : true_type {};
    template<> struct is_integral<char> : true_type {};
    template<> struct is_integral<signed char> : true_type {};
    template<> struct is_integral<unsigned char> : true_type {};
    template<> struct is_integral<wchar_t> : true_type {};
    template<> struct is_integral<char16_t> : true_type {};
    template<> struct is_integral<char32_t> : true_type {};
    template<> struct is_integral<short> : true_type {};
    template<> struct is_integral<unsigned short> : true_type {};
    template<> struct is_integral<int> : true_type {};
    template<> struct is_integral<unsigned int> : true_type {};
    template<> struct is_integral<long> : true_type {};
    template<> struct is_integral<unsigned long> : true_type {};
    template<> struct is_integral<long long> : true_type {};
    template<> struct is_integral<unsigned long long> : true_type {};

    template<typename T>
    inline constexpr bool is_integral_v = is_integral<T>::value;

    // is_floating_point
    template<typename T>
    struct is_floating_point : false_type {};

    template<> struct is_floating_point<float> : true_type {};
    template<> struct is_floating_point<double> : true_type {};
    template<> struct is_floating_point<long double> : true_type {};

    template<typename T>
    inline constexpr bool is_floating_point_v = is_floating_point<T>::value;

    // is_arithmetic
    template<typename T>
    struct is_arithmetic : integral_constant<bool,
        is_integral_v<T> || is_floating_point_v<T>> {};

    template<typename T>
    inline constexpr bool is_arithmetic_v = is_arithmetic<T>::value;

    // is_pointer
    template<typename T>
    struct is_pointer : false_type {};

    template<typename T>
    struct is_pointer<T*> : true_type {};

    template<typename T>
    inline constexpr bool is_pointer_v = is_pointer<T>::value;

    // is_array
    template<typename T>
    struct is_array : false_type {};

    template<typename T>
    struct is_array<T[]> : true_type {};

    template<typename T, size_t N>
    struct is_array<T[N]> : true_type {};

    template<typename T>
    inline constexpr bool is_array_v = is_array<T>::value;

    // is_function (simplified)
    template<typename T>
    struct is_function : integral_constant<bool,
        !is_const_v<const T> && !is_reference_v<T>> {};

    template<typename T>
    inline constexpr bool is_function_v = is_function<T>::value;

    // remove_extent
    template<typename T>
    struct remove_extent {
        using type = T;
    };

    template<typename T>
    struct remove_extent<T[]> {
        using type = T;
    };

    template<typename T, size_t N>
    struct remove_extent<T[N]> {
        using type = T;
    };

    template<typename T>
    using remove_extent_t = typename remove_extent<T>::type;

    // add_pointer
    template<typename T>
    struct add_pointer {
        using type = typename remove_reference<T>::type*;
    };

    template<typename T>
    using add_pointer_t = typename add_pointer<T>::type;

    // add_lvalue_reference
    template<typename T>
    struct add_lvalue_reference {
        using type = T&;
    };

    template<>
    struct add_lvalue_reference<void> {
        using type = void;
    };

    template<>
    struct add_lvalue_reference<const void> {
        using type = const void;
    };

    template<>
    struct add_lvalue_reference<volatile void> {
        using type = volatile void;
    };

    template<>
    struct add_lvalue_reference<const volatile void> {
        using type = const volatile void;
    };

    template<typename T>
    using add_lvalue_reference_t = typename add_lvalue_reference<T>::type;

    // decay
    template<typename T>
    struct decay {
        using U = typename remove_reference<T>::type;
        using type = typename conditional<
            is_array_v<U>,
            typename remove_extent<U>::type*,
            typename conditional<
                is_function_v<U>,
                typename add_pointer<U>::type,
                typename remove_cv<U>::type
            >::type
        >::type;
    };

    template<typename T>
    using decay_t = typename decay<T>::type;

    // Nothrow traits (simplified for kernel use - assume all operations are nothrow)
    template<typename T>
    struct is_nothrow_move_constructible : true_type {};

    template<typename T>
    inline constexpr bool is_nothrow_move_constructible_v = is_nothrow_move_constructible<T>::value;

    template<typename T>
    struct is_nothrow_move_assignable : true_type {};

    template<typename T>
    inline constexpr bool is_nothrow_move_assignable_v = is_nothrow_move_assignable<T>::value;

    // Trivially move constructible (simplified for kernel use)
    template<typename T>
    struct is_trivially_move_constructible : is_nothrow_move_constructible<T> {};

    template<typename T>
    inline constexpr bool is_trivially_move_constructible_v = is_trivially_move_constructible<T>::value;

    // Trivially move assignable (simplified for kernel use)
    template<typename T>
    struct is_trivially_move_assignable : is_nothrow_move_assignable<T> {};

    template<typename T>
    inline constexpr bool is_trivially_move_assignable_v = is_trivially_move_assignable<T>::value;

    // ========================================================================
    // Utility functions
    // ========================================================================

    /**
     * @brief Forward an lvalue
     */
    template<typename T>
    constexpr T&& forward(typename remove_reference<T>::type& t) noexcept {
        return static_cast<T&&>(t);
    }

    /**
     * @brief Forward an rvalue
     */
    template<typename T>
    constexpr T&& forward(typename remove_reference<T>::type&& t) noexcept {
        return static_cast<T&&>(t);
    }

    /**
     * @brief Convert a value to an rvalue
     */
    template<typename T>
    constexpr typename remove_reference<T>::type&& move(T&& t) noexcept {
        return static_cast<typename remove_reference<T>::type&&>(t);
    }

    // hash (forward declaration for string_view specialization)
    template<typename T>
    struct hash;

} // namespace std

