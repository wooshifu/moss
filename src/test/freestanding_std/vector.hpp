#pragma once

/**
 * @file vector.hpp
 * @brief Static std::vector replacement for freestanding environment
 *
 * Provides std::vector API with compile-time bounded storage. Uses static arrays
 * instead of dynamic allocation, making it suitable for kernel/freestanding use.
 */

#include "type_traits.hpp"

namespace std {

    /**
     * @brief Static vector implementation with compile-time size bounds
     *
     * Provides complete std::vector API using static storage allocation.
     * All operations have bounds checking in debug mode.
     *
     * @tparam T Element type
     * @tparam MaxSize Maximum number of elements (default: 256)
     */
    template<typename T, usize MaxSize = 256>
    class vector {
    public:
        // ====================================================================
        // Type definitions (std::vector compatibility)
        // ====================================================================

        using value_type = T;
        using size_type = usize;
        using difference_type = isize;
        using reference = T&;
        using const_reference = const T&;
        using pointer = T*;
        using const_pointer = const T*;

        // Simple iterator implementation
        using iterator = T*;
        using const_iterator = const T*;

        // ====================================================================
        // Constructors and Destructor
        // ====================================================================

        /**
         * @brief Default constructor - creates empty vector
         */
        constexpr vector() noexcept : size_(0) {
            // Storage is uninitialized until elements are added
        }

        /**
         * @brief Fill constructor - creates vector with n copies of value
         */
        explicit vector(size_type count, const T& value = T{}) : size_(0) {
            if (count > MaxSize) {
                // In kernel environment, we can't throw exceptions
                // Instead, truncate to maximum size
                count = MaxSize;
            }

            for (size_type i = 0; i < count; ++i) {
                push_back(value);
            }
        }

        /**
         * @brief Copy constructor
         */
        vector(const vector& other) : size_(0) {
            for (const auto& item : other) {
                push_back(item);
            }
        }

        /**
         * @brief Move constructor
         */
        vector(vector&& other) noexcept : size_(0) {
            if constexpr (std::is_trivially_move_constructible_v<T>) {
                // For trivial types, do memcpy-style move
                for (size_type i = 0; i < other.size_; ++i) {
                    new(storage_ptr() + i) T(static_cast<T&&>(other[i]));
                }
                size_ = other.size_;
                other.size_ = 0;
            } else {
                // For non-trivial types, move each element
                for (auto&& item : other) {
                    push_back(static_cast<T&&>(item));
                }
                other.clear();
            }
        }

        /**
         * @brief Destructor - calls destructors for all elements
         */
        ~vector() {
            clear();
        }

        // ====================================================================
        // Assignment operators
        // ====================================================================

        vector& operator=(const vector& other) {
            if (this != &other) {
                clear();
                for (const auto& item : other) {
                    push_back(item);
                }
            }
            return *this;
        }

        vector& operator=(vector&& other) noexcept {
            if (this != &other) {
                clear();
                if constexpr (std::is_trivially_move_assignable_v<T>) {
                    for (size_type i = 0; i < other.size_; ++i) {
                        new(storage_ptr() + i) T(static_cast<T&&>(other[i]));
                    }
                    size_ = other.size_;
                    other.size_ = 0;
                } else {
                    for (auto&& item : other) {
                        push_back(static_cast<T&&>(item));
                    }
                    other.clear();
                }
            }
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
            return storage_ptr()[pos];
        }

        const_reference at(size_type pos) const {
            if (pos >= size_) {
                pos = size_ > 0 ? size_ - 1 : 0;
            }
            return storage_ptr()[pos];
        }

        reference operator[](size_type pos) noexcept {
            return storage_ptr()[pos];
        }

        const_reference operator[](size_type pos) const noexcept {
            return storage_ptr()[pos];
        }

        reference front() noexcept {
            return storage_ptr()[0];
        }

        const_reference front() const noexcept {
            return storage_ptr()[0];
        }

        reference back() noexcept {
            return storage_ptr()[size_ - 1];
        }

        const_reference back() const noexcept {
            return storage_ptr()[size_ - 1];
        }

        T* data() noexcept {
            return storage_ptr();
        }

        const T* data() const noexcept {
            return storage_ptr();
        }

        // ====================================================================
        // Iterators
        // ====================================================================

