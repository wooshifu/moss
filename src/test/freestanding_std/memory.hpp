#pragma once

/**
 * @file memory.hpp
 * @brief Memory management layer for freestanding environment
 *
 * Provides smart pointer implementations and memory management utilities using
 * static allocation pools. Compatible with kernel/freestanding environments.
 */

#include "type_traits.hpp"

namespace std {

    // ========================================================================
    // nullptr_t support (define early for use throughout)
    // ========================================================================

    #ifndef __NULLPTR_T_DEFINED
    #define __NULLPTR_T_DEFINED
    using nullptr_t = decltype(nullptr);
    #endif

    // ========================================================================
    // Static memory pool for freestanding environment
    // ========================================================================

    namespace detail {
        // Simple static memory pool for testing framework
        constexpr usize MEMORY_POOL_SIZE = 16384; // 16KB
        constexpr usize MAX_ALLOCATIONS = 512;

        struct MemoryBlock {
            void* ptr;
            usize size;
            bool in_use;
        };

        class StaticMemoryPool {
        private:
            alignas(8) u8 memory_pool_[MEMORY_POOL_SIZE];
            MemoryBlock blocks_[MAX_ALLOCATIONS];
            usize next_offset_;
            usize num_allocations_;

        public:
            StaticMemoryPool() : next_offset_(0), num_allocations_(0) {
                for (usize i = 0; i < MAX_ALLOCATIONS; ++i) {
                    blocks_[i] = {nullptr, 0, false};
                }
            }

            void* allocate(usize size, usize alignment = 8) {
                // Align size to boundary
                size = (size + alignment - 1) & ~(alignment - 1);

                // Check if we have space
                if (next_offset_ + size > MEMORY_POOL_SIZE || num_allocations_ >= MAX_ALLOCATIONS) {
                    return nullptr;
                }

                // Find free block entry
                usize block_index = MAX_ALLOCATIONS;
                for (usize i = 0; i < MAX_ALLOCATIONS; ++i) {
                    if (!blocks_[i].in_use) {
                        block_index = i;
                        break;
                    }
                }

                if (block_index == MAX_ALLOCATIONS) {
                    return nullptr;
                }

                // Allocate memory
                void* ptr = memory_pool_ + next_offset_;
                next_offset_ += size;

                // Record allocation
                blocks_[block_index] = {ptr, size, true};
                ++num_allocations_;

                return ptr;
            }

            void deallocate(void* ptr) {
                if (!ptr) return;

                // Find and free block
                for (usize i = 0; i < MAX_ALLOCATIONS; ++i) {
                    if (blocks_[i].in_use && blocks_[i].ptr == ptr) {
                        blocks_[i].in_use = false;
                        blocks_[i].ptr = nullptr;
                        blocks_[i].size = 0;
                        --num_allocations_;

                        // Note: We don't reclaim memory in this simple implementation
                        // In a real kernel, we'd implement a more sophisticated allocator
                        break;
                    }
                }
            }

            usize get_used_memory() const {
                return next_offset_;
            }

            usize get_num_allocations() const {
                return num_allocations_;
            }
        };

        // Global memory pool instance
        inline StaticMemoryPool& get_memory_pool() {
            static StaticMemoryPool pool;
            return pool;
        }
    }

    // ========================================================================
    // Memory allocation functions
    // ========================================================================

    // Note: These are simple wrappers around our static pool
    // In a real kernel, these would integrate with the kernel's memory manager
    inline void* allocate_memory(usize size) {
        return detail::get_memory_pool().allocate(size);
    }

    inline void deallocate_memory(void* ptr) {
        detail::get_memory_pool().deallocate(ptr);
    }

    // ========================================================================
    // Default deleter
    // ========================================================================

    template<typename T>
    struct default_delete {
        constexpr default_delete() noexcept = default;

        template<typename U, typename = enable_if_t<is_convertible_v<U*, T*>>>
        default_delete(const default_delete<U>&) noexcept {}

        void operator()(T* ptr) const noexcept {
            static_assert(!is_void_v<T>, "Cannot delete incomplete type");
            static_assert(sizeof(T) > 0, "Cannot delete incomplete type");

            if (ptr) {
                ptr->~T();
                deallocate_memory(ptr);
            }
        }
    };

