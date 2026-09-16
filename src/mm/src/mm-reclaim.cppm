// MOSS Memory Management Module - Reclaim partition
// Contains: MemoryReclaim, MemoryCompaction, HugePageManager

export module moss.mm:reclaim;

import :core;
import :policy;

import moss.std;
import moss.types;
import moss.result;
import moss.containers;

// ========================================================================
// memory_reclaim.hpp
// ========================================================================
export namespace moss::kernel::mm {

// Re-export basic types used throughout
using moss::kernel::PhysAddr;
using moss::kernel::VirtAddr;

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

template <typename T> using ReclaimResult = moss::kernel::Result<T, ReclaimError>;
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

enum class AccessPattern : u8 { FREQUENT = 0, MODERATE = 1, RARE = 2, ONCE = 3, STREAMING = 4 };

enum class ReclaimPolicy : u32 {
  CONSERVATIVE = 0, // Conservative - preserve working set
  BALANCED = 1,     // Balanced - default policy
  AGGRESSIVE = 2,   // Aggressive - maximize free memory
  EMERGENCY = 3     // Emergency - OOM situations
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
      : phys_addr(0), virt_addr(0), state(PageState::ACTIVE), access_pattern(AccessPattern::MODERATE),
        migration_type(MigrationType::MOVABLE), age(0), reference_count(0), last_access_time(0), creation_time(0),
        is_dirty(false), is_mapped(false), is_locked(false), numa_node(NUMA_NO_NODE) {}
};

class LRUList {
public:
  struct LRUEntry {
    ReclaimPageInfo page_info;
    LRUEntry *next;
    LRUEntry *prev;
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

  ReclaimVoidResult add_page(const ReclaimPageInfo &page, ListType list_type) noexcept;
  ReclaimVoidResult remove_page(PhysAddr addr) noexcept;
  ReclaimVoidResult move_page(PhysAddr addr, ListType from, ListType to) noexcept;
  ReclaimVoidResult promote_page(PhysAddr addr) noexcept;
  ReclaimVoidResult demote_page(PhysAddr addr) noexcept;
  [[nodiscard]] LRUEntry *get_tail(ListType list_type) noexcept;
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
  LRUEntry *heads_[static_cast<u32>(ListType::LIST_COUNT)];
  LRUEntry *tails_[static_cast<u32>(ListType::LIST_COUNT)];
  moss::kernel::containers::AtomicSize sizes_[static_cast<u32>(ListType::LIST_COUNT)];
  moss::kernel::containers::AtomicU64 promotion_count_;
  moss::kernel::containers::AtomicU64 demotion_count_;
  moss::kernel::containers::AtomicU64 eviction_count_;

  void add_to_head(LRUEntry *entry, ListType list_type) noexcept;
  void remove_entry(LRUEntry *entry, ListType list_type) noexcept;
  [[nodiscard]] LRUEntry *find_entry(PhysAddr addr) noexcept;
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

  // Fixed 1024-entry history limits detector storage and retained accesses;
  // the original working-set/window sizing evidence is not recorded.
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

  PageScanner(LRUList *lru_list, WorkingSetDetector *ws_detector) noexcept;

  [[nodiscard]] ReclaimResult<ScanResult> scan_inactive_list(const ScanConfig &config) noexcept;
  [[nodiscard]] ReclaimResult<ScanResult> scan_active_list(const ScanConfig &config) noexcept;
  [[nodiscard]] ReclaimResult<usize> shrink_page_list(usize nr_to_reclaim, const ScanConfig &config) noexcept;

  [[nodiscard]] bool should_reclaim_page(const ReclaimPageInfo &page, const ScanConfig &config) const noexcept;
  [[nodiscard]] bool is_page_referenced(const ReclaimPageInfo &page) const noexcept;

private:
  LRUList *lru_list_;
  WorkingSetDetector *ws_detector_;
  moss::kernel::containers::AtomicU64 total_scans_;
  moss::kernel::containers::AtomicU64 total_reclaimed_;

  ReclaimVoidResult try_reclaim_page(LRUList::LRUEntry *entry, const ScanConfig &config) noexcept;
  ReclaimVoidResult writeback_page(const ReclaimPageInfo &page) noexcept;
  void update_page_age(LRUList::LRUEntry *entry) noexcept;
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

  MemoryPressureMonitor(const PressureConfig &config) noexcept;

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

