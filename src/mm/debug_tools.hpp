#pragma once

// MOSS内核内存调试和性能调优工具集
// 提供全面的内存分析、性能调优、泄漏检测和系统诊断功能

#include "../include/types.hpp"
#include "../include/result.hpp"
#include "mm_interface.hpp"
#include "memory_stats.hpp"

namespace moss::kernel::mm::debug {

// 调试错误类型
enum class DebugError : u32 {
    FeatureDisabled = 1,
    InvalidParameter = 2,
    BufferTooSmall = 3,
    SystemBusy = 4,
    AnalysisFailed = 5,
    ReportGenerationFailed = 6
};

// 调试结果类型
template<typename T>
using DebugResult = moss::kernel::Result<T, DebugError>;
using DebugVoidResult = moss::kernel::Result<void, DebugError>;

// ============================================================================
// 内存布局分析器
// ============================================================================

struct MemoryLayoutInfo {
    struct RegionInfo {
        moss::kernel::VirtAddr start_address;
        moss::kernel::VirtAddr end_address;
        moss::kernel::usize size;
        moss::kernel::PhysAddr physical_start;
        const char* region_name;
        AllocationType allocation_type;
        numa_node_t numa_node;
        bool is_huge_page;
        bool is_fragmented;
    };

    static constexpr moss::kernel::usize MAX_REGIONS = 256;
    RegionInfo regions[MAX_REGIONS];
    moss::kernel::u32 region_count;
    moss::kernel::usize total_virtual_space;
    moss::kernel::usize total_physical_memory;
    moss::kernel::usize total_allocated_memory;
    double fragmentation_ratio;
    double space_efficiency;

    MemoryLayoutInfo() noexcept : region_count(0), total_virtual_space(0),
        total_physical_memory(0), total_allocated_memory(0),
        fragmentation_ratio(0.0), space_efficiency(0.0) {}
};

class MemoryLayoutAnalyzer {
public:
    // 生成内存布局报告
    [[nodiscard]] static DebugResult<MemoryLayoutInfo> analyze_memory_layout() noexcept;

    // 检测内存热点
    [[nodiscard]] static DebugResult<moss::kernel::usize> detect_memory_hotspots(
        MemoryLayoutInfo::RegionInfo* hotspots, moss::kernel::usize max_count) noexcept;

    // 分析地址空间碎片化
    [[nodiscard]] static double analyze_address_space_fragmentation() noexcept;

    // 生成内存映射图
    static DebugVoidResult generate_memory_map(char* buffer, moss::kernel::usize buffer_size) noexcept;

private:
    static void scan_virtual_memory_regions(MemoryLayoutInfo& info) noexcept;
    static void analyze_region_characteristics(MemoryLayoutInfo::RegionInfo& region) noexcept;
    static double calculate_fragmentation_score(const MemoryLayoutInfo& info) noexcept;
};

// ============================================================================
// 性能分析器
// ============================================================================

struct PerformanceProfile {
    // 分配性能分析
    struct AllocationProfile {
        moss::kernel::u64 small_alloc_count;      // <1KB分配
        moss::kernel::u64 medium_alloc_count;     // 1KB-64KB分配
        moss::kernel::u64 large_alloc_count;      // >64KB分配
        moss::kernel::u64 small_alloc_latency_us;
        moss::kernel::u64 medium_alloc_latency_us;
        moss::kernel::u64 large_alloc_latency_us;
        double small_success_rate;
        double medium_success_rate;
        double large_success_rate;
    } allocation_profile;

    // 热路径分析
    struct HotPathAnalysis {
        moss::kernel::VirtAddr top_callers[16];   // 最频繁的分配调用者
        moss::kernel::u64 caller_frequencies[16]; // 调用频率
        moss::kernel::u64 caller_latencies[16];   // 调用延迟
        moss::kernel::u32 caller_count;
    } hot_path_analysis;

    // 缓存性能分析
    struct CachePerformance {
        moss::kernel::u64 l1_cache_hits;
        moss::kernel::u64 l1_cache_misses;
        moss::kernel::u64 tlb_hits;
        moss::kernel::u64 tlb_misses;
        double cache_efficiency;
        double tlb_efficiency;
    } cache_performance;

    moss::kernel::u64 profile_duration_ms;
    moss::kernel::u64 sample_count;

    PerformanceProfile() noexcept {
        allocation_profile = {};
        hot_path_analysis = {};
        cache_performance = {};
        profile_duration_ms = 0;
        sample_count = 0;
    }
};

class PerformanceProfiler {
public:
    struct ProfilerConfig {
        moss::kernel::u64 profile_duration_ms;    // 分析持续时间
        moss::kernel::u32 sampling_rate;          // 采样率 (1/N)
        bool enable_call_stack_tracking;          // 启用调用栈跟踪
        bool enable_cache_analysis;               // 启用缓存分析
        bool enable_numa_analysis;                // 启用NUMA分析
    };

