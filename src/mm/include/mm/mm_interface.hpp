#pragma once

// MOSS内核统一内存管理接口 - 世界级性能的内存子系统
// 整合所有内存管理组件：Vmalloc、回收引擎、压缩系统、NUMA策略、大页支持、监控系统

#include "core/types.hpp"
#include "core/result.hpp"
#include "numa_policy.hpp"
#include "vmalloc_allocator.hpp"
#include "memory_reclaim.hpp"
#include "memory_compaction.hpp"
#include "huge_pages.hpp"
#include "memory_stats.hpp"

namespace moss::kernel::mm {

// 统一内存管理错误类型
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

// 统一内存管理结果类型
template<typename T>
using MMResult = moss::kernel::Result<T, MMError>;
using MMVoidResult = moss::kernel::Result<void, MMError>;

// 内存分配标志
enum class AllocFlags : u32 {
    NONE = 0,
    ZERO_MEMORY = (1 << 0),         // 零初始化内存
    HIGH_PRIORITY = (1 << 1),       // 高优先级分配
    ATOMIC = (1 << 2),              // 原子分配（不会阻塞）
    NUMA_LOCAL = (1 << 3),          // NUMA本地分配优先
    HUGE_PAGES = (1 << 4),          // 尝试使用大页
    NO_RECLAIM = (1 << 5),          // 禁用内存回收
    NO_COMPACTION = (1 << 6),       // 禁用内存压缩
    TRACK_CALLER = (1 << 7),        // 跟踪分配调用者
    PREFER_CACHED = (1 << 8)        // 优先使用缓存内存
};

// 内存分配请求
struct MemoryRequest {
    moss::kernel::usize size;                    // 请求大小
    moss::kernel::usize alignment;               // 对齐要求
    AllocFlags flags;                            // 分配标志
    numa_node_t preferred_node;                  // 优选NUMA节点
    moss::kernel::VirtAddr caller_address;       // 调用者地址
    AllocationType allocation_type;              // 分配类型（用于统计）

    MemoryRequest(moss::kernel::usize sz, AllocFlags flgs = AllocFlags::NONE) noexcept
        : size(sz), alignment(moss::kernel::PAGE_SIZE), flags(flgs), preferred_node(NUMA_NO_NODE),
          caller_address(0), allocation_type(AllocationType::UNKNOWN) {}

    MemoryRequest(moss::kernel::usize sz, moss::kernel::usize align, AllocFlags flgs = AllocFlags::NONE) noexcept
        : size(sz), alignment(align), flags(flgs), preferred_node(NUMA_NO_NODE),
          caller_address(0), allocation_type(AllocationType::UNKNOWN) {}
};

// 内存信息查询结果
struct MemoryInfo {
    moss::kernel::VirtAddr virtual_address;      // 虚拟地址
    moss::kernel::PhysAddr physical_address;     // 物理地址
    moss::kernel::usize size;                    // 实际分配大小
    moss::kernel::usize alignment;               // 对齐大小
    numa_node_t numa_node;                       // 所在NUMA节点
    bool is_huge_page;                          // 是否为大页
    bool is_cached;                             // 是否来自缓存
    moss::kernel::u64 allocation_time;           // 分配时间戳

    MemoryInfo() noexcept
        : virtual_address(0), physical_address(0), size(0), alignment(0),
          numa_node(NUMA_NO_NODE), is_huge_page(false), is_cached(false), allocation_time(0) {}
};

// 前向声明
class UnifiedMemoryManagerImpl;

// 统一内存管理器 - 整合所有子系统的主控制器
class UnifiedMemoryManager {
    friend class UnifiedMemoryManagerImpl;

public:
    // 系统配置
    struct SystemConfig {
        // 子系统配置
        VmallocAllocator::VmallocConfig vmalloc_config;
        MemoryReclaimEngine::ReclaimConfig reclaim_config;
        MemoryCompactionEngine::CompactionConfig compaction_config;
        NUMAPolicyManager::NUMAConfig numa_config;
        HugePagesManager::HugePagesConfig hugepages_config;
        MemoryMonitoringSystem::MonitoringConfig monitoring_config;

        // 全局配置
        bool enable_aggressive_optimization;         // 启用激进优化
        bool enable_background_operations;           // 启用后台操作
        moss::kernel::u64 background_interval_ms;    // 后台任务间隔
        moss::kernel::u32 memory_pressure_threshold; // 内存压力阈值
        moss::kernel::usize min_free_memory;         // 最小空闲内存
    };

    // 系统性能统计
    struct SystemPerformanceStats {
        // 分配性能
        struct AllocationPerformance {
            moss::kernel::u64 total_allocations;
            moss::kernel::u64 failed_allocations;
            moss::kernel::u64 avg_allocation_latency_us;
            moss::kernel::u64 max_allocation_latency_us;
            moss::kernel::u64 total_allocated_bytes;
            double allocation_success_rate;
        } allocation_perf;

