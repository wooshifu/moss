// src/modules/std.cppm
// MOSS Standard Library Module - Freestanding C++20 Implementation
// Provides all basic types, type traits, and utility functions for kernel use

module;

export module moss.std;

// Basic type definitions
export namespace moss {

// Size types
using size_t = __SIZE_TYPE__;
using ptrdiff_t = __PTRDIFF_TYPE__;
using intptr_t = __INTPTR_TYPE__;
using uintptr_t = __UINTPTR_TYPE__;

// Standard integer types
using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;

using i8 = signed char;
using i16 = signed short;
using i32 = signed int;
using i64 = signed long long;

// Physical address type for kernel use
using PhysAddr = u64;
using VirtAddr = u64;

// Byte type
enum class byte : unsigned char {};

// Null pointer type
using nullptr_t = decltype(nullptr);

} // namespace moss

// Type traits implementation
export namespace moss {

// Primary type categories
template<typename T> struct remove_const { using type = T; };
template<typename T> struct remove_const<const T> { using type = T; };
template<typename T> using remove_const_t = typename remove_const<T>::type;

template<typename T> struct remove_volatile { using type = T; };
template<typename T> struct remove_volatile<volatile T> { using type = T; };
template<typename T> using remove_volatile_t = typename remove_volatile<T>::type;

template<typename T> struct remove_cv {
    using type = remove_volatile_t<remove_const_t<T>>;
};
template<typename T> using remove_cv_t = typename remove_cv<T>::type;

template<typename T> struct remove_reference { using type = T; };
template<typename T> struct remove_reference<T&> { using type = T; };
template<typename T> struct remove_reference<T&&> { using type = T; };
template<typename T> using remove_reference_t = typename remove_reference<T>::type;

template<typename T> struct remove_pointer { using type = T; };
template<typename T> struct remove_pointer<T*> { using type = T; };
template<typename T> using remove_pointer_t = typename remove_pointer<T>::type;

// Type relationships
template<typename T, typename U> struct is_same { static constexpr bool value = false; };
template<typename T> struct is_same<T, T> { static constexpr bool value = true; };
template<typename T, typename U> inline constexpr bool is_same_v = is_same<T, U>::value;

// Type properties
template<typename T> struct is_const { static constexpr bool value = false; };
template<typename T> struct is_const<const T> { static constexpr bool value = true; };
template<typename T> inline constexpr bool is_const_v = is_const<T>::value;

template<typename T> struct is_volatile { static constexpr bool value = false; };
template<typename T> struct is_volatile<volatile T> { static constexpr bool value = true; };
template<typename T> inline constexpr bool is_volatile_v = is_volatile<T>::value;

template<typename T> struct is_void { static constexpr bool value = is_same_v<remove_cv_t<T>, void>; };
template<typename T> inline constexpr bool is_void_v = is_void<T>::value;

template<typename T> struct is_integral { static constexpr bool value = false; };
template<> struct is_integral<bool> { static constexpr bool value = true; };
template<> struct is_integral<char> { static constexpr bool value = true; };
template<> struct is_integral<signed char> { static constexpr bool value = true; };
template<> struct is_integral<unsigned char> { static constexpr bool value = true; };
template<> struct is_integral<short> { static constexpr bool value = true; };
template<> struct is_integral<unsigned short> { static constexpr bool value = true; };
template<> struct is_integral<int> { static constexpr bool value = true; };
template<> struct is_integral<unsigned int> { static constexpr bool value = true; };
template<> struct is_integral<long> { static constexpr bool value = true; };
template<> struct is_integral<unsigned long> { static constexpr bool value = true; };
template<> struct is_integral<long long> { static constexpr bool value = true; };
template<> struct is_integral<unsigned long long> { static constexpr bool value = true; };
template<typename T> inline constexpr bool is_integral_v = is_integral<remove_cv_t<T>>::value;

template<typename T> struct is_floating_point { static constexpr bool value = false; };
template<> struct is_floating_point<float> { static constexpr bool value = true; };
template<> struct is_floating_point<double> { static constexpr bool value = true; };
template<> struct is_floating_point<long double> { static constexpr bool value = true; };
template<typename T> inline constexpr bool is_floating_point_v = is_floating_point<remove_cv_t<T>>::value;

template<typename T> struct is_array { static constexpr bool value = false; };
template<typename T> struct is_array<T[]> { static constexpr bool value = true; };
template<typename T, size_t N> struct is_array<T[N]> { static constexpr bool value = true; };
template<typename T> inline constexpr bool is_array_v = is_array<T>::value;

template<typename T> struct is_pointer { static constexpr bool value = false; };
template<typename T> struct is_pointer<T*> { static constexpr bool value = true; };
template<typename T> inline constexpr bool is_pointer_v = is_pointer<remove_cv_t<T>>::value;

template<typename T> struct is_lvalue_reference { static constexpr bool value = false; };
template<typename T> struct is_lvalue_reference<T&> { static constexpr bool value = true; };
template<typename T> inline constexpr bool is_lvalue_reference_v = is_lvalue_reference<T>::value;

template<typename T> struct is_rvalue_reference { static constexpr bool value = false; };
template<typename T> struct is_rvalue_reference<T&&> { static constexpr bool value = true; };
template<typename T> inline constexpr bool is_rvalue_reference_v = is_rvalue_reference<T>::value;

template<typename T> struct is_reference {
    static constexpr bool value = is_lvalue_reference_v<T> || is_rvalue_reference_v<T>;
};
template<typename T> inline constexpr bool is_reference_v = is_reference<T>::value;

// Conditional type selection
template<bool B, typename T, typename F> struct conditional { using type = T; };
template<typename T, typename F> struct conditional<false, T, F> { using type = F; };
template<bool B, typename T, typename F> using conditional_t = typename conditional<B, T, F>::type;

// Enable if
template<bool B, typename T = void> struct enable_if {};
template<typename T> struct enable_if<true, T> { using type = T; };
template<bool B, typename T = void> using enable_if_t = typename enable_if<B, T>::type;

} // namespace moss

