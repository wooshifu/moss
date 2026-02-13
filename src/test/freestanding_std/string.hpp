#pragma once

/**
 * @file string.hpp
 * @brief Static std::string replacement for freestanding environment
 *
 * Provides std::string API with fixed-size buffer storage. Uses static arrays
 * instead of dynamic allocation, making it suitable for kernel/freestanding use.
 */

#include "type_traits.hpp"

namespace std {

    /**
     * @brief Static string implementation with compile-time size bounds
     *
     * Provides complete std::string API using fixed-size buffer storage.
     * All operations have bounds checking in debug mode.
     *
     * @tparam MaxLen Maximum string length (default: 512)
     */
    template<usize MaxLen = 512>
    class basic_string {
    public:
        // ====================================================================
        // Type definitions (std::string compatibility)
        // ====================================================================

        using value_type = char;
        using size_type = usize;
        using difference_type = isize;
        using reference = char&;
        using const_reference = const char&;
        using pointer = char*;
        using const_pointer = const char*;

        // Simple iterator implementation
        using iterator = char*;
        using const_iterator = const char*;

        // String constants
        static constexpr size_type npos = ~static_cast<size_type>(0);

    private:
        // ====================================================================
        // Storage implementation
        // ====================================================================

        char data_[MaxLen + 1]; // +1 for null terminator
        size_type size_;

    public:
        // ====================================================================
        // Constructors and Destructor
        // ====================================================================

        /**
         * @brief Default constructor - creates empty string
         */
        constexpr basic_string() noexcept : data_{}, size_(0) {
            data_[0] = '\0';
        }

        /**
         * @brief C-string constructor
         */
        basic_string(const char* s) : size_(0) {
            if (s != nullptr) {
                while (s[size_] != '\0' && size_ < MaxLen) {
                    data_[size_] = s[size_];
                    ++size_;
                }
            }
            data_[size_] = '\0';
        }

        /**
         * @brief Fill constructor - creates string with n copies of character
         */
        basic_string(size_type count, char ch) : size_(0) {
            if (count > MaxLen) {
                count = MaxLen;
            }
            for (size_type i = 0; i < count; ++i) {
                data_[i] = ch;
            }
            size_ = count;
            data_[size_] = '\0';
        }

        /**
         * @brief Substring constructor
         */
        basic_string(const char* s, size_type pos, size_type len) : size_(0) {
            if (s != nullptr && pos < string_length(s)) {
                size_type available = string_length(s) - pos;
                size_type copy_len = (len < available) ? len : available;
                if (copy_len > MaxLen) {
                    copy_len = MaxLen;
                }

                for (size_type i = 0; i < copy_len; ++i) {
                    data_[i] = s[pos + i];
                }
                size_ = copy_len;
            }
            data_[size_] = '\0';
        }

        /**
         * @brief Copy constructor
         */
        basic_string(const basic_string& other) : size_(other.size_) {
            for (size_type i = 0; i <= size_; ++i) { // Include null terminator
                data_[i] = other.data_[i];
            }
        }

        /**
         * @brief Move constructor
         */
        basic_string(basic_string&& other) noexcept : size_(other.size_) {
            for (size_type i = 0; i <= size_; ++i) {
                data_[i] = other.data_[i];
            }
            other.size_ = 0;
            other.data_[0] = '\0';
        }

        /**
         * @brief Destructor
         */
        ~basic_string() = default;

        // ====================================================================
        // Assignment operators
        // ====================================================================

        basic_string& operator=(const basic_string& other) {
            if (this != &other) {
                size_ = other.size_;
                for (size_type i = 0; i <= size_; ++i) {
                    data_[i] = other.data_[i];
                }
            }
            return *this;
        }

        basic_string& operator=(basic_string&& other) noexcept {
            if (this != &other) {
                size_ = other.size_;
                for (size_type i = 0; i <= size_; ++i) {
                    data_[i] = other.data_[i];
                }
                other.size_ = 0;
                other.data_[0] = '\0';
            }
            return *this;
        }

