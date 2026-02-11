// src/modules/types.cppm
export module moss.types;

import moss.std;

export namespace moss::kernel {

// Architecture-dependent size type
#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X86_64) ||                   \
    defined(MOSS_ARCH_RISCV)
using usize = u64;
using isize = i64;
#else
using usize = size_t;
using isize = ptrdiff_t;
#endif

// Physical and virtual address types
using PhysAddr = u64;
using VirtAddr = u64;

// Kernel constants
constexpr usize PAGE_SIZE = 4096;
constexpr usize CACHE_LINE_SIZE = 64;
constexpr usize MAX_CPUS = 8;

// Process and Thread IDs
using ProcessId = u32;
using ThreadId = u32;

// Error codes
enum class ErrorCode : u32 {
  Success = 0,
  OutOfMemory = 1,
  InvalidParameter = 2,
  ResourceBusy = 3,
  NotFound = 4,
  PermissionDenied = 5,
  Timeout = 6,
  DeviceError = 7,
  NetworkError = 8,
  FileSystemError = 9,
  Unknown = 0xFFFFFFFF
};

// Handle types for kernel objects
using Handle = u64;
constexpr Handle INVALID_HANDLE = 0;

// Memory alignment utilities
template <usize Alignment> constexpr usize align_up(usize value) noexcept {
  static_assert((Alignment & (Alignment - 1)) == 0,
                "Alignment must be power of 2");
  return (value + Alignment - 1) & ~(Alignment - 1);
}

constexpr bool is_aligned(usize value, usize alignment) noexcept {
  return (value & (alignment - 1)) == 0;
}

// Non-copyable and non-movable base class
class NonCopyable {
protected:
  constexpr NonCopyable() = default;
  ~NonCopyable() = default;

  NonCopyable(const NonCopyable &) = delete;
  NonCopyable &operator=(const NonCopyable &) = delete;
  NonCopyable(NonCopyable &&) = delete;
  NonCopyable &operator=(NonCopyable &&) = delete;
};

} // namespace moss::kernel

// Macro for non-copyable, non-movable classes (currently unused but kept for future use)
// #define NON_COPYABLE_NON_MOVABLE(ClassName)                                    \
//   ClassName(const ClassName &) = delete;                                       \
//   ClassName &operator=(const ClassName &) = delete;                            \
//   ClassName(ClassName &&) = delete;                                            \
//   ClassName &operator=(ClassName &&) = delete
