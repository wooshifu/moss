#pragma once

// 内存压缩系统 - Linux风格页面迁移和碎片整理
// 实现智能页面迁移算法，减少内存碎片化，为大内存分配提供连续空间

#include "core/types.hpp"
#include "core/result.hpp"
#include "containers/atomic_types.hpp"
#include "containers/per_cpu_data.hpp"
#include "buddy_allocator_v2.hpp"
#include "memory_reclaim.hpp"

namespace moss::kernel::mm {

// 内存压缩错误类型
enum class CompactionError : u32 {
    OutOfMemory = 1,
    InvalidPage = 2,
    MigrationFailed = 3,
    CompactionAborted = 4,
    FragmentationSevere = 5,
    CMAAllocationFailed = 6,
    PageLocked = 7
};

// 内存压缩结果类型
template<typename T>
using CompactionResult = moss::kernel::Result<T, CompactionError>;
using CompactionVoidResult = moss::kernel::Result<void, CompactionError>;

// 压缩策略枚举
enum class CompactionStrategy : u32 {
    LIGHT = 0,      // 轻度压缩 - 仅处理容易迁移的页面
    MEDIUM = 1,     // 中等压缩 - 平衡性能和效果
    HEAVY = 2,      // 重度压缩 - 最大化压缩效果
    EMERGENCY = 3   // 紧急压缩 - 不考虑性能开销
};

// 页面迁移类型 - 基于Linux内核设计
enum class MigrationMode : u32 {
    SYNC = 0,       // 同步迁移 - 等待完成
    ASYNC = 1,      // 异步迁移 - 非阻塞
    LAZY = 2        // 延迟迁移 - 后台执行
};

// CMA (Contiguous Memory Allocator) 区域
struct CMARegion {
    moss::kernel::PhysAddr start_addr;          // 起始物理地址
    moss::kernel::usize size;                   // 区域大小
    moss::kernel::usize alignment;              // 对齐要求
    moss::kernel::containers::AtomicU32 allocated_count; // 已分配页面数
    moss::kernel::containers::AtomicU32 flags;  // 区域标志

    // CMA区域标志
    enum Flags : u32 {
        RESERVED = (1 << 0),    // 保留区域
        MOVABLE = (1 << 1),     // 可移动页面
        PINNED = (1 << 2),      // 固定页面
        DMA_COHERENT = (1 << 3) // DMA一致性
    };

    CMARegion(moss::kernel::PhysAddr start, moss::kernel::usize sz,
              moss::kernel::usize align = PAGE_SIZE) noexcept
        : start_addr(start), size(sz), alignment(align), allocated_count(0), flags(0) {}
};

// 页面迁移描述符
struct PageMigration {
    moss::kernel::PhysAddr source_addr;         // 源物理地址
    moss::kernel::PhysAddr target_addr;         // 目标物理地址
    ReclaimPageInfo* page_info;                 // 页面信息
    MigrationMode mode;                         // 迁移模式
    moss::kernel::u32 retry_count;             // 重试次数

    // 迁移状态
    enum class Status : u8 {
        PENDING = 0,    // 等待迁移
        IN_PROGRESS = 1,// 迁移中
        COMPLETED = 2,  // 迁移完成
        FAILED = 3,     // 迁移失败
        ABORTED = 4     // 迁移中止
    };

    moss::kernel::containers::AtomicU32 status; // 迁移状态

    PageMigration(moss::kernel::PhysAddr src, moss::kernel::PhysAddr dst,
                 ReclaimPageInfo* info, MigrationMode m) noexcept
        : source_addr(src), target_addr(dst), page_info(info), mode(m),
          retry_count(0), status(static_cast<moss::kernel::u32>(Status::PENDING)) {}
};

// 前向声明
class CompactionScannerImpl;
class PageMigratorImpl;
class CMAAllocatorImpl;

// 压缩扫描器 - 识别可迁移的页面
class CompactionScanner {
    friend class CompactionScannerImpl;

public:
    struct ScanConfig {
        moss::kernel::usize scan_window_size;       // 扫描窗口大小
        moss::kernel::u32 max_scan_pages;          // 最大扫描页面数
        CompactionStrategy strategy;               // 压缩策略
        moss::kernel::u64 scan_interval_us;        // 扫描间隔(微秒)
    };

