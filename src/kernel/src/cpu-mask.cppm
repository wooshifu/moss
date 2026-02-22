// CPU Mask Management Module
//
// Provides Linux-inspired CPU bitmask operations optimized for freestanding
// environment. Uses simple bit manipulation with u64 arrays for efficiency.
//
// Design Philosophy:
// - Use u64 arrays for bit storage (64 bits per word)
// - Support up to 64 CPUs with single u64 (most common case)
// - Dynamic allocation for >64 CPUs
// - API compatible with Linux cpumask operations

export module moss.kernel:cpu_mask;

import moss.std;
import moss.types;

export namespace moss::kernel::cpu_mask {

// ============================================================================
// Constants
// ============================================================================

// Maximum CPUs in a single u64 word
inline constexpr u32 BITS_PER_WORD = 64;
inline constexpr u32 SINGLE_WORD_MAX_CPUS = 64;

// ============================================================================
// CPU Mask Class - Simplified Implementation
// ============================================================================

class CpuMask {
private:
  u64 single_word_; // For ≤64 CPUs (common case)
  u64 *multi_word_; // For >64 CPUs (dynamic allocation)
  u32 cpu_count_;   // Number of CPUs this mask represents
  u32 word_count_;  // Number of u64 words needed

  // Calculate number of u64 words needed
  static constexpr u32 words_for_cpus(u32 cpu_count) noexcept {
    return (cpu_count + BITS_PER_WORD - 1) / BITS_PER_WORD;
  }

  // Get word pointer (single or multi)
  u64 *get_words() noexcept { return (word_count_ == 1) ? &single_word_ : multi_word_; }

  const u64 *get_words() const noexcept { return (word_count_ == 1) ? &single_word_ : multi_word_; }

public:
  // ============================================================================
  // Constructors and Destructor
  // ============================================================================

  // Default constructor: creates mask for 64 CPUs (single word)
  CpuMask() noexcept : single_word_(0), multi_word_(nullptr), cpu_count_(SINGLE_WORD_MAX_CPUS), word_count_(1) {}

  // Constructor with explicit CPU count
  explicit CpuMask(u32 cpu_count) noexcept : single_word_(0), multi_word_(nullptr), cpu_count_(cpu_count) {
    word_count_ = words_for_cpus(cpu_count_);

    if (word_count_ > 1) {
      // Allocate dynamic array for multiple words
      multi_word_ = new u64[word_count_];
      for (u32 i = 0; i < word_count_; ++i) {
        multi_word_[i] = 0;
      }
    }
  }

  // Destructor
  ~CpuMask() noexcept {
    if (multi_word_) {
      delete[] multi_word_;
    }
  }

  // Copy constructor
  CpuMask(const CpuMask &other) noexcept
      : single_word_(other.single_word_), multi_word_(nullptr), cpu_count_(other.cpu_count_),
        word_count_(other.word_count_) {

    if (other.multi_word_) {
      multi_word_ = new u64[word_count_];
      for (u32 i = 0; i < word_count_; ++i) {
        multi_word_[i] = other.multi_word_[i];
      }
    }
  }

  // Move constructor
  CpuMask(CpuMask &&other) noexcept
      : single_word_(other.single_word_), multi_word_(other.multi_word_), cpu_count_(other.cpu_count_),
        word_count_(other.word_count_) {

    other.multi_word_ = nullptr;
    other.single_word_ = 0;
    other.cpu_count_ = 0;
    other.word_count_ = 0;
  }

  // Copy assignment
  CpuMask &operator=(const CpuMask &other) noexcept {
    if (this != &other) {
      // Clean up current state
      if (multi_word_) {
        delete[] multi_word_;
        multi_word_ = nullptr;
      }

      // Copy state
      single_word_ = other.single_word_;
      cpu_count_ = other.cpu_count_;
      word_count_ = other.word_count_;

      if (other.multi_word_) {
        multi_word_ = new u64[word_count_];
        for (u32 i = 0; i < word_count_; ++i) {
          multi_word_[i] = other.multi_word_[i];
        }
      }
    }
    return *this;
  }

