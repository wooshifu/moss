#pragma once

/**
 * @file string_view.hpp
 * @brief std::string_view implementation for freestanding environment
 *
 * Provides lightweight, non-owning string view functionality compatible with
 * both static strings and C-style strings. Designed for kernel/freestanding use.
 */

#include "type_traits.hpp"

namespace std {

    /**
     * @brief Non-owning string view implementation
     *
     * Provides std::string_view API for non-owning string operations.
     * Lightweight and suitable for freestanding environments.
     */
    class string_view {
    public:
        // ====================================================================
        // Type definitions (std::string_view compatibility)
        // ====================================================================

        using value_type = char;
        using size_type = usize;
        using difference_type = isize;
        using reference = const char&;
        using const_reference = const char&;
        using pointer = const char*;
        using const_pointer = const char*;

        // Iterator types
        using iterator = const char*;
        using const_iterator = const char*;

        // Constants
        static constexpr size_type npos = ~static_cast<size_type>(0);

    private:
        const char* data_;
        size_type size_;

    public:
        // ====================================================================
        // Constructors
        // ====================================================================

        /**
         * @brief Default constructor - creates empty string_view
         */
        constexpr string_view() noexcept : data_(nullptr), size_(0) {}

        /**
         * @brief C-string constructor
         */
        constexpr string_view(const char* s) noexcept
            : data_(s), size_(s ? string_length(s) : 0) {}

        /**
         * @brief Pointer and length constructor
         */
        constexpr string_view(const char* s, size_type len) noexcept
            : data_(s), size_(len) {}

        /**
         * @brief Copy constructor
         */
        constexpr string_view(const string_view&) noexcept = default;

        /**
         * @brief Assignment operator
         */
        constexpr string_view& operator=(const string_view&) noexcept = default;

        // ====================================================================
        // Element access
        // ====================================================================

        constexpr reference operator[](size_type pos) const noexcept {
            return data_[pos];
        }

        constexpr reference at(size_type pos) const {
            // In kernel environment, we can't throw - bounds clamp instead
            return data_[pos < size_ ? pos : (size_ > 0 ? size_ - 1 : 0)];
        }

        constexpr reference front() const noexcept {
            return data_[0];
        }

        constexpr reference back() const noexcept {
            return data_[size_ > 0 ? size_ - 1 : 0];
        }

        constexpr const_pointer data() const noexcept {
            return data_;
        }

        // ====================================================================
        // Iterators
        // ====================================================================

        constexpr const_iterator begin() const noexcept {
            return data_;
        }

        constexpr const_iterator cbegin() const noexcept {
            return data_;
        }

        constexpr const_iterator end() const noexcept {
            return data_ + size_;
        }

        constexpr const_iterator cend() const noexcept {
            return data_ + size_;
        }

        // ====================================================================
        // Capacity
        // ====================================================================

        [[nodiscard]] constexpr bool empty() const noexcept {
            return size_ == 0;
        }

        constexpr size_type size() const noexcept {
            return size_;
        }

        constexpr size_type length() const noexcept {
            return size_;
        }

        constexpr size_type max_size() const noexcept {
            return ~static_cast<size_type>(0);
        }

        // ====================================================================
        // Operations
        // ====================================================================

        constexpr void remove_prefix(size_type n) noexcept {
            if (n > size_) {
                n = size_;
            }
            data_ += n;
            size_ -= n;
        }

        constexpr void remove_suffix(size_type n) noexcept {
            if (n > size_) {
                n = size_;
            }
            size_ -= n;
        }

        constexpr void swap(string_view& other) noexcept {
            const char* temp_data = data_;
            size_type temp_size = size_;
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = temp_data;
            other.size_ = temp_size;
        }

        // ====================================================================
        // String operations
        // ====================================================================

        constexpr int compare(string_view sv) const noexcept {
            size_type common_len = (size_ < sv.size_) ? size_ : sv.size_;

            for (size_type i = 0; i < common_len; ++i) {
                if (data_[i] != sv.data_[i]) {
                    return data_[i] - sv.data_[i];
                }
            }

            if (size_ < sv.size_) {
                return -1;
            } else if (size_ > sv.size_) {
                return 1;
            } else {
                return 0;
            }
        }

        constexpr int compare(size_type pos1, size_type count1, string_view sv) const {
            return substr(pos1, count1).compare(sv);
        }

        constexpr int compare(const char* s) const {
            return compare(string_view(s));
        }

        constexpr bool starts_with(string_view sv) const noexcept {
            return size_ >= sv.size_ && compare(0, sv.size_, sv) == 0;
        }

        constexpr bool starts_with(char ch) const noexcept {
            return !empty() && front() == ch;
        }

