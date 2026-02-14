// MOSS Memory Management Module - Unified MM subsystem
// Consolidates: page_frame_allocator, buddy_allocator_v2, runtime_heap_allocator,
// page_table, numa_policy, vmalloc_allocator, memory_reclaim, memory_compaction,
// huge_pages, memory_stats, mm_interface, kernel_memory

module;

// Architecture detection macros (do not cross module boundaries)
#ifndef MOSS_ARCH_ARM64
#ifndef MOSS_ARCH_X86_64
#ifndef MOSS_ARCH_RISCV
#if defined(__x86_64__) || defined(__x86_64) || defined(__amd64__) ||           \
    defined(__amd64) || defined(_M_X64)
#define MOSS_ARCH_X86_64
#elif defined(__aarch64__) || defined(_M_ARM64)
#define MOSS_ARCH_ARM64
#elif defined(__riscv) && __riscv_xlen == 64
#define MOSS_ARCH_RISCV
#else
#define MOSS_ARCH_X86_64
#endif
#endif
#endif
#endif

// Linker symbols (must be in global module fragment)
extern "C" {
    extern char _text_start_addr[];
    extern char _text_end_addr[];
    extern char _rodata_start_addr[];
    extern char _rodata_end_addr[];
    extern char _data_start_addr[];
    extern char _data_end_addr[];
    extern char _bss_start_addr[];
    extern char _bss_end_addr[];
    extern char _pagetable_start_addr[];
    extern char _pagetable_end_addr[];
    extern char _kernel_end_addr[];
    extern char _heap_start_addr[];
    extern char _heap_end_addr[];

    void early_debug_print(const char *message) noexcept;
}

export module moss.mm;

import moss.std;
import moss.types;
import moss.result;
import moss.fdt;
import moss.containers;
import moss.arch;

// ============================================================================
// Global-scope constants (originally outside namespace in buddy_allocator_v2.hpp)
// ============================================================================

// Page size constants
export inline constexpr moss::kernel::usize PAGE_SIZE = 4096;  // 4KB
export inline constexpr moss::kernel::usize PAGE_SHIFT = 12;   // log2(PAGE_SIZE)

// Buddy algorithm max order (supports up to 4MB = 4KB * 2^10)
export inline constexpr moss::kernel::usize MAX_ORDER = 10;

// ============================================================================
// Exported MM types and classes
// ============================================================================
export namespace moss::kernel::mm {

// Re-export basic types used throughout
using moss::kernel::PhysAddr;
using moss::kernel::VirtAddr;
using moss::kernel::ErrorCode;
using moss::kernel::KernelResult;
using moss::kernel::VoidResult;

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

template<typename T>
using MemoryStatsResult = moss::kernel::Result<T, MemoryStatsError>;
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

enum class UsagePattern : u8 {
    SEQUENTIAL = 0,
    RANDOM = 1,
    HOT_COLD = 2,
    STREAMING = 3,
    BATCH = 4,
    INTERACTIVE = 5
};

enum class MemoryPressure : u8 {
    LOW = 0,
    MEDIUM = 1,
    HIGH = 2,
    CRITICAL = 3
};

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
    InitializationFailed = 4
};

template<typename T>
using PageAllocResult = moss::kernel::Result<T, PageAllocError>;
using PageAllocVoidResult = moss::kernel::Result<void, PageAllocError>;

inline constexpr usize addr_to_page(PhysAddr addr) noexcept {
    return static_cast<usize>(addr) >> PAGE_SHIFT;
}

inline constexpr PhysAddr page_to_addr(usize page) noexcept {
    return static_cast<PhysAddr>(page << PAGE_SHIFT);
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
    };

    [[nodiscard]] static MemoryStats get_memory_stats() noexcept;

private:
    struct MemoryRegion {
        PhysAddr start_addr;
        usize page_count;
        MemoryRegion* next;
    };

    struct FreeBlock {
        FreeBlock* next;
        FreeBlock* prev;
        usize order;
    };

    struct PageMetadata {
        moss::kernel::containers::AtomicU32 ref_count;
        moss::kernel::containers::AtomicU32 flags;
        FreeBlock* free_list_node;
    };

    static bool initialized_;
    static MemoryRegion* memory_regions_;
    static FreeBlock* free_lists_[MAX_ORDER + 1];
    static PageMetadata* page_metadata_;
    static usize total_pages_;
    static moss::kernel::containers::AtomicSize free_pages_;
    static moss::kernel::containers::AtomicSize used_pages_;

    static PageAllocVoidResult parse_memory_layout() noexcept;
    static void initialize_free_lists() noexcept;
    static FreeBlock* find_buddy(FreeBlock* block, usize order) noexcept;
    static void merge_buddies(FreeBlock* block, usize order) noexcept;
    static void split_block(FreeBlock* block, usize order) noexcept;
    static void add_to_free_list(FreeBlock* block, usize order) noexcept;
    static FreeBlock* remove_from_free_list(usize order) noexcept;
    static bool is_valid_page_address(PhysAddr addr) noexcept;
};

// ========================================================================
// buddy_allocator_v2.hpp
// ========================================================================

inline constexpr usize PAGEBLOCK_ORDER = 9;
inline constexpr usize PAGEBLOCK_SIZE = (1UL << PAGEBLOCK_ORDER);
inline constexpr usize PAGEBLOCK_PAGES = PAGEBLOCK_SIZE >> PAGE_SHIFT;

enum class MigrationType : u32 {
    UNMOVABLE = 0,
    MOVABLE = 1,
    RECLAIMABLE = 2,
    TYPES_COUNT = 3
};

enum class BuddyError : u32 {
    OutOfMemory = 1,
    InvalidOrder = 2,
    InvalidAddress = 3,
    InvalidMigrationType = 4,
    InitializationFailed = 5,
    FragmentationSevere = 6
};

template<typename T>
using BuddyResult = moss::kernel::Result<T, BuddyError>;
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
    [[nodiscard]] static BuddyResult<PhysAddr> allocate_pages(const PageAllocRequest& request) noexcept;
    [[nodiscard]] static BuddyResult<PhysAddr> allocate_pages(usize order,
                                                              MigrationType migration_type = MigrationType::MOVABLE) noexcept {
        return allocate_pages(PageAllocRequest(order, migration_type));
    }
    static BuddyVoidResult free_pages(PhysAddr addr, usize order) noexcept;
    static BuddyVoidResult compact_memory() noexcept;

    enum class WaterMark : u32 {
        LOW = 0,
        MIN = 1,
        HIGH = 2
    };

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
        enum Flags : u32 {
            MIXED = (1 << 0),
            DIRTY = (1 << 1),
            RESERVED = (1 << 2)
        };
    };

    struct FreeBlock {
        FreeBlock* next;
        FreeBlock* prev;
        usize order;
        MigrationType migration_type;
        u64 last_access_time;
    };

    struct PageMetadata {
        moss::kernel::containers::AtomicU32 ref_count;
        moss::kernel::containers::AtomicU32 flags;
        MigrationType migration_type;
        FreeBlock* free_list_node;
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
    static FreeBlock* free_lists_[static_cast<u32>(MigrationType::TYPES_COUNT)][MAX_ORDER + 1];
    static PageMetadata* page_metadata_;
    static PageBlock* page_blocks_;
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
    static PageBlock* get_pageblock(PhysAddr addr) noexcept;
    static void set_pageblock_migration_type(PageBlock* block, MigrationType type) noexcept;
    static MigrationType get_pageblock_migration_type(PhysAddr addr) noexcept;
    static void mark_pageblock_mixed(PageBlock* block) noexcept;
    static MigrationType get_fallback_migration_type(MigrationType original, usize order) noexcept;
    static bool can_steal_from_pageblock(PageBlock* block, MigrationType target_type, usize order) noexcept;
    static BuddyResult<PhysAddr> steal_pages(PageBlock* source_block, usize order, MigrationType target_type) noexcept;
    static FreeBlock* find_buddy_in_migration_type(FreeBlock* block, usize order, MigrationType migration_type) noexcept;
    static void merge_buddies_with_migration_check(FreeBlock* block, usize order) noexcept;
    static void add_to_free_list_typed(FreeBlock* block, usize order, MigrationType migration_type) noexcept;
    static FreeBlock* remove_from_free_list_typed(usize order, MigrationType migration_type) noexcept;
    static BuddyVoidResult compact_pageblock(PageBlock* block) noexcept;
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
}

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

template<typename T>
using HeapAllocResult = moss::kernel::Result<T, HeapAllocError>;
using HeapAllocVoidResult = moss::kernel::Result<void, HeapAllocError>;

class RuntimeHeapAllocator {
public:
    static HeapAllocVoidResult initialize_heap(VirtAddr heap_start, usize initial_size) noexcept;
    [[nodiscard]] static HeapAllocResult<void*> allocate(usize size) noexcept;
    [[nodiscard]] static HeapAllocResult<void*> allocate_aligned(usize size, usize alignment) noexcept;
    static HeapAllocVoidResult deallocate(void* ptr, usize size) noexcept;
    static HeapAllocVoidResult expand_heap(usize additional_size) noexcept;
    static HeapAllocVoidResult shrink_heap() noexcept;

    struct HeapStats {
        usize total_heap_size;
        usize allocated_bytes;
        usize free_bytes;
        usize fragmentation_ratio;
        usize largest_free_block;
    };

    [[nodiscard]] static HeapStats get_heap_stats() noexcept;
    [[nodiscard]] static VirtAddr get_heap_start() noexcept { return heap_start_; }
    [[nodiscard]] static VirtAddr get_heap_end() noexcept { return heap_end_; }
    [[nodiscard]] static usize get_heap_size() noexcept {
        return static_cast<usize>(heap_end_ - heap_start_);
    }

private:
    struct FreeBlock {
        usize size;
        FreeBlock* next;
        FreeBlock* prev;
        static constexpr u32 MAGIC = 0xDEADC0DE;
        u32 magic;
        FreeBlock(usize block_size) noexcept
            : size(block_size), next(nullptr), prev(nullptr), magic(MAGIC) {}
        [[nodiscard]] bool is_valid() const noexcept { return magic == MAGIC; }
    };

    struct AllocatedBlock {
        usize size;
        u32 magic;
        static constexpr u32 MAGIC = 0xBEEFF00D;
        AllocatedBlock(usize block_size) noexcept : size(block_size), magic(MAGIC) {}
        [[nodiscard]] bool is_valid() const noexcept { return magic == MAGIC; }
    };

    static bool initialized_;
    static VirtAddr heap_start_;
    static VirtAddr heap_end_;
    static VirtAddr heap_limit_;
    static FreeBlock* free_list_head_;
    static usize allocated_bytes_;
    static usize total_allocations_;
    static constexpr usize BLOCK_ALIGN = 16;
    static constexpr usize MIN_BLOCK_SIZE = sizeof(FreeBlock);

    [[nodiscard]] static constexpr usize align_size(usize size, usize alignment) noexcept {
        return (size + alignment - 1) & ~(alignment - 1);
    }

    static HeapAllocVoidResult map_heap_pages(VirtAddr start, usize size) noexcept;
    static HeapAllocVoidResult unmap_heap_pages(VirtAddr start, usize size) noexcept;
    static FreeBlock* find_suitable_block(usize required_size) noexcept;
    static void split_block(FreeBlock* block, usize required_size) noexcept;
    static void merge_free_blocks() noexcept;
    static void add_to_free_list(FreeBlock* block) noexcept;
    static void remove_from_free_list(FreeBlock* block) noexcept;
    static bool is_heap_address(void* ptr) noexcept;
};

// ========================================================================
// page_table.hpp - ARM64 MMU types
// ========================================================================

enum class MemoryAttributes : u8 {
    NORMAL_CACHEABLE = 0,
    NORMAL_NON_CACHEABLE = 1,
    DEVICE_nGnRnE = 2,
    DEVICE_nGnRE = 3,
    DEVICE_GRE = 4
};

enum class PageLevel : u32 {
    PGD = 0,
    PUD = 1,
    PMD = 2,
    PTE = 3
};

enum class PageSize : u64 {
    Size4KB = PAGE_SIZE,
    Size2MB = 2 * 1024 * 1024,
    Size1GB = 1024ULL * 1024 * 1024
};

namespace PageAttr {
    inline constexpr u64 VALID = (1ULL << 0);
    inline constexpr u64 TABLE = (1ULL << 1);
    inline constexpr u64 USER = (1ULL << 6);
    inline constexpr u64 READONLY = (1ULL << 7);
    inline constexpr u64 SHARED = (1ULL << 8);
    inline constexpr u64 AF = (1ULL << 10);
    inline constexpr u64 NG = (1ULL << 11);
    inline constexpr u64 PXN = (1ULL << 53);
    inline constexpr u64 XN = (1ULL << 54);
    inline constexpr u64 ATTR_IDX_SHIFT = 2;
    inline constexpr u64 ATTR_DEVICE = (0ULL << ATTR_IDX_SHIFT);
    inline constexpr u64 ATTR_NORMAL = (1ULL << ATTR_IDX_SHIFT);
    inline constexpr u64 ATTR_NORMAL_NC = (2ULL << ATTR_IDX_SHIFT);
}