  // Move assignment
  CpuMask &operator=(CpuMask &&other) noexcept {
    if (this != &other) {
      // Clean up current state
      if (multi_word_) {
        delete[] multi_word_;
      }

      // Move state
      single_word_ = other.single_word_;
      multi_word_ = other.multi_word_;
      cpu_count_ = other.cpu_count_;
      word_count_ = other.word_count_;

      // Clear other
      other.multi_word_ = nullptr;
      other.single_word_ = 0;
      other.cpu_count_ = 0;
      other.word_count_ = 0;
    }
    return *this;
  }

  // ============================================================================
  // Basic Bit Operations
  // ============================================================================

  // Set a CPU bit
  void set(u32 cpu) noexcept {
    if (cpu >= cpu_count_) {
      return;
    }

    u32 word_idx = cpu / BITS_PER_WORD;
    u32 bit_idx = cpu % BITS_PER_WORD;

    u64 *words = get_words();
    words[word_idx] |= (1ULL << bit_idx);
  }

  // Clear a CPU bit
  void clear(u32 cpu) noexcept {
    if (cpu >= cpu_count_) {
      return;
    }

    u32 word_idx = cpu / BITS_PER_WORD;
    u32 bit_idx = cpu % BITS_PER_WORD;

    u64 *words = get_words();
    words[word_idx] &= ~(1ULL << bit_idx);
  }

  // Test a CPU bit
  [[nodiscard]] bool test(u32 cpu) const noexcept {
    if (cpu >= cpu_count_) {
      return false;
    }

    u32 word_idx = cpu / BITS_PER_WORD;
    u32 bit_idx = cpu % BITS_PER_WORD;

    const u64 *words = get_words();
    return (words[word_idx] & (1ULL << bit_idx)) != 0;
  }

  // Set all bits
  void set_all() noexcept {
    u64 *words = get_words();

    // Set full words
    u32 full_words = cpu_count_ / BITS_PER_WORD;
    for (u32 i = 0; i < full_words; ++i) {
      words[i] = ~0ULL;
    }

    // Set partial last word
    u32 remaining_bits = cpu_count_ % BITS_PER_WORD;
    if (remaining_bits > 0 && full_words < word_count_) {
      words[full_words] = (1ULL << remaining_bits) - 1;
    }

    // Clear any extra words (shouldn't happen but be safe)
    for (u32 i = full_words + 1; i < word_count_; ++i) {
      words[i] = 0;
    }
  }

  // Clear all bits
  void clear_all() noexcept {
    u64 *words = get_words();
    for (u32 i = 0; i < word_count_; ++i) {
      words[i] = 0;
    }
  }

  // Set range of CPUs
  void set_range(u32 start, u32 count) noexcept {
    for (u32 i = 0; i < count && (start + i) < cpu_count_; ++i) {
      set(start + i);
    }
  }

  // ============================================================================
  // Query Operations
  // ============================================================================

  // Check if any bits are set
  [[nodiscard]] bool any() const noexcept {
    const u64 *words = get_words();
    u32 full_words = cpu_count_ / BITS_PER_WORD;

    // Check full words
    for (u32 i = 0; i < full_words; ++i) {
      if (words[i] != 0) {
        return true;
      }
    }

    // Check partial last word
    u32 remaining_bits = cpu_count_ % BITS_PER_WORD;
    if (remaining_bits > 0 && full_words < word_count_) {
      u64 mask = (1ULL << remaining_bits) - 1;
      if ((words[full_words] & mask) != 0) {
        return true;
      }
    }

    return false;
  }

  // Check if no bits are set
  [[nodiscard]] bool none() const noexcept { return !any(); }

  // Count set bits
  [[nodiscard]] u32 count() const noexcept {
    const u64 *words = get_words();
    u32 total = 0;
    u32 full_words = cpu_count_ / BITS_PER_WORD;

    // Count full words
    for (u32 i = 0; i < full_words; ++i) {
      total += static_cast<u32>(__builtin_popcountll(words[i]));
    }

    // Count partial last word
    u32 remaining_bits = cpu_count_ % BITS_PER_WORD;
    if (remaining_bits > 0 && full_words < word_count_) {
      u64 mask = (1ULL << remaining_bits) - 1;
      total += static_cast<u32>(__builtin_popcountll(words[full_words] & mask));
    }

    return total;
  }

  // Get maximum CPU count this mask can represent
  [[nodiscard]] u32 size() const noexcept { return cpu_count_; }