        constexpr bool starts_with(const char* s) const {
            return starts_with(string_view(s));
        }

        constexpr bool ends_with(string_view sv) const noexcept {
            return size_ >= sv.size_ &&
                   compare(size_ - sv.size_, npos, sv) == 0;
        }

        constexpr bool ends_with(char ch) const noexcept {
            return !empty() && back() == ch;
        }

        constexpr bool ends_with(const char* s) const {
            return ends_with(string_view(s));
        }

        constexpr bool contains(string_view sv) const noexcept {
            return find(sv) != npos;
        }

        constexpr bool contains(char ch) const noexcept {
            return find(ch) != npos;
        }

        constexpr bool contains(const char* s) const {
            return find(s) != npos;
        }

        constexpr size_type find(string_view sv, size_type pos = 0) const noexcept {
            if (pos > size_ || sv.size_ == 0) {
                return sv.size_ == 0 ? pos : npos;
            }

            if (sv.size_ > size_ - pos) {
                return npos;
            }

            for (size_type i = pos; i <= size_ - sv.size_; ++i) {
                bool match = true;
                for (size_type j = 0; j < sv.size_; ++j) {
                    if (data_[i + j] != sv.data_[j]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    return i;
                }
            }
            return npos;
        }

        constexpr size_type find(char ch, size_type pos = 0) const noexcept {
            for (size_type i = pos; i < size_; ++i) {
                if (data_[i] == ch) {
                    return i;
                }
            }
            return npos;
        }

        constexpr size_type find(const char* s, size_type pos = 0) const {
            return find(string_view(s), pos);
        }

        constexpr size_type rfind(string_view sv, size_type pos = npos) const noexcept {
            if (sv.size_ > size_) {
                return npos;
            }

            if (pos > size_ - sv.size_) {
                pos = size_ - sv.size_;
            }

            // Search backwards from position
            for (size_type i = pos + 1; i > 0; --i) {
                size_type start = i - 1;
                bool match = true;
                for (size_type j = 0; j < sv.size_; ++j) {
                    if (data_[start + j] != sv.data_[j]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    return start;
                }
            }
            return npos;
        }

        constexpr size_type rfind(char ch, size_type pos = npos) const noexcept {
            if (empty()) {
                return npos;
            }

            if (pos >= size_) {
                pos = size_ - 1;
            }

            for (size_type i = pos + 1; i > 0; --i) {
                if (data_[i - 1] == ch) {
                    return i - 1;
                }
            }
            return npos;
        }

        constexpr size_type rfind(const char* s, size_type pos = npos) const {
            return rfind(string_view(s), pos);
        }

        constexpr string_view substr(size_type pos = 0, size_type count = npos) const {
            if (pos > size_) {
                pos = size_;
            }

            size_type actual_count = size_ - pos;
            if (count < actual_count) {
                actual_count = count;
            }

            return string_view(data_ + pos, actual_count);
        }

    private:
        // ====================================================================
        // Helper functions
        // ====================================================================

        static constexpr size_type string_length(const char* s) noexcept {
            if (s == nullptr) return 0;
            size_type len = 0;
            while (s[len] != '\0') {
                ++len;
            }
            return len;
        }
    };

    // ========================================================================
    // Non-member functions
    // ========================================================================

    // Comparison operators
    constexpr bool operator==(string_view lhs, string_view rhs) noexcept {
        return lhs.compare(rhs) == 0;
    }

    constexpr bool operator!=(string_view lhs, string_view rhs) noexcept {
        return lhs.compare(rhs) != 0;
    }

    constexpr bool operator<(string_view lhs, string_view rhs) noexcept {
        return lhs.compare(rhs) < 0;
    }

    constexpr bool operator<=(string_view lhs, string_view rhs) noexcept {
        return lhs.compare(rhs) <= 0;
    }

    constexpr bool operator>(string_view lhs, string_view rhs) noexcept {
        return lhs.compare(rhs) > 0;
    }

    constexpr bool operator>=(string_view lhs, string_view rhs) noexcept {
        return lhs.compare(rhs) >= 0;
    }

    // Hash function (basic implementation)
    template<>
    struct hash<string_view> {
        constexpr size_t operator()(const string_view& sv) const noexcept {
            // Simple FNV-1a hash for freestanding environment
            size_t hash = 14695981039346656037ULL; // FNV offset basis
            for (char c : sv) {
                hash ^= static_cast<unsigned char>(c);
                hash *= 1099511628211ULL; // FNV prime
            }
            return hash;
        }
    };

    // String literal operator (C++14)
    constexpr string_view operator""sv(const char* str, size_t len) noexcept {
        return string_view(str, len);
    }

} // namespace std
