#pragma once

// 高级内存统计和监控系统 - 现代性能分析与诊断
// 实现详细性能指标收集、内存使用模式分析、泄漏检测和实时性能调优

#include "../include/types.hpp"
#include "../include/result.hpp"
#include "../containers/atomic_types.hpp"
#include "../containers/per_cpu_data.hpp"

namespace moss::kernel::mm {

// 内存统计错误类型
enum class MemoryStatsError : u32 {
    InvalidParameter = 1,
    BufferOverflow = 2,
    StatsDisabled = 3,
    SystemBusy = 4,
    OutOfMemory = 5,
    CorruptedData = 6,
    InitializationFailed = 7
};

// 内存统计结果类型
template<typename T>
using MemoryStatsResult = moss::kernel::Result<T, MemoryStatsError>;
using MemoryStatsVoidResult = moss::kernel::Result<void, MemoryStatsError>;

// 内存分配类型分类
enum class AllocationType : u8 {
    KERNEL_CORE = 0,      // 内核核心分配
    DRIVER = 1,           // 驱动程序分配
    PROCESS = 2,          // 进程相关分配
    CACHE = 3,            // 缓存分配
    NETWORK = 4,          // 网络栈分配
    FILESYSTEM = 5,       // 文件系统分配
    TEMPORARY = 6,        // 临时分配
    UNKNOWN = 7           // 未分类分配
};

// 内存使用模式类型
enum class UsagePattern : u8 {
    SEQUENTIAL = 0,       // 顺序访问
    RANDOM = 1,           // 随机访问
    HOT_COLD = 2,         // 冷热分离
    STREAMING = 3,        // 流式访问
    BATCH = 4,            // 批处理模式
    INTERACTIVE = 5       // 交互式模式
};

// 内存压力级别
enum class MemoryPressure : u8 {
    LOW = 0,              // 低压力 (0-40%)
    MEDIUM = 1,           // 中等压力 (40-70%)
    HIGH = 2,             // 高压力 (70-90%)
    CRITICAL = 3          // 临界压力 (90%+)
};

// 基础内存统计信息
struct BasicMemoryStats {
    moss::kernel::containers::AtomicU64 total_allocated;      // 总分配字节数
    moss::kernel::containers::AtomicU64 total_freed;          // 总释放字节数
    moss::kernel::containers::AtomicU64 current_usage;        // 当前使用量
    moss::kernel::containers::AtomicU64 peak_usage;           // 峰值使用量
    moss::kernel::containers::AtomicU64 allocation_count;     // 分配次数
    moss::kernel::containers::AtomicU64 free_count;           // 释放次数
    moss::kernel::containers::AtomicU64 fragmented_memory;    // 碎片化内存量

    BasicMemoryStats() noexcept
        : total_allocated(0), total_freed(0), current_usage(0), peak_usage(0),
          allocation_count(0), free_count(0), fragmented_memory(0) {}
};

// 分配器特定统计
struct AllocatorStats {
    struct SlabStats {
        moss::kernel::containers::AtomicU64 objects_allocated;
        moss::kernel::containers::AtomicU64 objects_freed;
        moss::kernel::containers::AtomicU64 cache_hits;
        moss::kernel::containers::AtomicU64 cache_misses;
        moss::kernel::containers::AtomicU64 slab_pages;
        double cache_efficiency;                              // 缓存效率
        moss::kernel::u32 active_caches;                     // 活跃缓存数

        SlabStats() noexcept
            : objects_allocated(0), objects_freed(0), cache_hits(0), cache_misses(0),
              slab_pages(0), cache_efficiency(0.0), active_caches(0) {}
    } slab;

    struct BuddyStats {
        moss::kernel::containers::AtomicU64 pages_allocated;
        moss::kernel::containers::AtomicU64 pages_freed;
        moss::kernel::containers::AtomicU64 coalescence_events;  // 合并事件
        moss::kernel::containers::AtomicU64 split_events;        // 分裂事件
        double fragmentation_ratio;                              // 碎片化比例
        moss::kernel::u32 largest_free_block;                   // 最大空闲块