  static ReclaimVoidResult initialize(const ReclaimConfig &config) noexcept;
  [[nodiscard]] static ReclaimResult<usize> reclaim_pages(usize nr_to_reclaim) noexcept;
  [[nodiscard]] static ReclaimResult<usize> direct_reclaim(usize nr_to_reclaim) noexcept;
  static ReclaimVoidResult background_reclaim() noexcept;
  static ReclaimVoidResult shrink_all_caches() noexcept;
  [[nodiscard]] static bool should_reclaim() noexcept;
  [[nodiscard]] static MemoryPressure get_memory_pressure() noexcept;
  static void add_page_to_lru(const ReclaimPageInfo &page) noexcept;
  static void remove_page_from_lru(PhysAddr addr) noexcept;
  static void mark_page_accessed(PhysAddr addr) noexcept;
  static void mark_page_dirty(PhysAddr addr) noexcept;
  [[nodiscard]] static ReclaimStats get_stats() noexcept;
  [[nodiscard]] static MemoryReclaimEngine &get_instance() noexcept;

private:
  ReclaimConfig config_;
  LRUList lru_list_;
  WorkingSetDetector ws_detector_;
  PageScanner scanner_;
  MemoryPressureMonitor pressure_monitor_;
  ReclaimStats stats_;
  moss::kernel::containers::AtomicBool reclaim_active_;

  MemoryReclaimEngine(const ReclaimConfig &config) noexcept;
  ReclaimVoidResult background_reclaim_thread() noexcept;
  ReclaimResult<usize> do_reclaim(usize nr_to_reclaim, bool direct) noexcept;
  void update_reclaim_stats(usize scanned, usize reclaimed, u64 time_us) noexcept;
  [[nodiscard]] usize calculate_reclaim_target() const noexcept;
  [[nodiscard]] u32 calculate_scan_priority() const noexcept;

  static bool initialized_;
  static MemoryReclaimEngine *instance_;
};

namespace memory_reclaim {
inline ReclaimVoidResult initialize(const MemoryReclaimEngine::ReclaimConfig &config) noexcept {
  return MemoryReclaimEngine::initialize(config);
}
inline ReclaimResult<usize> reclaim(usize nr_pages) noexcept { return MemoryReclaimEngine::reclaim_pages(nr_pages); }
inline bool should_reclaim() noexcept { return MemoryReclaimEngine::should_reclaim(); }
inline MemoryPressure get_pressure() noexcept { return MemoryReclaimEngine::get_memory_pressure(); }
inline void page_accessed(PhysAddr addr) noexcept { MemoryReclaimEngine::mark_page_accessed(addr); }
inline void page_dirty(PhysAddr addr) noexcept { MemoryReclaimEngine::mark_page_dirty(addr); }
} // namespace memory_reclaim

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

template <typename T> using CompactionResult = moss::kernel::Result<T, CompactionError>;
using CompactionVoidResult = moss::kernel::Result<void, CompactionError>;

enum class CompactionStrategy : u32 {
  LIGHT = 0,    // Light - only easily movable pages
  MEDIUM = 1,   // Medium - balance performance and effect
  HEAVY = 2,    // Heavy - maximize compaction
  EMERGENCY = 3 // Emergency - ignore performance cost
};

enum class MigrationMode : u32 { SYNC = 0, ASYNC = 1, LAZY = 2 };

struct CMARegion {
  PhysAddr base_addr;
  usize size;
  usize free_pages;
  usize allocated_pages;
  u32 alignment_order;
  bool is_active;
  moss::kernel::containers::AtomicU32 ref_count;

  CMARegion() noexcept
      : base_addr(0), size(0), free_pages(0), allocated_pages(0), alignment_order(0), is_active(false), ref_count(0) {}

  CMARegion(PhysAddr base, usize sz, u32 align_order) noexcept
      : base_addr(base), size(sz), free_pages(sz / PAGE_SIZE), allocated_pages(0), alignment_order(align_order),
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
      : source(0), destination(0), virtual_addr(0), migration_type(MigrationType::MOVABLE), mode(MigrationMode::SYNC),
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

  [[nodiscard]] CompactionResult<ScanResult> scan_for_free_pages(PhysAddr start, PhysAddr end,
                                                                 const ScanConfig &config) noexcept;
  [[nodiscard]] CompactionResult<ScanResult> scan_for_movable_pages(PhysAddr start, PhysAddr end,
                                                                    const ScanConfig &config) noexcept;
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

  PageMigrator(const MigrationConfig &config) noexcept;