        // 系统效率
        struct SystemEfficiency {
            double memory_utilization;              // 内存利用率
            double fragmentation_ratio;             // 碎片化比例
            double numa_locality_ratio;             // NUMA本地性比例
            double huge_page_ratio;                 // 大页使用比例
            double cache_hit_ratio;                 // 缓存命中率
        } system_efficiency;

        // 子系统状态（暂时注释以简化集成实现）
        // VmallocAllocator::AllocationStats vmalloc_stats;
        // MemoryReclaimEngine::ReclaimStats reclaim_stats;
        // MemoryCompactionEngine::CompactionStats compaction_stats;
        // NUMAPolicyManager::NUMASystemStats numa_stats;
        // HugePagesManager::SystemStats hugepages_stats;
        // MemoryMonitoringSystem::SystemReport monitoring_report;

        moss::kernel::u64 report_timestamp;
        MemoryPressure overall_pressure;

        SystemPerformanceStats() noexcept
            : report_timestamp(0), overall_pressure(MemoryPressure::LOW) {
            allocation_perf = {};
            system_efficiency = {};
        }
    };

protected:
    SystemConfig config_;

    // 子系统管理器（暂时注释以简化集成实现）
    // VmallocAllocator vmalloc_allocator_;
    // MemoryReclaimEngine reclaim_engine_;
    // MemoryCompactionEngine compaction_system_;
    // NUMAPolicyManager numa_manager_;
    // HugePagesManager hugepages_manager_;
    // MemoryMonitoringSystem monitoring_system_;

    // 系统状态
    moss::kernel::containers::AtomicBool system_initialized_;
    moss::kernel::containers::AtomicBool background_thread_active_;
    moss::kernel::containers::AtomicU64 last_background_run_;

public:
    // 系统初始化和控制
    static MMVoidResult initialize_system(const SystemConfig& config) noexcept;
    static void shutdown_system() noexcept;
    [[nodiscard]] static bool is_system_initialized() noexcept;

    // 主要内存分配接口
    [[nodiscard]] static MMResult<moss::kernel::VirtAddr> allocate(const MemoryRequest& request) noexcept;
    static MMVoidResult free(moss::kernel::VirtAddr address) noexcept;
    static MMVoidResult free(moss::kernel::VirtAddr address, moss::kernel::usize size) noexcept;

    // 内存信息查询
    [[nodiscard]] static MMResult<MemoryInfo> query_memory_info(moss::kernel::VirtAddr address) noexcept;
    [[nodiscard]] static MMResult<moss::kernel::usize> get_allocated_size(moss::kernel::VirtAddr address) noexcept;

    // 高级内存操作
    static MMVoidResult reallocate(moss::kernel::VirtAddr& address, moss::kernel::usize old_size,
                                  moss::kernel::usize new_size, AllocFlags flags = AllocFlags::NONE) noexcept;
    static MMVoidResult prefault_memory(moss::kernel::VirtAddr address, moss::kernel::usize size) noexcept;
    static MMVoidResult advise_usage_pattern(moss::kernel::VirtAddr address, moss::kernel::usize size,
                                            UsagePattern pattern) noexcept;

    // 内存压力管理
    static MMVoidResult trigger_memory_reclaim() noexcept;
    static MMVoidResult trigger_memory_compaction() noexcept;
    [[nodiscard]] static MemoryPressure get_memory_pressure() noexcept;

    // 性能优化
    static MMVoidResult optimize_numa_placement(moss::kernel::VirtAddr address, moss::kernel::usize size) noexcept;
    static MMVoidResult promote_to_huge_pages(moss::kernel::VirtAddr address, moss::kernel::usize size) noexcept;
    static MMVoidResult compact_memory_region(moss::kernel::VirtAddr start, moss::kernel::usize size) noexcept;

    // 系统监控和统计
    [[nodiscard]] static SystemPerformanceStats get_performance_stats() noexcept;
    [[nodiscard]] static bool is_system_healthy() noexcept;
    static void reset_performance_counters() noexcept;

    // 调试和诊断
    static void dump_memory_layout() noexcept;
    static void dump_allocation_history() noexcept;
    static MMResult<MemoryLeakDetector::LeakReport> generate_leak_report() noexcept;

    // 单例访问
    [[nodiscard]] static UnifiedMemoryManager& get_instance() noexcept;

private:
    explicit UnifiedMemoryManager(const SystemConfig& config) noexcept;

    // 内部管理函数
    MMVoidResult initialize_subsystems() noexcept;
    void shutdown_subsystems() noexcept;
    void background_maintenance_thread() noexcept;
    MMVoidResult coordinate_subsystems() noexcept;

