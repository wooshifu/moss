// MOSS Memory Management Module - Interface partition
// Contains: Full MemoryStats types, UnifiedMemoryManager, kernel_memory
//           inline functions, extern "C" shims

export module moss.mm:interface;

import :core;
import :page_table;
import :policy;
import :reclaim;

import moss.std;
import moss.types;
import moss.result;
import moss.containers;
import moss.logging;

// ========================================================================
// memory_stats.hpp - Full types
// ========================================================================
export namespace moss::kernel::mm {

// Re-export basic types used throughout
using moss::kernel::PhysAddr;
using moss::kernel::VirtAddr;

namespace log = moss::kernel::logging;

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
    void *address;
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

  void update_latency_stats(PerformanceMetrics::LatencyStats &stats, u64 latency_ns) noexcept;
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

  MemoryLeakDetector(const LeakDetectorConfig &config) noexcept;

  void track_allocation(void *addr, usize size, AllocationType type) noexcept;
  void track_free(void *addr) noexcept;

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

  [[nodiscard]] bool is_allocation_suspicious(const LeakTrackingInfo::LeakEntry &entry) const noexcept;
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

  PerformanceProfiler(const ProfilerConfig &config) noexcept;

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

  static MemoryStatsVoidResult initialize(const MonitoringConfig &config) noexcept;

  static void collect_all_stats() noexcept;
  [[nodiscard]] static BasicMemoryStats get_basic_stats() noexcept;
  [[nodiscard]] static AllocatorStats get_allocator_stats() noexcept;
  [[nodiscard]] static PerformanceMetrics get_performance_metrics() noexcept;
  [[nodiscard]] static MonitoringReport generate_full_report() noexcept;

  static void track_allocation(void *addr, usize size, AllocationType type) noexcept;
  static void track_free(void *addr) noexcept;
  [[nodiscard]] static LeakTrackingInfo scan_for_leaks() noexcept;

  static void start_profiling() noexcept;
  static void stop_profiling() noexcept;
  [[nodiscard]] static bool is_monitoring_enabled() noexcept;

  [[nodiscard]] static MemoryMonitoringSystem &get_instance() noexcept;

private:
  MonitoringConfig config_;
  MemoryStatsCollector stats_collector_;
  MemoryLeakDetector leak_detector_;
  PerformanceProfiler profiler_;
  moss::kernel::containers::AtomicBool monitoring_active_;

  MemoryMonitoringSystem(const MonitoringConfig &config) noexcept;

  static bool initialized_;
  static MemoryMonitoringSystem *instance_;
};

namespace memory_stats {
inline MemoryStatsVoidResult initialize(const MemoryMonitoringSystem::MonitoringConfig &config) noexcept {
  return MemoryMonitoringSystem::initialize(config);
}
inline BasicMemoryStats get_basic_stats() noexcept { return MemoryMonitoringSystem::get_basic_stats(); }
inline AllocatorStats get_allocator_stats() noexcept { return MemoryMonitoringSystem::get_allocator_stats(); }
inline PerformanceMetrics get_performance() noexcept { return MemoryMonitoringSystem::get_performance_metrics(); }
inline void track_alloc(void *addr, usize size, AllocationType type = AllocationType::UNKNOWN) noexcept {
  MemoryMonitoringSystem::track_allocation(addr, size, type);
}
inline void track_free(void *addr) noexcept { MemoryMonitoringSystem::track_free(addr); }
inline LeakTrackingInfo check_leaks() noexcept { return MemoryMonitoringSystem::scan_for_leaks(); }
inline bool is_enabled() noexcept { return MemoryMonitoringSystem::is_monitoring_enabled(); }
} // namespace memory_stats

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

template <typename T> using MMResult = moss::kernel::Result<T, MMError>;
using MMVoidResult = moss::kernel::Result<void, MMError>;

enum class AllocFlags : u32 {
  NONE = 0,
  ZERO_MEMORY = (1 << 0),   // Zero-initialize memory
  HIGH_PRIORITY = (1 << 1), // High priority allocation
  ATOMIC = (1 << 2),        // Atomic allocation (non-blocking)
  NUMA_LOCAL = (1 << 3),    // Prefer NUMA-local allocation
  HUGE_PAGES = (1 << 4),    // Try to use huge pages
  NO_RECLAIM = (1 << 5),    // Disable memory reclaim
  NO_COMPACTION = (1 << 6), // Disable memory compaction
  TRACK_CALLER = (1 << 7),  // Track allocation caller
  PREFER_CACHED = (1 << 8)  // Prefer cached memory
};

