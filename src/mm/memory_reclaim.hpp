#pragma once

// 内存回收引擎 - Linux风格LRU页面回收机制
// 实现多级LRU列表、工作集检测和智能页面扫描算法

#include "../include/types.hpp"
#include "../include/result.hpp"
#include "../containers/atomic_types.hpp"
#include "../containers/per_cpu_data.hpp"
#include "buddy_allocator_v2.hpp"

namespace moss::kernel::mm {

// 内存回收错误类型
enum class ReclaimError : u32 {
    OutOfMemory = 1,
    InvalidPage = 2,
    ReclameFailed = 3,
    WorkingSetViolation = 4,
    ScannerBusy = 5,
    PressureThresholdExceeded = 6
};

// 内存回收结果类型
template<typename T>
using ReclaimResult = moss::kernel::Result<T, ReclaimError>;
using ReclaimVoidResult = moss::kernel::Result<void, ReclaimError>;

// 页面状态枚举
enum class PageState : u32 {
    ACTIVE = 0,        // 活跃页面 - 最近被访问
    INACTIVE = 1,      // 非活跃页面 - 候选回收页面
    UNEVICTABLE = 2,   // 不可回收页面 - 锁定或特殊用途
    RECLAIMABLE = 3,   // 可回收页面 - 缓存页面等
    FREE = 4           // 空闲页面
};

// 页面访问模式
enum class AccessPattern : u8 {
    RANDOM = 0,        // 随机访问
    SEQUENTIAL = 1,    // 顺序访问
    LOCALITY = 2       // 局部性访问
};

// 内存回收策略
enum class ReclaimPolicy : u32 {
    CONSERVATIVE = 0,   // 保守回收 - 优先保留工作集
    BALANCED = 1,       // 平衡回收 - 默认策略
    AGGRESSIVE = 2,     // 激进回收 - 最大化可用内存
    EMERGENCY = 3       // 紧急回收 - OOM情况下使用
};

// 页面元数据 - 扩展版本用于回收
struct ReclaimPageInfo {
    moss::kernel::PhysAddr phys_addr;           // 物理地址
    PageState state;                            // 页面状态
    AccessPattern access_pattern;               // 访问模式
    moss::kernel::u64 last_access_time;        // 最后访问时间
    moss::kernel::u32 access_count;            // 访问计数
    moss::kernel::u32 age;                     // 页面年龄

    // LRU链表节点
    ReclaimPageInfo* prev_lru;
    ReclaimPageInfo* next_lru;

    // 引用和标志
    moss::kernel::containers::AtomicU32 ref_count;
    moss::kernel::containers::AtomicU32 flags;

    // 页面标志
    enum Flags : u32 {
        DIRTY = (1 << 0),          // 脏页面 - 需要写回
        LOCKED = (1 << 1),         // 锁定页面 - 不可回收
        REFERENCED = (1 << 2),     // 被引用标记
        WORKINGSET = (1 << 3),     // 工作集页面
        UNEVICTABLE = (1 << 4),    // 永不回收
        MIGRATE_PENDING = (1 << 5) // 迁移中
    };
};

// 前向声明
class LRUListImpl;
class WorkingSetDetectorImpl;
class PageScannerImpl;
class MemoryPressureMonitorImpl;

// LRU链表管理
class LRUList {
    friend class LRUListImpl;
    friend class PageScannerImpl;

public:
    struct LRUStats {
        moss::kernel::usize active_pages;
        moss::kernel::usize inactive_pages;
        moss::kernel::usize unevictable_pages;
        moss::kernel::usize scan_rate;      // 页面/秒
        moss::kernel::usize reclaim_rate;   // 页面/秒
    };

protected:
    ReclaimPageInfo* active_head_;
    ReclaimPageInfo* active_tail_;
    ReclaimPageInfo* inactive_head_;
    ReclaimPageInfo* inactive_tail_;
    ReclaimPageInfo* unevictable_head_;
    ReclaimPageInfo* unevictable_tail_;

    moss::kernel::containers::AtomicSize active_count_;
    moss::kernel::containers::AtomicSize inactive_count_;
    moss::kernel::containers::AtomicSize unevictable_count_;

    // 统计信息
    moss::kernel::containers::AtomicU64 total_scans_;
    moss::kernel::containers::AtomicU64 total_reclaims_;
    moss::kernel::u64 last_scan_time_;

public:
    LRUList() noexcept;