    struct ScanStats {
        moss::kernel::usize pages_scanned;
        moss::kernel::usize movable_pages_found;
        moss::kernel::usize fragmented_blocks;
        moss::kernel::usize largest_free_block;
        double fragmentation_ratio;                // 碎片化比例
        moss::kernel::u64 scan_time_us;
    };

protected:
    ScanConfig config_;
    moss::kernel::containers::AtomicU32 scanner_active_;
    ScanStats current_stats_;

    // 扫描窗口
    moss::kernel::PhysAddr scan_start_;
    moss::kernel::PhysAddr scan_end_;

public:
    CompactionScanner(const ScanConfig& config) noexcept;

    // 扫描控制
    CompactionVoidResult start_scan(moss::kernel::PhysAddr start, moss::kernel::PhysAddr end) noexcept;
    void stop_scan() noexcept;
    [[nodiscard]] bool is_scanning() const noexcept;

    // 页面扫描
    CompactionResult<moss::kernel::usize> scan_for_movable_pages(
        moss::kernel::PhysAddr* page_list, moss::kernel::usize max_pages) noexcept;
    CompactionResult<moss::kernel::usize> identify_fragmented_regions(
        moss::kernel::PhysAddr* regions, moss::kernel::usize max_regions) noexcept;

    // 碎片化分析
    [[nodiscard]] double calculate_fragmentation_ratio(moss::kernel::PhysAddr start,
                                                       moss::kernel::PhysAddr end) const noexcept;
    [[nodiscard]] moss::kernel::usize find_largest_free_block(moss::kernel::PhysAddr start,
                                                              moss::kernel::PhysAddr end) const noexcept;

    // 统计信息
    [[nodiscard]] ScanStats get_stats() const noexcept { return current_stats_; }
    void reset_stats() noexcept;

private:
    [[nodiscard]] bool is_page_movable(moss::kernel::PhysAddr addr) const noexcept;
    [[nodiscard]] bool should_compact_region(moss::kernel::PhysAddr start, moss::kernel::usize size) const noexcept;
};

// 页面迁移器 - 执行实际的页面迁移
class PageMigrator {
    friend class PageMigratorImpl;

public:
    struct MigrationConfig {
        moss::kernel::u32 max_concurrent_migrations;   // 最大并发迁移数
        moss::kernel::u32 max_retry_count;             // 最大重试次数
        moss::kernel::u64 migration_timeout_us;        // 迁移超时(微秒)
        MigrationMode default_mode;                    // 默认迁移模式
    };

    struct MigrationStats {
        moss::kernel::usize pages_migrated;
        moss::kernel::usize migration_failures;
        moss::kernel::usize retries_performed;
        moss::kernel::u64 total_migration_time_us;
        moss::kernel::u64 average_migration_time_us;
        double success_ratio;
    };

protected:
    MigrationConfig config_;
    moss::kernel::containers::AtomicU32 active_migrations_;
    MigrationStats current_stats_;

    // 迁移队列 (简化实现)
    static constexpr moss::kernel::usize MAX_MIGRATION_QUEUE = 256;
    PageMigration migration_queue_[MAX_MIGRATION_QUEUE];
    moss::kernel::containers::AtomicU32 queue_head_;
    moss::kernel::containers::AtomicU32 queue_tail_;

public:
    PageMigrator(const MigrationConfig& config) noexcept;

    // 迁移控制
    CompactionVoidResult migrate_page(moss::kernel::PhysAddr source, moss::kernel::PhysAddr target,
                                     MigrationMode mode = MigrationMode::SYNC) noexcept;
    CompactionResult<moss::kernel::usize> migrate_page_batch(const moss::kernel::PhysAddr* sources,
                                                            const moss::kernel::PhysAddr* targets,
                                                            moss::kernel::usize count,
                                                            MigrationMode mode = MigrationMode::ASYNC) noexcept;

    // 队列管理
    CompactionVoidResult enqueue_migration(const PageMigration& migration) noexcept;
    CompactionResult<PageMigration> dequeue_migration() noexcept;
    void process_migration_queue() noexcept;

    // 迁移状态查询
    [[nodiscard]] PageMigration::Status get_migration_status(moss::kernel::PhysAddr source) const noexcept;
    [[nodiscard]] bool is_migration_complete(moss::kernel::PhysAddr source) const noexcept;

