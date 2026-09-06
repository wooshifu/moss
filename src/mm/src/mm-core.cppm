// MOSS Memory Management Module - Core partition
// Contains: Global constants, basic enums, NUMA base types,
//           PageFrameAllocator, BuddyAllocatorV2, RuntimeHeapAllocator

export module moss.mm:core;

import moss.std;
import moss.types;
import moss.result;
import moss.fdt;
import moss.containers;
import moss.arch;
import moss.platform;
import moss.hal.mmu;
import moss.logging;
import moss.abi;

// ============================================================================
// Global-scope constants (originally outside namespace in buddy_allocator_v2.hpp)
// ============================================================================

// Page size constants
export inline constexpr moss::kernel::usize PAGE_SIZE = 4096; // 4KB
export inline constexpr moss::kernel::usize PAGE_SHIFT = 12;  // log2(PAGE_SIZE)

// Buddy algorithm max order (supports up to 4MB = 4KB * 2^10)
export inline constexpr moss::kernel::usize MAX_ORDER = 10;

// ============================================================================
// Core MM types and classes
// ============================================================================
export namespace moss::kernel::mm {

// Re-export basic types used throughout
using moss::kernel::ErrorCode;
using moss::kernel::KernelResult;
using moss::kernel::PhysAddr;
using moss::kernel::VirtAddr;
using moss::kernel::VoidResult;

namespace log = moss::kernel::logging;

// ========================================================================
// memory_stats.hpp - Basic enums (needed by mm_interface types)
// ========================================================================

enum class MemoryStatsError : u32 {
  InvalidParameter = 1,
  BufferOverflow = 2,
  StatsDisabled = 3,
  SystemBusy = 4,
  OutOfMemory = 5,
  CorruptedData = 6,
  InitializationFailed = 7
};

template <typename T> using MemoryStatsResult = moss::kernel::Result<T, MemoryStatsError>;
using MemoryStatsVoidResult = moss::kernel::Result<void, MemoryStatsError>;

enum class AllocationType : u8 {
  KERNEL_CORE = 0,
  DRIVER = 1,
  PROCESS = 2,
  CACHE = 3,
  NETWORK = 4,
  FILESYSTEM = 5,
  TEMPORARY = 6,
  UNKNOWN = 7
};

enum class UsagePattern : u8 { SEQUENTIAL = 0, RANDOM = 1, HOT_COLD = 2, STREAMING = 3, BATCH = 4, INTERACTIVE = 5 };

enum class MemoryPressure : u8 { LOW = 0, MEDIUM = 1, HIGH = 2, CRITICAL = 3 };

// ========================================================================
// NUMA base types (needed by vmalloc, huge_pages, mm_interface)
// ========================================================================

using numa_node_t = u32;
inline constexpr numa_node_t NUMA_NO_NODE = static_cast<numa_node_t>(-1);
inline constexpr u32 MAX_NUMA_NODES = 16;

// ========================================================================
// page_frame_allocator.hpp
// ========================================================================

enum class PageAllocError : u32 {
  OutOfMemory = 1,
  InvalidOrder = 2,
  InvalidAddress = 3,
  InitializationFailed = 4,
  PageInUse = 5
};

template <typename T> using PageAllocResult = moss::kernel::Result<T, PageAllocError>;
using PageAllocVoidResult = moss::kernel::Result<void, PageAllocError>;

constexpr usize addr_to_page(PhysAddr addr) noexcept { return static_cast<usize>(addr) >> PAGE_SHIFT; }

constexpr PhysAddr page_to_addr(usize page) noexcept {
  return static_cast<PhysAddr>(static_cast<u64>(page) << PAGE_SHIFT);
}

class PageFrameAllocator {
public:
  static PageAllocVoidResult initialize() noexcept;
  [[nodiscard]] static PageAllocResult<PhysAddr> allocate_pages(usize order) noexcept;
  static PageAllocVoidResult free_pages(PhysAddr addr, usize order) noexcept;

  struct MemoryStats {
    usize total_pages;
    usize free_pages;
    usize used_pages;
    usize kernel_pages;
    PhysAddr metadata_start;
    usize metadata_size; // Reserved bytes, including page-alignment padding.
  };

  [[nodiscard]] static MemoryStats get_memory_stats() noexcept;

  // Page reference counting for COW (Copy-on-Write)
  static void page_ref_inc(PhysAddr addr) noexcept;
  static u32 page_ref_dec(PhysAddr addr) noexcept; // returns new refcount
  static u32 page_ref_get(PhysAddr addr) noexcept;
  static void page_ref_set(PhysAddr addr, u32 count) noexcept;

private:
  struct MemoryRegion {
    PhysAddr start_addr;
    usize page_count;
    MemoryRegion *next;
  };