    // 页面状态转换
    void add_to_active(ReclaimPageInfo* page) noexcept;
    void add_to_inactive(ReclaimPageInfo* page) noexcept;
    void add_to_unevictable(ReclaimPageInfo* page) noexcept;

    void move_to_active(ReclaimPageInfo* page) noexcept;
    void move_to_inactive(ReclaimPageInfo* page) noexcept;
    void move_to_unevictable(ReclaimPageInfo* page) noexcept;

    void remove_from_lru(ReclaimPageInfo* page) noexcept;

    // 扫描接口
    [[nodiscard]] ReclaimPageInfo* get_inactive_tail() const noexcept { return inactive_tail_; }
    [[nodiscard]] ReclaimPageInfo* get_active_tail() const noexcept { return active_tail_; }

    // 统计信息
    [[nodiscard]] LRUStats get_stats() const noexcept;
    [[nodiscard]] moss::kernel::usize get_active_count() const noexcept;
    [[nodiscard]] moss::kernel::usize get_inactive_count() const noexcept;

private:
    void insert_to_list(ReclaimPageInfo** head, ReclaimPageInfo** tail, ReclaimPageInfo* page) noexcept;
    void remove_from_list(ReclaimPageInfo** head, ReclaimPageInfo** tail, ReclaimPageInfo* page) noexcept;
};

// 工作集检测器
class WorkingSetDetector {
    friend class WorkingSetDetectorImpl;

public:
    struct WorkingSetStats {
        moss::kernel::usize working_set_size;   // 工作集大小
        moss::kernel::usize thrashing_pages;    // 颠簸页面数
        double hit_ratio;                       // 命中率
        moss::kernel::u64 refault_rate;         // 重新访问率
    };

    // 工作集检测配置
    struct WorkingSetConfig {
        moss::kernel::usize min_working_set_size;    // 最小工作集大小
        moss::kernel::usize max_working_set_size;    // 最大工作集大小
        moss::kernel::u64 sample_interval_ms;        // 采样间隔
        double thrashing_threshold;                  // 颠簸阈值
    };

protected:
    WorkingSetConfig config_;
    moss::kernel::containers::AtomicSize current_working_set_size_;
    moss::kernel::containers::AtomicU64 refault_count_;
    moss::kernel::containers::AtomicU64 access_count_;
    moss::kernel::u64 last_sample_time_;

    // 页面访问历史环形缓冲区
    static constexpr moss::kernel::usize HISTORY_SIZE = 1024;
    moss::kernel::PhysAddr access_history_[HISTORY_SIZE];
    moss::kernel::containers::AtomicU32 history_index_;

public:
    WorkingSetDetector(const WorkingSetConfig& config) noexcept;

    // 工作集管理
    void record_page_access(moss::kernel::PhysAddr addr) noexcept;
    void record_page_fault(moss::kernel::PhysAddr addr) noexcept;
    [[nodiscard]] bool is_in_working_set(moss::kernel::PhysAddr addr) const noexcept;
    [[nodiscard]] bool is_thrashing_detected() const noexcept;

    // 统计和调优
    [[nodiscard]] WorkingSetStats get_stats() const noexcept;
    void update_working_set_size() noexcept;
    [[nodiscard]] moss::kernel::usize recommend_working_set_size() const noexcept;

private:
    [[nodiscard]] bool page_recently_accessed(moss::kernel::PhysAddr addr) const noexcept;
    [[nodiscard]] double calculate_hit_ratio() const noexcept;
};

// 页面扫描器 - 核心回收算法
class PageScanner {
    friend class PageScannerImpl;

public:
    struct ScanConfig {
        moss::kernel::usize scan_batch_size;        // 每次扫描页面数
        moss::kernel::u64 scan_interval_ms;         // 扫描间隔
        moss::kernel::u32 max_scan_priority;        // 最大扫描优先级
        double active_inactive_ratio;               // 活跃/非活跃比例
    };

    struct ScanStats {
        moss::kernel::usize pages_scanned;
        moss::kernel::usize pages_reclaimed;
        moss::kernel::usize pages_activated;
        moss::kernel::usize pages_deactivated;
        moss::kernel::u64 scan_time_us;
        double efficiency_ratio;  // 回收效率
    };

protected:
    ScanConfig config_;
    LRUList* lru_list_;
    WorkingSetDetector* working_set_detector_;
    moss::kernel::containers::AtomicU32 scan_priority_;
    moss::kernel::containers::AtomicBool scanning_active_;