struct MemoryRequest {
  usize size;                     // Requested size
  usize alignment;                // Alignment requirement
  AllocFlags flags;               // Allocation flags
  numa_node_t preferred_node;     // Preferred NUMA node
  VirtAddr caller_address;        // Caller address
  AllocationType allocation_type; // Allocation type (for statistics)

  MemoryRequest(usize sz, AllocFlags flgs = AllocFlags::NONE) noexcept
      : size(sz), alignment(PAGE_SIZE), flags(flgs), preferred_node(NUMA_NO_NODE), caller_address(0),
        allocation_type(AllocationType::UNKNOWN) {}

  MemoryRequest(usize sz, usize align, AllocFlags flgs = AllocFlags::NONE) noexcept
      : size(sz), alignment(align), flags(flgs), preferred_node(NUMA_NO_NODE), caller_address(0),
        allocation_type(AllocationType::UNKNOWN) {}
};

struct MemoryInfo {
  VirtAddr virtual_address;  // Virtual address
  PhysAddr physical_address; // Physical address
  usize size;                // Actual allocated size
  usize alignment;           // Alignment size
  numa_node_t numa_node;     // NUMA node
  bool is_huge_page;         // Whether it's a huge page
  bool is_cached;            // Whether it came from cache
  u64 allocation_time;       // Allocation timestamp

  MemoryInfo() noexcept
      : virtual_address(0), physical_address(0), size(0), alignment(0), numa_node(NUMA_NO_NODE), is_huge_page(false),
        is_cached(false), allocation_time(0) {}
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

    SystemPerformanceStats() noexcept : report_timestamp(0), overall_pressure(MemoryPressure::LOW) {
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
  static MMVoidResult initialize_system(const SystemConfig &config) noexcept;
  static void shutdown_system() noexcept;
  [[nodiscard]] static bool is_system_initialized() noexcept;

  // Main memory allocation interface
  [[nodiscard]] static MMResult<VirtAddr> allocate(const MemoryRequest &request) noexcept;
  static MMVoidResult free(VirtAddr address) noexcept;
  static MMVoidResult free(VirtAddr address, usize size) noexcept;

  // Memory info query
  [[nodiscard]] static MMResult<MemoryInfo> query_memory_info(VirtAddr address) noexcept;
  [[nodiscard]] static MMResult<usize> get_allocated_size(VirtAddr address) noexcept;

  // Advanced memory operations
  static MMVoidResult reallocate(VirtAddr &address, usize old_size, usize new_size,
                                 AllocFlags flags = AllocFlags::NONE) noexcept;
  static MMVoidResult prefault_memory(VirtAddr address, usize size) noexcept;
  static MMVoidResult advise_usage_pattern(VirtAddr address, usize size, UsagePattern pattern) noexcept;

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
  [[nodiscard]] static UnifiedMemoryManager &get_instance() noexcept;

private:
  explicit UnifiedMemoryManager(const SystemConfig &config) noexcept;

  // Internal management functions
  MMVoidResult initialize_subsystems() noexcept;
  void shutdown_subsystems() noexcept;
  void background_maintenance_thread() noexcept;
  MMVoidResult coordinate_subsystems() noexcept;

  // Smart allocation strategy selection
  [[nodiscard]] MMResult<VirtAddr> smart_allocate(const MemoryRequest &request) noexcept;
  [[nodiscard]] AllocationType classify_allocation(const MemoryRequest &request) noexcept;
  [[nodiscard]] bool should_use_huge_pages(const MemoryRequest &request) noexcept;
  [[nodiscard]] numa_node_t select_optimal_numa_node(const MemoryRequest &request) noexcept;

  // Performance optimization decisions
  void adaptive_performance_tuning() noexcept;
  void balance_subsystem_loads() noexcept;
  void update_allocation_strategies() noexcept;

  // Singleton management
  static bool initialized_;
  static UnifiedMemoryManager *instance_;
};

// Convenience namespace
namespace mm {
inline MMResult<VirtAddr> alloc(usize size) noexcept { return UnifiedMemoryManager::allocate(MemoryRequest(size)); }

inline MMResult<VirtAddr> alloc_zero(usize size) noexcept {
  return UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::ZERO_MEMORY));
}