  struct FreeBlock {
    FreeBlock *next;
    FreeBlock *prev;
    usize order;
  };

  struct PageMetadata {
    moss::kernel::containers::AtomicU32 ref_count;
    moss::kernel::containers::AtomicU32 flags;
    FreeBlock *free_list_node;
  };

  static containers::IrqSpinLock lock_;
  static bool initialized_;
  static MemoryRegion *memory_regions_;
  static FreeBlock *free_lists_[MAX_ORDER + 1];
  static PageMetadata *page_metadata_;
  static usize total_pages_;
  static usize metadata_pages_;
  static moss::kernel::containers::AtomicSize free_pages_;
  static moss::kernel::containers::AtomicSize used_pages_;

  static PageAllocVoidResult parse_memory_layout() noexcept;
  static void initialize_free_lists() noexcept;
  static FreeBlock *find_buddy(FreeBlock *block, usize order) noexcept;
  static void merge_buddies(FreeBlock *block, usize order) noexcept;
  static void split_block(FreeBlock *block, usize order) noexcept;
  static void add_to_free_list(FreeBlock *block, usize order) noexcept;
  static FreeBlock *remove_from_free_list(usize order) noexcept;
  static bool is_valid_page_address(PhysAddr addr) noexcept;
};

// ========================================================================
// buddy_allocator_v2.hpp
// ========================================================================

inline constexpr usize PAGEBLOCK_ORDER = 9;
inline constexpr usize PAGEBLOCK_SIZE = (1UL << PAGEBLOCK_ORDER);
inline constexpr usize PAGEBLOCK_PAGES = PAGEBLOCK_SIZE >> PAGE_SHIFT;

enum class MigrationType : u32 { UNMOVABLE = 0, MOVABLE = 1, RECLAIMABLE = 2, TYPES_COUNT = 3 };

enum class BuddyError : u32 {
  OutOfMemory = 1,
  InvalidOrder = 2,
  InvalidAddress = 3,
  InvalidMigrationType = 4,
  InitializationFailed = 5,
  FragmentationSevere = 6
};

template <typename T> using BuddyResult = moss::kernel::Result<T, BuddyError>;
using BuddyVoidResult = moss::kernel::Result<void, BuddyError>;

enum class BuddyAllocFlags : u32 {
  NONE = 0,
  EMERGENCY = (1 << 0),
  NO_FALLBACK = (1 << 1),
  PREFAULT = (1 << 2),
  HIGH_PRIORITY = (1 << 3)
};

struct PageAllocRequest {
  usize order;
  MigrationType migration_type;
  BuddyAllocFlags flags;

  PageAllocRequest(usize o, MigrationType mt, BuddyAllocFlags f = BuddyAllocFlags::NONE) noexcept
      : order(o), migration_type(mt), flags(f) {}
};

class BuddyAllocatorV2 {
public:
  static BuddyVoidResult initialize() noexcept;
  [[nodiscard]] static BuddyResult<PhysAddr> allocate_pages(const PageAllocRequest &request) noexcept;
  [[nodiscard]] static BuddyResult<PhysAddr>
  allocate_pages(usize order, MigrationType migration_type = MigrationType::MOVABLE) noexcept {
    return allocate_pages(PageAllocRequest(order, migration_type));
  }
  static BuddyVoidResult free_pages(PhysAddr addr, usize order) noexcept;
  static BuddyVoidResult compact_memory() noexcept;

  enum class WaterMark : u32 { LOW = 0, MIN = 1, HIGH = 2 };

  [[nodiscard]] static WaterMark get_water_mark() noexcept;
  [[nodiscard]] static bool is_memory_pressure() noexcept;

  struct FragmentationStats {
    usize total_free_pages;
    usize largest_free_block_pages;
    double fragmentation_index;
    usize free_pages_by_migration[static_cast<u32>(MigrationType::TYPES_COUNT)];
    usize unusable_pages;
  };

  [[nodiscard]] static FragmentationStats get_fragmentation_stats() noexcept;

  struct MemoryStats {
    usize total_pages;
    usize free_pages;
    usize used_pages;
    usize kernel_pages;
    usize pages_by_migration[static_cast<u32>(MigrationType::TYPES_COUNT)];
    usize pageblock_count;
    usize mixed_pageblocks;
    usize steal_count;
  };

  [[nodiscard]] static MemoryStats get_memory_stats() noexcept;

private:
  struct PageBlock {
    MigrationType migration_type;
    moss::kernel::containers::AtomicU32 free_pages;
    moss::kernel::containers::AtomicU32 flags;
    enum Flags : u32 { MIXED = (1 << 0), DIRTY = (1 << 1), RESERVED = (1 << 2) };
  };