namespace PagePerms {
    inline constexpr u64 KERNEL_RO = PageAttr::VALID | PageAttr::AF |
                                     PageAttr::ATTR_NORMAL | PageAttr::READONLY |
                                     PageAttr::PXN | PageAttr::XN;
    inline constexpr u64 KERNEL_RW = PageAttr::VALID | PageAttr::AF |
                                     PageAttr::ATTR_NORMAL | PageAttr::PXN |
                                     PageAttr::XN;
    inline constexpr u64 KERNEL_RX = PageAttr::VALID | PageAttr::AF |
                                     PageAttr::ATTR_NORMAL | PageAttr::READONLY;
    inline constexpr u64 USER_RO = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                                   PageAttr::READONLY | PageAttr::ATTR_NORMAL;
    inline constexpr u64 USER_RW = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                                   PageAttr::ATTR_NORMAL;
    inline constexpr u64 USER_RX = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                                   PageAttr::READONLY | PageAttr::ATTR_NORMAL;
    inline constexpr u64 DEVICE = PageAttr::VALID | PageAttr::AF |
                                  PageAttr::ATTR_DEVICE | PageAttr::XN |
                                  PageAttr::PXN;
}

struct [[gnu::packed]] PageTableEntry {
    u64 raw;
    constexpr PageTableEntry() : raw(0) {}
    constexpr explicit PageTableEntry(u64 value) : raw(value) {}
    [[nodiscard]] constexpr bool is_valid() const { return raw & PageAttr::VALID; }
    [[nodiscard]] constexpr bool is_table() const { return raw & PageAttr::TABLE; }
    [[nodiscard]] constexpr bool is_block() const { return is_valid() && !is_table(); }
    [[nodiscard]] constexpr PhysAddr get_phys_addr() const {
        return raw & 0x0000FFFFFFFFF000ULL;
    }
    constexpr void set_table(PhysAddr next_table_pa) {
        raw = (next_table_pa & 0x0000FFFFFFFFF000ULL) | PageAttr::VALID | PageAttr::TABLE;
    }
    constexpr void set_block(PhysAddr block_pa, u64 attributes) {
        raw = (block_pa & 0x0000FFFFFFFFF000ULL) | attributes | PageAttr::VALID;
    }
    constexpr void clear() { raw = 0; }
};

static_assert(sizeof(PageTableEntry) == 8, "PageTableEntry must be 8 bytes");

struct alignas(PAGE_SIZE) PageTable {
    static constexpr usize ENTRIES_PER_TABLE = PAGE_SIZE / sizeof(PageTableEntry);
    PageTableEntry entries[ENTRIES_PER_TABLE];
    constexpr PageTable() : entries{} {}
    [[nodiscard]] constexpr PageTableEntry& operator[](usize index) { return entries[index]; }
    [[nodiscard]] constexpr const PageTableEntry& operator[](usize index) const { return entries[index]; }
};

static_assert(sizeof(PageTable) == PAGE_SIZE, "PageTable must be one page");

struct VirtualAddressBreakdown {
    u16 pgd_index;
    u16 pud_index;
    u16 pmd_index;
    u16 pte_index;
    u16 page_offset;
};

inline constexpr VirtualAddressBreakdown break_virtual_address(VirtAddr vaddr) {
    return {.pgd_index = static_cast<u16>((vaddr >> 39) & 0x1FF),
            .pud_index = static_cast<u16>((vaddr >> 30) & 0x1FF),
            .pmd_index = static_cast<u16>((vaddr >> 21) & 0x1FF),
            .pte_index = static_cast<u16>((vaddr >> 12) & 0x1FF),
            .page_offset = static_cast<u16>(vaddr & 0xFFF)};
}

struct AddressSpaceConfig {
    static constexpr u64 TCR_VALUE =
        (16ULL << 0) | (16ULL << 16) | (0ULL << 6) | (0ULL << 23) |
        (0ULL << 14) | (0ULL << 30) | (1ULL << 8) | (1ULL << 10) |
        (3ULL << 12) | (1ULL << 24) | (1ULL << 26) | (3ULL << 28) |
        (5ULL << 32);
    static constexpr u64 MAIR_DEVICE_nGnRnE = 0x00ULL;
    static constexpr u64 MAIR_NORMAL_WBWA = 0xFFULL;
    static constexpr u64 MAIR_NORMAL_NC = 0x44ULL;
    static constexpr u64 MAIR_VALUE =
        (MAIR_DEVICE_nGnRnE << 0) | (MAIR_NORMAL_WBWA << 8) | (MAIR_NORMAL_NC << 16);
};

class PageTableManager {
private:
    static constexpr usize MAX_EARLY_TABLES = 64;
    alignas(PAGE_SIZE) static inline PageTable early_tables[MAX_EARLY_TABLES];
    static inline usize next_table_index = 0;
    static inline PageTable* kernel_pgd = nullptr;

public:
    [[nodiscard]] static KernelResult<PageTable*> allocate_page_table();
    [[nodiscard]] static PhysAddr get_physical_address(const PageTable* table) {
        return reinterpret_cast<PhysAddr>(table);
    }
    [[nodiscard]] static PageTable* get_table_from_physical(PhysAddr pa) {
        return reinterpret_cast<PageTable*>(pa);
    }
    [[nodiscard]] static usize get_table_index(const PageTable* table) {
        if (!table || table < early_tables || table >= early_tables + MAX_EARLY_TABLES) {
            return MAX_EARLY_TABLES;
        }
        return static_cast<usize>(table - early_tables);
    }
    [[nodiscard]] static PageTable* get_table_by_index(usize index) {
        if (index >= MAX_EARLY_TABLES) { return nullptr; }
        return &early_tables[index];
    }
    [[nodiscard]] static VoidResult setup_kernel_page_tables();
    [[nodiscard]] static VoidResult map_region(VirtAddr virt_addr, PhysAddr phys_addr,
                                               usize size, u64 permissions);
    [[nodiscard]] static VoidResult map_page(VirtAddr virt_addr, PhysAddr phys_addr,
                                             u64 permissions);
    [[nodiscard]] static VoidResult enable_mmu();
    [[nodiscard]] static PageTable* get_kernel_pgd() { return kernel_pgd; }
    static void invalidate_tlb() {
        moss::kernel::arch::flush_tlb();
    }
    [[nodiscard]] static VoidResult initialize_from_current() {
        return VoidResult{};
    }
    static void print_page_table_details();
    static void print_pgd_entries();
    static void print_mmu_registers();
};

[[nodiscard]] VoidResult setup_mmu();
void invalidate_all_tlb();

// ========================================================================
// numa_policy.hpp
// ========================================================================

enum class NUMAError : u32 {
    InvalidNode = 1,
    NodeUnavailable = 2,
    AllocationFailed = 3,
    MigrationFailed = 4,
    TopologyInvalid = 5,
    PolicyViolation = 6,
    OutOfMemory = 7
};

template<typename T>
using NUMAResult = moss::kernel::Result<T, NUMAError>;
using NUMAVoidResult = moss::kernel::Result<void, NUMAError>;

enum class NUMAPolicy : u32 {
    DEFAULT = 0,
    BIND = 1,
    INTERLEAVE = 2,
    PREFERRED = 3,
    LOCAL = 4
};

enum class NodeState : u8 {
    OFFLINE = 0,
    ONLINE = 1,
    PARTIAL = 2,
    RESERVED = 3
};

struct NUMADistance {
    static constexpr u8 LOCAL_DISTANCE = 10;
    static constexpr u8 REMOTE_DISTANCE = 20;
    static constexpr u8 UNREACHABLE_DISTANCE = 255;

    u8 distance[MAX_NUMA_NODES][MAX_NUMA_NODES];

    NUMADistance() noexcept {
        for (u32 i = 0; i < MAX_NUMA_NODES; ++i) {
            for (u32 j = 0; j < MAX_NUMA_NODES; ++j) {
                if (i == j) {
                    distance[i][j] = LOCAL_DISTANCE;
                } else {
                    distance[i][j] = UNREACHABLE_DISTANCE;
                }
            }
        }
    }

    [[nodiscard]] u8 get_distance(numa_node_t from, numa_node_t to) const noexcept {
        if (from >= MAX_NUMA_NODES || to >= MAX_NUMA_NODES) { return UNREACHABLE_DISTANCE; }
        return distance[from][to];
    }

    void set_distance(numa_node_t from, numa_node_t to, u8 dist) noexcept {
        if (from < MAX_NUMA_NODES && to < MAX_NUMA_NODES) {
            distance[from][to] = dist;
            distance[to][from] = dist;
        }
    }
};

struct NUMANode {
    numa_node_t node_id;
    NodeState state;
    PhysAddr memory_start;
    usize memory_size;
    moss::kernel::containers::AtomicSize free_memory;
    moss::kernel::containers::AtomicSize allocated_memory;
    u64 cpu_mask;
    u32 cpu_count;
    moss::kernel::containers::AtomicU64 local_allocations;
    moss::kernel::containers::AtomicU64 remote_allocations;
    moss::kernel::containers::AtomicU64 migration_count;
    moss::kernel::containers::AtomicU32 memory_pressure;
    u64 last_balance_time;

    NUMANode() noexcept
        : node_id(NUMA_NO_NODE), state(NodeState::OFFLINE),
          memory_start(0), memory_size(0), free_memory(0), allocated_memory(0),
          cpu_mask(0), cpu_count(0),
          local_allocations(0), remote_allocations(0), migration_count(0),
          memory_pressure(0), last_balance_time(0) {}

    NUMANode(numa_node_t id, PhysAddr start, usize size) noexcept
        : node_id(id), state(NodeState::ONLINE),
          memory_start(start), memory_size(size), free_memory(size), allocated_memory(0),
          cpu_mask(0), cpu_count(0),
          local_allocations(0), remote_allocations(0), migration_count(0),
          memory_pressure(0), last_balance_time(0) {}
};

class NUMATopologyImpl;
class NUMAAllocatorImpl;
class NUMABalancerImpl;

class NUMATopology {
    friend class NUMATopologyImpl;
public:
    struct TopologyStats {
        u32 online_nodes;
        u32 total_nodes;
        usize total_memory;
        usize available_memory;
        double memory_balance_ratio;
        u32 active_cpus;
    };

protected:
    NUMANode nodes_[MAX_NUMA_NODES];
    NUMADistance distance_matrix_;
    moss::kernel::containers::AtomicU32 online_node_count_;
    u64 topology_version_;
    numa_node_t cpu_to_node_[moss::kernel::MAX_CPUS];

public:
    NUMATopology() noexcept;
    NUMAVoidResult discover_topology() noexcept;
    NUMAVoidResult add_node(numa_node_t node_id, PhysAddr start, usize size) noexcept;
    NUMAVoidResult remove_node(numa_node_t node_id) noexcept;
    NUMAVoidResult set_node_state(numa_node_t node_id, NodeState state) noexcept;
    [[nodiscard]] NodeState get_node_state(numa_node_t node_id) const noexcept;
    [[nodiscard]] bool is_node_online(numa_node_t node_id) const noexcept;
    NUMAVoidResult bind_cpu_to_node(u32 cpu_id, numa_node_t node_id) noexcept;
    [[nodiscard]] numa_node_t get_cpu_node(u32 cpu_id) const noexcept;
    [[nodiscard]] numa_node_t get_current_node() const noexcept;
    void set_node_distance(numa_node_t from, numa_node_t to, u8 distance) noexcept;
    [[nodiscard]] u8 get_node_distance(numa_node_t from, numa_node_t to) const noexcept;
    [[nodiscard]] numa_node_t find_nearest_node(numa_node_t from) const noexcept;
    [[nodiscard]] const NUMANode* get_node(numa_node_t node_id) const noexcept;
    [[nodiscard]] NUMANode* get_node_mutable(numa_node_t node_id) noexcept;
    [[nodiscard]] usize get_node_free_memory(numa_node_t node_id) const noexcept;
    [[nodiscard]] u32 get_online_node_count() const noexcept;
    [[nodiscard]] TopologyStats get_topology_stats() const noexcept;
    void update_topology_version() noexcept { topology_version_++; }
private:
    void initialize_default_topology() noexcept;
    void detect_cpu_topology() noexcept;
    void calculate_distances() noexcept;
};

class NUMAAllocator {
    friend class NUMAAllocatorImpl;
public:
    struct AllocationRequest {
        usize size;
        usize alignment;
        NUMAPolicy policy;
        numa_node_t preferred_node;
        u64 node_mask;
        u32 flags;
        enum Flags : u32 {
            NONE = 0,
            ZERO_MEMORY = (1 << 0),
            HIGH_PRIORITY = (1 << 1),
            NO_FALLBACK = (1 << 2),
            MIGRATE_ALLOWED = (1 << 3)
        };
        AllocationRequest(usize sz, NUMAPolicy pol = NUMAPolicy::DEFAULT,
                         numa_node_t node = NUMA_NO_NODE) noexcept
            : size(sz), alignment(PAGE_SIZE), policy(pol),
              preferred_node(node), node_mask(~0UL), flags(Flags::NONE) {}
    };