// Utility functions
export namespace moss {

// Move semantics
template<typename T>
constexpr remove_reference_t<T>&& move(T&& t) noexcept {
    return static_cast<remove_reference_t<T>&&>(t);
}

// Perfect forwarding
template<typename T>
constexpr T&& forward(remove_reference_t<T>& t) noexcept {
    return static_cast<T&&>(t);
}

template<typename T>
constexpr T&& forward(remove_reference_t<T>&& t) noexcept {
    static_assert(!is_lvalue_reference_v<T>, "Cannot forward an rvalue as an lvalue");
    return static_cast<T&&>(t);
}

// Exchange
template<typename T, typename U = T>
constexpr T exchange(T& obj, U&& new_value) noexcept {
    T old_value = move(obj);
    obj = forward<U>(new_value);
    return old_value;
}

// Swap
template<typename T>
constexpr void swap(T& a, T& b) noexcept {
    T temp = move(a);
    a = move(b);
    b = move(temp);
}

// Min/max
template<typename T>
constexpr const T& min(const T& a, const T& b) {
    return (b < a) ? b : a;
}

template<typename T>
constexpr const T& max(const T& a, const T& b) {
    return (a < b) ? b : a;
}

// Clamp
template<typename T>
constexpr const T& clamp(const T& v, const T& lo, const T& hi) {
    return (v < lo) ? lo : (hi < v) ? hi : v;
}

} // namespace moss

// Memory operations (freestanding implementations)
export namespace moss {

// Memory set
constexpr void* memset(void* dest, int ch, size_t count) noexcept {
    unsigned char* d = static_cast<unsigned char*>(dest);
    unsigned char c = static_cast<unsigned char>(ch);
    for (size_t i = 0; i < count; ++i) {
        d[i] = c;
    }
    return dest;
}

// Memory copy
constexpr void* memcpy(void* dest, const void* src, size_t count) noexcept {
    unsigned char* d = static_cast<unsigned char*>(dest);
    const unsigned char* s = static_cast<const unsigned char*>(src);
    for (size_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }
    return dest;
}

// Memory move (handles overlapping regions)
constexpr void* memmove(void* dest, const void* src, size_t count) noexcept {
    unsigned char* d = static_cast<unsigned char*>(dest);
    const unsigned char* s = static_cast<const unsigned char*>(src);

    if (d < s) {
        // Copy forward
        for (size_t i = 0; i < count; ++i) {
            d[i] = s[i];
        }
    } else if (d > s) {
        // Copy backward
        for (size_t i = count; i > 0; --i) {
            d[i-1] = s[i-1];
        }
    }
    return dest;
}

// Memory compare
constexpr int memcmp(const void* lhs, const void* rhs, size_t count) noexcept {
    const unsigned char* l = static_cast<const unsigned char*>(lhs);
    const unsigned char* r = static_cast<const unsigned char*>(rhs);

    for (size_t i = 0; i < count; ++i) {
        if (l[i] < r[i]) return -1;
        if (l[i] > r[i]) return 1;
    }
    return 0;
}

} // namespace moss