        BuddyStats() noexcept
            : pages_allocated(0), pages_freed(0), coalescence_events(0), split_events(0),
              fragmentation_ratio(0.0), largest_free_block(0) {}
    } buddy;

    struct VmallocStats {
        moss::kernel::containers::AtomicU64 vmalloc_calls;
        moss::kernel::containers::AtomicU64 vfree_calls;
        moss::kernel::containers::AtomicU64 lazy_free_count;
        moss::kernel::containers::AtomicU64 address_space_usage;
        double address_space_efficiency;                         // 地址空间效率

        VmallocStats() noexcept
            : vmalloc_calls(0), vfree_calls(0), lazy_free_count(0),
              address_space_usage(0), address_space_efficiency(0.0) {}
    } vmalloc;
};

// 性能指标统计
struct PerformanceMetrics {
    // 延迟统计 (微秒)
    moss::kernel::containers::AtomicU64 total_alloc_latency_us;
    moss::kernel::containers::AtomicU64 total_free_latency_us;
    moss::kernel::containers::AtomicU64 max_alloc_latency_us;
    moss::kernel::containers::AtomicU64 max_free_latency_us;

    // 吞吐量统计
    moss::kernel::containers::AtomicU64 allocations_per_second;
    moss::kernel::containers::AtomicU64 frees_per_second;
    moss::kernel::containers::AtomicU64 bytes_per_second;

    // 内存访问模式
    moss::kernel::containers::AtomicU64 sequential_access;
    moss::kernel::containers::AtomicU64 random_access;
    moss::kernel::containers::AtomicU64 page_faults;
    moss::kernel::containers::AtomicU64 tlb_misses;

    PerformanceMetrics() noexcept
        : total_alloc_latency_us(0), total_free_latency_us(0), max_alloc_latency_us(0), max_free_latency_us(0),
          allocations_per_second(0), frees_per_second(0), bytes_per_second(0),
          sequential_access(0), random_access(0), page_faults(0), tlb_misses(0) {}
};

// 内存泄漏追踪信息
struct LeakTrackingInfo {
    struct AllocationInfo {
        moss::kernel::VirtAddr address;                       // 分配地址
        moss::kernel::usize size;                             // 分配大小
        AllocationType type;                                  // 分配类型
        moss::kernel::u64 timestamp;                         // 分配时间戳
        moss::kernel::VirtAddr caller_address;               // 调用者地址
        moss::kernel::u32 allocation_id;                     // 分配ID
    };

    static constexpr moss::kernel::usize MAX_TRACKED_ALLOCATIONS = 1024;
    AllocationInfo tracked_allocations[MAX_TRACKED_ALLOCATIONS];
    moss::kernel::containers::AtomicU32 tracked_count;
    moss::kernel::containers::AtomicU64 potential_leaks;
    moss::kernel::containers::AtomicU64 confirmed_leaks;

    LeakTrackingInfo() noexcept : tracked_count(0), potential_leaks(0), confirmed_leaks(0) {}
};

// 前向声明
class MemoryStatsCollectorImpl;
class MemoryLeakDetectorImpl;
class PerformanceProfilerImpl;

// 内存统计收集器
class MemoryStatsCollector {
    friend class MemoryStatsCollectorImpl;

public:
    struct CollectorConfig {
        bool enable_detailed_stats;                          // 启用详细统计
        bool enable_performance_profiling;                   // 启用性能分析
        bool enable_leak_detection;                          // 启用泄漏检测
        moss::kernel::u64 sampling_interval_ms;              // 采样间隔
        moss::kernel::u32 history_depth;                     // 历史深度
        moss::kernel::usize max_tracked_allocations;         // 最大跟踪分配数
    };