    struct AllocationStats {
        usize total_allocations;
        usize local_allocations;
        usize remote_allocations;
        usize fallback_allocations;
        usize failed_allocations;
        double locality_ratio;
        u64 average_allocation_time_us;
    };

protected:
    NUMATopology* topology_;
    AllocationStats stats_;
    moss::kernel::containers::AtomicU32 current_interleave_node_;

public:
    NUMAAllocator(NUMATopology* topo) noexcept : topology_(topo), stats_{}, current_interleave_node_(0) {}
    [[nodiscard]] NUMAResult<PhysAddr> allocate_pages(const AllocationRequest& request) noexcept;
    NUMAVoidResult free_pages(PhysAddr addr, usize size) noexcept;
    [[nodiscard]] NUMAResult<PhysAddr> allocate_default(const AllocationRequest& request) noexcept;
    [[nodiscard]] NUMAResult<PhysAddr> allocate_bind(const AllocationRequest& request) noexcept;
    [[nodiscard]] NUMAResult<PhysAddr> allocate_interleave(const AllocationRequest& request) noexcept;
    [[nodiscard]] NUMAResult<PhysAddr> allocate_preferred(const AllocationRequest& request) noexcept;
    [[nodiscard]] NUMAResult<PhysAddr> allocate_local(const AllocationRequest& request) noexcept;
    [[nodiscard]] numa_node_t select_allocation_node(const AllocationRequest& request) const noexcept;
    [[nodiscard]] numa_node_t select_fallback_node(numa_node_t failed_node, const AllocationRequest& request) const noexcept;
    [[nodiscard]] AllocationStats get_stats() const noexcept { return stats_; }
    void reset_stats() noexcept;
private:
    [[nodiscard]] NUMAResult<PhysAddr> allocate_from_node(numa_node_t node, const AllocationRequest& request) noexcept;
    void update_allocation_stats(numa_node_t allocated_node, numa_node_t preferred_node, bool fallback_used) noexcept;
};

class NUMABalancer {
    friend class NUMABalancerImpl;
public:
    struct BalancerConfig {
        u64 balance_interval_ms;
        u32 imbalance_threshold;
        u32 migration_rate_limit;
        double memory_threshold_ratio;
    };
    struct BalancerStats {
        usize migrations_performed;
        usize migrations_failed;
        u64 total_balance_time_us;
        u32 balance_cycles;
        double average_imbalance_before;
        double average_imbalance_after;
    };
protected:
    BalancerConfig config_;
    NUMATopology* topology_;
    NUMAAllocator* allocator_;
    BalancerStats stats_;
    moss::kernel::containers::AtomicBool balancer_active_;
    u64 last_balance_time_;
public:
    NUMABalancer(const BalancerConfig& config, NUMATopology* topo, NUMAAllocator* alloc) noexcept;
    NUMAVoidResult start_balancer() noexcept;
    void stop_balancer() noexcept;
    [[nodiscard]] bool is_balancer_active() const noexcept;
    NUMAVoidResult balance_memory_load() noexcept;
    NUMAVoidResult migrate_pages_between_nodes(numa_node_t from, numa_node_t to, usize page_count) noexcept;
    [[nodiscard]] double calculate_memory_imbalance() const noexcept;
    [[nodiscard]] bool should_balance() const noexcept;
    [[nodiscard]] NUMAResult<numa_node_t> find_source_node() const noexcept;
    [[nodiscard]] NUMAResult<numa_node_t> find_target_node(numa_node_t source) const noexcept;
    [[nodiscard]] BalancerStats get_stats() const noexcept { return stats_; }
    void reset_stats() noexcept;
private:
    NUMAVoidResult background_balance_thread() noexcept;
    [[nodiscard]] usize calculate_migration_count(numa_node_t from, numa_node_t to) const noexcept;
};

class NUMAPolicyManager {
public:
    struct NUMAConfig {
        NUMABalancer::BalancerConfig balancer_config;
        bool enable_auto_balancing;
        bool enable_migration;
        NUMAPolicy default_policy;
    };
    struct NUMASystemStats {
        NUMATopology::TopologyStats topology_stats;
        NUMAAllocator::AllocationStats allocator_stats;
        NUMABalancer::BalancerStats balancer_stats;
        double overall_numa_efficiency;
    };
protected:
    NUMAConfig config_;
    NUMATopology topology_;
    NUMAAllocator allocator_;
    NUMABalancer balancer_;
    moss::kernel::containers::AtomicBool system_initialized_;
public:
    static NUMAVoidResult initialize(const NUMAConfig& config) noexcept;
    NUMAVoidResult start_numa_system() noexcept;
    void stop_numa_system() noexcept;
    [[nodiscard]] bool is_numa_enabled() const noexcept;
    [[nodiscard]] NUMAResult<PhysAddr> numa_alloc_pages(usize count,
                                                                     NUMAPolicy policy = NUMAPolicy::DEFAULT,
                                                                     numa_node_t preferred_node = NUMA_NO_NODE) noexcept;
    NUMAVoidResult numa_free_pages(PhysAddr addr, usize count) noexcept;
    NUMAVoidResult set_default_policy(NUMAPolicy policy) noexcept;
    [[nodiscard]] NUMAPolicy get_default_policy() const noexcept;
    [[nodiscard]] numa_node_t get_current_numa_node() const noexcept;
    [[nodiscard]] numa_node_t get_preferred_node(usize size) const noexcept;
    [[nodiscard]] bool is_numa_node_available(numa_node_t node) const noexcept;
    [[nodiscard]] NUMASystemStats get_system_stats() const noexcept;
    [[nodiscard]] double get_numa_efficiency() const noexcept;
    [[nodiscard]] static NUMAPolicyManager& get_instance() noexcept;
private:
    NUMAPolicyManager(const NUMAConfig& config) noexcept;
    static bool initialized_;
    static NUMAPolicyManager* instance_;
};

namespace numa {
    inline NUMAVoidResult initialize(const NUMAPolicyManager::NUMAConfig& config) noexcept {
        return NUMAPolicyManager::initialize(config);
    }
    inline NUMAResult<PhysAddr> alloc_pages(usize count,
                                                          NUMAPolicy policy = NUMAPolicy::DEFAULT) noexcept {
        return NUMAPolicyManager::get_instance().numa_alloc_pages(count, policy);
    }
    inline NUMAVoidResult free_pages(PhysAddr addr, usize count) noexcept {
        return NUMAPolicyManager::get_instance().numa_free_pages(addr, count);
    }
    inline numa_node_t get_current_node() noexcept {
        return NUMAPolicyManager::get_instance().get_current_numa_node();
    }
    inline double get_efficiency() noexcept {
        return NUMAPolicyManager::get_instance().get_numa_efficiency();
    }
    inline bool is_available() noexcept {
        return NUMAPolicyManager::get_instance().is_numa_enabled();
    }
}

// ========================================================================
// vmalloc_allocator.hpp
// ========================================================================

enum class VmallocError : u32 {
    OutOfMemory = 1,
    InvalidAddress = 2,
    InvalidSize = 3,
    AlignmentError = 4,
    MappingFailed = 5,
    UnmappingFailed = 6,
    AddressSpaceExhausted = 7,
    InitializationFailed = 8,
    PermissionDenied = 9,
    OverlappingRegion = 10,
    RegionNotFound = 11,
    LazyFreeFailed = 12
};

template<typename T>
using VmallocResult = moss::kernel::Result<T, VmallocError>;
using VmallocVoidResult = moss::kernel::Result<void, VmallocError>;

enum class VmAreaType : u32 {
    VMALLOC = 0,
    IOREMAP = 1,
    MODULE = 2,
    KERNEL_STACK = 3,
    GUARD_PAGE = 4,
    DMA = 5,
    PERCPU = 6,
    HUGE_PAGE = 7
};

struct VmallocRequest {
    usize size;
    usize alignment;
    VmAreaType type;
    u64 permissions;
    numa_node_t preferred_node;
    u32 flags;

    enum Flags : u32 {
        NONE = 0,
        ZERO_MEMORY = (1 << 0),
        GUARD_PAGES = (1 << 1),
        NO_LAZY_FREE = (1 << 2),
        HUGE_PAGES = (1 << 3),
        DMA_COHERENT = (1 << 4),
        EXECUTABLE = (1 << 5)
    };

    VmallocRequest(usize sz, VmAreaType t = VmAreaType::VMALLOC) noexcept
        : size(sz), alignment(PAGE_SIZE), type(t),
          permissions(PagePerms::KERNEL_RW),
          preferred_node(NUMA_NO_NODE), flags(Flags::GUARD_PAGES) {}
};

enum class RBColor : u8 {
    RED = 0,
    BLACK = 1
};

struct VmArea {
    VirtAddr start;
    VirtAddr end;
    usize size;
    VmAreaType type;
    u64 permissions;
    numa_node_t numa_node;
    u32 flags;

    // Red-black tree linkage
    VmArea* parent;
    VmArea* left;
    VmArea* right;
    RBColor color;

    // Linked list for ordered traversal
    VmArea* list_next;
    VmArea* list_prev;

    // Statistics
    u64 creation_time;
    moss::kernel::containers::AtomicU64 access_count;
    moss::kernel::containers::AtomicU32 ref_count;

    VmArea() noexcept
        : start(0), end(0), size(0), type(VmAreaType::VMALLOC),
          permissions(0), numa_node(NUMA_NO_NODE), flags(0),
          parent(nullptr), left(nullptr), right(nullptr), color(RBColor::RED),
          list_next(nullptr), list_prev(nullptr),
          creation_time(0), access_count(0), ref_count(0) {}

    VmArea(VirtAddr s, usize sz, VmAreaType t, u64 perms) noexcept
        : start(s), end(s + sz), size(sz), type(t),
          permissions(perms), numa_node(NUMA_NO_NODE), flags(0),
          parent(nullptr), left(nullptr), right(nullptr), color(RBColor::RED),
          list_next(nullptr), list_prev(nullptr),
          creation_time(0), access_count(0), ref_count(0) {}

    [[nodiscard]] bool contains(VirtAddr addr) const noexcept {
        return addr >= start && addr < end;
    }

    [[nodiscard]] bool overlaps(VirtAddr other_start, VirtAddr other_end) const noexcept {
        return start < other_end && other_start < end;
    }
};

class VirtualAddressSpace {
public:
    VirtualAddressSpace(VirtAddr space_start, VirtAddr space_end) noexcept;

    [[nodiscard]] VmallocResult<VirtAddr> allocate_range(usize size, usize alignment = PAGE_SIZE) noexcept;
    VmallocVoidResult free_range(VirtAddr addr) noexcept;
    [[nodiscard]] VmArea* find_area(VirtAddr addr) const noexcept;
    [[nodiscard]] VmArea* find_area_containing(VirtAddr addr) const noexcept;

    struct AddressSpaceStats {
        usize total_size;
        usize allocated_size;
        usize free_size;
        usize area_count;
        usize largest_free_gap;
        usize fragmentation_ratio;
    };

    [[nodiscard]] AddressSpaceStats get_stats() const noexcept;

private:
    VirtAddr space_start_;
    VirtAddr space_end_;
    VmArea* rb_root_;
    VmArea* area_list_head_;
    usize area_count_;
    moss::kernel::containers::AtomicSize allocated_size_;

    // Red-black tree operations
    void rb_insert(VmArea* area) noexcept;
    void rb_remove(VmArea* area) noexcept;
    void rb_insert_fixup(VmArea* area) noexcept;
    void rb_remove_fixup(VmArea* area) noexcept;
    void rb_rotate_left(VmArea* area) noexcept;
    void rb_rotate_right(VmArea* area) noexcept;
    void rb_transplant(VmArea* old_area, VmArea* new_area) noexcept;
    [[nodiscard]] VmArea* rb_minimum(VmArea* area) const noexcept;

    // List operations
    void list_insert(VmArea* area) noexcept;
    void list_remove(VmArea* area) noexcept;

    // Gap finding
    [[nodiscard]] VmallocResult<VirtAddr> find_free_gap(usize size, usize alignment) const noexcept;
};

class LazyFreeManager {
public:
    struct LazyFreeEntry {
        VirtAddr start;
        usize size;
        u64 free_time;
        LazyFreeEntry* next;
    };

    LazyFreeManager() noexcept;

    VmallocVoidResult add_lazy_free(VirtAddr addr, usize size) noexcept;
    VmallocVoidResult flush_lazy_frees() noexcept;
    VmallocVoidResult flush_if_needed() noexcept;

    [[nodiscard]] usize get_pending_count() const noexcept { return pending_count_; }
    [[nodiscard]] usize get_pending_size() const noexcept { return pending_size_; }

private:
    LazyFreeEntry* pending_list_;
    usize pending_count_;
    usize pending_size_;
    static constexpr usize MAX_PENDING_SIZE = 64 * 1024 * 1024;  // 64MB
    static constexpr usize MAX_PENDING_COUNT = 256;

    VmallocVoidResult do_flush(LazyFreeEntry* entry) noexcept;
};

class VmallocAllocator {
public:
    struct VmallocConfig {
        bool enable_lazy_free;
        usize lazy_free_threshold;
        usize max_lazy_free_memory;
        bool enable_numa_awareness;
        u32 default_numa_policy;
    };

    static VmallocVoidResult initialize() noexcept;

