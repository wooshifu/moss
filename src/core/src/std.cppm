// MOSS Standard Library Module - Freestanding C++26 Implementation
// Provides all basic types, type traits, and utility functions for kernel use
// Type traits and memory ops use Clang builtins for correctness and codegen.

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

// Floating point types
using f32 = float;
using f64 = double;

// Physical/virtual address and page types
using PhysAddr = u64;
using VirtAddr = u64;
using PageFrame = u64;

// Byte type
enum class byte : unsigned char {};

// Null pointer and max alignment types
using nullptr_t = decltype(nullptr);
using max_align_t = long double;

// Integer limits
inline constexpr u8 UINT8_MAX = 255U;
inline constexpr u16 UINT16_MAX = 65535U;
inline constexpr u32 UINT32_MAX = 4294967295U;
inline constexpr u64 UINT64_MAX = 18446744073709551615ULL;

} // namespace moss

// ============================================================================
// Type traits — Clang builtin type transforms & intrinsics
// ============================================================================
export namespace moss {

// --- Type transformations (Clang TransformTypeTraits) -----------------------

template <typename T> using remove_const_t = __remove_const(T);
template <typename T> struct remove_const {
  using type = remove_const_t<T>;
};

template <typename T> using remove_volatile_t = __remove_volatile(T);
template <typename T> struct remove_volatile {
  using type = remove_volatile_t<T>;
};

template <typename T> using remove_cv_t = __remove_cv(T);
template <typename T> struct remove_cv {
  using type = remove_cv_t<T>;
};

template <typename T> using remove_reference_t = __remove_reference_t(T);
template <typename T> struct remove_reference {
  using type = remove_reference_t<T>;
};

template <typename T> using remove_pointer_t = __remove_pointer(T);
template <typename T> struct remove_pointer {
  using type = remove_pointer_t<T>;
};

// --- Type property traits (Clang __is_* intrinsics) -------------------------

template <typename T, typename U> inline constexpr bool is_same_v = __is_same(T, U);
template <typename T, typename U> struct is_same {
  static constexpr bool value = is_same_v<T, U>;
};

template <typename T> inline constexpr bool is_const_v = __is_const(T);
template <typename T> struct is_const {
  static constexpr bool value = is_const_v<T>;
};

template <typename T> inline constexpr bool is_volatile_v = __is_volatile(T);
template <typename T> struct is_volatile {
  static constexpr bool value = is_volatile_v<T>;
};

template <typename T> inline constexpr bool is_void_v = __is_void(T);
template <typename T> struct is_void {
  static constexpr bool value = is_void_v<T>;
};

template <typename T> inline constexpr bool is_integral_v = __is_integral(T);
template <typename T> struct is_integral {
  static constexpr bool value = is_integral_v<T>;
};

template <typename T> inline constexpr bool is_floating_point_v = __is_floating_point(T);
template <typename T> struct is_floating_point {
  static constexpr bool value = is_floating_point_v<T>;
};

template <typename T> inline constexpr bool is_array_v = __is_array(T);
template <typename T> struct is_array {
  static constexpr bool value = is_array_v<T>;
};

template <typename T> inline constexpr bool is_pointer_v = __is_pointer(T);
template <typename T> struct is_pointer {
  static constexpr bool value = is_pointer_v<T>;
};

template <typename T> inline constexpr bool is_lvalue_reference_v = __is_lvalue_reference(T);
template <typename T> struct is_lvalue_reference {
  static constexpr bool value = is_lvalue_reference_v<T>;
};

template <typename T> inline constexpr bool is_rvalue_reference_v = __is_rvalue_reference(T);
template <typename T> struct is_rvalue_reference {
  static constexpr bool value = is_rvalue_reference_v<T>;
};

template <typename T> inline constexpr bool is_reference_v = __is_reference(T);
template <typename T> struct is_reference {
  static constexpr bool value = is_reference_v<T>;
};

template <typename T> inline constexpr bool is_function_v = __is_function(T);
template <typename T> struct is_function {
  static constexpr bool value = is_function_v<T>;
};

// --- Conditional / enable_if (no builtin equivalent) ------------------------

template <bool B, typename T, typename F> struct conditional {
  using type = T;
};
template <typename T, typename F> struct conditional<false, T, F> {
  using type = F;
};
template <bool B, typename T, typename F> using conditional_t = typename conditional<B, T, F>::type;

template <bool B, typename T = void> struct enable_if {};
template <typename T> struct enable_if<true, T> {
  using type = T;
};
template <bool B, typename T = void> using enable_if_t = typename enable_if<B, T>::type;

// --- Nothrow traits (Clang __is_nothrow_* intrinsics) -----------------------

template <typename T> inline constexpr bool is_nothrow_copy_constructible_v = __is_nothrow_constructible(T, const T &);
template <typename T> struct is_nothrow_copy_constructible {
  static constexpr bool value = is_nothrow_copy_constructible_v<T>;
};

template <typename T> inline constexpr bool is_nothrow_move_constructible_v = __is_nothrow_constructible(T, T &&);
template <typename T> struct is_nothrow_move_constructible {
  static constexpr bool value = is_nothrow_move_constructible_v<T>;
};

template <typename T> inline constexpr bool is_nothrow_copy_assignable_v = __is_nothrow_assignable(T &, const T &);
template <typename T> struct is_nothrow_copy_assignable {
  static constexpr bool value = is_nothrow_copy_assignable_v<T>;
};

template <typename T> inline constexpr bool is_nothrow_move_assignable_v = __is_nothrow_assignable(T &, T &&);
template <typename T> struct is_nothrow_move_assignable {
  static constexpr bool value = is_nothrow_move_assignable_v<T>;
};

template <typename T, typename... Args>
inline constexpr bool is_nothrow_constructible_v = __is_nothrow_constructible(T, Args...);
template <typename T, typename... Args> struct is_nothrow_constructible {
  static constexpr bool value = is_nothrow_constructible_v<T, Args...>;
};

template <typename T, typename U> inline constexpr bool is_nothrow_assignable_v = __is_nothrow_assignable(T, U);
template <typename T, typename U> struct is_nothrow_assignable {
  static constexpr bool value = is_nothrow_assignable_v<T, U>;
};

} // namespace moss