  struct FreeBlock {
    FreeBlock *next;
    FreeBlock *prev;
    usize order;
    MigrationType migration_type;
    u64 last_access_time;
  };

  struct PageMetadata {
    moss::kernel::containers::AtomicU32 ref_count;
    moss::kernel::containers::AtomicU32 flags;
    MigrationType migration_type;
    FreeBlock *free_list_node;
    enum Flags : u32 {
      ALLOCATED = (1 << 0),
      MOVABLE = (1 << 1),
      RECLAIMABLE = (1 << 2),
      COMPOUND_HEAD = (1 << 3),
      COMPOUND_TAIL = (1 << 4)
    };
  };

  struct PerCpuPageCache {
    static constexpr usize CACHE_SIZE = 64;
    struct Cache {
      PhysAddr pages[CACHE_SIZE];
      moss::kernel::containers::AtomicU32 count;
      moss::kernel::containers::AtomicU64 alloc_count;
      moss::kernel::containers::AtomicU64 free_count;
    } caches[static_cast<u32>(MigrationType::TYPES_COUNT)];

    BuddyResult<usize> refill_cache(MigrationType migration_type) noexcept;
    BuddyVoidResult drain_cache(MigrationType migration_type) noexcept;
  };

  static bool initialized_;
  static moss::kernel::containers::PerCpuData<PerCpuPageCache> per_cpu_caches_;
  static FreeBlock *free_lists_[static_cast<u32>(MigrationType::TYPES_COUNT)][MAX_ORDER + 1];
  static PageMetadata *page_metadata_;
  static PageBlock *page_blocks_;
  static usize total_pages_;
  static usize total_pageblocks_;
  static moss::kernel::containers::AtomicSize free_pages_;
  static moss::kernel::containers::AtomicSize used_pages_;
  static moss::kernel::containers::AtomicU64 allocation_count_;
  static moss::kernel::containers::AtomicU64 steal_count_;
  static usize low_watermark_;
  static usize min_watermark_;
  static usize high_watermark_;

  static BuddyResult<PhysAddr> allocate_from_migration_type(usize order, MigrationType migration_type) noexcept;
  static BuddyResult<PhysAddr> fallback_allocate(usize order, MigrationType preferred_type) noexcept;
  static PageBlock *get_pageblock(PhysAddr addr) noexcept;
  static void set_pageblock_migration_type(PageBlock *block, MigrationType type) noexcept;
  static MigrationType get_pageblock_migration_type(PhysAddr addr) noexcept;
  static void mark_pageblock_mixed(PageBlock *block) noexcept;
  static MigrationType get_fallback_migration_type(MigrationType original, usize order) noexcept;
  static bool can_steal_from_pageblock(PageBlock *block, MigrationType target_type, usize order) noexcept;
  static BuddyResult<PhysAddr> steal_pages(PageBlock *source_block, usize order, MigrationType target_type) noexcept;
  static FreeBlock *find_buddy_in_migration_type(FreeBlock *block, usize order, MigrationType migration_type) noexcept;
  static void merge_buddies_with_migration_check(FreeBlock *block, usize order) noexcept;
  static void add_to_free_list_typed(FreeBlock *block, usize order, MigrationType migration_type) noexcept;
  static FreeBlock *remove_from_free_list_typed(usize order, MigrationType migration_type) noexcept;
  static BuddyVoidResult compact_pageblock(PageBlock *block) noexcept;
  static bool is_compaction_needed() noexcept;
  static usize calculate_fragmentation_index() noexcept;
  static BuddyResult<PhysAddr> allocate_from_percpu_cache(MigrationType migration_type) noexcept;
  static BuddyVoidResult free_to_percpu_cache(PhysAddr addr, MigrationType migration_type) noexcept;
  static void update_watermarks() noexcept;
  static bool should_trigger_reclaim() noexcept;
  static void trigger_memory_reclaim() noexcept;
  static void update_allocation_stats(usize order, MigrationType migration_type) noexcept;
  static void update_fragmentation_stats() noexcept;
  static BuddyVoidResult parse_memory_layout() noexcept;
  static void initialize_pageblocks() noexcept;
  static void initialize_free_lists_typed() noexcept;
  static void initialize_watermarks() noexcept;
};

// Global interface functions - backward compatible
[[nodiscard]] inline BuddyResult<PhysAddr> allocate_pages(usize order) noexcept {
  return BuddyAllocatorV2::allocate_pages(order, MigrationType::MOVABLE);
}

inline BuddyVoidResult free_pages(PhysAddr addr, usize order) noexcept {
  return BuddyAllocatorV2::free_pages(addr, order);
}

namespace page_alloc {
[[nodiscard]] inline BuddyResult<PhysAddr> alloc_kernel_pages(usize order) noexcept {
  return BuddyAllocatorV2::allocate_pages(order, MigrationType::UNMOVABLE);
}
[[nodiscard]] inline BuddyResult<PhysAddr> alloc_user_pages(usize order) noexcept {
  return BuddyAllocatorV2::allocate_pages(order, MigrationType::MOVABLE);
}
[[nodiscard]] inline BuddyResult<PhysAddr> alloc_cache_pages(usize order) noexcept {
  return BuddyAllocatorV2::allocate_pages(order, MigrationType::RECLAIMABLE);
}
[[nodiscard]] inline BuddyResult<PhysAddr> alloc_emergency_pages(usize order) noexcept {
  PageAllocRequest request(order, MigrationType::MOVABLE, BuddyAllocFlags::EMERGENCY);
  return BuddyAllocatorV2::allocate_pages(request);
}
} // namespace page_alloc

// ========================================================================
// runtime_heap_allocator.hpp
// ========================================================================

enum class HeapAllocError : u32 {
  OutOfMemory = 1,
  InvalidAddress = 2,
  InvalidSize = 3,
  InitializationFailed = 4,
  HeapCorruption = 5
};

template <typename T> using HeapAllocResult = moss::kernel::Result<T, HeapAllocError>;
using HeapAllocVoidResult = moss::kernel::Result<void, HeapAllocError>;

class RuntimeHeapAllocator {
public:
  static HeapAllocVoidResult initialize_heap(VirtAddr heap_start, usize initial_size) noexcept;
  [[nodiscard]] static HeapAllocResult<void *> allocate(usize size) noexcept;
  [[nodiscard]] static HeapAllocResult<void *> allocate_aligned(usize size, usize alignment) noexcept;
  static HeapAllocVoidResult deallocate(void *ptr, usize size) noexcept;
  static HeapAllocVoidResult expand_heap(usize additional_size) noexcept;