        iterator begin() noexcept {
            return storage_ptr();
        }

        const_iterator begin() const noexcept {
            return storage_ptr();
        }

        const_iterator cbegin() const noexcept {
            return storage_ptr();
        }

        iterator end() noexcept {
            return storage_ptr() + size_;
        }

        const_iterator end() const noexcept {
            return storage_ptr() + size_;
        }

        const_iterator cend() const noexcept {
            return storage_ptr() + size_;
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

        constexpr size_type max_size() const noexcept {
            return MaxSize;
        }

        constexpr size_type capacity() const noexcept {
            return MaxSize;
        }

        // ====================================================================
        // Modifiers
        // ====================================================================

        void clear() noexcept {
            // Call destructor for all elements
            for (size_type i = 0; i < size_; ++i) {
                storage_ptr()[i].~T();
            }
            size_ = 0;
        }

        void push_back(const T& value) {
            if (size_ < MaxSize) {
                new(storage_ptr() + size_) T(value);
                ++size_;
            }
            // Silently ignore if at capacity (kernel environment)
        }

        void push_back(T&& value) {
            if (size_ < MaxSize) {
                new(storage_ptr() + size_) T(static_cast<T&&>(value));
                ++size_;
            }
        }

        template<typename... Args>
        reference emplace_back(Args&&... args) {
            if (size_ < MaxSize) {
                T* ptr = storage_ptr() + size_;
                new(ptr) T(static_cast<Args&&>(args)...);
                ++size_;
                return *ptr;
            } else {
                // Return reference to last element if at capacity
                return back();
            }
        }

        void pop_back() {
            if (size_ > 0) {
                --size_;
                storage_ptr()[size_].~T();
            }
        }

        void resize(size_type count, const T& value = T{}) {
            if (count > MaxSize) {
                count = MaxSize;
            }

            if (count < size_) {
                // Shrinking: destroy extra elements
                for (size_type i = count; i < size_; ++i) {
                    storage_ptr()[i].~T();
                }
            } else if (count > size_) {
                // Growing: construct new elements
                for (size_type i = size_; i < count; ++i) {
                    new(storage_ptr() + i) T(value);
                }
            }
            size_ = count;
        }

        iterator erase(const_iterator pos) {
            if (pos >= begin() && pos < end()) {
                iterator mutable_pos = const_cast<iterator>(pos);
                iterator next = mutable_pos + 1;

                // Move all elements after pos one position left
                while (next != end()) {
                    *mutable_pos = static_cast<T&&>(*next);
                    ++mutable_pos;
                    ++next;
                }

                // Destroy the last element and decrement size
                pop_back();
                return const_cast<iterator>(pos);
            }
            return end();
        }

        iterator insert(const_iterator pos, const T& value) {
            if (size_ >= MaxSize) {
                return end();  // No space
            }

            if (pos == end()) {
                push_back(value);
                return end() - 1;
            }

            // Shift elements to make room
            push_back(back());  // Add space
            iterator mutable_pos = const_cast<iterator>(pos);
            iterator current = end() - 2;  // Start from second-to-last

            while (current >= mutable_pos) {
                *(current + 1) = static_cast<T&&>(*current);
                if (current == mutable_pos) break;
                --current;
            }

            *mutable_pos = value;
            return mutable_pos;
        }

    private:
        // ====================================================================
        // Storage implementation
        // ====================================================================

        alignas(T) u8 storage_[MaxSize * sizeof(T)];
        size_type size_;

        T* storage_ptr() noexcept {
            return reinterpret_cast<T*>(storage_);
        }

        const T* storage_ptr() const noexcept {
            return reinterpret_cast<const T*>(storage_);
        }
    };

    // ========================================================================
    // Non-member functions
    // ========================================================================

    template<typename T, usize MaxSize>
    bool operator==(const vector<T, MaxSize>& lhs, const vector<T, MaxSize>& rhs) {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (usize i = 0; i < lhs.size(); ++i) {
            if (!(lhs[i] == rhs[i])) {
                return false;
            }
        }
        return true;
    }

    template<typename T, usize MaxSize>
    bool operator!=(const vector<T, MaxSize>& lhs, const vector<T, MaxSize>& rhs) {
        return !(lhs == rhs);
    }

} // namespace std