// ============================================================================
// Utility functions
// ============================================================================
export namespace moss {

template <typename T> constexpr remove_reference_t<T> &&move(T &&t) noexcept {
  return static_cast<remove_reference_t<T> &&>(t);
}

template <typename T> constexpr T &&forward(remove_reference_t<T> &t) noexcept { return static_cast<T &&>(t); }

template <typename T> constexpr T &&forward(remove_reference_t<T> &&t) noexcept {
  static_assert(!is_lvalue_reference_v<T>, "Cannot forward an rvalue as an lvalue");
  return static_cast<T &&>(t);
}

template <typename T, typename U = T> constexpr T exchange(T &obj, U &&new_value) noexcept {
  T old_value = move(obj);
  obj = forward<U>(new_value);
  return old_value;
}

template <typename T> constexpr void swap(T &a, T &b) noexcept {
  T temp = move(a);
  a = move(b);
  b = move(temp);
}

template <typename T> constexpr const T &min(const T &a, const T &b) { return (b < a) ? b : a; }

template <typename T> constexpr const T &max(const T &a, const T &b) { return (a < b) ? b : a; }

template <typename T> constexpr const T &clamp(const T &v, const T &lo, const T &hi) {
  return (v < lo) ? lo : (hi < v) ? hi : v;
}

} // namespace moss

// ============================================================================
// Memory operations — delegated to Clang builtins
// ============================================================================
export namespace moss {

constexpr void *memset(void *dest, int ch, size_t count) noexcept { return __builtin_memset(dest, ch, count); }

constexpr void *memcpy(void *dest, const void *src, size_t count) noexcept {
  return __builtin_memcpy(dest, src, count);
}

constexpr void *memmove(void *dest, const void *src, size_t count) noexcept {
  return __builtin_memmove(dest, src, count);
}

constexpr int memcmp(const void *lhs, const void *rhs, size_t count) noexcept {
  return __builtin_memcmp(lhs, rhs, count);
}

template <typename T> constexpr T abs(const T &value) noexcept { return (value < 0) ? -value : value; }

} // namespace moss

