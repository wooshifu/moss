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
constexpr usize LARGE_PAGE_SIZE = 2ULL * 1024 * 1024;       // 2MB
constexpr usize HUGE_PAGE_SIZE = 1ULL * 1024 * 1024 * 1024; // 1GB

// Memory layout constants
//
// RISC-V Sv39: 39-bit VA, kernel half starts at 0xFFFFFFC000000000 (bit[38]=1)
// ARM64/x86_64: 48-bit VA, kernel half starts at 0xFFFF800000000000 (bit[47]=1)
#if defined(MOSS_ARCH_RISCV)
constexpr VirtAddr KERNEL_BASE = 0xFFFFFFC000000000ULL;
constexpr VirtAddr USER_MAX = 0x0000004000000000ULL; // 256 GB user space (Sv39)
#else
constexpr VirtAddr KERNEL_BASE = 0xFFFF800000000000ULL;
constexpr VirtAddr USER_MAX = 0x0000800000000000ULL;
#endif
constexpr VirtAddr USER_BASE = 0x0000000000000000ULL;

// Direct-map: physical RAM is mapped at KERNEL_BASE + phys_addr (post-trampoline)
constexpr VirtAddr KERNEL_DIRECT_MAP_BASE = KERNEL_BASE;
constexpr PhysAddr PHYS_BASE = 0x40000000ULL; // QEMU virt RAM start

// Address translation: physical ↔ virtual (valid only after boot trampoline)
inline VirtAddr phys_to_virt(PhysAddr pa) noexcept { return pa + KERNEL_DIRECT_MAP_BASE; }
inline PhysAddr virt_to_phys(VirtAddr va) noexcept { return va - KERNEL_DIRECT_MAP_BASE; }

// Check if an address is in the high-half kernel region
inline bool is_kernel_addr(VirtAddr va) noexcept { return va >= KERNEL_BASE; }

// Hardware constants
constexpr usize CACHE_LINE_SIZE = 64;

// Boot-time maximum CPUs (compile-time constant for assembly/linker only).
// Assembly uses ASM_MAX_CPUS and linker allocates BOOT_MAX_CPUS × 32KB stacks.
// This is the upper bound for early boot before the memory allocator is available.
constexpr usize BOOT_MAX_CPUS = 16;

// Runtime CPU count — set from FDT during early boot, read-only after SMP init.
// All per-CPU iteration and bounds checks should use this instead of compile-time constants.
inline u32 g_num_cpus = 1;

// CPU bitmap: dynamically-sized bitmask for CPU sets.
// Uses a single inline u64 for ≤64 CPUs (zero heap allocation).
// For >64 CPUs, dynamically allocates via operator new.
class CpuBitmap {
  static constexpr u32 BITS_PER_WORD = 64;

  u64 inline_word_{0};
  u64 *words_{&inline_word_};
  u32 num_words_{1};

public:
  // Default: empty bitmap (single inline word, all zeros)
  constexpr CpuBitmap() noexcept = default;

  // Bitmap for a specific CPU count (allocates if num_cpus > 64)
  explicit CpuBitmap(u32 num_cpus, bool set_all) noexcept : inline_word_{0}, words_{&inline_word_}, num_words_{1} {
    u32 needed = (num_cpus + BITS_PER_WORD - 1) / BITS_PER_WORD;
    if (needed < 1) {
      needed = 1;
    }
    if (needed > 1) {
      words_ = new u64[needed]();
      num_words_ = needed;
    }
    if (set_all) {
      set_all_up_to(num_cpus);
    }
  }

  // All-CPUs-set factory (uses g_num_cpus)
  static CpuBitmap all() noexcept {
    CpuBitmap bm(g_num_cpus, true);
    return bm;
  }

  // Single-CPU bitmap factory
  static CpuBitmap single(u32 cpu) noexcept {
    CpuBitmap bm;
    bm.set(cpu);
    return bm;
  }

  // Copy
  CpuBitmap(const CpuBitmap &other) noexcept : inline_word_{0}, words_{&inline_word_}, num_words_{other.num_words_} {
    if (num_words_ > 1) {
      words_ = new u64[num_words_]();
      for (u32 i = 0; i < num_words_; ++i) {
        words_[i] = other.words_[i];
      }
    } else {
      inline_word_ = other.inline_word_;
    }
  }

  CpuBitmap &operator=(const CpuBitmap &other) noexcept {
    if (this != &other) {
      if (words_ != &inline_word_) {
        delete[] words_;
      }
      num_words_ = other.num_words_;
      if (num_words_ > 1) {
        words_ = new u64[num_words_]();
        for (u32 i = 0; i < num_words_; ++i) {
          words_[i] = other.words_[i];
        }
      } else {
        inline_word_ = other.inline_word_;
        words_ = &inline_word_;
      }
    }
    return *this;
  }