    struct SystemStatsSnapshot {
        BasicMemoryStats basic_stats;
        AllocatorStats allocator_stats;
        PerformanceMetrics performance_metrics;
        moss::kernel::u64 timestamp;                         // 快照时间戳
        MemoryPressure pressure_level;                       // 内存压力等级
        UsagePattern dominant_pattern;                       // 主导使用模式
    };

protected:
    CollectorConfig config_;
    SystemStatsSnapshot current_snapshot_;
    SystemStatsSnapshot historical_snapshots_[16];          // 历史快照循环缓冲区
    moss::kernel::containers::AtomicU32 snapshot_index_;
    moss::kernel::containers::AtomicBool collector_active_;

    // Per-CPU统计数据
    static constexpr moss::kernel::u32 MAX_CPUS = 64;
    moss::kernel::containers::PerCpuData<BasicMemoryStats> per_cpu_stats_;

public:
    MemoryStatsCollector() noexcept;
    explicit MemoryStatsCollector(const CollectorConfig& config) noexcept;

    // 统计收集控制
    MemoryStatsVoidResult start_collection() noexcept;
    void stop_collection() noexcept;
    [[nodiscard]] bool is_collecting() const noexcept;

    // 统计数据记录
    void record_allocation(moss::kernel::usize size, AllocationType type, moss::kernel::VirtAddr caller) noexcept;
    void record_free(moss::kernel::usize size, AllocationType type) noexcept;
    void record_performance_event(moss::kernel::u64 latency_us, bool is_allocation) noexcept;

    // 快照管理
    [[nodiscard]] SystemStatsSnapshot take_snapshot() noexcept;
    [[nodiscard]] SystemStatsSnapshot get_current_snapshot() const noexcept;
    void get_historical_snapshots(SystemStatsSnapshot* snapshots, moss::kernel::u32 count) const noexcept;

    // 统计分析
    [[nodiscard]] double calculate_allocation_efficiency() const noexcept;
    [[nodiscard]] double calculate_fragmentation_ratio() const noexcept;
    [[nodiscard]] MemoryPressure assess_memory_pressure() const noexcept;
    [[nodiscard]] UsagePattern detect_usage_pattern() const noexcept;

private:
    void update_performance_metrics() noexcept;
    void update_pressure_assessment() noexcept;
    MemoryStatsVoidResult initialize_per_cpu_stats() noexcept;
};

// 内存泄漏检测器
class MemoryLeakDetector {
    friend class MemoryLeakDetectorImpl;

public:
    struct DetectorConfig {
        bool enable_stack_trace;                             // 启用堆栈跟踪
        bool enable_caller_tracking;                         // 启用调用者跟踪
        moss::kernel::u64 leak_check_interval_ms;            // 泄漏检查间隔
        moss::kernel::u32 leak_threshold_minutes;            // 泄漏阈值(分钟)
        moss::kernel::usize min_leak_size;                   // 最小泄漏大小
    };

    struct LeakReport {
        moss::kernel::usize total_leaked_bytes;
        moss::kernel::u32 leak_count;
        LeakTrackingInfo::AllocationInfo top_leaks[16];      // 最大的泄漏
        moss::kernel::u64 report_timestamp;

        LeakReport() noexcept : total_leaked_bytes(0), leak_count(0), report_timestamp(0) {}
    };

protected:
    DetectorConfig config_;
    LeakTrackingInfo tracking_info_;
    moss::kernel::containers::AtomicBool detector_active_;
    moss::kernel::containers::AtomicU32 allocation_id_counter_;

public:
    MemoryLeakDetector() noexcept;
    explicit MemoryLeakDetector(const DetectorConfig& config) noexcept;

    // 泄漏检测控制
    MemoryStatsVoidResult start_detection() noexcept;
    void stop_detection() noexcept;
    [[nodiscard]] bool is_detecting() const noexcept;

    // 分配跟踪
    void track_allocation(moss::kernel::VirtAddr address, moss::kernel::usize size,
                         AllocationType type, moss::kernel::VirtAddr caller) noexcept;
    void track_free(moss::kernel::VirtAddr address) noexcept;