inline MMResult<VirtAddr> alloc_aligned(usize size, usize alignment) noexcept {
  return UnifiedMemoryManager::allocate(MemoryRequest(size, alignment));
}

inline MMResult<VirtAddr> alloc_huge(usize size) noexcept {
  return UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::HUGE_PAGES));
}

inline MMVoidResult free(VirtAddr addr) noexcept { return UnifiedMemoryManager::free(addr); }

inline MMVoidResult free_sized(VirtAddr addr, usize size) noexcept { return UnifiedMemoryManager::free(addr, size); }

inline MemoryPressure get_pressure() noexcept { return UnifiedMemoryManager::get_memory_pressure(); }

inline bool is_healthy() noexcept { return UnifiedMemoryManager::is_system_healthy(); }

inline MMVoidResult gc() noexcept { return UnifiedMemoryManager::trigger_memory_reclaim(); }

inline MMVoidResult optimize(VirtAddr addr, usize size) noexcept {
  return UnifiedMemoryManager::optimize_numa_placement(addr, size);
}

inline MMVoidResult compact() noexcept { return UnifiedMemoryManager::trigger_memory_compaction(); }
} // namespace mm

// ========================================================================
// kernel_memory.hpp - Inline allocation functions
// ========================================================================

// Standard kernel memory allocation
inline void *kmalloc(usize size) noexcept {
  auto result = UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::NONE));
  if (result.is_ok()) {
    return reinterpret_cast<void *>(*result);
  }
  return nullptr;
}

// Zero-initialized allocation
inline void *kzalloc(usize size) noexcept {
  auto result = UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::ZERO_MEMORY));
  if (result.is_ok()) {
    return reinterpret_cast<void *>(*result);
  }
  return nullptr;
}

// Aligned allocation
inline void *kmalloc_aligned(usize size, usize alignment) noexcept {
  auto result = UnifiedMemoryManager::allocate(MemoryRequest(size, alignment, AllocFlags::NONE));
  if (result.is_ok()) {
    return reinterpret_cast<void *>(*result);
  }
  return nullptr;
}

// Atomic allocation (non-blocking)
inline void *kmalloc_atomic(usize size) noexcept {
  auto result = UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::ATOMIC));
  if (result.is_ok()) {
    return reinterpret_cast<void *>(*result);
  }
  return nullptr;
}

// NUMA-local allocation
inline void *kmalloc_numa(usize size, numa_node_t node) noexcept {
  MemoryRequest request(size, AllocFlags::NUMA_LOCAL);
  request.preferred_node = node;
  auto result = UnifiedMemoryManager::allocate(request);
  if (result.is_ok()) {
    return reinterpret_cast<void *>(*result);
  }
  return nullptr;
}

// Huge page allocation
inline void *kmalloc_huge(usize size) noexcept {
  auto result = UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::HUGE_PAGES));
  if (result.is_ok()) {
    return reinterpret_cast<void *>(*result);
  }
  return nullptr;
}

// Memory free
inline void kfree(void *ptr) noexcept {
  if (ptr) {
    (void)UnifiedMemoryManager::free(reinterpret_cast<VirtAddr>(ptr));
  }
}

// Sized free (more efficient)
inline void kfree_sized(void *ptr, usize size) noexcept {
  if (ptr) {
    (void)UnifiedMemoryManager::free(reinterpret_cast<VirtAddr>(ptr), size);
  }
}

// Reallocation
inline void *krealloc(void *ptr, usize old_size, usize new_size) noexcept {
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
    return reinterpret_cast<void *>(addr);
  }
  return nullptr;
}