    // 启动性能分析
    [[nodiscard]] static DebugResult<PerformanceProfile> profile_system_performance(
        const ProfilerConfig& config) noexcept;

    // 分析特定分配模式
    [[nodiscard]] static DebugResult<PerformanceProfile> profile_allocation_pattern(
        moss::kernel::usize min_size, moss::kernel::usize max_size,
        moss::kernel::u32 sample_count) noexcept;

    // 基准测试
    [[nodiscard]] static DebugResult<PerformanceProfile> benchmark_allocator_performance() noexcept;

    // 性能回归检测
    [[nodiscard]] static bool detect_performance_regression(
        const PerformanceProfile& baseline, const PerformanceProfile& current,
        double threshold_percent) noexcept;

private:
    static void collect_allocation_samples(PerformanceProfile& profile,
                                          const ProfilerConfig& config) noexcept;
    static void analyze_hot_paths(PerformanceProfile& profile) noexcept;
    static void measure_cache_performance(PerformanceProfile& profile) noexcept;
};

// ============================================================================
// 高级内存泄漏检测器
// ============================================================================

struct AdvancedLeakReport {
    struct LeakPattern {
        moss::kernel::VirtAddr caller_address;
        moss::kernel::usize allocation_size;
        moss::kernel::u32 leak_count;
        moss::kernel::u64 total_leaked_bytes;
        moss::kernel::u64 avg_leak_lifetime_ms;
        AllocationType allocation_type;
        const char* suspected_cause;
    };

    static constexpr moss::kernel::usize MAX_LEAK_PATTERNS = 32;
    LeakPattern leak_patterns[MAX_LEAK_PATTERNS];
    moss::kernel::u32 pattern_count;

    moss::kernel::usize total_leaked_memory;
    moss::kernel::u32 total_leak_count;
    double leak_rate_per_hour;
    moss::kernel::u64 analysis_timestamp;

    AdvancedLeakReport() noexcept : pattern_count(0), total_leaked_memory(0),
        total_leak_count(0), leak_rate_per_hour(0.0), analysis_timestamp(0) {}
};

class AdvancedLeakDetector {
public:
    struct DetectorConfig {
        bool enable_pattern_analysis;             // 启用模式分析
        bool enable_predictive_detection;         // 启用预测检测
        moss::kernel::u64 analysis_window_hours;  // 分析窗口
        moss::kernel::usize min_pattern_count;    // 最小模式匹配数
        double confidence_threshold;              // 置信度阈值
    };

    // 执行高级泄漏分析
    [[nodiscard]] static DebugResult<AdvancedLeakReport> analyze_memory_leaks(
        const DetectorConfig& config) noexcept;

    // 预测潜在泄漏
    [[nodiscard]] static DebugResult<moss::kernel::u32> predict_potential_leaks(
        moss::kernel::VirtAddr* potential_leaks, moss::kernel::u32 max_count) noexcept;

    // 生成泄漏修复建议
    static DebugVoidResult generate_leak_fix_suggestions(
        const AdvancedLeakReport& report, char* suggestions, moss::kernel::usize buffer_size) noexcept;

private:
    static void identify_leak_patterns(AdvancedLeakReport& report,
                                      const DetectorConfig& config) noexcept;
    static void classify_leak_causes(AdvancedLeakReport::LeakPattern& pattern) noexcept;
    static double calculate_leak_confidence(const AdvancedLeakReport::LeakPattern& pattern) noexcept;
};

// ============================================================================
// 系统健康检查器
// ============================================================================

struct SystemHealthReport {
    enum class HealthStatus : u8 {
        EXCELLENT = 0,    // 优秀
        GOOD = 1,         // 良好
        WARNING = 2,      // 警告
        CRITICAL = 3,     // 危险
        FAILURE = 4       // 故障
    };

    struct ComponentHealth {
        const char* component_name;
        HealthStatus status;
        double health_score;      // 0.0 - 100.0
        const char* issues;       // 问题描述
        const char* recommendations; // 改进建议
    };

    static constexpr moss::kernel::u32 MAX_COMPONENTS = 16;
    ComponentHealth components[MAX_COMPONENTS];
    moss::kernel::u32 component_count;

    HealthStatus overall_status;
    double overall_score;
    moss::kernel::u64 check_timestamp;

    // 关键指标
    double memory_utilization;
    double fragmentation_level;
    double allocation_success_rate;
    moss::kernel::u64 average_allocation_latency_us;
    MemoryPressure pressure_level;

    SystemHealthReport() noexcept : component_count(0), overall_status(HealthStatus::GOOD),
        overall_score(100.0), check_timestamp(0), memory_utilization(0.0),
        fragmentation_level(0.0), allocation_success_rate(100.0),
        average_allocation_latency_us(0), pressure_level(MemoryPressure::LOW) {}
};

class SystemHealthChecker {
public:
    struct CheckConfig {
        bool enable_comprehensive_check;          // 启用全面检查
        bool enable_performance_check;            // 启用性能检查
        bool enable_stability_check;              // 启用稳定性检查
        moss::kernel::u64 check_duration_ms;      // 检查持续时间
        double warning_threshold;                 // 警告阈值
        double critical_threshold;                // 危险阈值
    };