    // 扫描统计
    ScanStats current_stats_;
    moss::kernel::u64 last_scan_time_;

public:
    PageScanner(const ScanConfig& config, LRUList* lru, WorkingSetDetector* wsd) noexcept;

    // 扫描控制
    ReclaimVoidResult start_scan(moss::kernel::u32 priority = 0) noexcept;
    void stop_scan() noexcept;
    [[nodiscard]] bool is_scanning() const noexcept;

    // 回收策略
    ReclaimResult<moss::kernel::usize> scan_inactive_list(moss::kernel::usize target_pages) noexcept;
    ReclaimResult<moss::kernel::usize> scan_active_list(moss::kernel::usize target_pages) noexcept;
    ReclaimVoidResult balance_active_inactive() noexcept;

    // 页面评估
    [[nodiscard]] bool should_reclaim_page(ReclaimPageInfo* page) const noexcept;
    [[nodiscard]] bool should_activate_page(ReclaimPageInfo* page) const noexcept;
    ReclaimVoidResult reclaim_page(ReclaimPageInfo* page) noexcept;

    // 统计信息
    [[nodiscard]] ScanStats get_stats() const noexcept { return current_stats_; }
    void reset_stats() noexcept;

private:
    [[nodiscard]] moss::kernel::u32 calculate_page_age(ReclaimPageInfo* page) const noexcept;
    [[nodiscard]] bool is_page_recently_accessed(ReclaimPageInfo* page) const noexcept;
    void update_page_access_info(ReclaimPageInfo* page) noexcept;
};

// 内存压力监控
class MemoryPressureMonitor {
    friend class MemoryPressureMonitorImpl;

public:
    enum class PressureLevel : u32 {
        LOW = 0,      // 低压力 - 正常操作
        MEDIUM = 1,   // 中等压力 - 开始回收
        HIGH = 2,     // 高压力 - 激进回收
        CRITICAL = 3  // 临界压力 - 紧急回收
    };

    struct PressureStats {
        PressureLevel current_level;
        moss::kernel::usize free_memory;
        moss::kernel::usize available_memory;
        moss::kernel::usize cached_memory;
        double pressure_ratio;
    };

protected:
    moss::kernel::containers::AtomicU32 current_pressure_level_;
    moss::kernel::containers::AtomicSize free_threshold_low_;
    moss::kernel::containers::AtomicSize free_threshold_high_;
    moss::kernel::containers::AtomicSize critical_threshold_;

    // 压力历史
    static constexpr moss::kernel::usize PRESSURE_HISTORY_SIZE = 64;
    PressureLevel pressure_history_[PRESSURE_HISTORY_SIZE];
    moss::kernel::containers::AtomicU32 history_index_;

public:
    MemoryPressureMonitor() noexcept;

    // 压力检测
    void update_pressure_level() noexcept;
    [[nodiscard]] PressureLevel get_pressure_level() const noexcept;
    [[nodiscard]] bool should_start_reclaim() const noexcept;
    [[nodiscard]] moss::kernel::u32 get_scan_priority() const noexcept;

    // 阈值管理
    void set_free_thresholds(moss::kernel::usize low, moss::kernel::usize high) noexcept;
    void set_critical_threshold(moss::kernel::usize critical) noexcept;

    // 统计信息
    [[nodiscard]] PressureStats get_stats() const noexcept;
    [[nodiscard]] bool is_pressure_increasing() const noexcept;

    // 内部状态访问（供friend类使用）
    void record_pressure_level(PressureLevel level) noexcept;

private:
    [[nodiscard]] moss::kernel::usize get_free_memory() const noexcept;
    [[nodiscard]] moss::kernel::usize get_available_memory() const noexcept;
};

// 内存回收引擎 - 主控制器
class MemoryReclaimEngine {
public:
    // 回收配置
    struct ReclaimConfig {
        ReclaimPolicy default_policy;
        moss::kernel::usize min_free_pages;
        moss::kernel::usize target_free_pages;
        PageScanner::ScanConfig scan_config;
        WorkingSetDetector::WorkingSetConfig workingset_config;
    };