// Initialize kernel memory system
inline bool initialize_kernel_memory() noexcept {
  UnifiedMemoryManager::SystemConfig config = {};

  config.vmalloc_config = {.enable_lazy_free = true,
                           .lazy_free_threshold = 64 * 1024,
                           .max_lazy_free_memory = 16 * 1024 * 1024,
                           .enable_numa_awareness = true,
                           .default_numa_policy = static_cast<u32>(NUMAPolicy::DEFAULT)};

  config.reclaim_config = {.scan_config = {},
                           .pressure_config = {},
                           .default_policy = ReclaimPolicy::BALANCED,
                           .min_free_pages = 1024,
                           .target_free_pages = 4096,
                           .enable_background_reclaim = true,
                           .background_reclaim_interval_ms = 5000};

  config.compaction_config = {.scan_config = {},
                              .migration_config = {},
                              .cma_config = {},
                              .default_strategy = CompactionStrategy::MEDIUM,
                              .enable_background_compaction = true,
                              .compaction_interval_ms = 5000,
                              .fragmentation_threshold = 70};

  config.numa_config = {.balancer_config = {.balance_interval_ms = 2000,
                                            .imbalance_threshold = 25,
                                            .migration_rate_limit = 1000,
                                            .memory_threshold_ratio = 0.8},
                        .enable_auto_balancing = true,
                        .enable_migration = true,
                        .default_policy = NUMAPolicy::DEFAULT};

  config.hugepages_config = {.pool_config = {.initial_2mb_pages = 128,
                                             .initial_1gb_pages = 4,
                                             .max_2mb_pages = 1024,
                                             .max_1gb_pages = 32,
                                             .reserve_2mb_pages = 64,
                                             .reserve_1gb_pages = 2},
                             .thp_config = {.policy = THPPolicy::DEFER,
                                            .split_policy = SplitPolicy::LAZY,
                                            .defrag_policy = DefragPolicy::DEFER,
                                            .max_thp_pages = 4096,
                                            .enable_khugepaged = true,
                                            .scan_interval_ms = 10000,
                                            .pages_to_scan = 4096},
                             .hugetlb_config = {.default_pool_2mb = 256,
                                                .default_pool_1gb = 4,
                                                .overcommit_allowed = false,
                                                .overcommit_ratio = 0},
                             .enable_huge_pages = true,
                             .enable_thp = true,
                             .enable_hugetlb = true};

  config.monitoring_config = {.leak_config = {.enable_tracking = true,
                                              .max_tracked_allocations = 1024,
                                              .suspicious_age_ms = 600000,
                                              .track_stack_traces = false},
                              .profiler_config = {.enable_profiling = true,
                                                  .sampling_interval_ms = 500,
                                                  .max_samples = 1024,
                                                  .profile_allocations = true,
                                                  .profile_page_faults = true,
                                                  .profile_reclaim = true},
                              .stats_collection_interval_ms = 1000,
                              .enable_monitoring = true,
                              .enable_leak_detection = true,
                              .enable_profiling = true};

  config.enable_aggressive_optimization = true;
  config.enable_background_operations = true;
  config.background_interval_ms = 1000;
  config.memory_pressure_threshold = 80;
  config.min_free_memory = 128 * 1024 * 1024;

  auto result = UnifiedMemoryManager::initialize_system(config);
  return result.is_ok();
}

// Shutdown kernel memory system
inline void shutdown_kernel_memory() noexcept { UnifiedMemoryManager::shutdown_system(); }

// Check system health
inline bool is_memory_system_healthy() noexcept { return UnifiedMemoryManager::is_system_healthy(); }

// Trigger memory GC
inline void trigger_memory_gc() noexcept { (void)UnifiedMemoryManager::trigger_memory_reclaim(); }

// Get memory pressure
inline MemoryPressure get_memory_pressure() noexcept { return UnifiedMemoryManager::get_memory_pressure(); }

// Get memory stats
inline UnifiedMemoryManager::SystemPerformanceStats get_memory_stats() noexcept {
  return UnifiedMemoryManager::get_performance_stats();
}

// Print memory stats (uses klog for UART output)
inline void print_memory_stats() noexcept {
  [[maybe_unused]] auto stats = get_memory_stats();
  log::klog::info("=== MEMORY SYSTEM STATS ===");
}

// Check memory leaks (uses klog for UART output)
inline void check_memory_leaks() noexcept {
  auto leak_result = UnifiedMemoryManager::generate_leak_report();
  if (leak_result.is_ok()) {
    auto report = *leak_result;
    if (report.total_leaked_bytes > 0) {
      log::klog::warn("MEMORY LEAKS DETECTED!");
    }
  }
}

// Page allocator shim (C-linkage bridge for containers→mm dependency)
extern "C" {
unsigned long long moss_slab_alloc_pages(unsigned long long order) noexcept;
int moss_slab_free_pages(unsigned long long addr, unsigned long long order) noexcept;
}

} // namespace moss::kernel::mm