  struct HeapStats {
    usize total_heap_size;
    usize allocated_bytes;
    usize free_bytes;
    usize fragmentation_ratio;
    usize largest_free_block;
  };

  [[nodiscard]] static HeapStats get_heap_stats() noexcept;
  [[nodiscard]] static VirtAddr get_heap_start() noexcept;
  [[nodiscard]] static VirtAddr get_heap_end() noexcept;
  [[nodiscard]] static usize get_heap_size() noexcept;

private:
  struct FreeBlock {
    usize size;
    FreeBlock *next;
    FreeBlock *prev;
    static constexpr u32 MAGIC = 0xDEADC0DE;
    u32 magic;
    FreeBlock(usize block_size) noexcept : size(block_size), next(nullptr), prev(nullptr), magic(MAGIC) {}
    [[nodiscard]] bool is_valid() const noexcept { return magic == MAGIC; }
  };

  struct alignas(16) AllocatedBlock {
    usize size;
    VirtAddr block_start;
    usize requested_size;
    u32 magic;
    static constexpr u32 MAGIC = 0xBEEFF00D;
    AllocatedBlock(usize block_size, VirtAddr start, usize requested) noexcept
        : size(block_size), block_start(start), requested_size(requested), magic(MAGIC) {}
    [[nodiscard]] bool is_valid() const noexcept { return magic == MAGIC; }
  };

  static containers::IrqSpinLock lock_;
  static bool initialized_;
  static VirtAddr heap_start_;
  static VirtAddr heap_end_;
  static VirtAddr heap_limit_;
  static FreeBlock *free_list_head_;
  static usize allocated_bytes_;
  static usize total_allocations_;
  static constexpr usize BLOCK_ALIGN = 16;
  static constexpr usize MIN_BLOCK_SIZE = sizeof(FreeBlock);

  [[nodiscard]] static constexpr usize align_size(usize size, usize alignment) noexcept {
    return (size + alignment - 1) & ~(alignment - 1);
  }

  static HeapAllocVoidResult expand_heap_locked(usize additional_size) noexcept;
  static FreeBlock *find_suitable_block(usize payload_size, usize alignment) noexcept;
  static void split_block(FreeBlock *block, usize required_size) noexcept;
  static void merge_free_blocks() noexcept;
  static void add_to_free_list(FreeBlock *block) noexcept;
  static void remove_from_free_list(FreeBlock *block) noexcept;
};

} // namespace moss::kernel::mm