    [[nodiscard]] static VmallocResult<void*> vmalloc(usize size) noexcept;
    [[nodiscard]] static VmallocResult<void*> vmalloc_aligned(usize size, usize alignment) noexcept;
    [[nodiscard]] static VmallocResult<void*> vmalloc_request(const VmallocRequest& request) noexcept;
    static VmallocVoidResult vfree(void* addr) noexcept;

    [[nodiscard]] static VmallocResult<void*> ioremap(PhysAddr phys_addr, usize size) noexcept;
    static VmallocVoidResult iounmap(void* addr) noexcept;

    [[nodiscard]] static VmallocResult<void*> alloc_kernel_stack(usize stack_size = 16 * PAGE_SIZE) noexcept;
    static VmallocVoidResult free_kernel_stack(void* stack_base) noexcept;

    [[nodiscard]] static VmallocResult<void*> alloc_module_space(usize size) noexcept;
    static VmallocVoidResult free_module_space(void* addr) noexcept;

    struct VmallocStats {
        usize total_vmalloc_size;
        usize allocated_vmalloc_size;
        usize free_vmalloc_size;
        usize area_count;
        usize ioremap_count;
        usize kernel_stack_count;
        usize lazy_free_pending;
        usize largest_free_block;
    };

    [[nodiscard]] static VmallocStats get_stats() noexcept;

    static VmallocVoidResult flush_lazy_frees() noexcept;

private:
    static bool initialized_;
    static VirtualAddressSpace* address_space_;
    static LazyFreeManager* lazy_free_manager_;

    static VmallocVoidResult map_vmalloc_pages(VirtAddr vaddr, usize size,
                                               u64 permissions, numa_node_t node) noexcept;
    static VmallocVoidResult unmap_vmalloc_pages(VirtAddr vaddr, usize size) noexcept;
    static VmallocVoidResult setup_guard_pages(VirtAddr start, usize total_size, usize usable_size) noexcept;
};

namespace vmalloc {
    [[nodiscard]] inline VmallocResult<void*> alloc(usize size) noexcept {
        return VmallocAllocator::vmalloc(size);
    }
    [[nodiscard]] inline VmallocResult<void*> alloc_aligned(usize size, usize alignment) noexcept {
        return VmallocAllocator::vmalloc_aligned(size, alignment);
    }
    inline VmallocVoidResult free(void* addr) noexcept {
        return VmallocAllocator::vfree(addr);
    }
    [[nodiscard]] inline VmallocResult<void*> ioremap(PhysAddr phys_addr, usize size) noexcept {
        return VmallocAllocator::ioremap(phys_addr, size);
    }
    inline VmallocVoidResult iounmap(void* addr) noexcept {
        return VmallocAllocator::iounmap(addr);
    }
    [[nodiscard]] inline VmallocResult<void*> alloc_stack(usize size = 16 * PAGE_SIZE) noexcept {
        return VmallocAllocator::alloc_kernel_stack(size);
    }
    inline VmallocVoidResult free_stack(void* base) noexcept {
        return VmallocAllocator::free_kernel_stack(base);
    }
}

// ========================================================================
// memory_reclaim.hpp
// ========================================================================

enum class ReclaimError : u32 {
    NoReclaimablePages = 1,
    ScanFailed = 2,
    EvictionFailed = 3,
    WritebackFailed = 4,
    InvalidPage = 5,
    SystemBusy = 6,
    OutOfMemory = 7,
    InitializationFailed = 8,
    PolicyViolation = 9
};

template<typename T>
using ReclaimResult = moss::kernel::Result<T, ReclaimError>;
using ReclaimVoidResult = moss::kernel::Result<void, ReclaimError>;

enum class PageState : u8 {
    ACTIVE = 0,
    INACTIVE = 1,
    REFERENCED = 2,
    DIRTY = 3,
    WRITEBACK = 4,
    LOCKED = 5,
    SLAB = 6,
    COMPOUND = 7,
    UNEVICTABLE = 8
};

enum class AccessPattern : u8 {
    FREQUENT = 0,
    MODERATE = 1,
    RARE = 2,
    ONCE = 3,
    STREAMING = 4
};

enum class ReclaimPolicy : u32 {
    CONSERVATIVE = 0,   // Conservative - preserve working set
    BALANCED = 1,       // Balanced - default policy
    AGGRESSIVE = 2,     // Aggressive - maximize free memory
    EMERGENCY = 3       // Emergency - OOM situations
};

struct ReclaimPageInfo {
    PhysAddr phys_addr;
    VirtAddr virt_addr;
    PageState state;
    AccessPattern access_pattern;
    MigrationType migration_type;
    u32 age;
    u32 reference_count;
    u64 last_access_time;
    u64 creation_time;
    bool is_dirty;
    bool is_mapped;
    bool is_locked;
    numa_node_t numa_node;

    ReclaimPageInfo() noexcept
        : phys_addr(0), virt_addr(0), state(PageState::ACTIVE),
          access_pattern(AccessPattern::MODERATE),
          migration_type(MigrationType::MOVABLE),
          age(0), reference_count(0), last_access_time(0), creation_time(0),
          is_dirty(false), is_mapped(false), is_locked(false),
          numa_node(NUMA_NO_NODE) {}
};

class LRUList {
public:
    struct LRUEntry {
        ReclaimPageInfo page_info;
        LRUEntry* next;
        LRUEntry* prev;
        u32 scan_count;

        LRUEntry() noexcept : page_info(), next(nullptr), prev(nullptr), scan_count(0) {}
    };

    enum class ListType : u32 {
        ACTIVE_ANON = 0,
        INACTIVE_ANON = 1,
        ACTIVE_FILE = 2,
        INACTIVE_FILE = 3,
        UNEVICTABLE = 4,
        LIST_COUNT = 5
    };

    LRUList() noexcept;

    ReclaimVoidResult add_page(const ReclaimPageInfo& page, ListType list_type) noexcept;
    ReclaimVoidResult remove_page(PhysAddr addr) noexcept;
    ReclaimVoidResult move_page(PhysAddr addr, ListType from, ListType to) noexcept;
    ReclaimVoidResult promote_page(PhysAddr addr) noexcept;
    ReclaimVoidResult demote_page(PhysAddr addr) noexcept;
    [[nodiscard]] LRUEntry* get_tail(ListType list_type) noexcept;
    [[nodiscard]] usize get_list_size(ListType list_type) const noexcept;
    [[nodiscard]] usize get_total_pages() const noexcept;

    struct LRUStats {
        usize list_sizes[static_cast<u32>(ListType::LIST_COUNT)];
        usize total_pages;
        u64 promotions;
        u64 demotions;
        u64 evictions;
    };

    [[nodiscard]] LRUStats get_stats() const noexcept;

private:
    LRUEntry* heads_[static_cast<u32>(ListType::LIST_COUNT)];
    LRUEntry* tails_[static_cast<u32>(ListType::LIST_COUNT)];
    moss::kernel::containers::AtomicSize sizes_[static_cast<u32>(ListType::LIST_COUNT)];
    moss::kernel::containers::AtomicU64 promotion_count_;
    moss::kernel::containers::AtomicU64 demotion_count_;
    moss::kernel::containers::AtomicU64 eviction_count_;

    void add_to_head(LRUEntry* entry, ListType list_type) noexcept;
    void remove_entry(LRUEntry* entry, ListType list_type) noexcept;
    [[nodiscard]] LRUEntry* find_entry(PhysAddr addr) noexcept;
    [[nodiscard]] ListType find_entry_list(PhysAddr addr) noexcept;
};

class WorkingSetDetector {
public:
    struct WorkingSetInfo {
        usize estimated_size;
        usize hot_pages;
        usize warm_pages;
        usize cold_pages;
        double refault_distance;
        u64 detection_time;
    };

    WorkingSetDetector() noexcept;

    void record_page_access(PhysAddr addr, u64 timestamp) noexcept;
    void record_page_eviction(PhysAddr addr, u64 timestamp) noexcept;
    void record_page_refault(PhysAddr addr, u64 timestamp) noexcept;

    [[nodiscard]] WorkingSetInfo estimate_working_set() const noexcept;
    [[nodiscard]] bool is_page_in_working_set(PhysAddr addr) const noexcept;
    [[nodiscard]] AccessPattern classify_page_access(PhysAddr addr) const noexcept;

    void update_refault_distance() noexcept;
    void age_working_set() noexcept;

private:
    struct PageAccessInfo {
        u64 last_access;
        u64 eviction_time;
        u32 access_frequency;
        u32 refault_count;
    };

    static constexpr usize ACCESS_HISTORY_SIZE = 1024;
    PageAccessInfo access_history_[ACCESS_HISTORY_SIZE];
    moss::kernel::containers::AtomicSize history_index_;
    usize estimated_working_set_size_;
    double average_refault_distance_;
    u64 last_aging_time_;
};

class PageScanner {
public:
    struct ScanConfig {
        usize scan_batch_size;
        usize max_scan_pages;
        u32 scan_priority;
        ReclaimPolicy policy;
        bool scan_anon_pages;
        bool scan_file_pages;
        bool allow_writeback;
    };

    struct ScanResult {
        usize scanned_pages;
        usize reclaimed_pages;
        usize promoted_pages;
        usize demoted_pages;
        usize writeback_pages;
        u64 scan_time_us;
    };

    PageScanner(LRUList* lru_list, WorkingSetDetector* ws_detector) noexcept;

    [[nodiscard]] ReclaimResult<ScanResult> scan_inactive_list(const ScanConfig& config) noexcept;
    [[nodiscard]] ReclaimResult<ScanResult> scan_active_list(const ScanConfig& config) noexcept;
    [[nodiscard]] ReclaimResult<usize> shrink_page_list(usize nr_to_reclaim, const ScanConfig& config) noexcept;

    [[nodiscard]] bool should_reclaim_page(const ReclaimPageInfo& page, const ScanConfig& config) const noexcept;
    [[nodiscard]] bool is_page_referenced(const ReclaimPageInfo& page) const noexcept;

private:
    LRUList* lru_list_;
    WorkingSetDetector* ws_detector_;
    moss::kernel::containers::AtomicU64 total_scans_;
    moss::kernel::containers::AtomicU64 total_reclaimed_;

    ReclaimVoidResult try_reclaim_page(LRUList::LRUEntry* entry, const ScanConfig& config) noexcept;
    ReclaimVoidResult writeback_page(const ReclaimPageInfo& page) noexcept;
    void update_page_age(LRUList::LRUEntry* entry) noexcept;
};

class MemoryPressureMonitor {
public:
    struct PressureConfig {
        usize low_threshold_pages;
        usize medium_threshold_pages;
        usize high_threshold_pages;
        usize critical_threshold_pages;
        u64 check_interval_ms;
    };

    struct PressureInfo {
        MemoryPressure current_level;
        MemoryPressure previous_level;
        usize available_pages;
        usize reclaimable_pages;
        double pressure_score;
        u64 time_in_pressure_ms;
        u32 oom_kill_count;
    };

    MemoryPressureMonitor(const PressureConfig& config) noexcept;

    void update_pressure() noexcept;
    [[nodiscard]] MemoryPressure get_current_pressure() const noexcept;
    [[nodiscard]] PressureInfo get_pressure_info() const noexcept;
    [[nodiscard]] bool is_oom_imminent() const noexcept;

    void register_pressure_callback(void (*callback)(MemoryPressure)) noexcept;
    void notify_pressure_change() noexcept;

private:
    PressureConfig config_;
    moss::kernel::containers::AtomicU32 current_pressure_;
    PressureInfo pressure_info_;
    u64 pressure_start_time_;
    void (*pressure_callback_)(MemoryPressure);

    MemoryPressure calculate_pressure_level(usize available_pages) const noexcept;
};

class MemoryReclaimEngine {
public:
    struct ReclaimConfig {
        PageScanner::ScanConfig scan_config;
        MemoryPressureMonitor::PressureConfig pressure_config;
        ReclaimPolicy default_policy;
        usize min_free_pages;
        usize target_free_pages;
        bool enable_background_reclaim;
        u64 background_reclaim_interval_ms;
    };

    struct ReclaimStats {
        u64 total_reclaim_calls;
        u64 total_pages_reclaimed;
        u64 total_pages_scanned;
        u64 direct_reclaim_count;
        u64 background_reclaim_count;
        u64 oom_kill_count;
        double average_reclaim_efficiency;
        u64 total_reclaim_time_us;
    };

    static ReclaimVoidResult initialize(const ReclaimConfig& config) noexcept;
    [[nodiscard]] static ReclaimResult<usize> reclaim_pages(usize nr_to_reclaim) noexcept;
    [[nodiscard]] static ReclaimResult<usize> direct_reclaim(usize nr_to_reclaim) noexcept;
    static ReclaimVoidResult background_reclaim() noexcept;
    static ReclaimVoidResult shrink_all_caches() noexcept;
    [[nodiscard]] static bool should_reclaim() noexcept;
    [[nodiscard]] static MemoryPressure get_memory_pressure() noexcept;
    static void add_page_to_lru(const ReclaimPageInfo& page) noexcept;
    static void remove_page_from_lru(PhysAddr addr) noexcept;
    static void mark_page_accessed(PhysAddr addr) noexcept;
    static void mark_page_dirty(PhysAddr addr) noexcept;
    [[nodiscard]] static ReclaimStats get_stats() noexcept;
    [[nodiscard]] static MemoryReclaimEngine& get_instance() noexcept;

private:
    ReclaimConfig config_;
    LRUList lru_list_;
    WorkingSetDetector ws_detector_;
    PageScanner scanner_;
    MemoryPressureMonitor pressure_monitor_;
    ReclaimStats stats_;
    moss::kernel::containers::AtomicBool reclaim_active_;

