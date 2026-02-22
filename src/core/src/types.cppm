// MOSS Types Module - Kernel Type Definitions
export module moss.types;

import moss.std;

export namespace moss::kernel {

// Re-export basic integer types from moss:: namespace for backward compatibility
using moss::i16;
using moss::i32;
using moss::i64;
using moss::i8;
using moss::u16;
using moss::u32;
using moss::u64;
using moss::u8;

// Architecture-dependent size type
// Must match ABI size_t (unsigned long on LP64) - NOT u64 (unsigned long long)
#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X86_64) || defined(MOSS_ARCH_RISCV)
using usize = unsigned long;
using isize = signed long;
#else
using usize = size_t;
using isize = ptrdiff_t;
#endif

// Physical and virtual address types
using PhysAddr = u64;
using VirtAddr = u64;

// Page-related constants
constexpr usize PAGE_SIZE = 4096;
constexpr usize PAGE_SHIFT = 12;
constexpr usize LARGE_PAGE_SIZE = 2 * 1024 * 1024;   // 2MB
constexpr usize HUGE_PAGE_SIZE = 1024 * 1024 * 1024; // 1GB

// Memory layout constants
constexpr VirtAddr KERNEL_BASE = 0xFFFF800000000000ULL;
constexpr VirtAddr USER_BASE = 0x0000000000000000ULL;
constexpr VirtAddr USER_MAX = 0x0000800000000000ULL;

// Direct-map: physical RAM is mapped at KERNEL_BASE + phys_addr (post-trampoline)
constexpr VirtAddr KERNEL_DIRECT_MAP_BASE = KERNEL_BASE; // 0xFFFF800000000000
constexpr PhysAddr PHYS_BASE = 0x40000000ULL;            // QEMU virt RAM start

// Address translation: physical ↔ virtual (valid only after boot trampoline)
inline VirtAddr phys_to_virt(PhysAddr pa) noexcept { return pa + KERNEL_DIRECT_MAP_BASE; }
inline PhysAddr virt_to_phys(VirtAddr va) noexcept { return va - KERNEL_DIRECT_MAP_BASE; }

// Check if an address is in the high-half kernel region
inline bool is_kernel_addr(VirtAddr va) noexcept { return va >= KERNEL_BASE; }

// Hardware constants
constexpr usize CACHE_LINE_SIZE = 64;
constexpr usize MAX_CPUS = 8;

// Process and Thread IDs
using ProcessId = u32;
using ThreadId = u64;
using EndpointId = u32;
using DeviceId = u32;
using InterruptId = u32;

// IPC-related types
using MessageId = u64;
using ChannelId = u32;
using ShmId = u32;
using ServiceId = u32;

// Special ID values
constexpr ProcessId INVALID_PROCESS_ID = 0;
constexpr ThreadId INVALID_THREAD_ID = 0;
constexpr EndpointId INVALID_ENDPOINT_ID = 0;

// Error codes (superset of both types.hpp and result.hpp)
enum class ErrorCode : u32 {
  Success = 0,
  OutOfMemory,
  InvalidParameter,
  PermissionDenied,
  NotFound,
  AlreadyExists,
  ResourceBusy,
  Timeout,
  DeviceError,
  DeviceBusy,
  IoError,
  NetworkError,
  FileSystemError,
  InvalidState,
  Interrupted,
  TooManyFiles,
  NoSpace,
  ReadOnly,
  NotSupported,
  // IPC-related errors
  InvalidArgument,
  ResourceExhausted,
  Busy,
  InternalError,
  Unknown = 0xFFFFFFFF
};

// Kernel error type alias
using KernelError = ErrorCode;

// Handle types for kernel objects
using Handle = u64;
constexpr Handle INVALID_HANDLE = 0;

// Error code to string conversion
const char *error_to_string(ErrorCode error) noexcept;

// Memory alignment utilities
template <usize Alignment> constexpr usize align_up(usize value) noexcept {
  static_assert((Alignment & (Alignment - 1)) == 0, "Alignment must be power of 2");
  return (value + Alignment - 1) & ~(Alignment - 1);
}

template <usize Alignment> constexpr usize align_down(usize value) noexcept {
  static_assert((Alignment & (Alignment - 1)) == 0, "Alignment must be power of 2");
  return value & ~(Alignment - 1);
}

constexpr bool is_aligned(usize value, usize alignment) noexcept { return (value & (alignment - 1)) == 0; }

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