    // 回收统计
    struct ReclaimEngineStats {
        moss::kernel::usize total_pages_reclaimed;
        moss::kernel::usize direct_reclaims;
        moss::kernel::usize background_reclaims;
        moss::kernel::u64 total_reclaim_time_us;
        LRUList::LRUStats lru_stats;
        PageScanner::ScanStats scanner_stats;
        MemoryPressureMonitor::PressureStats pressure_stats;
        WorkingSetDetector::WorkingSetStats workingset_stats;
    };

    // 类型别名，用于统一接口
    using ReclaimStats = ReclaimEngineStats;

protected:
    ReclaimConfig config_;
    LRUList lru_list_;
    PageScanner scanner_;
    MemoryPressureMonitor pressure_monitor_;
    WorkingSetDetector working_set_detector_;

    moss::kernel::containers::AtomicBool engine_active_;
    moss::kernel::containers::AtomicU64 total_reclaims_;
    moss::kernel::containers::AtomicU64 direct_reclaims_;
    moss::kernel::containers::AtomicU64 background_reclaims_;

public:
    static ReclaimVoidResult initialize(const ReclaimConfig& config) noexcept;

    // 回收控制
    ReclaimResult<moss::kernel::usize> direct_reclaim(moss::kernel::usize target_pages,
                                                     ReclaimPolicy policy = ReclaimPolicy::BALANCED) noexcept;
    ReclaimVoidResult start_background_reclaim() noexcept;
    void stop_background_reclaim() noexcept;

    // 页面生命周期管理
    ReclaimVoidResult add_page(moss::kernel::PhysAddr addr, PageState initial_state = PageState::ACTIVE) noexcept;
    ReclaimVoidResult remove_page(moss::kernel::PhysAddr addr) noexcept;
    ReclaimVoidResult mark_page_accessed(moss::kernel::PhysAddr addr) noexcept;

    // 压力响应
    ReclaimVoidResult handle_memory_pressure() noexcept;
    [[nodiscard]] bool should_trigger_oom() const noexcept;

    // 统计和监控
    [[nodiscard]] ReclaimEngineStats get_stats() const noexcept;
    [[nodiscard]] static MemoryReclaimEngine& get_instance() noexcept;

private:
    MemoryReclaimEngine(const ReclaimConfig& config) noexcept;

    // 内部实现
    ReclaimVoidResult background_reclaim_thread() noexcept;
    [[nodiscard]] moss::kernel::usize calculate_reclaim_target() const noexcept;
    ReclaimVoidResult update_page_access_pattern(ReclaimPageInfo* page) noexcept;

    // 策略实现
    ReclaimResult<moss::kernel::usize> conservative_reclaim(moss::kernel::usize target) noexcept;
    ReclaimResult<moss::kernel::usize> balanced_reclaim(moss::kernel::usize target) noexcept;
    ReclaimResult<moss::kernel::usize> aggressive_reclaim(moss::kernel::usize target) noexcept;
    ReclaimResult<moss::kernel::usize> emergency_reclaim(moss::kernel::usize target) noexcept;

    // 单例实例
    static bool initialized_;
    static MemoryReclaimEngine* instance_;
};

// 全局便利接口
namespace memory_reclaim {
    // 初始化回收引擎
    inline ReclaimVoidResult initialize(const MemoryReclaimEngine::ReclaimConfig& config) noexcept {
        return MemoryReclaimEngine::initialize(config);
    }

    // 直接回收
    inline ReclaimResult<moss::kernel::usize> reclaim_pages(moss::kernel::usize count) noexcept {
        return MemoryReclaimEngine::get_instance().direct_reclaim(count);
    }

    // 标记页面访问
    inline ReclaimVoidResult mark_accessed(moss::kernel::PhysAddr addr) noexcept {
        return MemoryReclaimEngine::get_instance().mark_page_accessed(addr);
    }

    // 获取内存压力等级
    inline MemoryPressureMonitor::PressureLevel get_memory_pressure() noexcept {
        return MemoryReclaimEngine::get_instance().get_stats().pressure_stats.current_level;
    }

    // 启动后台回收
    inline ReclaimVoidResult start_background() noexcept {
        return MemoryReclaimEngine::get_instance().start_background_reclaim();
    }
}

} // namespace moss::kernel::mm