    MemoryReclaimEngine(const ReclaimConfig& config) noexcept;
    ReclaimVoidResult background_reclaim_thread() noexcept;
    ReclaimResult<usize> do_reclaim(usize nr_to_reclaim, bool direct) noexcept;
    void update_reclaim_stats(usize scanned, usize reclaimed, u64 time_us) noexcept;
    [[nodiscard]] usize calculate_reclaim_target() const noexcept;
    [[nodiscard]] u32 calculate_scan_priority() const noexcept;

    static bool initialized_;
    static MemoryReclaimEngine* instance_;
};

namespace memory_reclaim {
    inline ReclaimVoidResult initialize(const MemoryReclaimEngine::ReclaimConfig& config) noexcept {
        return MemoryReclaimEngine::initialize(config);
    }
    inline ReclaimResult<usize> reclaim(usize nr_pages) noexcept {
        return MemoryReclaimEngine::reclaim_pages(nr_pages);
    }
    inline bool should_reclaim() noexcept {
        return MemoryReclaimEngine::should_reclaim();
    }
    inline MemoryPressure get_pressure() noexcept {
        return MemoryReclaimEngine::get_memory_pressure();
    }
    inline void page_accessed(PhysAddr addr) noexcept {
        MemoryReclaimEngine::mark_page_accessed(addr);
    }
    inline void page_dirty(PhysAddr addr) noexcept {
        MemoryReclaimEngine::mark_page_dirty(addr);
    }
}

// ========================================================================
// memory_compaction.hpp
// ========================================================================

enum class CompactionError : u32 {
    NoCompactionNeeded = 1,
    MigrationFailed = 2,
    SourcePageLocked = 3,
    TargetPageUnavailable = 4,
    InvalidPage = 5,
    SystemBusy = 6,
    OutOfMemory = 7,
    InitializationFailed = 8,
    CMARegionFull = 9,
    CMARegionNotFound = 10
};

template<typename T>
using CompactionResult = moss::kernel::Result<T, CompactionError>;
using CompactionVoidResult = moss::kernel::Result<void, CompactionError>;

enum class CompactionStrategy : u32 {
    LIGHT = 0,      // Light - only easily movable pages
    MEDIUM = 1,     // Medium - balance performance and effect
    HEAVY = 2,      // Heavy - maximize compaction
    EMERGENCY = 3   // Emergency - ignore performance cost
};

enum class MigrationMode : u32 {
    SYNC = 0,
    ASYNC = 1,
    LAZY = 2
};

struct CMARegion {
    PhysAddr base_addr;
    usize size;
    usize free_pages;
    usize allocated_pages;
    u32 alignment_order;
    bool is_active;
    moss::kernel::containers::AtomicU32 ref_count;

    CMARegion() noexcept
        : base_addr(0), size(0), free_pages(0), allocated_pages(0),
          alignment_order(0), is_active(false), ref_count(0) {}

    CMARegion(PhysAddr base, usize sz, u32 align_order) noexcept
        : base_addr(base), size(sz), free_pages(sz / PAGE_SIZE),
          allocated_pages(0), alignment_order(align_order),
          is_active(true), ref_count(0) {}
};

struct PageMigration {
    PhysAddr source;
    PhysAddr destination;
    VirtAddr virtual_addr;
    MigrationType migration_type;
    MigrationMode mode;
    bool completed;
    u64 start_time;
    u64 completion_time;

    PageMigration() noexcept
        : source(0), destination(0), virtual_addr(0),
          migration_type(MigrationType::MOVABLE), mode(MigrationMode::SYNC),
          completed(false), start_time(0), completion_time(0) {}
};

class CompactionScanner {
public:
    struct ScanConfig {
        usize scan_batch_size;
        usize max_migrate_pages;
        CompactionStrategy strategy;
        bool scan_whole_zone;
        u32 scan_priority;
    };

    struct ScanResult {
        usize free_pages_found;
        usize movable_pages_found;
        usize migration_candidates;
        usize pages_scanned;
        u64 scan_time_us;
    };

    CompactionScanner() noexcept;

    [[nodiscard]] CompactionResult<ScanResult> scan_for_free_pages(PhysAddr start,
                                                                    PhysAddr end,
                                                                    const ScanConfig& config) noexcept;
    [[nodiscard]] CompactionResult<ScanResult> scan_for_movable_pages(PhysAddr start,
                                                                      PhysAddr end,
                                                                      const ScanConfig& config) noexcept;
    [[nodiscard]] bool is_page_movable(PhysAddr addr) const noexcept;
    [[nodiscard]] bool is_page_free(PhysAddr addr) const noexcept;

private:
    PhysAddr free_scanner_pos_;
    PhysAddr migration_scanner_pos_;
    moss::kernel::containers::AtomicU64 total_scans_;
};

class PageMigrator {
public:
    struct MigrationConfig {
        MigrationMode default_mode;
        usize max_batch_size;
        u32 max_retries;
        u64 timeout_us;
    };

    struct MigrationStats {
        u64 total_migrations;
        u64 successful_migrations;
        u64 failed_migrations;
        u64 total_migration_time_us;
        u64 pages_migrated;
        double average_migration_time_us;
    };

    PageMigrator(const MigrationConfig& config) noexcept;

    [[nodiscard]] CompactionResult<PhysAddr> migrate_page(PhysAddr source,
                                                           PhysAddr destination,
                                                           MigrationMode mode = MigrationMode::SYNC) noexcept;
    [[nodiscard]] CompactionResult<usize> migrate_pages_batch(PageMigration* migrations,
                                                               usize count) noexcept;
    [[nodiscard]] CompactionVoidResult update_page_tables(PhysAddr old_addr,
                                                          PhysAddr new_addr,
                                                          VirtAddr virt_addr) noexcept;

    [[nodiscard]] MigrationStats get_stats() const noexcept { return stats_; }
    void reset_stats() noexcept;

private:
    MigrationConfig config_;
    MigrationStats stats_;
    moss::kernel::containers::AtomicBool migration_in_progress_;

    CompactionVoidResult copy_page_data(PhysAddr source, PhysAddr destination) noexcept;
    CompactionVoidResult verify_migration(PhysAddr source, PhysAddr destination) noexcept;
    void update_migration_stats(bool success, u64 time_us) noexcept;
};

class CMAAllocator {
public:
    static constexpr usize MAX_CMA_REGIONS = 8;

    struct CMAConfig {
        usize default_region_size;
        u32 default_alignment_order;
        bool enable_migration;
    };

    static CompactionVoidResult initialize(const CMAConfig& config) noexcept;
    [[nodiscard]] static CompactionResult<PhysAddr> allocate(usize size, u32 alignment_order = 0) noexcept;
    static CompactionVoidResult free(PhysAddr addr, usize size) noexcept;
    static CompactionVoidResult add_region(PhysAddr base, usize size, u32 alignment_order) noexcept;

    struct CMAStats {
        usize total_regions;
        usize total_size;
        usize allocated_size;
        usize free_size;
        u64 allocation_count;
        u64 free_count;
        u64 migration_count;
    };

    [[nodiscard]] static CMAStats get_stats() noexcept;

private:
    static bool initialized_;
    static CMARegion regions_[MAX_CMA_REGIONS];
    static usize region_count_;
    static CMAConfig config_;
    static moss::kernel::containers::AtomicU64 allocation_count_;
    static moss::kernel::containers::AtomicU64 free_count_;

    [[nodiscard]] static CMARegion* find_suitable_region(usize size, u32 alignment_order) noexcept;
    static CompactionVoidResult migrate_pages_from_region(CMARegion* region, usize required_pages) noexcept;
};

class MemoryCompactionEngine {
public:
    struct CompactionConfig {
        CompactionScanner::ScanConfig scan_config;
        PageMigrator::MigrationConfig migration_config;
        CMAAllocator::CMAConfig cma_config;
        CompactionStrategy default_strategy;
        bool enable_background_compaction;
        u64 compaction_interval_ms;
        usize fragmentation_threshold;
    };

    struct CompactionStats {
        u64 total_compaction_runs;
        u64 successful_compactions;
        u64 failed_compactions;
        u64 pages_migrated;
        u64 total_compaction_time_us;
        double average_fragmentation_before;
        double average_fragmentation_after;
        double compaction_efficiency;
    };

    static CompactionVoidResult initialize(const CompactionConfig& config) noexcept;
    [[nodiscard]] static CompactionResult<usize> compact_zone(CompactionStrategy strategy = CompactionStrategy::MEDIUM) noexcept;
    [[nodiscard]] static CompactionResult<usize> compact_for_order(usize order) noexcept;
    static CompactionVoidResult background_compaction() noexcept;
    [[nodiscard]] static bool should_compact() noexcept;
    [[nodiscard]] static double get_fragmentation_score() noexcept;
    [[nodiscard]] static CompactionStats get_stats() noexcept;
    [[nodiscard]] static MemoryCompactionEngine& get_instance() noexcept;

private:
    CompactionConfig config_;
    CompactionScanner scanner_;
    PageMigrator migrator_;
    CompactionStats stats_;
    moss::kernel::containers::AtomicBool compaction_active_;

    MemoryCompactionEngine(const CompactionConfig& config) noexcept;
    CompactionVoidResult background_compaction_thread() noexcept;
    CompactionResult<usize> do_compaction(CompactionStrategy strategy) noexcept;
    void update_compaction_stats(usize migrated, u64 time_us, bool success) noexcept;
    [[nodiscard]] CompactionStrategy select_strategy() const noexcept;

    static bool initialized_;
    static MemoryCompactionEngine* instance_;
};

namespace memory_compaction {
    inline CompactionVoidResult initialize(const MemoryCompactionEngine::CompactionConfig& config) noexcept {
        return MemoryCompactionEngine::initialize(config);
    }
    inline CompactionResult<usize> compact(CompactionStrategy strategy = CompactionStrategy::MEDIUM) noexcept {
        return MemoryCompactionEngine::compact_zone(strategy);
    }
    inline CompactionResult<usize> compact_for_order(usize order) noexcept {
        return MemoryCompactionEngine::compact_for_order(order);
    }
    inline bool should_compact() noexcept {
        return MemoryCompactionEngine::should_compact();
    }
    inline double fragmentation_score() noexcept {
        return MemoryCompactionEngine::get_fragmentation_score();
    }

    namespace cma {
        inline CompactionVoidResult initialize(const CMAAllocator::CMAConfig& config) noexcept {
            return CMAAllocator::initialize(config);
        }
        inline CompactionResult<PhysAddr> allocate(usize size, u32 alignment_order = 0) noexcept {
            return CMAAllocator::allocate(size, alignment_order);
        }
        inline CompactionVoidResult free(PhysAddr addr, usize size) noexcept {
            return CMAAllocator::free(addr, size);
        }
    }
}

// ========================================================================
// huge_pages.hpp
// ========================================================================

enum class HugePagesError : u32 {
    OutOfMemory = 1,
    InvalidSize = 2,
    InvalidAddress = 3,
    AlignmentError = 4,
    SplitFailed = 5,
    MergeFailed = 6,
    PoolExhausted = 7,
    InitializationFailed = 8,
    THPDisabled = 9,
    DefragFailed = 10,
    ReservationFailed = 11
};

template<typename T>
using HugePagesResult = moss::kernel::Result<T, HugePagesError>;
using HugePagesVoidResult = moss::kernel::Result<void, HugePagesError>;

enum class HugePageSize : u32 {
    SIZE_2MB = 0,    // 2MB pages
    SIZE_1GB = 1,    // 1GB pages
    SIZE_16MB = 2,   // 16MB pages (ARM64)
    SIZE_32MB = 3,   // 32MB pages (ARM64)
    COUNT = 4        // Total number of sizes
};

// Huge page size constants
inline constexpr usize HUGE_PAGE_2MB = 2 * 1024 * 1024;
inline constexpr usize HUGE_PAGE_1GB = 1024 * 1024 * 1024;
inline constexpr usize HUGE_PAGE_16MB = 16 * 1024 * 1024;
inline constexpr usize HUGE_PAGE_32MB = 32 * 1024 * 1024;

inline constexpr usize HUGE_PAGE_2MB_SIZE = 2ULL * 1024 * 1024;
inline constexpr usize HUGE_PAGE_2MB_SHIFT = 21;
inline constexpr usize HUGE_PAGE_2MB_MASK = HUGE_PAGE_2MB_SIZE - 1;
inline constexpr usize HUGE_PAGE_1GB_SIZE = 1ULL * 1024 * 1024 * 1024;
inline constexpr usize HUGE_PAGE_1GB_SHIFT = 30;
inline constexpr usize HUGE_PAGE_1GB_MASK = HUGE_PAGE_1GB_SIZE - 1;
inline constexpr usize PAGES_PER_2MB = HUGE_PAGE_2MB_SIZE / PAGE_SIZE;
inline constexpr usize PAGES_PER_1GB = HUGE_PAGE_1GB_SIZE / PAGE_SIZE;