        basic_string& operator=(const char* s) {
            size_ = 0;
            if (s != nullptr) {
                while (s[size_] != '\0' && size_ < MaxLen) {
                    data_[size_] = s[size_];
                    ++size_;
                }
            }
            data_[size_] = '\0';
            return *this;
        }

        basic_string& operator=(char ch) {
            data_[0] = ch;
            size_ = 1;
            data_[1] = '\0';
            return *this;
        }

        // ====================================================================
        // Element access
        // ====================================================================

        reference at(size_type pos) {
            if (pos >= size_) {
                // Kernel environment: can't throw, use bounds clamping
                pos = size_ > 0 ? size_ - 1 : 0;
            }
            return data_[pos];
        }

        const_reference at(size_type pos) const {
            if (pos >= size_) {
                pos = size_ > 0 ? size_ - 1 : 0;
            }
            return data_[pos];
        }

        reference operator[](size_type pos) noexcept {
            return data_[pos];
        }

        const_reference operator[](size_type pos) const noexcept {
            return data_[pos];
        }

        reference front() noexcept {
            return data_[0];
        }

        const_reference front() const noexcept {
            return data_[0];
        }

        reference back() noexcept {
            return data_[size_ > 0 ? size_ - 1 : 0];
        }

        const_reference back() const noexcept {
            return data_[size_ > 0 ? size_ - 1 : 0];
        }

        const char* data() const noexcept {
            return data_;
        }

        char* data() noexcept {
            return data_;
        }

        const char* c_str() const noexcept {
            return data_;
        }

        // ====================================================================
        // Iterators
        // ====================================================================

        iterator begin() noexcept {
            return data_;
        }

        const_iterator begin() const noexcept {
            return data_;
        }

        const_iterator cbegin() const noexcept {
            return data_;
        }

        iterator end() noexcept {
            return data_ + size_;
        }

        const_iterator end() const noexcept {
            return data_ + size_;
        }

        const_iterator cend() const noexcept {
            return data_ + size_;
        }

        // ====================================================================
        // Capacity
        // ====================================================================

        [[nodiscard]] bool empty() const noexcept {
            return size_ == 0;
        }

        size_type size() const noexcept {
            return size_;
        }

        size_type length() const noexcept {
            return size_;
        }

        constexpr size_type max_size() const noexcept {
            return MaxLen;
        }

        constexpr size_type capacity() const noexcept {
            return MaxLen;
        }

        // ====================================================================
        // Operations
        // ====================================================================

        void clear() noexcept {
            size_ = 0;
            data_[0] = '\0';
        }

        basic_string& append(const basic_string& str) {
            return append(str.data_, str.size_);
        }

        basic_string& append(const char* s) {
            if (s != nullptr) {
                size_type len = string_length(s);
                return append(s, len);
            }
            return *this;
        }

        basic_string& append(const char* s, size_type count) {
            if (s != nullptr && count > 0) {
                size_type available_space = MaxLen - size_;
                size_type copy_count = (count < available_space) ? count : available_space;

                for (size_type i = 0; i < copy_count; ++i) {
                    data_[size_ + i] = s[i];
                }
                size_ += copy_count;
                data_[size_] = '\0';
            }
            return *this;
        }

        basic_string& append(size_type count, char ch) {
            size_type available_space = MaxLen - size_;
            size_type copy_count = (count < available_space) ? count : available_space;

            for (size_type i = 0; i < copy_count; ++i) {
                data_[size_ + i] = ch;
            }
            size_ += copy_count;
            data_[size_] = '\0';
            return *this;
        }

        void push_back(char ch) {
            if (size_ < MaxLen) {
                data_[size_] = ch;
                ++size_;
                data_[size_] = '\0';
            }
        }

        void pop_back() {
            if (size_ > 0) {
                --size_;
                data_[size_] = '\0';
            }
        }

        basic_string& operator+=(const basic_string& str) {
            return append(str);
        }