    // Specialization for arrays
    template<typename T>
    struct default_delete<T[]> {
        constexpr default_delete() noexcept = default;

        template<typename U, typename = enable_if_t<is_convertible_v<U(*)[], T(*)[]>>>
        default_delete(const default_delete<U[]>&) noexcept {}

        template<typename U>
        void operator()(U* ptr) const noexcept {
            static_assert(is_convertible_v<U(*)[], T(*)[]>, "Invalid array delete");
            // Note: Array delete not fully implemented in this simple version
            if (ptr) {
                deallocate_memory(ptr);
            }
        }
    };

    // ========================================================================
    // unique_ptr implementation
    // ========================================================================

    template<typename T, typename Deleter = default_delete<T>>
    class unique_ptr {
    public:
        using pointer = T*;
        using element_type = T;
        using deleter_type = Deleter;

    private:
        pointer ptr_;
        Deleter deleter_;

    public:
        // Constructors
        unique_ptr() noexcept : ptr_(nullptr), deleter_() {}

        unique_ptr(nullptr_t) noexcept : ptr_(nullptr), deleter_() {}

        explicit unique_ptr(pointer p) noexcept : ptr_(p), deleter_() {}

        unique_ptr(pointer p, const Deleter& d) noexcept : ptr_(p), deleter_(d) {}

        unique_ptr(pointer p, Deleter&& d) noexcept : ptr_(p), deleter_(static_cast<Deleter&&>(d)) {}

        // Move constructor
        unique_ptr(unique_ptr&& other) noexcept : ptr_(other.ptr_), deleter_(static_cast<Deleter&&>(other.deleter_)) {
            other.ptr_ = nullptr;
        }

        template<typename U, typename E, typename = enable_if_t<is_convertible_v<typename unique_ptr<U, E>::pointer, pointer>>>
        unique_ptr(unique_ptr<U, E>&& other) noexcept : ptr_(other.release()), deleter_(static_cast<E&&>(other.get_deleter())) {}

        // Destructor
        ~unique_ptr() {
            if (ptr_) {
                deleter_(ptr_);
            }
        }

        // Assignment
        unique_ptr& operator=(unique_ptr&& other) noexcept {
            if (this != &other) {
                reset(other.release());
                deleter_ = static_cast<Deleter&&>(other.deleter_);
            }
            return *this;
        }

        template<typename U, typename E>
        unique_ptr& operator=(unique_ptr<U, E>&& other) noexcept {
            reset(other.release());
            deleter_ = static_cast<E&&>(other.get_deleter());
            return *this;
        }

        unique_ptr& operator=(nullptr_t) noexcept {
            reset();
            return *this;
        }

        // Observers
        pointer get() const noexcept {
            return ptr_;
        }

        deleter_type& get_deleter() noexcept {
            return deleter_;
        }

        const deleter_type& get_deleter() const noexcept {
            return deleter_;
        }

        explicit operator bool() const noexcept {
            return ptr_ != nullptr;
        }

        // Dereference
        typename add_lvalue_reference<T>::type operator*() const noexcept {
            return *ptr_;
        }

        pointer operator->() const noexcept {
            return ptr_;
        }

        // Modifiers
        pointer release() noexcept {
            pointer p = ptr_;
            ptr_ = nullptr;
            return p;
        }

        void reset(pointer p = pointer()) noexcept {
            pointer old_p = ptr_;
            ptr_ = p;
            if (old_p) {
                deleter_(old_p);
            }
        }

        void swap(unique_ptr& other) noexcept {
            pointer temp_ptr = ptr_;
            ptr_ = other.ptr_;
            other.ptr_ = temp_ptr;

            // Simple swap for deleter (assumes it's trivial)
            Deleter temp_deleter = static_cast<Deleter&&>(deleter_);
            deleter_ = static_cast<Deleter&&>(other.deleter_);
            other.deleter_ = static_cast<Deleter&&>(temp_deleter);
        }

        // Disable copy
        unique_ptr(const unique_ptr&) = delete;
        unique_ptr& operator=(const unique_ptr&) = delete;
    };

