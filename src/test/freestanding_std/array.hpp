#pragma once

/**
 * @file array.hpp
 * @brief std::array implementation for freestanding environment
 *
 * Provides std::array API which is already a compile-time bounded container.
 * This is essentially a wrapper around C-style arrays with STL-like interface.
 */

#include "type_traits.hpp"

namespace std {

    /**
     * @brief Array implementation with compile-time size
     *
     * Provides complete std::array API using compile-time bounded storage.
     * This is a thin wrapper around C-style arrays.
     *
     * @tparam T Element type
     * @tparam N Array size
     */
    template<typename T, size_t N>
    struct array {
        // ====================================================================
        // Type definitions (std::array compatibility)
        // ====================================================================

        using value_type = T;
        using size_type = size_t;
        using difference_type = ptrdiff_t;
        using reference = T&;
        using const_reference = const T&;
        using pointer = T*;
        using const_pointer = const T*;

        // Iterator types
        using iterator = T*;
        using const_iterator = const T*;

        // ====================================================================
        // Storage (public for aggregate initialization)
        // ====================================================================

        T data_[N > 0 ? N : 1]; // Avoid zero-size arrays

        // ====================================================================
        // Element access
        // ====================================================================

        constexpr reference at(size_type pos) {
            // In kernel environment: can't throw, use bounds clamping
            if (pos >= N) {
                pos = N > 0 ? N - 1 : 0;
            }
            return data_[pos];
        }

        constexpr const_reference at(size_type pos) const {
            if (pos >= N) {
                pos = N > 0 ? N - 1 : 0;
            }
            return data_[pos];
        }

        constexpr reference operator[](size_type pos) noexcept {
            return data_[pos];
        }

        constexpr const_reference operator[](size_type pos) const noexcept {
            return data_[pos];
        }

        constexpr reference front() noexcept {
            return data_[0];
        }

        constexpr const_reference front() const noexcept {
            return data_[0];
        }

        constexpr reference back() noexcept {
            return data_[N > 0 ? N - 1 : 0];
        }

        constexpr const_reference back() const noexcept {
            return data_[N > 0 ? N - 1 : 0];
        }

        constexpr T* data() noexcept {
            return data_;
        }

        constexpr const T* data() const noexcept {
            return data_;
        }

        // ====================================================================
        // Iterators
        // ====================================================================

        constexpr iterator begin() noexcept {
            return data_;
        }

        constexpr const_iterator begin() const noexcept {
            return data_;
        }

        constexpr const_iterator cbegin() const noexcept {
            return data_;
        }

        constexpr iterator end() noexcept {
            return data_ + N;
        }

        constexpr const_iterator end() const noexcept {
            return data_ + N;
        }

        constexpr const_iterator cend() const noexcept {
            return data_ + N;
        }

        // ====================================================================
        // Capacity
        // ====================================================================

        [[nodiscard]] constexpr bool empty() const noexcept {
            return N == 0;
        }

        constexpr size_type size() const noexcept {
            return N;
        }

        constexpr size_type max_size() const noexcept {
            return N;
        }

        // ====================================================================
        // Operations
        // ====================================================================

        void fill(const T& value) {
            for (size_type i = 0; i < N; ++i) {
                data_[i] = value;
            }
        }

        void swap(array& other) noexcept {
            for (size_type i = 0; i < N; ++i) {
                T temp = static_cast<T&&>(data_[i]);
                data_[i] = static_cast<T&&>(other.data_[i]);
                other.data_[i] = static_cast<T&&>(temp);
            }
        }
    };

    // ========================================================================
    // Specialization for zero-size arrays
    // ========================================================================

    template<typename T>
    struct array<T, 0> {
        using value_type = T;
        using size_type = size_t;
        using difference_type = ptrdiff_t;
        using reference = T&;
        using const_reference = const T&;
        using pointer = T*;
        using const_pointer = const T*;
        using iterator = T*;
        using const_iterator = const T*;

        // No actual storage for zero-size arrays

        constexpr reference at(size_type) {
            // Return reference to dummy static object
            static T dummy{};
            return dummy;
        }

        constexpr const_reference at(size_type) const {
            static const T dummy{};
            return dummy;
        }

        constexpr reference operator[](size_type) noexcept {
            static T dummy{};
            return dummy;
        }

        constexpr const_reference operator[](size_type) const noexcept {
            static const T dummy{};
            return dummy;
        }

        constexpr reference front() noexcept {
            static T dummy{};
            return dummy;
        }

        constexpr const_reference front() const noexcept {
            static const T dummy{};
            return dummy;
        }

        constexpr reference back() noexcept {
            static T dummy{};
            return dummy;
        }

        constexpr const_reference back() const noexcept {
            static const T dummy{};
            return dummy;
        }

        constexpr T* data() noexcept {
            return nullptr;
        }

        constexpr const T* data() const noexcept {
            return nullptr;
        }

        constexpr iterator begin() noexcept {
            return nullptr;
        }

        constexpr const_iterator begin() const noexcept {
            return nullptr;
        }

        constexpr const_iterator cbegin() const noexcept {
            return nullptr;
        }

        constexpr iterator end() noexcept {
            return nullptr;
        }

        constexpr const_iterator end() const noexcept {
            return nullptr;
        }

        constexpr const_iterator cend() const noexcept {
            return nullptr;
        }

        [[nodiscard]] constexpr bool empty() const noexcept {
            return true;
        }

        constexpr size_type size() const noexcept {
            return 0;
        }

        constexpr size_type max_size() const noexcept {
            return 0;
        }

        void fill(const T&) {
            // No-op for zero-size array
        }

        void swap(array&) noexcept {
            // No-op for zero-size array
        }
    };

    // ========================================================================
    // Non-member functions
    // ========================================================================

    template<typename T, size_t N>
    constexpr bool operator==(const array<T, N>& lhs, const array<T, N>& rhs) {
        for (size_t i = 0; i < N; ++i) {
            if (!(lhs[i] == rhs[i])) {
                return false;
            }
        }
        return true;
    }

    template<typename T, size_t N>
    constexpr bool operator!=(const array<T, N>& lhs, const array<T, N>& rhs) {
        return !(lhs == rhs);
    }

    template<typename T, size_t N>
    constexpr bool operator<(const array<T, N>& lhs, const array<T, N>& rhs) {
        for (size_t i = 0; i < N; ++i) {
            if (lhs[i] < rhs[i]) {
                return true;
            }
            if (rhs[i] < lhs[i]) {
                return false;
            }
        }
        return false; // Arrays are equal
    }

    template<typename T, size_t N>
    constexpr bool operator<=(const array<T, N>& lhs, const array<T, N>& rhs) {
        return !(rhs < lhs);
    }

    template<typename T, size_t N>
    constexpr bool operator>(const array<T, N>& lhs, const array<T, N>& rhs) {
        return rhs < lhs;
    }

    template<typename T, size_t N>
    constexpr bool operator>=(const array<T, N>& lhs, const array<T, N>& rhs) {
        return !(lhs < rhs);
    }

    template<typename T, size_t N>
    void swap(array<T, N>& lhs, array<T, N>& rhs) noexcept {
        lhs.swap(rhs);
    }

} // namespace std