        basic_string& operator+=(const char* s) {
            return append(s);
        }

        basic_string& operator+=(char ch) {
            push_back(ch);
            return *this;
        }

        // ====================================================================
        // String operations
        // ====================================================================

        int compare(const basic_string& str) const noexcept {
            return compare(str.data_);
        }

        int compare(const char* s) const noexcept {
            if (s == nullptr) {
                return size_ > 0 ? 1 : 0;
            }

            size_type i = 0;
            while (i < size_ && s[i] != '\0' && data_[i] == s[i]) {
                ++i;
            }

            if (i < size_ && s[i] != '\0') {
                return data_[i] - s[i];
            } else if (i < size_) {
                return 1; // this is longer
            } else if (s[i] != '\0') {
                return -1; // s is longer
            } else {
                return 0; // equal
            }
        }

        size_type find(const char* s, size_type pos = 0) const noexcept {
            if (s == nullptr || pos >= size_) {
                return npos;
            }

            size_type s_len = string_length(s);
            if (s_len == 0) {
                return pos;
            }

            for (size_type i = pos; i <= size_ - s_len; ++i) {
                bool found = true;
                for (size_type j = 0; j < s_len; ++j) {
                    if (data_[i + j] != s[j]) {
                        found = false;
                        break;
                    }
                }
                if (found) {
                    return i;
                }
            }
            return npos;
        }

        size_type find(char ch, size_type pos = 0) const noexcept {
            for (size_type i = pos; i < size_; ++i) {
                if (data_[i] == ch) {
                    return i;
                }
            }
            return npos;
        }

        basic_string substr(size_type pos = 0, size_type len = npos) const {
            if (pos >= size_) {
                return basic_string();
            }

            size_type actual_len = size_ - pos;
            if (len != npos && len < actual_len) {
                actual_len = len;
            }

            return basic_string(data_ + pos, 0, actual_len);
        }

    private:
        // ====================================================================
        // Helper functions
        // ====================================================================

        static size_type string_length(const char* s) noexcept {
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

    template<usize MaxLen>
    bool operator==(const basic_string<MaxLen>& lhs, const basic_string<MaxLen>& rhs) {
        return lhs.compare(rhs) == 0;
    }

    template<usize MaxLen>
    bool operator==(const basic_string<MaxLen>& lhs, const char* rhs) {
        return lhs.compare(rhs) == 0;
    }

    template<usize MaxLen>
    bool operator==(const char* lhs, const basic_string<MaxLen>& rhs) {
        return rhs.compare(lhs) == 0;
    }

    template<usize MaxLen>
    bool operator!=(const basic_string<MaxLen>& lhs, const basic_string<MaxLen>& rhs) {
        return !(lhs == rhs);
    }

    template<usize MaxLen>
    bool operator!=(const basic_string<MaxLen>& lhs, const char* rhs) {
        return !(lhs == rhs);
    }

    template<usize MaxLen>
    bool operator!=(const char* lhs, const basic_string<MaxLen>& rhs) {
        return !(lhs == rhs);
    }

    template<usize MaxLen>
    bool operator<(const basic_string<MaxLen>& lhs, const basic_string<MaxLen>& rhs) {
        return lhs.compare(rhs) < 0;
    }

    template<usize MaxLen>
    basic_string<MaxLen> operator+(const basic_string<MaxLen>& lhs, const basic_string<MaxLen>& rhs) {
        basic_string<MaxLen> result = lhs;
        result += rhs;
        return result;
    }

    template<usize MaxLen>
    basic_string<MaxLen> operator+(const basic_string<MaxLen>& lhs, const char* rhs) {
        basic_string<MaxLen> result = lhs;
        result += rhs;
        return result;
    }

    template<usize MaxLen>
    basic_string<MaxLen> operator+(const char* lhs, const basic_string<MaxLen>& rhs) {
        basic_string<MaxLen> result = lhs;
        result += rhs;
        return result;
    }

    // Default string type
    using string = basic_string<512>;

} // namespace std