  [[nodiscard]] CompactionResult<PhysAddr> migrate_page(PhysAddr source, PhysAddr destination,
                                                        MigrationMode mode = MigrationMode::SYNC) noexcept;
  [[nodiscard]] CompactionResult<usize> migrate_pages_batch(PageMigration *migrations, usize count) noexcept;
  [[nodiscard]] CompactionVoidResult update_page_tables(PhysAddr old_addr, PhysAddr new_addr,
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
  // Fixed region-descriptor capacity; eight is a software storage budget,
  // not a discovered DMA/hardware limit. Its sizing rationale is unrecorded.
  static constexpr usize MAX_CMA_REGIONS = 8;

  struct CMAConfig {
    usize default_region_size;
    u32 default_alignment_order;
    bool enable_migration;
  };

  static CompactionVoidResult initialize(const CMAConfig &config) noexcept;
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

  [[nodiscard]] static CMARegion *find_suitable_region(usize size, u32 alignment_order) noexcept;
  static CompactionVoidResult migrate_pages_from_region(CMARegion *region, usize required_pages) noexcept;
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

  static CompactionVoidResult initialize(const CompactionConfig &config) noexcept;
  [[nodiscard]] static CompactionResult<usize>
  compact_zone(CompactionStrategy strategy = CompactionStrategy::MEDIUM) noexcept;
  [[nodiscard]] static CompactionResult<usize> compact_for_order(usize order) noexcept;
  static CompactionVoidResult background_compaction() noexcept;
  [[nodiscard]] static bool should_compact() noexcept;
  [[nodiscard]] static double get_fragmentation_score() noexcept;
  [[nodiscard]] static CompactionStats get_stats() noexcept;
  [[nodiscard]] static MemoryCompactionEngine &get_instance() noexcept;

private:
  CompactionConfig config_;
  CompactionScanner scanner_;
  PageMigrator migrator_;
  CompactionStats stats_;
  moss::kernel::containers::AtomicBool compaction_active_;

  MemoryCompactionEngine(const CompactionConfig &config) noexcept;
  CompactionVoidResult background_compaction_thread() noexcept;
  CompactionResult<usize> do_compaction(CompactionStrategy strategy) noexcept;
  void update_compaction_stats(usize migrated, u64 time_us, bool success) noexcept;
  [[nodiscard]] CompactionStrategy select_strategy() const noexcept;

  static bool initialized_;
  static MemoryCompactionEngine *instance_;
};

namespace memory_compaction {
inline CompactionVoidResult initialize(const MemoryCompactionEngine::CompactionConfig &config) noexcept {
  return MemoryCompactionEngine::initialize(config);
}
inline CompactionResult<usize> compact(CompactionStrategy strategy = CompactionStrategy::MEDIUM) noexcept {
  return MemoryCompactionEngine::compact_zone(strategy);
}
inline CompactionResult<usize> compact_for_order(usize order) noexcept {
  return MemoryCompactionEngine::compact_for_order(order);
}
inline bool should_compact() noexcept { return MemoryCompactionEngine::should_compact(); }
inline double fragmentation_score() noexcept { return MemoryCompactionEngine::get_fragmentation_score(); }

namespace cma {
inline CompactionVoidResult initialize(const CMAAllocator::CMAConfig &config) noexcept {
  return CMAAllocator::initialize(config);
}
inline CompactionResult<PhysAddr> allocate(usize size, u32 alignment_order = 0) noexcept {
  return CMAAllocator::allocate(size, alignment_order);
}
inline CompactionVoidResult free(PhysAddr addr, usize size) noexcept { return CMAAllocator::free(addr, size); }
} // namespace cma
} // namespace memory_compaction

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

template <typename T> using HugePagesResult = moss::kernel::Result<T, HugePagesError>;
using HugePagesVoidResult = moss::kernel::Result<void, HugePagesError>;

enum class HugePageSize : u32 {
  SIZE_2MB = 0,  // 2MB pages
  SIZE_1GB = 1,  // 1GB pages
  SIZE_16MB = 2, // 16MB pages (ARM64)
  SIZE_32MB = 3, // 32MB pages (ARM64)
  COUNT = 4      // Total number of sizes
};

// Byte sizes for the declared huge-page variants. With 4 KiB base pages,
// 2 MiB/1 GiB leaves correspond to the next two 9-bit page-table levels;
// 16/32 MiB declarations do not imply the active mapper supports those sizes;
// the reason for retaining those two variants is not recorded.
inline constexpr usize HUGE_PAGE_2MB = 2ULL * 1024 * 1024;
inline constexpr usize HUGE_PAGE_1GB = 1ULL * 1024 * 1024 * 1024;
inline constexpr usize HUGE_PAGE_16MB = 16ULL * 1024 * 1024;
inline constexpr usize HUGE_PAGE_32MB = 32ULL * 1024 * 1024;

inline constexpr usize HUGE_PAGE_2MB_SIZE = 2ULL * 1024 * 1024;
inline constexpr usize HUGE_PAGE_2MB_SHIFT = 21; // log2(2 MiB); mask keeps the block-offset bits.
inline constexpr usize HUGE_PAGE_2MB_MASK = HUGE_PAGE_2MB_SIZE - 1;
inline constexpr usize HUGE_PAGE_1GB_SIZE = 1ULL * 1024 * 1024 * 1024;
inline constexpr usize HUGE_PAGE_1GB_SHIFT = 30; // log2(1 GiB); mask keeps the block-offset bits.
inline constexpr usize HUGE_PAGE_1GB_MASK = HUGE_PAGE_1GB_SIZE - 1;
inline constexpr usize PAGES_PER_2MB = HUGE_PAGE_2MB_SIZE / PAGE_SIZE;
inline constexpr usize PAGES_PER_1GB = HUGE_PAGE_1GB_SIZE / PAGE_SIZE;

enum class THPPolicy : u32 { ALWAYS = 0, MADVISE = 1, NEVER = 2, DEFER = 3 };

enum class SplitPolicy : u32 { IMMEDIATE = 0, LAZY = 1, NEVER = 2 };

enum class DefragPolicy : u32 { ALWAYS = 0, DEFER = 1, MADVISE = 2, NEVER = 3 };

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
      : phys_addr(0), virt_addr(0), size(HugePageSize::SIZE_2MB), numa_node(NUMA_NO_NODE), ref_count(0),
        is_compound(false), is_reserved(false), is_thp(false), allocation_time(0), access_count(0) {}
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