// Memory ordering for atomics
export namespace moss {

enum class memory_order : int {
    relaxed = 0,
    consume = 1,
    acquire = 2,
    release = 3,
    acq_rel = 4,
    seq_cst = 5
};

inline constexpr memory_order memory_order_relaxed = memory_order::relaxed;
inline constexpr memory_order memory_order_consume = memory_order::consume;
inline constexpr memory_order memory_order_acquire = memory_order::acquire;
inline constexpr memory_order memory_order_release = memory_order::release;
inline constexpr memory_order memory_order_acq_rel = memory_order::acq_rel;
inline constexpr memory_order memory_order_seq_cst = memory_order::seq_cst;

} // namespace moss

// Atomic operations support
export namespace moss {

// Basic atomic type template
template<typename T>
struct atomic {
    static_assert(is_integral_v<T> || is_pointer_v<T>, "Atomic type must be integral or pointer");

private:
    T value_{};

public:
    using value_type = T;

    atomic() noexcept = default;
    atomic(T desired) noexcept : value_(desired) {}

    atomic(const atomic&) = delete;
    atomic& operator=(const atomic&) = delete;

    T load(memory_order order = memory_order_seq_cst) const noexcept {
        return __atomic_load_n(&value_, static_cast<int>(order));
    }

    void store(T desired, memory_order order = memory_order_seq_cst) noexcept {
        __atomic_store_n(&value_, desired, static_cast<int>(order));
    }

    T exchange(T desired, memory_order order = memory_order_seq_cst) noexcept {
        return __atomic_exchange_n(&value_, desired, static_cast<int>(order));
    }

    bool compare_exchange_weak(T& expected, T desired,
                             memory_order success = memory_order_seq_cst,
                             memory_order failure = memory_order_seq_cst) noexcept {
        return __atomic_compare_exchange_n(&value_, &expected, desired, true,
                                         static_cast<int>(success), static_cast<int>(failure));
    }

    bool compare_exchange_strong(T& expected, T desired,
                               memory_order success = memory_order_seq_cst,
                               memory_order failure = memory_order_seq_cst) noexcept {
        return __atomic_compare_exchange_n(&value_, &expected, desired, false,
                                         static_cast<int>(success), static_cast<int>(failure));
    }

    // Atomic arithmetic operations (for integral types)
    template<typename U = T>
    enable_if_t<is_integral_v<U>, T> fetch_add(T arg, memory_order order = memory_order_seq_cst) noexcept {
        return __atomic_fetch_add(&value_, arg, static_cast<int>(order));
    }

    template<typename U = T>
    enable_if_t<is_integral_v<U>, T> fetch_sub(T arg, memory_order order = memory_order_seq_cst) noexcept {
        return __atomic_fetch_sub(&value_, arg, static_cast<int>(order));
    }

    template<typename U = T>
    enable_if_t<is_integral_v<U>, T> fetch_and(T arg, memory_order order = memory_order_seq_cst) noexcept {
        return __atomic_fetch_and(&value_, arg, static_cast<int>(order));
    }

    template<typename U = T>
    enable_if_t<is_integral_v<U>, T> fetch_or(T arg, memory_order order = memory_order_seq_cst) noexcept {
        return __atomic_fetch_or(&value_, arg, static_cast<int>(order));
    }

    template<typename U = T>
    enable_if_t<is_integral_v<U>, T> fetch_xor(T arg, memory_order order = memory_order_seq_cst) noexcept {
        return __atomic_fetch_xor(&value_, arg, static_cast<int>(order));
    }