    // 泄漏分析
    [[nodiscard]] LeakReport generate_leak_report() noexcept;
    [[nodiscard]] bool check_for_leaks() noexcept;
    [[nodiscard]] moss::kernel::usize get_tracked_allocations() const noexcept;

private:
    [[nodiscard]] moss::kernel::u32 find_allocation_index(moss::kernel::VirtAddr address) const noexcept;
    void cleanup_old_allocations() noexcept;
    [[nodiscard]] bool is_potential_leak(const LeakTrackingInfo::AllocationInfo& alloc) const noexcept;
};

// 性能分析器
class PerformanceProfiler {
    friend class PerformanceProfilerImpl;

public:
    struct ProfilerConfig {
        bool enable_latency_profiling;                       // 启用延迟分析
        bool enable_throughput_profiling;                    // 启用吞吐量分析
        bool enable_pattern_analysis;                        // 启用模式分析
        moss::kernel::u64 profiling_window_ms;               // 分析窗口
        moss::kernel::u32 sample_rate;                       // 采样率 (1/N)
    };

    struct PerformanceReport {
        // 延迟统计
        struct LatencyStats {
            moss::kernel::u64 avg_alloc_latency_us;
            moss::kernel::u64 avg_free_latency_us;
            moss::kernel::u64 p95_alloc_latency_us;           // 95百分位数
            moss::kernel::u64 p99_alloc_latency_us;           // 99百分位数
        } latency;

        // 吞吐量统计
        struct ThroughputStats {
            moss::kernel::u64 allocs_per_second;
            moss::kernel::u64 frees_per_second;
            moss::kernel::u64 total_throughput_mbs;           // MB/s
        } throughput;

        // 模式分析
        struct PatternAnalysis {
            UsagePattern dominant_pattern;
            double pattern_confidence;                        // 模式置信度
            moss::kernel::u32 pattern_changes;                // 模式变化次数
        } pattern;

        moss::kernel::u64 report_timestamp;

        PerformanceReport() noexcept : report_timestamp(0) {
            latency = {};
            throughput = {};
            pattern = {};
        }
    };

protected:
    ProfilerConfig config_;
    PerformanceMetrics current_metrics_;
    moss::kernel::containers::AtomicBool profiler_active_;

    // 延迟样本缓冲区
    static constexpr moss::kernel::usize LATENCY_SAMPLE_SIZE = 1000;
    moss::kernel::u64 alloc_latency_samples_[LATENCY_SAMPLE_SIZE];
    moss::kernel::u64 free_latency_samples_[LATENCY_SAMPLE_SIZE];
    moss::kernel::containers::AtomicU32 sample_index_;

public:
    PerformanceProfiler() noexcept;
    explicit PerformanceProfiler(const ProfilerConfig& config) noexcept;

    // 性能分析控制
    MemoryStatsVoidResult start_profiling() noexcept;
    void stop_profiling() noexcept;
    [[nodiscard]] bool is_profiling() const noexcept;

    // 性能数据记录
    void record_allocation_latency(moss::kernel::u64 latency_us) noexcept;
    void record_free_latency(moss::kernel::u64 latency_us) noexcept;
    void record_throughput_sample(moss::kernel::u64 bytes, moss::kernel::u64 time_us) noexcept;
    void record_access_pattern(UsagePattern pattern) noexcept;

    // 性能分析报告
    [[nodiscard]] PerformanceReport generate_report() noexcept;
    [[nodiscard]] PerformanceMetrics get_current_metrics() const noexcept;

private:
    [[nodiscard]] moss::kernel::u64 calculate_percentile(moss::kernel::u64* samples,
                                                         moss::kernel::usize count,
                                                         moss::kernel::u32 percentile) noexcept;
    void analyze_usage_patterns() noexcept;
    void update_throughput_metrics() noexcept;
};

// 内存监控系统主管理器
class MemoryMonitoringSystem {
public:
    struct MonitoringConfig {
        MemoryStatsCollector::CollectorConfig collector_config;
        MemoryLeakDetector::DetectorConfig detector_config;
        PerformanceProfiler::ProfilerConfig profiler_config;
        bool enable_real_time_alerts;                        // 启用实时告警
        moss::kernel::u64 alert_check_interval_ms;           // 告警检查间隔
    };