// ============================================================================
// Memory ordering for atomics
// ============================================================================
export namespace moss {

// CamelCase MemoryOrder - used by 415+ callsites across the codebase
enum class MemoryOrder { Relaxed = 0, Consume = 1, Acquire = 2, Release = 3, AcqRel = 4, SeqCst = 5 };

// lowercase memory_order - std-compatible alias
enum class memory_order : int { relaxed = 0, consume = 1, acquire = 2, release = 3, acq_rel = 4, seq_cst = 5 };

inline constexpr memory_order memory_order_relaxed = memory_order::relaxed;
inline constexpr memory_order memory_order_consume = memory_order::consume;
inline constexpr memory_order memory_order_acquire = memory_order::acquire;
inline constexpr memory_order memory_order_release = memory_order::release;
inline constexpr memory_order memory_order_acq_rel = memory_order::acq_rel;
inline constexpr memory_order memory_order_seq_cst = memory_order::seq_cst;

// Integer conversion helpers for __atomic builtins
constexpr int memory_order_to_int(MemoryOrder order) noexcept { return static_cast<int>(order); }

constexpr int memory_order_to_int(memory_order order) noexcept { return static_cast<int>(order); }

// Atomic thread fence (multi-architecture support)
inline void atomic_thread_fence(MemoryOrder /*order*/) noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dmb sy" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("mfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence rw,rw" ::: "memory");
#else
  asm volatile("" ::: "memory"); // compiler barrier fallback
#endif
}

inline void atomic_thread_fence(memory_order /*order*/) noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dmb sy" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("mfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence rw,rw" ::: "memory");
#else
  asm volatile("" ::: "memory"); // compiler barrier fallback
#endif
}

inline void atomic_thread_fence(int /*order*/) noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dmb sy" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("mfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence rw,rw" ::: "memory");
#else
  asm volatile("" ::: "memory"); // compiler barrier fallback
#endif
}

} // namespace moss