    // 统计信息
    [[nodiscard]] MigrationStats get_stats() const noexcept { return current_stats_; }
    void reset_stats() noexcept;

private:
    CompactionVoidResult perform_single_migration(const PageMigration& migration) noexcept;
    CompactionVoidResult copy_page_data(moss::kernel::PhysAddr source, moss::kernel::PhysAddr target) noexcept;
    CompactionVoidResult update_page_references(moss::kernel::PhysAddr old_addr, moss::kernel::PhysAddr new_addr) noexcept;
};

// CMA连续内存分配器
class CMAAllocator {
    friend class CMAAllocatorImpl;

public:
    struct CMAConfig {
        moss::kernel::usize region_count;              // CMA区域数量
        moss::kernel::usize min_region_size;           // 最小区域大小
        moss::kernel::usize max_region_size;           // 最大区域大小
        moss::kernel::usize alignment_requirement;     // 对齐要求
    };

    struct CMAStats {
        moss::kernel::usize total_regions;
        moss::kernel::usize active_regions;
        moss::kernel::usize total_size;
        moss::kernel::usize allocated_size;
        moss::kernel::usize largest_available_block;
        double utilization_ratio;                      // 利用率
    };

protected:
    CMAConfig config_;
    static constexpr moss::kernel::usize MAX_CMA_REGIONS = 64;
    CMARegion regions_[MAX_CMA_REGIONS];
    moss::kernel::containers::AtomicU32 region_count_;

    CMAStats current_stats_;

public:
    CMAAllocator(const CMAConfig& config) noexcept;

    // CMA区域管理
    CompactionResult<moss::kernel::u32> create_cma_region(moss::kernel::PhysAddr start,
                                                         moss::kernel::usize size,
                                                         moss::kernel::usize alignment = PAGE_SIZE) noexcept;
    CompactionVoidResult destroy_cma_region(moss::kernel::u32 region_id) noexcept;

    // 连续内存分配
    CompactionResult<moss::kernel::PhysAddr> allocate_contiguous(moss::kernel::usize size,
                                                                moss::kernel::usize alignment = PAGE_SIZE) noexcept;
    CompactionVoidResult free_contiguous(moss::kernel::PhysAddr addr, moss::kernel::usize size) noexcept;

    // 区域状态查询
    [[nodiscard]] bool is_cma_address(moss::kernel::PhysAddr addr) const noexcept;
    [[nodiscard]] moss::kernel::u32 find_region_for_address(moss::kernel::PhysAddr addr) const noexcept;
    [[nodiscard]] moss::kernel::usize get_available_space(moss::kernel::u32 region_id) const noexcept;

    // 统计信息
    [[nodiscard]] CMAStats get_stats() const noexcept { return current_stats_; }
    void update_stats() noexcept;

private:
    [[nodiscard]] moss::kernel::u32 find_suitable_region(moss::kernel::usize size, moss::kernel::usize alignment) const noexcept;
    CompactionVoidResult prepare_cma_region(moss::kernel::u32 region_id, moss::kernel::usize required_size) noexcept;
};

// 内存压缩引擎 - 主控制器
class MemoryCompactionEngine {
    friend class MemoryCompactionEngineImpl;

public:
    struct CompactionConfig {
        CompactionScanner::ScanConfig scanner_config;
        PageMigrator::MigrationConfig migrator_config;
        CMAAllocator::CMAConfig cma_config;
        CompactionStrategy default_strategy;

        // 触发条件
        double fragmentation_threshold;                // 碎片化触发阈值
        moss::kernel::usize min_free_memory_threshold; // 最小空闲内存阈值
        moss::kernel::u64 compaction_interval_ms;      // 压缩间隔(毫秒)
    };

    struct CompactionEngineStats {
        moss::kernel::usize total_compactions;
        moss::kernel::usize successful_compactions;
        moss::kernel::u64 total_compaction_time_us;
        moss::kernel::usize pages_migrated_total;
        CompactionScanner::ScanStats scanner_stats;
        PageMigrator::MigrationStats migrator_stats;
        CMAAllocator::CMAStats cma_stats;
        double overall_fragmentation_improvement;      // 整体碎片改善率
    };