    struct SystemReport {
        MemoryStatsCollector::SystemStatsSnapshot stats_snapshot;
        MemoryLeakDetector::LeakReport leak_report;
        PerformanceProfiler::PerformanceReport performance_report;
        moss::kernel::u64 report_timestamp;
        MemoryPressure overall_pressure;
    };

protected:
    MonitoringConfig config_;
    MemoryStatsCollector collector_;
    MemoryLeakDetector detector_;
    PerformanceProfiler profiler_;
    moss::kernel::containers::AtomicBool system_active_;

public:
    static MemoryStatsVoidResult initialize(const MonitoringConfig& config) noexcept;

    // 系统控制
    MemoryStatsVoidResult start_monitoring() noexcept;
    void stop_monitoring() noexcept;
    [[nodiscard]] bool is_monitoring() const noexcept;

    // 监控接口
    void on_allocation(moss::kernel::VirtAddr addr, moss::kernel::usize size,
                      AllocationType type, moss::kernel::VirtAddr caller,
                      moss::kernel::u64 latency_us) noexcept;
    void on_free(moss::kernel::VirtAddr addr, moss::kernel::usize size,
                AllocationType type, moss::kernel::u64 latency_us) noexcept;

    // 报告生成
    [[nodiscard]] SystemReport generate_system_report() noexcept;
    [[nodiscard]] MemoryPressure get_current_pressure() const noexcept;
    [[nodiscard]] bool check_system_health() noexcept;

    // 单例访问
    [[nodiscard]] static MemoryMonitoringSystem& get_instance() noexcept;

private:
    explicit MemoryMonitoringSystem(const MonitoringConfig& config) noexcept;

    void check_alerts() noexcept;
    void process_real_time_data() noexcept;

    // 单例实例
    static bool initialized_;
    static MemoryMonitoringSystem* instance_;
};

// 全局便利接口
namespace memory_stats {
    // 初始化监控系统
    inline MemoryStatsVoidResult initialize(const MemoryMonitoringSystem::MonitoringConfig& config) noexcept {
        return MemoryMonitoringSystem::initialize(config);
    }

    // 记录分配事件
    inline void record_allocation(moss::kernel::VirtAddr addr, moss::kernel::usize size,
                                 AllocationType type = AllocationType::UNKNOWN,
                                 moss::kernel::u64 latency_us = 0) noexcept {
        moss::kernel::VirtAddr caller = 0; // 实际应该获取调用者地址
        MemoryMonitoringSystem::get_instance().on_allocation(addr, size, type, caller, latency_us);
    }

    // 记录释放事件
    inline void record_free(moss::kernel::VirtAddr addr, moss::kernel::usize size,
                           AllocationType type = AllocationType::UNKNOWN,
                           moss::kernel::u64 latency_us = 0) noexcept {
        MemoryMonitoringSystem::get_instance().on_free(addr, size, type, latency_us);
    }

    // 获取当前内存压力
    inline MemoryPressure get_memory_pressure() noexcept {
        return MemoryMonitoringSystem::get_instance().get_current_pressure();
    }

    // 生成系统报告
    inline MemoryMonitoringSystem::SystemReport get_system_report() noexcept {
        return MemoryMonitoringSystem::get_instance().generate_system_report();
    }

    // 检查系统健康状况
    inline bool is_system_healthy() noexcept {
        return MemoryMonitoringSystem::get_instance().check_system_health();
    }
}

} // namespace moss::kernel::mm