enum class THPPolicy : u32 {
    ALWAYS = 0,
    MADVISE = 1,
    NEVER = 2,
    DEFER = 3
};

enum class SplitPolicy : u32 {
    IMMEDIATE = 0,
    LAZY = 1,
    NEVER = 2
};

enum class DefragPolicy : u32 {
    ALWAYS = 0,
    DEFER = 1,
    MADVISE = 2,
    NEVER = 3
};

struct HugePageInfo {
    PhysAddr phys_addr;
    VirtAddr virt_addr;
    HugePageSize size;
    numa_node_t numa_node;
    u32 ref_count;
    bool is_compound;
    bool is_reserved;
    bool is_thp;
    u64 allocation_time;
    moss::kernel::containers::AtomicU64 access_count;

    HugePageInfo() noexcept
        : phys_addr(0), virt_addr(0), size(HugePageSize::SIZE_2MB),
          numa_node(NUMA_NO_NODE), ref_count(0),
          is_compound(false), is_reserved(false), is_thp(false),
          allocation_time(0), access_count(0) {}
};

class HugePagePool {
public:
    struct PoolConfig {
        usize initial_2mb_pages;
        usize initial_1gb_pages;
        usize max_2mb_pages;
        usize max_1gb_pages;
        usize reserve_2mb_pages;
        usize reserve_1gb_pages;
    };

    struct PoolStats {
        usize total_2mb_pages;
        usize free_2mb_pages;
        usize allocated_2mb_pages;
        usize reserved_2mb_pages;
        usize total_1gb_pages;
        usize free_1gb_pages;
        usize allocated_1gb_pages;
        usize reserved_1gb_pages;
        u64 allocation_count;
        u64 free_count;
        u64 split_count;
        u64 merge_count;
    };

    HugePagePool(const PoolConfig& config) noexcept;

    [[nodiscard]] HugePagesResult<PhysAddr> allocate_huge_page(HugePageSize size,
                                                                numa_node_t node = NUMA_NO_NODE) noexcept;
    HugePagesVoidResult free_huge_page(PhysAddr addr, HugePageSize size) noexcept;
    HugePagesVoidResult reserve_huge_pages(HugePageSize size, usize count) noexcept;
    HugePagesVoidResult unreserve_huge_pages(HugePageSize size, usize count) noexcept;

    [[nodiscard]] usize get_free_count(HugePageSize size) const noexcept;
    [[nodiscard]] usize get_total_count(HugePageSize size) const noexcept;
    [[nodiscard]] PoolStats get_stats() const noexcept;

    HugePagesVoidResult grow_pool(HugePageSize size, usize count) noexcept;
    HugePagesVoidResult shrink_pool(HugePageSize size, usize count) noexcept;

private:
    PoolConfig config_;
    PoolStats stats_;

    struct FreeHugePage {
        PhysAddr addr;
        FreeHugePage* next;
    };

    FreeHugePage* free_2mb_list_;
    FreeHugePage* free_1gb_list_;
    moss::kernel::containers::AtomicSize free_2mb_count_;
    moss::kernel::containers::AtomicSize free_1gb_count_;
    moss::kernel::containers::AtomicU64 alloc_count_;
    moss::kernel::containers::AtomicU64 free_count_stat_;

    [[nodiscard]] HugePagesResult<PhysAddr> allocate_from_buddy(HugePageSize size) noexcept;
    HugePagesVoidResult return_to_buddy(PhysAddr addr, HugePageSize size) noexcept;
};

class THPManager {
public:
    struct THPConfig {
        THPPolicy policy;
        SplitPolicy split_policy;
        DefragPolicy defrag_policy;
        usize max_thp_pages;
        bool enable_khugepaged;
        u64 scan_interval_ms;
        usize pages_to_scan;
    };

    struct THPStats {
        u64 thp_fault_alloc;
        u64 thp_fault_fallback;
        u64 thp_collapse_alloc;
        u64 thp_split;
        u64 thp_deferred_split;
        usize current_thp_count;
        double thp_hit_ratio;
    };

    THPManager(const THPConfig& config, HugePagePool* pool) noexcept;

    [[nodiscard]] HugePagesResult<PhysAddr> try_allocate_thp(VirtAddr vaddr,
                                                              numa_node_t node = NUMA_NO_NODE) noexcept;
    HugePagesVoidResult split_huge_page(PhysAddr addr) noexcept;
    HugePagesVoidResult deferred_split(PhysAddr addr) noexcept;
    HugePagesVoidResult collapse_pages(VirtAddr vaddr, usize page_count) noexcept;

    [[nodiscard]] bool should_use_thp(VirtAddr vaddr, usize size) const noexcept;
    [[nodiscard]] bool can_collapse(VirtAddr vaddr) const noexcept;

    void khugepaged_scan() noexcept;
    [[nodiscard]] THPStats get_stats() const noexcept;
    void set_policy(THPPolicy policy) noexcept { config_.policy = policy; }
    [[nodiscard]] THPPolicy get_policy() const noexcept { return config_.policy; }

private:
    THPConfig config_;
    HugePagePool* pool_;
    THPStats stats_;
    moss::kernel::containers::AtomicBool khugepaged_active_;

    [[nodiscard]] bool is_address_aligned_2mb(VirtAddr addr) const noexcept {
        return (addr & HUGE_PAGE_2MB_MASK) == 0;
    }
    [[nodiscard]] bool are_pages_contiguous(VirtAddr vaddr, usize count) const noexcept;
    HugePagesVoidResult do_collapse(VirtAddr vaddr, PhysAddr huge_page) noexcept;
    void update_thp_stats(bool success, bool collapse) noexcept;
};

class HugeTLBManager {
public:
    struct HugeTLBConfig {
        usize default_pool_2mb;
        usize default_pool_1gb;
        bool overcommit_allowed;
        u32 overcommit_ratio;
    };

    HugeTLBManager(const HugeTLBConfig& config, HugePagePool* pool) noexcept;

    [[nodiscard]] HugePagesResult<PhysAddr> hugetlb_alloc(HugePageSize size,
                                                           numa_node_t node = NUMA_NO_NODE) noexcept;
    HugePagesVoidResult hugetlb_free(PhysAddr addr, HugePageSize size) noexcept;

    [[nodiscard]] HugePagesResult<VirtAddr> mmap_hugetlb(usize size, HugePageSize page_size) noexcept;
    HugePagesVoidResult munmap_hugetlb(VirtAddr addr, usize size) noexcept;

    [[nodiscard]] usize get_available_pages(HugePageSize size) const noexcept;
    [[nodiscard]] bool can_allocate(HugePageSize size, usize count) const noexcept;

private:
    HugeTLBConfig config_;
    HugePagePool* pool_;
    moss::kernel::containers::AtomicSize reserved_2mb_;
    moss::kernel::containers::AtomicSize reserved_1gb_;
    moss::kernel::containers::AtomicU64 mmap_count_;
};

class HugePagesManager {
public:
    struct HugePagesConfig {
        HugePagePool::PoolConfig pool_config;
        THPManager::THPConfig thp_config;
        HugeTLBManager::HugeTLBConfig hugetlb_config;
        bool enable_huge_pages;
        bool enable_thp;
        bool enable_hugetlb;
    };

    struct HugePagesSystemStats {
        HugePagePool::PoolStats pool_stats;
        THPManager::THPStats thp_stats;
        usize total_huge_memory;
        usize free_huge_memory;
        double huge_page_utilization;
    };

    static HugePagesVoidResult initialize(const HugePagesConfig& config) noexcept;

    [[nodiscard]] static HugePagesResult<PhysAddr> allocate_huge_page(
        HugePageSize size = HugePageSize::SIZE_2MB,
        numa_node_t node = NUMA_NO_NODE) noexcept;
    static HugePagesVoidResult free_huge_page(PhysAddr addr,
                                               HugePageSize size = HugePageSize::SIZE_2MB) noexcept;

    [[nodiscard]] static HugePagesResult<PhysAddr> try_thp_allocation(VirtAddr vaddr) noexcept;
    static HugePagesVoidResult split_thp(PhysAddr addr) noexcept;

    [[nodiscard]] static bool is_huge_pages_enabled() noexcept;
    [[nodiscard]] static bool is_thp_enabled() noexcept;
    static void set_thp_policy(THPPolicy policy) noexcept;
    [[nodiscard]] static THPPolicy get_thp_policy() noexcept;

    [[nodiscard]] static HugePagesSystemStats get_system_stats() noexcept;
    [[nodiscard]] static HugePagesManager& get_instance() noexcept;

private:
    HugePagesConfig config_;
    HugePagePool pool_;
    THPManager thp_manager_;
    HugeTLBManager hugetlb_manager_;
    moss::kernel::containers::AtomicBool system_initialized_;

    HugePagesManager(const HugePagesConfig& config) noexcept;

    static bool initialized_;
    static HugePagesManager* instance_;
};

namespace huge_pages {
    inline HugePagesVoidResult initialize(const HugePagesManager::HugePagesConfig& config) noexcept {
        return HugePagesManager::initialize(config);
    }
    inline HugePagesResult<PhysAddr> alloc_2mb(numa_node_t node = NUMA_NO_NODE) noexcept {
        return HugePagesManager::allocate_huge_page(HugePageSize::SIZE_2MB, node);
    }
    inline HugePagesResult<PhysAddr> alloc_1gb(numa_node_t node = NUMA_NO_NODE) noexcept {
        return HugePagesManager::allocate_huge_page(HugePageSize::SIZE_1GB, node);
    }
    inline HugePagesVoidResult free_2mb(PhysAddr addr) noexcept {
        return HugePagesManager::free_huge_page(addr, HugePageSize::SIZE_2MB);
    }
    inline HugePagesVoidResult free_1gb(PhysAddr addr) noexcept {
        return HugePagesManager::free_huge_page(addr, HugePageSize::SIZE_1GB);
    }
    inline HugePagesResult<PhysAddr> try_thp(VirtAddr vaddr) noexcept {
        return HugePagesManager::try_thp_allocation(vaddr);
    }
    inline bool is_enabled() noexcept {
        return HugePagesManager::is_huge_pages_enabled();
    }
    inline bool is_thp_enabled() noexcept {
        return HugePagesManager::is_thp_enabled();
    }
}

// ========================================================================
// memory_stats.hpp - Full types
// ========================================================================

struct BasicMemoryStats {
    usize total_memory;
    usize free_memory;
    usize used_memory;
    usize kernel_memory;
    usize user_memory;
    usize cached_memory;
    usize buffer_memory;
    usize swap_total;
    usize swap_used;
    usize swap_free;
    usize huge_pages_total;
    usize huge_pages_free;
    usize huge_pages_reserved;
    double memory_utilization;
    MemoryPressure pressure_level;
};

struct AllocatorStats {
    // Buddy allocator stats
    struct BuddyStats {
        usize total_pages;
        usize free_pages;
        usize used_pages;
        usize pages_by_order[MAX_ORDER + 1];
        usize pages_by_migration[static_cast<u32>(MigrationType::TYPES_COUNT)];
        double fragmentation_index;
        u64 allocation_count;
        u64 free_count;
        u64 compact_count;
    } buddy;

    // Heap allocator stats
    struct HeapStats {
        usize total_heap_size;
        usize allocated_bytes;
        usize free_bytes;
        usize largest_free_block;
        double fragmentation_ratio;
        u64 allocation_count;
        u64 free_count;
    } heap;

    // Vmalloc stats
    struct VmallocStats {
        usize total_vmalloc_size;
        usize allocated_vmalloc_size;
        usize free_vmalloc_size;
        usize area_count;
        usize largest_free_area;
    } vmalloc;

    // NUMA stats
    struct NUMAStats {
        usize per_node_free[MAX_NUMA_NODES];
        usize per_node_used[MAX_NUMA_NODES];
        u64 local_allocations;
        u64 remote_allocations;
        double locality_ratio;
    } numa;
};

struct PerformanceMetrics {
    // Allocation latency (in nanoseconds)
    struct LatencyStats {
        u64 min_latency_ns;
        u64 max_latency_ns;
        u64 avg_latency_ns;
        u64 p50_latency_ns;
        u64 p95_latency_ns;
        u64 p99_latency_ns;
        u64 sample_count;
    };

    LatencyStats buddy_alloc_latency;
    LatencyStats buddy_free_latency;
    LatencyStats heap_alloc_latency;
    LatencyStats heap_free_latency;
    LatencyStats vmalloc_latency;
    LatencyStats page_fault_latency;

    // Throughput
    struct ThroughputStats {
        u64 allocations_per_second;
        u64 frees_per_second;
        u64 bytes_allocated_per_second;
        u64 bytes_freed_per_second;
        u64 measurement_period_ms;
    } throughput;

    // Cache performance
    struct CacheStats {
        u64 percpu_cache_hits;
        u64 percpu_cache_misses;
        double cache_hit_ratio;
        u64 cache_refill_count;
        u64 cache_drain_count;
    } cache;

    // TLB statistics
    struct TLBStats {
        u64 tlb_flush_count;
        u64 tlb_shootdown_count;
        u64 huge_page_tlb_hits;
    } tlb;