    // 类型别名，用于统一接口
    using CompactionStats = CompactionEngineStats;

protected:
    CompactionConfig config_;
    CompactionScanner scanner_;
    PageMigrator migrator_;
    CMAAllocator cma_allocator_;

    moss::kernel::containers::AtomicBool engine_active_;
    moss::kernel::containers::AtomicU32 compaction_priority_;
    CompactionEngineStats current_stats_;

    // 后台压缩控制
    moss::kernel::containers::AtomicBool background_compaction_enabled_;
    moss::kernel::u64 last_compaction_time_;

public:
    static CompactionVoidResult initialize(const CompactionConfig& config) noexcept;

    // 压缩控制
    CompactionResult<moss::kernel::usize> perform_compaction(CompactionStrategy strategy,
                                                           moss::kernel::PhysAddr start_addr = 0,
                                                           moss::kernel::usize size = 0) noexcept;
    CompactionVoidResult start_background_compaction() noexcept;
    void stop_background_compaction() noexcept;

    // 触发条件检查
    [[nodiscard]] bool should_compact() const noexcept;
    [[nodiscard]] CompactionStrategy recommend_strategy() const noexcept;
    CompactionVoidResult handle_fragmentation_pressure() noexcept;

    // CMA连续内存接口
    CompactionResult<moss::kernel::PhysAddr> allocate_large_contiguous(moss::kernel::usize size,
                                                                      moss::kernel::usize alignment = PAGE_SIZE) noexcept;
    CompactionVoidResult free_large_contiguous(moss::kernel::PhysAddr addr, moss::kernel::usize size) noexcept;

    // 统计和监控
    [[nodiscard]] CompactionEngineStats get_stats() const noexcept;
    [[nodiscard]] double get_current_fragmentation_ratio() const noexcept;
    [[nodiscard]] static MemoryCompactionEngine& get_instance() noexcept;

private:
    MemoryCompactionEngine(const CompactionConfig& config) noexcept;

    // 内部实现
    CompactionVoidResult background_compaction_thread() noexcept;
    [[nodiscard]] moss::kernel::usize calculate_compaction_target() const noexcept;
    CompactionVoidResult update_fragmentation_stats() noexcept;

    // 策略实现
    CompactionResult<moss::kernel::usize> light_compaction(moss::kernel::PhysAddr start, moss::kernel::usize size) noexcept;
    CompactionResult<moss::kernel::usize> medium_compaction(moss::kernel::PhysAddr start, moss::kernel::usize size) noexcept;
    CompactionResult<moss::kernel::usize> heavy_compaction(moss::kernel::PhysAddr start, moss::kernel::usize size) noexcept;
    CompactionResult<moss::kernel::usize> emergency_compaction(moss::kernel::PhysAddr start, moss::kernel::usize size) noexcept;

    // 单例实例
    static bool initialized_;
    static MemoryCompactionEngine* instance_;
};

// 全局便利接口
namespace memory_compaction {
    // 初始化压缩引擎
    inline CompactionVoidResult initialize(const MemoryCompactionEngine::CompactionConfig& config) noexcept {
        return MemoryCompactionEngine::initialize(config);
    }

    // 执行压缩
    inline CompactionResult<moss::kernel::usize> compact_memory(CompactionStrategy strategy = CompactionStrategy::MEDIUM) noexcept {
        return MemoryCompactionEngine::get_instance().perform_compaction(strategy);
    }

    // 连续内存分配
    inline CompactionResult<moss::kernel::PhysAddr> allocate_contiguous(moss::kernel::usize size,
                                                                       moss::kernel::usize alignment = PAGE_SIZE) noexcept {
        return MemoryCompactionEngine::get_instance().allocate_large_contiguous(size, alignment);
    }

    // 释放连续内存
    inline CompactionVoidResult free_contiguous(moss::kernel::PhysAddr addr, moss::kernel::usize size) noexcept {
        return MemoryCompactionEngine::get_instance().free_large_contiguous(addr, size);
    }

    // 获取碎片化比例
    inline double get_fragmentation_ratio() noexcept {
        return MemoryCompactionEngine::get_instance().get_current_fragmentation_ratio();
    }

    // 启动后台压缩
    inline CompactionVoidResult start_background() noexcept {
        return MemoryCompactionEngine::get_instance().start_background_compaction();
    }
}

} // namespace moss::kernel::mm