    // Array specialization
    template<typename T, typename Deleter>
    class unique_ptr<T[], Deleter> {
    public:
        using pointer = T*;
        using element_type = T;
        using deleter_type = Deleter;

    private:
        pointer ptr_;
        Deleter deleter_;

    public:
        unique_ptr() noexcept : ptr_(nullptr), deleter_() {}
        unique_ptr(nullptr_t) noexcept : ptr_(nullptr), deleter_() {}
        explicit unique_ptr(pointer p) noexcept : ptr_(p), deleter_() {}

        unique_ptr(unique_ptr&& other) noexcept : ptr_(other.ptr_), deleter_(static_cast<Deleter&&>(other.deleter_)) {
            other.ptr_ = nullptr;
        }

        ~unique_ptr() {
            if (ptr_) {
                deleter_(ptr_);
            }
        }

        unique_ptr& operator=(unique_ptr&& other) noexcept {
            if (this != &other) {
                reset(other.release());
                deleter_ = static_cast<Deleter&&>(other.deleter_);
            }
            return *this;
        }

        unique_ptr& operator=(nullptr_t) noexcept {
            reset();
            return *this;
        }

        T& operator[](usize i) const noexcept {
            return ptr_[i];
        }

        pointer get() const noexcept {
            return ptr_;
        }

        deleter_type& get_deleter() noexcept {
            return deleter_;
        }

        const deleter_type& get_deleter() const noexcept {
            return deleter_;
        }

        explicit operator bool() const noexcept {
            return ptr_ != nullptr;
        }

        pointer release() noexcept {
            pointer p = ptr_;
            ptr_ = nullptr;
            return p;
        }

        void reset(pointer p = pointer()) noexcept {
            pointer old_p = ptr_;
            ptr_ = p;
            if (old_p) {
                deleter_(old_p);
            }
        }

        void swap(unique_ptr& other) noexcept {
            pointer temp_ptr = ptr_;
            ptr_ = other.ptr_;
            other.ptr_ = temp_ptr;
        }

        // Disable copy
        unique_ptr(const unique_ptr&) = delete;
        unique_ptr& operator=(const unique_ptr&) = delete;
    };

    // ========================================================================
    // make_unique implementation
    // ========================================================================

    template<typename T, typename... Args>
    unique_ptr<T> make_unique(Args&&... args) {
        static_assert(!is_array_v<T>, "make_unique does not support arrays");

        void* memory = allocate_memory(sizeof(T));
        if (!memory) {
            return unique_ptr<T>(nullptr);
        }

        T* ptr = new(memory) T(static_cast<Args&&>(args)...);
        return unique_ptr<T>(ptr);
    }

    // ========================================================================
    // Basic shared_ptr implementation (simplified)
    // ========================================================================

    namespace detail {
        struct ControlBlock {
            usize ref_count;
            usize weak_count;

            ControlBlock() : ref_count(1), weak_count(1) {}
            virtual ~ControlBlock() = default;
            virtual void destroy_object() = 0;
            virtual void destroy_self() = 0;
        };

        template<typename T, typename Deleter>
        struct ControlBlockPtr : ControlBlock {
            T* ptr;
            Deleter deleter;

            ControlBlockPtr(T* p, Deleter d) : ptr(p), deleter(d) {}

            void destroy_object() override {
                if (ptr) {
                    deleter(ptr);
                    ptr = nullptr;
                }
            }

            void destroy_self() override {
                deallocate_memory(this);
            }
        };
    }

    template<typename T>
    class shared_ptr {
    public:
        using element_type = T;
        using pointer = T*;

    private:
        pointer ptr_;
        detail::ControlBlock* control_;

        void add_ref() {
            if (control_) {
                ++control_->ref_count;
            }
        }

        void release() {
            if (control_) {
                --control_->ref_count;
                if (control_->ref_count == 0) {
                    control_->destroy_object();
                    --control_->weak_count;
                    if (control_->weak_count == 0) {
                        control_->destroy_self();
                    }
                }
            }
        }

    public:
        // Constructors
        shared_ptr() noexcept : ptr_(nullptr), control_(nullptr) {}