    // 执行系统健康检查
    [[nodiscard]] static DebugResult<SystemHealthReport> check_system_health(
        const CheckConfig& config) noexcept;

    // 连续健康监控
    [[nodiscard]] static DebugVoidResult start_continuous_monitoring(
        moss::kernel::u64 check_interval_ms) noexcept;
    static void stop_continuous_monitoring() noexcept;

    // 生成健康改进建议
    static DebugVoidResult generate_improvement_suggestions(
        const SystemHealthReport& report, char* suggestions, moss::kernel::usize buffer_size) noexcept;

private:
    static void check_memory_management_health(SystemHealthReport& report) noexcept;
    static void check_allocation_performance(SystemHealthReport& report) noexcept;
    static void check_system_stability(SystemHealthReport& report) noexcept;
    static SystemHealthReport::HealthStatus calculate_component_status(double score) noexcept;
};

// ============================================================================
// 性能调优建议引擎
// ============================================================================

struct TuningRecommendations {
    enum class RecommendationType : u8 {
        CONFIGURATION = 0,    // 配置调整
        ALGORITHM = 1,        // 算法优化
        MEMORY_LAYOUT = 2,    // 内存布局优化
        NUMA_TUNING = 3,      // NUMA调优
        CACHE_OPTIMIZATION = 4, // 缓存优化
        BACKGROUND_TASK = 5   // 后台任务调整
    };

    struct Recommendation {
        RecommendationType type;
        const char* title;
        const char* description;
        const char* implementation_guide;
        double expected_improvement_percent;
        moss::kernel::u32 implementation_effort;  // 1-5难度等级
        moss::kernel::u32 priority;              // 1-5优先级
    };

    static constexpr moss::kernel::u32 MAX_RECOMMENDATIONS = 32;
    Recommendation recommendations[MAX_RECOMMENDATIONS];
    moss::kernel::u32 recommendation_count;

    double potential_performance_gain;
    moss::kernel::u64 analysis_timestamp;

    TuningRecommendations() noexcept : recommendation_count(0),
        potential_performance_gain(0.0), analysis_timestamp(0) {}
};

class PerformanceTuningEngine {
public:
    // 生成性能调优建议
    [[nodiscard]] static DebugResult<TuningRecommendations> analyze_and_recommend(
        const SystemHealthReport& health_report,
        const PerformanceProfile& perf_profile) noexcept;

    // 自动应用安全的调优建议
    [[nodiscard]] static DebugVoidResult apply_safe_recommendations(
        const TuningRecommendations& recommendations) noexcept;

    // 评估调优效果
    [[nodiscard]] static DebugResult<double> evaluate_tuning_effectiveness(
        const PerformanceProfile& before, const PerformanceProfile& after) noexcept;

private:
    static void analyze_allocation_patterns(TuningRecommendations& recs,
                                           const PerformanceProfile& profile) noexcept;
    static void analyze_memory_layout(TuningRecommendations& recs,
                                     const SystemHealthReport& health) noexcept;
    static void analyze_numa_efficiency(TuningRecommendations& recs) noexcept;
    static void prioritize_recommendations(TuningRecommendations& recs) noexcept;
};

// ============================================================================
// 全局调试接口
// ============================================================================

class MemoryDebugInterface {
public:
    // 快速系统检查
    [[nodiscard]] static bool quick_system_check() noexcept;

    // 生成综合诊断报告
    [[nodiscard]] static DebugVoidResult generate_diagnostic_report(
        char* buffer, moss::kernel::usize buffer_size) noexcept;

    // 启用/禁用调试功能
    static void enable_debug_features(bool enable) noexcept;
    static void enable_verbose_logging(bool enable) noexcept;

    // 内存转储和分析
    static DebugVoidResult dump_memory_state(const char* filename) noexcept;
    static DebugVoidResult analyze_memory_dump(const char* filename) noexcept;

    // 压力测试
    [[nodiscard]] static DebugResult<PerformanceProfile> run_stress_test(
        moss::kernel::u64 duration_ms, moss::kernel::u32 thread_count) noexcept;

    // 基准测试套件
    [[nodiscard]] static DebugResult<PerformanceProfile> run_benchmark_suite() noexcept;
};

// 便利宏和函数
#define MOSS_DEBUG_ASSERT(condition) \
    do { if (!(condition)) moss::kernel::mm::debug::debug_panic(__FILE__, __LINE__, #condition); } while(0)

#define MOSS_DEBUG_CHECK_HEALTH() \
    moss::kernel::mm::debug::MemoryDebugInterface::quick_system_check()

// 调试辅助函数
void debug_panic(const char* file, int line, const char* condition) noexcept;
void debug_log(const char* format, ...) noexcept;
void debug_trace_allocation(moss::kernel::VirtAddr addr, moss::kernel::usize size) noexcept;

} // namespace moss::kernel::mm::debug