    // 智能分配策略选择
    [[nodiscard]] MMResult<moss::kernel::VirtAddr> smart_allocate(const MemoryRequest& request) noexcept;
    [[nodiscard]] AllocationType classify_allocation(const MemoryRequest& request) noexcept;
    [[nodiscard]] bool should_use_huge_pages(const MemoryRequest& request) noexcept;
    [[nodiscard]] numa_node_t select_optimal_numa_node(const MemoryRequest& request) noexcept;

    // 性能优化决策
    void adaptive_performance_tuning() noexcept;
    void balance_subsystem_loads() noexcept;
    void update_allocation_strategies() noexcept;

    // 单例管理
    static bool initialized_;
    static UnifiedMemoryManager* instance_;
};

// ============================================================================
// 便利宏和内联函数
// ============================================================================

// 便利分配宏
#define MOSS_ALLOC(size) \
    moss::kernel::mm::UnifiedMemoryManager::allocate(moss::kernel::mm::MemoryRequest(size))

#define MOSS_ALLOC_ALIGNED(size, alignment) \
    moss::kernel::mm::UnifiedMemoryManager::allocate(moss::kernel::mm::MemoryRequest(size, alignment))

#define MOSS_ALLOC_ZERO(size) \
    moss::kernel::mm::UnifiedMemoryManager::allocate(moss::kernel::mm::MemoryRequest(size, moss::kernel::mm::AllocFlags::ZERO_MEMORY))

#define MOSS_ALLOC_HUGE(size) \
    moss::kernel::mm::UnifiedMemoryManager::allocate(moss::kernel::mm::MemoryRequest(size, moss::kernel::mm::AllocFlags::HUGE_PAGES))

#define MOSS_FREE(addr) \
    moss::kernel::mm::UnifiedMemoryManager::free(addr)

#define MOSS_FREE_SIZED(addr, size) \
    moss::kernel::mm::UnifiedMemoryManager::free(addr, size)

// C风格兼容接口
extern "C" {
    // 标准内存分配接口
    void* moss_malloc(moss::kernel::usize size) noexcept;
    void* moss_calloc(moss::kernel::usize count, moss::kernel::usize size) noexcept;
    void* moss_realloc(void* ptr, moss::kernel::usize size) noexcept;
    void moss_free(void* ptr) noexcept;
    void* moss_aligned_alloc(moss::kernel::usize alignment, moss::kernel::usize size) noexcept;

    // 大页分配接口
    void* moss_huge_malloc(moss::kernel::usize size) noexcept;
    void moss_huge_free(void* ptr, moss::kernel::usize size) noexcept;

    // NUMA感知分配接口
    void* moss_numa_alloc(moss::kernel::usize size, int node) noexcept;
    void moss_numa_free(void* ptr, moss::kernel::usize size) noexcept;

    // 系统状态查询
    int moss_get_memory_pressure() noexcept;
    int moss_is_system_healthy() noexcept;
    void moss_trigger_gc() noexcept;
}

// 全局便利接口
namespace mm {
    // 快速分配接口
    inline MMResult<moss::kernel::VirtAddr> alloc(moss::kernel::usize size) noexcept {
        return UnifiedMemoryManager::allocate(MemoryRequest(size));
    }

    inline MMResult<moss::kernel::VirtAddr> alloc_zero(moss::kernel::usize size) noexcept {
        return UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::ZERO_MEMORY));
    }

    inline MMResult<moss::kernel::VirtAddr> alloc_aligned(moss::kernel::usize size, moss::kernel::usize alignment) noexcept {
        return UnifiedMemoryManager::allocate(MemoryRequest(size, alignment));
    }

    inline MMResult<moss::kernel::VirtAddr> alloc_huge(moss::kernel::usize size) noexcept {
        return UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::HUGE_PAGES));
    }

    // 释放接口
    inline MMVoidResult free(moss::kernel::VirtAddr addr) noexcept {
        return UnifiedMemoryManager::free(addr);
    }

    inline MMVoidResult free_sized(moss::kernel::VirtAddr addr, moss::kernel::usize size) noexcept {
        return UnifiedMemoryManager::free(addr, size);
    }

    // 系统管理
    inline MemoryPressure get_pressure() noexcept {
        return UnifiedMemoryManager::get_memory_pressure();
    }

    inline bool is_healthy() noexcept {
        return UnifiedMemoryManager::is_system_healthy();
    }

    inline MMVoidResult gc() noexcept {
        return UnifiedMemoryManager::trigger_memory_reclaim();
    }

    // 性能优化
    inline MMVoidResult optimize(moss::kernel::VirtAddr addr, moss::kernel::usize size) noexcept {
        return UnifiedMemoryManager::optimize_numa_placement(addr, size);
    }

    inline MMVoidResult compact() noexcept {
        return UnifiedMemoryManager::trigger_memory_compaction();
    }
}

} // namespace moss::kernel::mm