  // Move
  CpuBitmap(CpuBitmap &&other) noexcept : inline_word_{0}, words_{&inline_word_}, num_words_{other.num_words_} {
    if (num_words_ > 1) {
      words_ = other.words_;
      other.words_ = &other.inline_word_;
      other.inline_word_ = 0;
      other.num_words_ = 1;
    } else {
      inline_word_ = other.inline_word_;
      other.inline_word_ = 0;
    }
  }

  CpuBitmap &operator=(CpuBitmap &&other) noexcept {
    if (this != &other) {
      if (words_ != &inline_word_) {
        delete[] words_;
      }
      num_words_ = other.num_words_;
      if (num_words_ > 1) {
        words_ = other.words_;
        other.words_ = &other.inline_word_;
        other.inline_word_ = 0;
        other.num_words_ = 1;
      } else {
        inline_word_ = other.inline_word_;
        words_ = &inline_word_;
        other.inline_word_ = 0;
      }
    }
    return *this;
  }

  ~CpuBitmap() noexcept {
    if (words_ != &inline_word_) {
      delete[] words_;
    }
  }

  // Bit manipulation
  void set(u32 cpu) noexcept {
    u32 word_idx = cpu / BITS_PER_WORD;
    if (word_idx < num_words_) {
      words_[word_idx] |= (1ULL << (cpu % BITS_PER_WORD));
    }
  }

  void clear(u32 cpu) noexcept {
    u32 word_idx = cpu / BITS_PER_WORD;
    if (word_idx < num_words_) {
      words_[word_idx] &= ~(1ULL << (cpu % BITS_PER_WORD));
    }
  }

  [[nodiscard]] bool test(u32 cpu) const noexcept {
    u32 word_idx = cpu / BITS_PER_WORD;
    if (word_idx >= num_words_) {
      return false;
    }
    return (words_[word_idx] & (1ULL << (cpu % BITS_PER_WORD))) != 0;
  }

  [[nodiscard]] bool empty() const noexcept {
    for (u32 i = 0; i < num_words_; ++i) {
      if (words_[i] != 0) {
        return false;
      }
    }
    return true;
  }

  // Extract low 8 bits as u8 (for GICv2 SGIR target mask)
  [[nodiscard]] u8 low_byte() const noexcept { return static_cast<u8>(words_[0] & 0xFF); }

  // Extract low 32 bits as u32 (for syscall ABI compatibility)
  [[nodiscard]] u32 low_word() const noexcept { return static_cast<u32>(words_[0] & 0xFFFFFFFFULL); }

  // Set from a u32 bitmask (for syscall ABI compatibility)
  void set_from_u32(u32 mask) noexcept {
    for (u32 i = 0; i < num_words_; ++i) {
      words_[i] = 0;
    }
    words_[0] = mask;
  }

private:
  void set_all_up_to(u32 num_cpus) noexcept {
    u32 full_words = num_cpus / BITS_PER_WORD;
    u32 remaining_bits = num_cpus % BITS_PER_WORD;
    for (u32 i = 0; i < full_words && i < num_words_; ++i) {
      words_[i] = ~0ULL;
    }
    if (remaining_bits > 0 && full_words < num_words_) {
      words_[full_words] = (1ULL << remaining_bits) - 1;
    }
  }
};

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
  OutOfMemory = 1,
  InvalidParameter = 2,
  PermissionDenied = 3,
  NotFound = 4,
  AlreadyExists = 5,
  ResourceBusy = 6,
  Timeout = 7,
  DeviceError = 8,
  DeviceBusy = 9,
  IoError = 10,
  NetworkError = 11,
  FileSystemError = 12,
  InvalidState = 13,
  Interrupted = 14,
  TooManyFiles = 15,
  NoSpace = 16,
  ReadOnly = 17,
  NotSupported = 18,
  // IPC-related errors
  InvalidArgument = 19,
  ResourceExhausted = 20,
  Busy = 21,
  InternalError = 22,
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
public:
  NonCopyable(const NonCopyable &) = delete;
  NonCopyable &operator=(const NonCopyable &) = delete;
  NonCopyable(NonCopyable &&) = delete;
  NonCopyable &operator=(NonCopyable &&) = delete;

protected:
  constexpr NonCopyable() = default;
  ~NonCopyable() = default;
};

} // namespace moss::kernel