// Atomic operations support
export namespace moss {

// Basic atomic type template
template <typename T> struct atomic {
  static_assert(is_integral_v<T> || is_pointer_v<T>, "Atomic type must be integral or pointer");

private:
  T value_{};

public:
  using value_type = T;

  atomic() noexcept = default;
  atomic(T desired) noexcept : value_(desired) {}

  atomic(const atomic &) = delete;
  atomic &operator=(const atomic &) = delete;

  T load(memory_order order = memory_order_seq_cst) const noexcept {
    return __atomic_load_n(&value_, static_cast<int>(order));
  }

  void store(T desired, memory_order order = memory_order_seq_cst) noexcept {
    __atomic_store_n(&value_, desired, static_cast<int>(order));
  }

  T exchange(T desired, memory_order order = memory_order_seq_cst) noexcept {
    return __atomic_exchange_n(&value_, desired, static_cast<int>(order));
  }

  bool compare_exchange_weak(T &expected, T desired, memory_order success = memory_order_seq_cst,
                             memory_order failure = memory_order_seq_cst) noexcept {
    return __atomic_compare_exchange_n(&value_, &expected, desired, true, static_cast<int>(success),
                                       static_cast<int>(failure));
  }

  bool compare_exchange_strong(T &expected, T desired, memory_order success = memory_order_seq_cst,
                               memory_order failure = memory_order_seq_cst) noexcept {
    return __atomic_compare_exchange_n(&value_, &expected, desired, false, static_cast<int>(success),
                                       static_cast<int>(failure));
  }

  // Atomic arithmetic operations (for integral types)
  template <typename U = T>
  enable_if_t<is_integral_v<U>, T> fetch_add(T arg, memory_order order = memory_order_seq_cst) noexcept {
    return __atomic_fetch_add(&value_, arg, static_cast<int>(order));
  }

  template <typename U = T>
  enable_if_t<is_integral_v<U>, T> fetch_sub(T arg, memory_order order = memory_order_seq_cst) noexcept {
    return __atomic_fetch_sub(&value_, arg, static_cast<int>(order));
  }

  template <typename U = T>
  enable_if_t<is_integral_v<U>, T> fetch_and(T arg, memory_order order = memory_order_seq_cst) noexcept {
    return __atomic_fetch_and(&value_, arg, static_cast<int>(order));
  }

  template <typename U = T>
  enable_if_t<is_integral_v<U>, T> fetch_or(T arg, memory_order order = memory_order_seq_cst) noexcept {
    return __atomic_fetch_or(&value_, arg, static_cast<int>(order));
  }

  template <typename U = T>
  enable_if_t<is_integral_v<U>, T> fetch_xor(T arg, memory_order order = memory_order_seq_cst) noexcept {
    return __atomic_fetch_xor(&value_, arg, static_cast<int>(order));
  }

  // Operators
  operator T() const noexcept { return load(); }

  T operator=(T desired) noexcept {
    store(desired);
    return desired;
  }

  template <typename U = T> enable_if_t<is_integral_v<U>, T> operator++() noexcept { return fetch_add(1) + 1; }

  template <typename U = T> enable_if_t<is_integral_v<U>, T> operator++(int) noexcept { return fetch_add(1); }

  template <typename U = T> enable_if_t<is_integral_v<U>, T> operator--() noexcept { return fetch_sub(1) - 1; }

  template <typename U = T> enable_if_t<is_integral_v<U>, T> operator--(int) noexcept { return fetch_sub(1); }

  template <typename U = T> enable_if_t<is_integral_v<U>, T> operator+=(T arg) noexcept { return fetch_add(arg) + arg; }

  template <typename U = T> enable_if_t<is_integral_v<U>, T> operator-=(T arg) noexcept { return fetch_sub(arg) - arg; }

  template <typename U = T> enable_if_t<is_integral_v<U>, T> operator&=(T arg) noexcept { return fetch_and(arg) & arg; }

  template <typename U = T> enable_if_t<is_integral_v<U>, T> operator|=(T arg) noexcept { return fetch_or(arg) | arg; }

  template <typename U = T> enable_if_t<is_integral_v<U>, T> operator^=(T arg) noexcept { return fetch_xor(arg) ^ arg; }
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
template <typename T> T atomic_load(const atomic<T> *obj) noexcept { return obj->load(); }

template <typename T> void atomic_store(atomic<T> *obj, T desired) noexcept { obj->store(desired); }

template <typename T> T atomic_exchange(atomic<T> *obj, T desired) noexcept { return obj->exchange(desired); }

} // namespace moss

// Placement new operators - must be in global namespace
export {

  // Placement new
  inline void *operator new(moss::size_t /*unused*/, void *ptr) noexcept { return ptr; }

  inline void *operator new[](moss::size_t /*unused*/, void *ptr) noexcept { return ptr; }

  // Placement delete (for completeness)
  inline void operator delete(void * /*unused*/, void * /*unused*/) noexcept {}
  inline void operator delete[](void * /*unused*/, void * /*unused*/) noexcept {}
}

// Memory alignment utilities
export namespace moss {

// Alignment helpers
template <size_t Align> constexpr size_t align_up(size_t value) noexcept {
  static_assert((Align & (Align - 1)) == 0, "Alignment must be power of 2");
  return (value + Align - 1) & ~(Align - 1);
}

template <size_t Align> constexpr size_t align_down(size_t value) noexcept {
  static_assert((Align & (Align - 1)) == 0, "Alignment must be power of 2");
  return value & ~(Align - 1);
}

template <size_t Align> constexpr bool is_aligned(const void *ptr) noexcept {
  static_assert((Align & (Align - 1)) == 0, "Alignment must be power of 2");
  return (reinterpret_cast<uintptr_t>(ptr) & (Align - 1)) == 0;
}

// Alignment constants commonly used in kernel
inline constexpr size_t CACHE_LINE_SIZE = 64;
inline constexpr size_t PAGE_SIZE = 4096;
inline constexpr size_t WORD_SIZE = sizeof(void *);

} // namespace moss