  // ============================================================================
  // Efficient Iteration
  // ============================================================================

  // Iterate over all set CPUs
  template <typename F> void for_each_set(F &&func) const noexcept {
    const u64 *words = get_words();

    for (u32 word_idx = 0; word_idx < word_count_; ++word_idx) {
      u64 word = words[word_idx];
      while (word != 0) {
        u32 bit = static_cast<u32>(__builtin_ctzll(word)); // Count trailing zeros
        u32 cpu = word_idx * BITS_PER_WORD + bit;
        if (cpu >= cpu_count_) {
          break;
        }

        func(cpu);
        word &= word - 1; // Clear lowest set bit
      }
    }
  }

  // Find first set CPU (returns cpu_count_ if none found)
  [[nodiscard]] u32 first_set() const noexcept {
    const u64 *words = get_words();

    for (u32 word_idx = 0; word_idx < word_count_; ++word_idx) {
      u64 word = words[word_idx];
      if (word != 0) {
        u32 bit = static_cast<u32>(__builtin_ctzll(word));
        u32 cpu = word_idx * BITS_PER_WORD + bit;
        if (cpu < cpu_count_) {
          return cpu;
        }
      }
    }

    return cpu_count_; // Not found
  }

  // ============================================================================
  // Set Operations
  // ============================================================================

  // Bitwise AND
  [[nodiscard]] CpuMask operator&(const CpuMask &other) const noexcept {
    u32 max_cpus = (cpu_count_ > other.cpu_count_) ? cpu_count_ : other.cpu_count_;
    CpuMask result(max_cpus);

    for (u32 cpu = 0; cpu < max_cpus; ++cpu) {
      if ((cpu < cpu_count_ && test(cpu)) && (cpu < other.cpu_count_ && other.test(cpu))) {
        result.set(cpu);
      }
    }

    return result;
  }

  // Bitwise OR
  [[nodiscard]] CpuMask operator|(const CpuMask &other) const noexcept {
    u32 max_cpus = (cpu_count_ > other.cpu_count_) ? cpu_count_ : other.cpu_count_;
    CpuMask result(max_cpus);

    for (u32 cpu = 0; cpu < max_cpus; ++cpu) {
      if ((cpu < cpu_count_ && test(cpu)) || (cpu < other.cpu_count_ && other.test(cpu))) {
        result.set(cpu);
      }
    }

    return result;
  }

  // Bitwise XOR
  [[nodiscard]] CpuMask operator^(const CpuMask &other) const noexcept {
    u32 max_cpus = (cpu_count_ > other.cpu_count_) ? cpu_count_ : other.cpu_count_;
    CpuMask result(max_cpus);

    for (u32 cpu = 0; cpu < max_cpus; ++cpu) {
      bool bit1 = (cpu < cpu_count_ && test(cpu));
      bool bit2 = (cpu < other.cpu_count_ && other.test(cpu));
      if (bit1 != bit2) {
        result.set(cpu);
      }
    }

    return result;
  }

  // In-place operations
  CpuMask &operator&=(const CpuMask &other) noexcept {
    *this = *this & other;
    return *this;
  }

  CpuMask &operator|=(const CpuMask &other) noexcept {
    *this = *this | other;
    return *this;
  }

  CpuMask &operator^=(const CpuMask &other) noexcept {
    *this = *this ^ other;
    return *this;
  }
};

// ============================================================================
// CpuMask Factory Functions
// ============================================================================

// Create a CPU mask with specific CPU count
[[nodiscard]] inline CpuMask create_cpu_mask(u32 cpu_count) noexcept { return CpuMask(cpu_count); }

// Create a CPU mask for all CPUs in a range
[[nodiscard]] inline CpuMask create_cpu_range_mask(u32 start, u32 count, u32 total_cpus = 64) noexcept {
  CpuMask mask(total_cpus);
  mask.set_range(start, count);
  return mask;
}

} // namespace moss::kernel::cpu_mask

// ============================================================================
// Global alias for convenient access
// ============================================================================

export namespace moss::kernel {
// Re-export key types and functions at kernel namespace level
using cpu_mask::CpuMask;
using cpu_mask::create_cpu_mask;
using cpu_mask::create_cpu_range_mask;
} // namespace moss::kernel