    // Reclaim statistics
    struct ReclaimMetrics {
        u64 direct_reclaim_count;
        u64 background_reclaim_count;
        u64 pages_reclaimed;
        u64 pages_scanned;
        double reclaim_efficiency;
        u64 oom_kill_count;
    } reclaim;

    // Compaction statistics
    struct CompactionMetrics {
        u64 compaction_runs;
        u64 pages_migrated;
        double compaction_efficiency;
        double fragmentation_before;
        double fragmentation_after;
    } compaction;
};

struct LeakTrackingInfo {
    struct LeakEntry {
        void* address;
        usize size;
        u64 allocation_time;
        u32 stack_hash;
        AllocationType type;
        bool is_suspicious;
    };

    static constexpr usize MAX_TRACKED_ALLOCATIONS = 4096;
    LeakEntry entries[MAX_TRACKED_ALLOCATIONS];
    usize entry_count;
    usize suspicious_count;
    usize total_leaked_bytes;
};

class MemoryStatsCollector {
public:
    MemoryStatsCollector() noexcept;

    void collect_basic_stats() noexcept;
    void collect_allocator_stats() noexcept;
    void collect_performance_metrics() noexcept;

    [[nodiscard]] BasicMemoryStats get_basic_stats() const noexcept { return basic_stats_; }
    [[nodiscard]] AllocatorStats get_allocator_stats() const noexcept { return allocator_stats_; }
    [[nodiscard]] PerformanceMetrics get_performance_metrics() const noexcept { return perf_metrics_; }

    void record_allocation(usize size, AllocationType type, u64 latency_ns) noexcept;
    void record_free(usize size, AllocationType type, u64 latency_ns) noexcept;
    void record_page_fault(u64 latency_ns) noexcept;

    void reset_performance_counters() noexcept;

private:
    BasicMemoryStats basic_stats_;
    AllocatorStats allocator_stats_;
    PerformanceMetrics perf_metrics_;
    u64 last_collection_time_;

    void update_latency_stats(PerformanceMetrics::LatencyStats& stats, u64 latency_ns) noexcept;
    void update_throughput_stats() noexcept;
};

class MemoryLeakDetector {
public:
    struct LeakDetectorConfig {
        bool enable_tracking;
        usize max_tracked_allocations;
        u64 suspicious_age_ms;
        bool track_stack_traces;
    };

    struct DetectorConfig {
        bool enable_stack_trace;
        bool enable_caller_tracking;
        u64 leak_check_interval_ms;
        u32 leak_threshold_minutes;
        usize min_leak_size;
    };

    struct LeakReport {
        usize total_leaked_bytes;
        u32 leak_count;
        LeakTrackingInfo::LeakEntry top_leaks[16];
        u64 report_timestamp;

        LeakReport() noexcept : total_leaked_bytes(0), leak_count(0), report_timestamp(0) {}
    };

    MemoryLeakDetector(const LeakDetectorConfig& config) noexcept;

    void track_allocation(void* addr, usize size, AllocationType type) noexcept;
    void track_free(void* addr) noexcept;

    [[nodiscard]] LeakTrackingInfo scan_for_leaks() noexcept;
    [[nodiscard]] usize get_active_allocation_count() const noexcept;
    [[nodiscard]] usize get_total_tracked_bytes() const noexcept;

    void dump_leak_report() noexcept;
    void reset() noexcept;

private:
    LeakDetectorConfig config_;
    LeakTrackingInfo tracking_info_;
    moss::kernel::containers::AtomicSize active_allocations_;
    moss::kernel::containers::AtomicSize tracked_bytes_;

    [[nodiscard]] bool is_allocation_suspicious(const LeakTrackingInfo::LeakEntry& entry) const noexcept;
    [[nodiscard]] u32 calculate_stack_hash() const noexcept;
};

class PerformanceProfiler {
public:
    struct ProfilerConfig {
        bool enable_profiling;
        u64 sampling_interval_ms;
        usize max_samples;
        bool profile_allocations;
        bool profile_page_faults;
        bool profile_reclaim;
    };

    struct ProfileSample {
        u64 timestamp;
        usize free_memory;
        usize allocated_memory;
        u32 allocation_rate;
        u32 free_rate;
        MemoryPressure pressure;
        double fragmentation;
    };

    PerformanceProfiler(const ProfilerConfig& config) noexcept;

    void start_profiling() noexcept;
    void stop_profiling() noexcept;
    void take_sample() noexcept;

    [[nodiscard]] bool is_profiling() const noexcept;
    [[nodiscard]] usize get_sample_count() const noexcept;

    void generate_report() noexcept;
    void reset() noexcept;

private:
    ProfilerConfig config_;
    static constexpr usize MAX_PROFILE_SAMPLES = 1024;
    ProfileSample samples_[MAX_PROFILE_SAMPLES];
    usize sample_count_;
    moss::kernel::containers::AtomicBool profiling_active_;
    u64 profiling_start_time_;
};

class MemoryMonitoringSystem {
public:
    struct MonitoringConfig {
        MemoryLeakDetector::LeakDetectorConfig leak_config;
        PerformanceProfiler::ProfilerConfig profiler_config;
        u64 stats_collection_interval_ms;
        bool enable_monitoring;
        bool enable_leak_detection;
        bool enable_profiling;
    };

    struct MonitoringReport {
        BasicMemoryStats basic_stats;
        AllocatorStats allocator_stats;
        PerformanceMetrics performance;
        LeakTrackingInfo leak_info;
        u64 report_generation_time;
    };

    static MemoryStatsVoidResult initialize(const MonitoringConfig& config) noexcept;

    static void collect_all_stats() noexcept;
    [[nodiscard]] static BasicMemoryStats get_basic_stats() noexcept;
    [[nodiscard]] static AllocatorStats get_allocator_stats() noexcept;
    [[nodiscard]] static PerformanceMetrics get_performance_metrics() noexcept;
    [[nodiscard]] static MonitoringReport generate_full_report() noexcept;

    static void track_allocation(void* addr, usize size, AllocationType type) noexcept;
    static void track_free(void* addr) noexcept;
    [[nodiscard]] static LeakTrackingInfo scan_for_leaks() noexcept;

    static void start_profiling() noexcept;
    static void stop_profiling() noexcept;
    [[nodiscard]] static bool is_monitoring_enabled() noexcept;

    [[nodiscard]] static MemoryMonitoringSystem& get_instance() noexcept;

private:
    MonitoringConfig config_;
    MemoryStatsCollector stats_collector_;
    MemoryLeakDetector leak_detector_;
    PerformanceProfiler profiler_;
    moss::kernel::containers::AtomicBool monitoring_active_;

    MemoryMonitoringSystem(const MonitoringConfig& config) noexcept;

    static bool initialized_;
    static MemoryMonitoringSystem* instance_;
};

namespace memory_stats {
    inline MemoryStatsVoidResult initialize(const MemoryMonitoringSystem::MonitoringConfig& config) noexcept {
        return MemoryMonitoringSystem::initialize(config);
    }
    inline BasicMemoryStats get_basic_stats() noexcept {
        return MemoryMonitoringSystem::get_basic_stats();
    }
    inline AllocatorStats get_allocator_stats() noexcept {
        return MemoryMonitoringSystem::get_allocator_stats();
    }
    inline PerformanceMetrics get_performance() noexcept {
        return MemoryMonitoringSystem::get_performance_metrics();
    }
    inline void track_alloc(void* addr, usize size, AllocationType type = AllocationType::UNKNOWN) noexcept {
        MemoryMonitoringSystem::track_allocation(addr, size, type);
    }
    inline void track_free(void* addr) noexcept {
        MemoryMonitoringSystem::track_free(addr);
    }
    inline LeakTrackingInfo check_leaks() noexcept {
        return MemoryMonitoringSystem::scan_for_leaks();
    }
    inline bool is_enabled() noexcept {
        return MemoryMonitoringSystem::is_monitoring_enabled();
    }
}

// ========================================================================
// mm_interface.hpp - Unified Memory Management Interface
// ========================================================================

enum class MMError : u32 {
    Success = 0,
    OutOfMemory = 1,
    InvalidAddress = 2,
    InvalidSize = 3,
    SystemNotInitialized = 4,
    OperationFailed = 5,
    ResourceBusy = 6,
    PermissionDenied = 7,
    FragmentationSevere = 8,
    ConfigurationInvalid = 9
};

template<typename T>
using MMResult = moss::kernel::Result<T, MMError>;
using MMVoidResult = moss::kernel::Result<void, MMError>;

enum class AllocFlags : u32 {
    NONE = 0,
    ZERO_MEMORY = (1 << 0),         // Zero-initialize memory
    HIGH_PRIORITY = (1 << 1),       // High priority allocation
    ATOMIC = (1 << 2),              // Atomic allocation (non-blocking)
    NUMA_LOCAL = (1 << 3),          // Prefer NUMA-local allocation
    HUGE_PAGES = (1 << 4),          // Try to use huge pages
    NO_RECLAIM = (1 << 5),          // Disable memory reclaim
    NO_COMPACTION = (1 << 6),       // Disable memory compaction
    TRACK_CALLER = (1 << 7),        // Track allocation caller
    PREFER_CACHED = (1 << 8)        // Prefer cached memory
};

struct MemoryRequest {
    usize size;                      // Requested size
    usize alignment;                 // Alignment requirement
    AllocFlags flags;                // Allocation flags
    numa_node_t preferred_node;      // Preferred NUMA node
    VirtAddr caller_address;         // Caller address
    AllocationType allocation_type;  // Allocation type (for statistics)

    MemoryRequest(usize sz, AllocFlags flgs = AllocFlags::NONE) noexcept
        : size(sz), alignment(PAGE_SIZE), flags(flgs), preferred_node(NUMA_NO_NODE),
          caller_address(0), allocation_type(AllocationType::UNKNOWN) {}

    MemoryRequest(usize sz, usize align, AllocFlags flgs = AllocFlags::NONE) noexcept
        : size(sz), alignment(align), flags(flgs), preferred_node(NUMA_NO_NODE),
          caller_address(0), allocation_type(AllocationType::UNKNOWN) {}
};

struct MemoryInfo {
    VirtAddr virtual_address;        // Virtual address
    PhysAddr physical_address;       // Physical address
    usize size;                      // Actual allocated size
    usize alignment;                 // Alignment size
    numa_node_t numa_node;           // NUMA node
    bool is_huge_page;               // Whether it's a huge page
    bool is_cached;                  // Whether it came from cache
    u64 allocation_time;             // Allocation timestamp

    MemoryInfo() noexcept
        : virtual_address(0), physical_address(0), size(0), alignment(0),
          numa_node(NUMA_NO_NODE), is_huge_page(false), is_cached(false), allocation_time(0) {}
};

// Forward declaration
class UnifiedMemoryManagerImpl;

// Unified Memory Manager - main controller integrating all subsystems
class UnifiedMemoryManager {
    friend class UnifiedMemoryManagerImpl;

public:
    // System configuration
    struct SystemConfig {
        // Subsystem configurations
        VmallocAllocator::VmallocConfig vmalloc_config;
        MemoryReclaimEngine::ReclaimConfig reclaim_config;
        MemoryCompactionEngine::CompactionConfig compaction_config;
        NUMAPolicyManager::NUMAConfig numa_config;
        HugePagesManager::HugePagesConfig hugepages_config;
        MemoryMonitoringSystem::MonitoringConfig monitoring_config;

        // Global configuration
        bool enable_aggressive_optimization;
        bool enable_background_operations;
        u64 background_interval_ms;
        u32 memory_pressure_threshold;
        usize min_free_memory;
    };

    // System performance statistics
    struct SystemPerformanceStats {
        struct AllocationPerformance {
            u64 total_allocations;
            u64 failed_allocations;
            u64 avg_allocation_latency_us;
            u64 max_allocation_latency_us;
            u64 total_allocated_bytes;
            double allocation_success_rate;
        } allocation_perf;

        struct SystemEfficiency {
            double memory_utilization;
            double fragmentation_ratio;
            double numa_locality_ratio;
            double huge_page_ratio;
            double cache_hit_ratio;
        } system_efficiency;

        u64 report_timestamp;
        MemoryPressure overall_pressure;

        SystemPerformanceStats() noexcept
            : report_timestamp(0), overall_pressure(MemoryPressure::LOW) {
            allocation_perf = {};
            system_efficiency = {};
        }
    };

protected:
    SystemConfig config_;

    // System state
    moss::kernel::containers::AtomicBool system_initialized_;
    moss::kernel::containers::AtomicBool background_thread_active_;
    moss::kernel::containers::AtomicU64 last_background_run_;

public:
    // System initialization and control
    static MMVoidResult initialize_system(const SystemConfig& config) noexcept;
    static void shutdown_system() noexcept;
    [[nodiscard]] static bool is_system_initialized() noexcept;

    // Main memory allocation interface
    [[nodiscard]] static MMResult<VirtAddr> allocate(const MemoryRequest& request) noexcept;
    static MMVoidResult free(VirtAddr address) noexcept;
    static MMVoidResult free(VirtAddr address, usize size) noexcept;

    // Memory info query
    [[nodiscard]] static MMResult<MemoryInfo> query_memory_info(VirtAddr address) noexcept;
    [[nodiscard]] static MMResult<usize> get_allocated_size(VirtAddr address) noexcept;

