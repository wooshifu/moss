#pragma once

/**
 * @file utility.hpp
 * @brief Minimal freestanding utility functions for kernel ut.hpp
 */

namespace std {
    // Remove reference type traits
    template<typename T>
    struct remove_reference { using type = T; };

    template<typename T>
    struct remove_reference<T&> { using type = T; };

    template<typename T>
    struct remove_reference<T&&> { using type = T; };

    template<typename T>
    using remove_reference_t = typename remove_reference<T>::type;

    // Perfect forwarding
    template<typename T>
    constexpr T&& forward(remove_reference_t<T>& arg) noexcept {
        return static_cast<T&&>(arg);
    }

    template<typename T>
    constexpr T&& forward(remove_reference_t<T>&& arg) noexcept {
        return static_cast<T&&>(arg);
    }

    // Move semantics
    template<typename T>
    constexpr remove_reference_t<T>&& move(T&& arg) noexcept {
        return static_cast<remove_reference_t<T>&&>(arg);
    }

    // Size type
    using size_t = decltype(sizeof(int));
}