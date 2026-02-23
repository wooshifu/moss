// -*- C++ -*- (Tell editors this is C++26 module code)
//
// SPDX-License-Identifier: MIT
//
// MOSS Kernel - Intrinsics Module: Atomic Operations
//
// This partition provides lock-free atomic operations using Clang intrinsics.
// All operations are sequentially consistent by default but support relaxed orderings.

export module moss.intrinsics:atomic;

/// Atomic operations using Clang intrinsics.
/// Provides type-safe wrappers around __atomic_* builtins.
export namespace moss::intrinsics::atomic {

// ============================================================================
// Memory Ordering
// ============================================================================

/// Memory ordering constraints for atomic operations (C++ compatible).
enum class memory_order : int {
  relaxed = __ATOMIC_RELAXED, ///< No synchronization or ordering constraints
  consume = __ATOMIC_CONSUME, ///< Data dependency ordering (deprecated)
  acquire = __ATOMIC_ACQUIRE, ///< Acquire semantics (reads)
  release = __ATOMIC_RELEASE, ///< Release semantics (writes)
  acq_rel = __ATOMIC_ACQ_REL, ///< Acquire + Release (read-modify-write)
  seq_cst = __ATOMIC_SEQ_CST  ///< Sequentially consistent (strongest)
};

// ============================================================================
// Atomic Load
// ============================================================================

/// Atomically load value from ptr.
/// Default: sequentially consistent ordering.
template <typename T> [[nodiscard]] T load(const T *ptr, memory_order order = memory_order::seq_cst) noexcept {
  return __atomic_load_n(ptr, static_cast<int>(order));
}

// ============================================================================
// Atomic Store
// ============================================================================

/// Atomically store val to ptr.
/// Default: sequentially consistent ordering.
template <typename T> void store(T *ptr, T val, memory_order order = memory_order::seq_cst) noexcept {
  __atomic_store_n(ptr, val, static_cast<int>(order));
}

// ============================================================================
// Atomic Exchange
// ============================================================================

/// Atomically replace value at ptr with val, return old value.
/// Default: sequentially consistent ordering.
template <typename T> [[nodiscard]] T exchange(T *ptr, T val, memory_order order = memory_order::seq_cst) noexcept {
  return __atomic_exchange_n(ptr, val, static_cast<int>(order));
}

// ============================================================================
// Compare-And-Swap (CAS)
// ============================================================================

/// Atomically compare *ptr with *expected, replace with desired if equal.
/// Strong variant: never spuriously fails.
/// Returns true if exchange succeeded, false otherwise.
/// On failure, *expected is updated with current value of *ptr.
template <typename T>
bool compare_exchange_strong(T *ptr, T *expected, T desired, memory_order success = memory_order::seq_cst,
                             memory_order failure = memory_order::seq_cst) noexcept {
  return __atomic_compare_exchange_n(ptr, expected, desired, false, static_cast<int>(success),
                                     static_cast<int>(failure));
}

/// Atomically compare *ptr with *expected, replace with desired if equal.
/// Weak variant: may spuriously fail even if *ptr == *expected.
/// Use in loops for better performance on some architectures (ARM LL/SC).
template <typename T>
bool compare_exchange_weak(T *ptr, T *expected, T desired, memory_order success = memory_order::seq_cst,
                           memory_order failure = memory_order::seq_cst) noexcept {
  return __atomic_compare_exchange_n(ptr, expected, desired, true, static_cast<int>(success),
                                     static_cast<int>(failure));
}

// ============================================================================
// Fetch-And-Modify Operations
// ============================================================================

/// Atomically add val to *ptr, return old value.
/// Default: sequentially consistent ordering.
template <typename T> [[nodiscard]] T fetch_add(T *ptr, T val, memory_order order = memory_order::seq_cst) noexcept {
  return __atomic_fetch_add(ptr, val, static_cast<int>(order));
}

/// Atomically subtract val from *ptr, return old value.
/// Default: sequentially consistent ordering.
template <typename T> [[nodiscard]] T fetch_sub(T *ptr, T val, memory_order order = memory_order::seq_cst) noexcept {
  return __atomic_fetch_sub(ptr, val, static_cast<int>(order));
}

/// Atomically AND *ptr with val, return old value.
/// Default: sequentially consistent ordering.
template <typename T> [[nodiscard]] T fetch_and(T *ptr, T val, memory_order order = memory_order::seq_cst) noexcept {
  return __atomic_fetch_and(ptr, val, static_cast<int>(order));
}

/// Atomically OR *ptr with val, return old value.
/// Default: sequentially consistent ordering.
template <typename T> [[nodiscard]] T fetch_or(T *ptr, T val, memory_order order = memory_order::seq_cst) noexcept {
  return __atomic_fetch_or(ptr, val, static_cast<int>(order));
}

/// Atomically XOR *ptr with val, return old value.
/// Default: sequentially consistent ordering.
template <typename T> [[nodiscard]] T fetch_xor(T *ptr, T val, memory_order order = memory_order::seq_cst) noexcept {
  return __atomic_fetch_xor(ptr, val, static_cast<int>(order));
}

} // namespace moss::intrinsics::atomic