    // Advanced memory operations
    static MMVoidResult reallocate(VirtAddr& address, usize old_size,
                                  usize new_size, AllocFlags flags = AllocFlags::NONE) noexcept;
    static MMVoidResult prefault_memory(VirtAddr address, usize size) noexcept;
    static MMVoidResult advise_usage_pattern(VirtAddr address, usize size,
                                            UsagePattern pattern) noexcept;

    // Memory pressure management
    static MMVoidResult trigger_memory_reclaim() noexcept;
    static MMVoidResult trigger_memory_compaction() noexcept;
    [[nodiscard]] static MemoryPressure get_memory_pressure() noexcept;

    // Performance optimization
    static MMVoidResult optimize_numa_placement(VirtAddr address, usize size) noexcept;
    static MMVoidResult promote_to_huge_pages(VirtAddr address, usize size) noexcept;
    static MMVoidResult compact_memory_region(VirtAddr start, usize size) noexcept;

    // System monitoring and statistics
    [[nodiscard]] static SystemPerformanceStats get_performance_stats() noexcept;
    [[nodiscard]] static bool is_system_healthy() noexcept;
    static void reset_performance_counters() noexcept;

    // Debug and diagnostics
    static void dump_memory_layout() noexcept;
    static void dump_allocation_history() noexcept;
    static MMResult<MemoryLeakDetector::LeakReport> generate_leak_report() noexcept;

    // Singleton access
    [[nodiscard]] static UnifiedMemoryManager& get_instance() noexcept;

private:
    explicit UnifiedMemoryManager(const SystemConfig& config) noexcept;

    // Internal management functions
    MMVoidResult initialize_subsystems() noexcept;
    void shutdown_subsystems() noexcept;
    void background_maintenance_thread() noexcept;
    MMVoidResult coordinate_subsystems() noexcept;

    // Smart allocation strategy selection
    [[nodiscard]] MMResult<VirtAddr> smart_allocate(const MemoryRequest& request) noexcept;
    [[nodiscard]] AllocationType classify_allocation(const MemoryRequest& request) noexcept;
    [[nodiscard]] bool should_use_huge_pages(const MemoryRequest& request) noexcept;
    [[nodiscard]] numa_node_t select_optimal_numa_node(const MemoryRequest& request) noexcept;

    // Performance optimization decisions
    void adaptive_performance_tuning() noexcept;
    void balance_subsystem_loads() noexcept;
    void update_allocation_strategies() noexcept;

    // Singleton management
    static bool initialized_;
    static UnifiedMemoryManager* instance_;
};

// Convenience namespace
namespace mm {
    inline MMResult<VirtAddr> alloc(usize size) noexcept {
        return UnifiedMemoryManager::allocate(MemoryRequest(size));
    }

    inline MMResult<VirtAddr> alloc_zero(usize size) noexcept {
        return UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::ZERO_MEMORY));
    }

    inline MMResult<VirtAddr> alloc_aligned(usize size, usize alignment) noexcept {
        return UnifiedMemoryManager::allocate(MemoryRequest(size, alignment));
    }

    inline MMResult<VirtAddr> alloc_huge(usize size) noexcept {
        return UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::HUGE_PAGES));
    }

    inline MMVoidResult free(VirtAddr addr) noexcept {
        return UnifiedMemoryManager::free(addr);
    }

    inline MMVoidResult free_sized(VirtAddr addr, usize size) noexcept {
        return UnifiedMemoryManager::free(addr, size);
    }

    inline MemoryPressure get_pressure() noexcept {
        return UnifiedMemoryManager::get_memory_pressure();
    }

    inline bool is_healthy() noexcept {
        return UnifiedMemoryManager::is_system_healthy();
    }

    inline MMVoidResult gc() noexcept {
        return UnifiedMemoryManager::trigger_memory_reclaim();
    }

    inline MMVoidResult optimize(VirtAddr addr, usize size) noexcept {
        return UnifiedMemoryManager::optimize_numa_placement(addr, size);
    }

    inline MMVoidResult compact() noexcept {
        return UnifiedMemoryManager::trigger_memory_compaction();
    }
}

// ========================================================================
// kernel_memory.hpp - Inline allocation functions
// ========================================================================

// Standard kernel memory allocation
inline void* kmalloc(usize size) noexcept {
    auto result = UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::NONE));
    if (result.is_ok()) {
        return reinterpret_cast<void*>(*result);
    }
    return nullptr;
}

// Zero-initialized allocation
inline void* kzalloc(usize size) noexcept {
    auto result = UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::ZERO_MEMORY));
    if (result.is_ok()) {
        return reinterpret_cast<void*>(*result);
    }
    return nullptr;
}

// Aligned allocation
inline void* kmalloc_aligned(usize size, usize alignment) noexcept {
    auto result = UnifiedMemoryManager::allocate(MemoryRequest(size, alignment, AllocFlags::NONE));
    if (result.is_ok()) {
        return reinterpret_cast<void*>(*result);
    }
    return nullptr;
}

// Atomic allocation (non-blocking)
inline void* kmalloc_atomic(usize size) noexcept {
    auto result = UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::ATOMIC));
    if (result.is_ok()) {
        return reinterpret_cast<void*>(*result);
    }
    return nullptr;
}

// NUMA-local allocation
inline void* kmalloc_numa(usize size, numa_node_t node) noexcept {
    MemoryRequest request(size, AllocFlags::NUMA_LOCAL);
    request.preferred_node = node;
    auto result = UnifiedMemoryManager::allocate(request);
    if (result.is_ok()) {
        return reinterpret_cast<void*>(*result);
    }
    return nullptr;
}

// Huge page allocation
inline void* kmalloc_huge(usize size) noexcept {
    auto result = UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::HUGE_PAGES));
    if (result.is_ok()) {
        return reinterpret_cast<void*>(*result);
    }
    return nullptr;
}

// Memory free
inline void kfree(void* ptr) noexcept {
    if (ptr) {
        (void)UnifiedMemoryManager::free(reinterpret_cast<VirtAddr>(ptr));
    }
}

// Sized free (more efficient)
inline void kfree_sized(void* ptr, usize size) noexcept {
    if (ptr) {
        (void)UnifiedMemoryManager::free(reinterpret_cast<VirtAddr>(ptr), size);
    }
}

// Reallocation
inline void* krealloc(void* ptr, usize old_size, usize new_size) noexcept {
    if (!ptr) {
        return kmalloc(new_size);
    }

    if (new_size == 0) {
        kfree_sized(ptr, old_size);
        return nullptr;
    }

    VirtAddr addr = reinterpret_cast<VirtAddr>(ptr);
    auto result = UnifiedMemoryManager::reallocate(addr, old_size, new_size);
    if (result.is_ok()) {
        return reinterpret_cast<void*>(addr);
    }
    return nullptr;
}

// Initialize kernel memory system
inline bool initialize_kernel_memory() noexcept {
    UnifiedMemoryManager::SystemConfig config = {};

    config.vmalloc_config = {
        .enable_lazy_free = true,
        .lazy_free_threshold = 64 * 1024,
        .max_lazy_free_memory = 16 * 1024 * 1024,
        .enable_numa_awareness = true,
        .default_numa_policy = static_cast<u32>(NUMAPolicy::DEFAULT)
    };

    config.reclaim_config = {
        .scan_config = {},
        .pressure_config = {},
        .default_policy = ReclaimPolicy::BALANCED,
        .min_free_pages = 1024,
        .target_free_pages = 4096,
        .enable_background_reclaim = true,
        .background_reclaim_interval_ms = 5000
    };

    config.compaction_config = {
        .scan_config = {},
        .migration_config = {},
        .cma_config = {},
        .default_strategy = CompactionStrategy::MEDIUM,
        .enable_background_compaction = true,
        .compaction_interval_ms = 5000,
        .fragmentation_threshold = 70
    };

    config.numa_config = {
        .balancer_config = {
            .balance_interval_ms = 2000,
            .imbalance_threshold = 25,
            .migration_rate_limit = 1000,
            .memory_threshold_ratio = 0.8
        },
        .enable_auto_balancing = true,
        .enable_migration = true,
        .default_policy = NUMAPolicy::DEFAULT
    };

    config.hugepages_config = {
        .pool_config = {
            .initial_2mb_pages = 128,
            .initial_1gb_pages = 4,
            .max_2mb_pages = 1024,
            .max_1gb_pages = 32,
            .reserve_2mb_pages = 64,
            .reserve_1gb_pages = 2
        },
        .thp_config = {
            .policy = THPPolicy::DEFER,
            .split_policy = SplitPolicy::LAZY,
            .defrag_policy = DefragPolicy::DEFER,
            .max_thp_pages = 4096,
            .enable_khugepaged = true,
            .scan_interval_ms = 10000,
            .pages_to_scan = 4096
        },
        .hugetlb_config = {
            .default_pool_2mb = 256,
            .default_pool_1gb = 4,
            .overcommit_allowed = false,
            .overcommit_ratio = 0
        },
        .enable_huge_pages = true,
        .enable_thp = true,
        .enable_hugetlb = true
    };

    config.monitoring_config = {
        .leak_config = {
            .enable_tracking = true,
            .max_tracked_allocations = 1024,
            .suspicious_age_ms = 600000,
            .track_stack_traces = false
        },
        .profiler_config = {
            .enable_profiling = true,
            .sampling_interval_ms = 500,
            .max_samples = 1024,
            .profile_allocations = true,
            .profile_page_faults = true,
            .profile_reclaim = true
        },
        .stats_collection_interval_ms = 1000,
        .enable_monitoring = true,
        .enable_leak_detection = true,
        .enable_profiling = true
    };

    config.enable_aggressive_optimization = true;
    config.enable_background_operations = true;
    config.background_interval_ms = 1000;
    config.memory_pressure_threshold = 80;
    config.min_free_memory = 128 * 1024 * 1024;

    auto result = UnifiedMemoryManager::initialize_system(config);
    return result.is_ok();
}

// Shutdown kernel memory system
inline void shutdown_kernel_memory() noexcept {
    UnifiedMemoryManager::shutdown_system();
}

// Check system health
inline bool is_memory_system_healthy() noexcept {
    return UnifiedMemoryManager::is_system_healthy();
}

// Trigger memory GC
inline void trigger_memory_gc() noexcept {
    (void)UnifiedMemoryManager::trigger_memory_reclaim();
}

// Get memory pressure
inline MemoryPressure get_memory_pressure() noexcept {
    return UnifiedMemoryManager::get_memory_pressure();
}

// Get memory stats
inline UnifiedMemoryManager::SystemPerformanceStats get_memory_stats() noexcept {
    return UnifiedMemoryManager::get_performance_stats();
}

// Print memory stats (uses centralized early_debug_print for UART output)
inline void print_memory_stats() noexcept {
    [[maybe_unused]] auto stats = get_memory_stats();
    early_debug_print("\n=== MEMORY SYSTEM STATS ===\n");
}

// Check memory leaks (uses centralized early_debug_print for UART output)
inline void check_memory_leaks() noexcept {
    auto leak_result = UnifiedMemoryManager::generate_leak_report();
    if (leak_result.is_ok()) {
        auto report = *leak_result;
        if (report.total_leaked_bytes > 0) {
            early_debug_print("\nMEMORY LEAKS DETECTED!\n");
        }
    }
}

// C-compatible interface declarations
extern "C" {
    bool moss_memory_init(void) noexcept;
    void moss_memory_shutdown(void) noexcept;
    void* moss_kmalloc(usize size) noexcept;
    void* moss_kzalloc(usize size) noexcept;
    void* moss_kmalloc_aligned(usize size, usize alignment) noexcept;
    void* moss_kmalloc_atomic(usize size) noexcept;
    void moss_kfree(void* ptr) noexcept;
    void moss_kfree_sized(void* ptr, usize size) noexcept;
    void* moss_krealloc(void* ptr, usize old_size, usize new_size) noexcept;
    int moss_memory_is_healthy(void) noexcept;
    int moss_memory_get_pressure(void) noexcept;
    void moss_memory_gc(void) noexcept;
    void moss_memory_check_leaks(void) noexcept;
    void moss_memory_print_stats(void) noexcept;

    // Page allocator shim (C-linkage wrappers for PageFrameAllocator)
    unsigned long long moss_slab_alloc_pages(unsigned long long order) noexcept;
    int moss_slab_free_pages(unsigned long long addr,
                             unsigned long long order) noexcept;
}

} // namespace moss::kernel::mm

// === Page allocator shim definitions (extern "C") ===
extern "C" {

unsigned long long moss_slab_alloc_pages(unsigned long long order) noexcept {
    auto result =
        moss::kernel::mm::PageFrameAllocator::allocate_pages(
            static_cast<moss::kernel::usize>(order));
    if (!result) {
        return 0;
    }
    return static_cast<unsigned long long>(*result);
}

int moss_slab_free_pages(unsigned long long addr,
                         unsigned long long order) noexcept {
    auto result = moss::kernel::mm::PageFrameAllocator::free_pages(
        static_cast<moss::kernel::PhysAddr>(addr),
        static_cast<moss::kernel::usize>(order));
    return result.has_value() ? 0 : 1;
}

} // extern "C"