        shared_ptr(nullptr_t) noexcept : ptr_(nullptr), control_(nullptr) {}

        template<typename Y>
        explicit shared_ptr(Y* ptr) : ptr_(ptr), control_(nullptr) {
            if (ptr) {
                void* memory = allocate_memory(sizeof(detail::ControlBlockPtr<Y, default_delete<Y>>));
                if (memory) {
                    control_ = new(memory) detail::ControlBlockPtr<Y, default_delete<Y>>(ptr, default_delete<Y>{});
                } else {
                    delete ptr; // Fallback if control block allocation fails
                    ptr_ = nullptr;
                }
            }
        }

        // Copy constructor
        shared_ptr(const shared_ptr& other) noexcept : ptr_(other.ptr_), control_(other.control_) {
            add_ref();
        }

        template<typename Y>
        shared_ptr(const shared_ptr<Y>& other) noexcept : ptr_(other.ptr_), control_(other.control_) {
            add_ref();
        }

        // Move constructor
        shared_ptr(shared_ptr&& other) noexcept : ptr_(other.ptr_), control_(other.control_) {
            other.ptr_ = nullptr;
            other.control_ = nullptr;
        }

        template<typename Y>
        shared_ptr(shared_ptr<Y>&& other) noexcept : ptr_(other.ptr_), control_(other.control_) {
            other.ptr_ = nullptr;
            other.control_ = nullptr;
        }

        // Destructor
        ~shared_ptr() {
            release();
        }

        // Assignment
        shared_ptr& operator=(const shared_ptr& other) noexcept {
            if (this != &other) {
                release();
                ptr_ = other.ptr_;
                control_ = other.control_;
                add_ref();
            }
            return *this;
        }

        shared_ptr& operator=(shared_ptr&& other) noexcept {
            if (this != &other) {
                release();
                ptr_ = other.ptr_;
                control_ = other.control_;
                other.ptr_ = nullptr;
                other.control_ = nullptr;
            }
            return *this;
        }

        shared_ptr& operator=(nullptr_t) noexcept {
            release();
            ptr_ = nullptr;
            control_ = nullptr;
            return *this;
        }

        // Observers
        pointer get() const noexcept {
            return ptr_;
        }

        T& operator*() const noexcept {
            return *ptr_;
        }

        pointer operator->() const noexcept {
            return ptr_;
        }

        usize use_count() const noexcept {
            return control_ ? control_->ref_count : 0;
        }

        bool unique() const noexcept {
            return use_count() == 1;
        }

        explicit operator bool() const noexcept {
            return ptr_ != nullptr;
        }

        // Modifiers
        void reset() noexcept {
            *this = shared_ptr();
        }

        template<typename Y>
        void reset(Y* ptr) {
            *this = shared_ptr(ptr);
        }

        void swap(shared_ptr& other) noexcept {
            pointer temp_ptr = ptr_;
            ptr_ = other.ptr_;
            other.ptr_ = temp_ptr;

            detail::ControlBlock* temp_control = control_;
            control_ = other.control_;
            other.control_ = temp_control;
        }
    };

    // ========================================================================
    // Utility functions
    // ========================================================================

    template<typename T, typename U>
    bool operator==(const shared_ptr<T>& lhs, const shared_ptr<U>& rhs) noexcept {
        return lhs.get() == rhs.get();
    }

    template<typename T>
    bool operator==(const shared_ptr<T>& lhs, nullptr_t) noexcept {
        return !lhs;
    }

    template<typename T>
    bool operator==(nullptr_t, const shared_ptr<T>& rhs) noexcept {
        return !rhs;
    }

    template<typename T, typename U>
    bool operator!=(const shared_ptr<T>& lhs, const shared_ptr<U>& rhs) noexcept {
        return !(lhs == rhs);
    }

    template<typename T>
    bool operator!=(const shared_ptr<T>& lhs, nullptr_t) noexcept {
        return static_cast<bool>(lhs);
    }

    template<typename T>
    bool operator!=(nullptr_t, const shared_ptr<T>& rhs) noexcept {
        return static_cast<bool>(rhs);
    }

} // namespace std