    // Operators
    operator T() const noexcept {
        return load();
    }

    T operator=(T desired) noexcept {
        store(desired);
        return desired;
    }

    template<typename U = T>
    enable_if_t<is_integral_v<U>, T> operator++() noexcept {
        return fetch_add(1) + 1;
    }

    template<typename U = T>
    enable_if_t<is_integral_v<U>, T> operator++(int) noexcept {
        return fetch_add(1);
    }

    template<typename U = T>
    enable_if_t<is_integral_v<U>, T> operator--() noexcept {
        return fetch_sub(1) - 1;
    }

    template<typename U = T>
    enable_if_t<is_integral_v<U>, T> operator--(int) noexcept {
        return fetch_sub(1);
    }

    template<typename U = T>
    enable_if_t<is_integral_v<U>, T> operator+=(T arg) noexcept {
        return fetch_add(arg) + arg;
    }

    template<typename U = T>
    enable_if_t<is_integral_v<U>, T> operator-=(T arg) noexcept {
        return fetch_sub(arg) - arg;
    }

    template<typename U = T>
    enable_if_t<is_integral_v<U>, T> operator&=(T arg) noexcept {
        return fetch_and(arg) & arg;
    }

    template<typename U = T>
    enable_if_t<is_integral_v<U>, T> operator|=(T arg) noexcept {
        return fetch_or(arg) | arg;
    }

    template<typename U = T>
    enable_if_t<is_integral_v<U>, T> operator^=(T arg) noexcept {
        return fetch_xor(arg) ^ arg;
    }
};

// Common atomic type aliases
using atomic_bool = atomic<bool>;
using atomic_char = atomic<char>;
using atomic_schar = atomic<signed char>;
using atomic_uchar = atomic<unsigned char>;
using atomic_short = atomic<short>;
using atomic_ushort = atomic<unsigned short>;
using atomic_int = atomic<int>;
using atomic_uint = atomic<unsigned int>;
using atomic_long = atomic<long>;
using atomic_ulong = atomic<unsigned long>;
using atomic_llong = atomic<long long>;
using atomic_ullong = atomic<unsigned long long>;
using atomic_size_t = atomic<size_t>;
using atomic_ptrdiff_t = atomic<ptrdiff_t>;
using atomic_intptr_t = atomic<intptr_t>;
using atomic_uintptr_t = atomic<uintptr_t>;

// Atomic utility functions
template<typename T>
T atomic_load(const atomic<T>* obj) noexcept {
    return obj->load();
}

template<typename T>
void atomic_store(atomic<T>* obj, T desired) noexcept {
    obj->store(desired);
}

template<typename T>
T atomic_exchange(atomic<T>* obj, T desired) noexcept {
    return obj->exchange(desired);
}

} // namespace moss

// Placement new operators - must be in global namespace
export {

// Placement new
inline void* operator new(moss::size_t, void* ptr) noexcept {
    return ptr;
}

inline void* operator new[](moss::size_t, void* ptr) noexcept {
    return ptr;
}

// Placement delete (for completeness)
inline void operator delete(void*, void*) noexcept {}
inline void operator delete[](void*, void*) noexcept {}

}

// Memory alignment utilities
export namespace moss {

// Alignment helpers
template<size_t Align>
constexpr size_t align_up(size_t value) noexcept {
    static_assert((Align & (Align - 1)) == 0, "Alignment must be power of 2");
    return (value + Align - 1) & ~(Align - 1);
}

template<size_t Align>
constexpr size_t align_down(size_t value) noexcept {
    static_assert((Align & (Align - 1)) == 0, "Alignment must be power of 2");
    return value & ~(Align - 1);
}

template<size_t Align>
constexpr bool is_aligned(const void* ptr) noexcept {
    static_assert((Align & (Align - 1)) == 0, "Alignment must be power of 2");
    return (reinterpret_cast<uintptr_t>(ptr) & (Align - 1)) == 0;
}

// Alignment constants commonly used in kernel
inline constexpr size_t CACHE_LINE_SIZE = 64;
inline constexpr size_t PAGE_SIZE = 4096;
inline constexpr size_t WORD_SIZE = sizeof(void*);

} // namespace moss