  HugePagePool(const PoolConfig &config) noexcept;

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
    FreeHugePage *next;
  };

  FreeHugePage *free_2mb_list_;
  FreeHugePage *free_1gb_list_;
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

  THPManager(const THPConfig &config, HugePagePool *pool) noexcept;

  [[nodiscard]] HugePagesResult<PhysAddr> try_allocate_thp(VirtAddr vaddr, numa_node_t node = NUMA_NO_NODE) noexcept;
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
  HugePagePool *pool_;
  THPStats stats_;
  moss::kernel::containers::AtomicBool khugepaged_active_;

  [[nodiscard]] bool is_address_aligned_2mb(VirtAddr addr) const noexcept { return (addr & HUGE_PAGE_2MB_MASK) == 0; }
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

  HugeTLBManager(const HugeTLBConfig &config, HugePagePool *pool) noexcept;

  [[nodiscard]] HugePagesResult<PhysAddr> hugetlb_alloc(HugePageSize size, numa_node_t node = NUMA_NO_NODE) noexcept;
  HugePagesVoidResult hugetlb_free(PhysAddr addr, HugePageSize size) noexcept;

  [[nodiscard]] HugePagesResult<VirtAddr> mmap_hugetlb(usize size, HugePageSize page_size) noexcept;
  HugePagesVoidResult munmap_hugetlb(VirtAddr addr, usize size) noexcept;

  [[nodiscard]] usize get_available_pages(HugePageSize size) const noexcept;
  [[nodiscard]] bool can_allocate(HugePageSize size, usize count) const noexcept;

private:
  HugeTLBConfig config_;
  HugePagePool *pool_;
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

  static HugePagesVoidResult initialize(const HugePagesConfig &config) noexcept;

  [[nodiscard]] static HugePagesResult<PhysAddr> allocate_huge_page(HugePageSize size = HugePageSize::SIZE_2MB,
                                                                    numa_node_t node = NUMA_NO_NODE) noexcept;
  static HugePagesVoidResult free_huge_page(PhysAddr addr, HugePageSize size = HugePageSize::SIZE_2MB) noexcept;

  [[nodiscard]] static HugePagesResult<PhysAddr> try_thp_allocation(VirtAddr vaddr) noexcept;
  static HugePagesVoidResult split_thp(PhysAddr addr) noexcept;

  [[nodiscard]] static bool is_huge_pages_enabled() noexcept;
  [[nodiscard]] static bool is_thp_enabled() noexcept;
  static void set_thp_policy(THPPolicy policy) noexcept;
  [[nodiscard]] static THPPolicy get_thp_policy() noexcept;

  [[nodiscard]] static HugePagesSystemStats get_system_stats() noexcept;
  [[nodiscard]] static HugePagesManager &get_instance() noexcept;

private:
  HugePagesConfig config_;
  HugePagePool pool_;
  THPManager thp_manager_;
  HugeTLBManager hugetlb_manager_;
  moss::kernel::containers::AtomicBool system_initialized_;

  HugePagesManager(const HugePagesConfig &config) noexcept;

  static bool initialized_;
  static HugePagesManager *instance_;
};

namespace huge_pages {
inline HugePagesVoidResult initialize(const HugePagesManager::HugePagesConfig &config) noexcept {
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
inline bool is_enabled() noexcept { return HugePagesManager::is_huge_pages_enabled(); }
inline bool is_thp_enabled() noexcept { return HugePagesManager::is_thp_enabled(); }
} // namespace huge_pages

} // namespace moss::kernel::mm
