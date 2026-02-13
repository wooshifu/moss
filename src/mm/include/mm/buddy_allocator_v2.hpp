#pragma once

// 增强Buddy分配器 - 实现迁移类型反碎片化
// 基于Linux内核的anti-fragmentation设计，支持页面迁移类型分类和智能分配策略

#include "core/types.hpp"
#include "core/result.hpp"
#include "containers/atomic_types.hpp"
#include "containers/per_cpu_data.hpp"

// 页面大小常量
constexpr moss::kernel::usize PAGE_SIZE = 4096;  // 4KB
constexpr moss::kernel::usize PAGE_SHIFT = 12;   // log2(PAGE_SIZE)

// Buddy算法最大阶数 (支持最大4MB分配 = 4KB * 2^10 = 4MB)
constexpr moss::kernel::usize MAX_ORDER = 10;

namespace moss::kernel::mm {

// 页面迁移类型 - 反碎片化的核心
enum class MigrationType : u32 {
    UNMOVABLE = 0,    // 不可移动页面：内核代码、数据结构等
    MOVABLE = 1,      // 可移动页面：用户空间页面、匿名页面等
    RECLAIMABLE = 2,  // 可回收页面：文件缓存、buffer等
    TYPES_COUNT = 3   // 迁移类型总数
};

// 页面块大小 - 用于迁移类型分组 (通常为2MB，对应9阶)
constexpr usize PAGEBLOCK_ORDER = 9;  // 2MB = 4KB * 2^9
constexpr usize PAGEBLOCK_SIZE = (1UL << PAGEBLOCK_ORDER);
constexpr usize PAGEBLOCK_PAGES = PAGEBLOCK_SIZE >> PAGE_SHIFT;

// 页面分配器错误类型
enum class BuddyError : u32 {
    OutOfMemory = 1,
    InvalidOrder = 2,
    InvalidAddress = 3,
    InvalidMigrationType = 4,
    InitializationFailed = 5,
    FragmentationSevere = 6
};

// 页面分配结果类型
template<typename T>
using BuddyResult = moss::kernel::Result<T, BuddyError>;
using BuddyVoidResult = moss::kernel::Result<void, BuddyError>;

// 分配标志
enum class BuddyAllocFlags : u32 {
    NONE = 0,
    EMERGENCY = (1 << 0),     // 紧急分配，可以打破反碎片化规则
    NO_FALLBACK = (1 << 1),   // 禁用fallback，严格按迁移类型分配
    PREFAULT = (1 << 2),      // 预分配，用于减少页面错误
    HIGH_PRIORITY = (1 << 3)  // 高优先级分配
};

// 页面分配请求结构
struct PageAllocRequest {
    usize order;                    // 分配阶数
    MigrationType migration_type;   // 迁移类型
    BuddyAllocFlags flags;              // 分配标志

    PageAllocRequest(usize o, MigrationType mt, BuddyAllocFlags f = BuddyAllocFlags::NONE) noexcept
        : order(o), migration_type(mt), flags(f) {}
};

// 增强的Buddy分配器 - 支持反碎片化
class BuddyAllocatorV2 {
public:
    // 初始化增强Buddy分配器
    static BuddyVoidResult initialize() noexcept;

    // 高级页面分配接口
    [[nodiscard]] static BuddyResult<PhysAddr> allocate_pages(const PageAllocRequest& request) noexcept;

    // 简化分配接口（向后兼容）
    [[nodiscard]] static BuddyResult<PhysAddr> allocate_pages(usize order,
                                                              MigrationType migration_type = MigrationType::MOVABLE) noexcept {
        return allocate_pages(PageAllocRequest(order, migration_type));
    }

    // 释放页面
    static BuddyVoidResult free_pages(PhysAddr addr, usize order) noexcept;

    // 内存碎片整理接口
    static BuddyVoidResult compact_memory() noexcept;

    // 水位控制 - 内存压力检测
    enum class WaterMark : u32 {
        LOW = 0,      // 低水位 - 开始回收
        MIN = 1,      // 最低水位 - 紧急分配
        HIGH = 2      // 高水位 - 正常操作
    };

    [[nodiscard]] static WaterMark get_water_mark() noexcept;
    [[nodiscard]] static bool is_memory_pressure() noexcept;

    // 反碎片化统计信息
    struct FragmentationStats {
        usize total_free_pages;
        usize largest_free_block_pages;  // 最大连续空闲块大小
        double fragmentation_index;      // 碎片化指数 (0-1, 越小越好)
        usize free_pages_by_migration[static_cast<u32>(MigrationType::TYPES_COUNT)];
        usize unusable_pages;            // 无法利用的小碎片页面数
    };

    [[nodiscard]] static FragmentationStats get_fragmentation_stats() noexcept;

    // 内存统计信息
    struct MemoryStats {
        usize total_pages;
        usize free_pages;
        usize used_pages;
        usize kernel_pages;

        // 按迁移类型统计
        usize pages_by_migration[static_cast<u32>(MigrationType::TYPES_COUNT)];

        // 反碎片化统计
        usize pageblock_count;
        usize mixed_pageblocks;      // 混合迁移类型的页面块数
        usize steal_count;           // fallback偷取计数
    };

    [[nodiscard]] static MemoryStats get_memory_stats() noexcept;

private:
    // 页面块元数据 - 跟踪2MB块的迁移类型
    struct PageBlock {
        MigrationType migration_type;                    // 页面块的主要迁移类型
        moss::kernel::containers::AtomicU32 free_pages; // 块内空闲页面数
        moss::kernel::containers::AtomicU32 flags;       // 页面块标志

        // 页面块状态
        enum Flags : u32 {
            MIXED = (1 << 0),        // 包含多种迁移类型的页面
            DIRTY = (1 << 1),        // 需要压缩整理
            RESERVED = (1 << 2)      // 保留块，不参与分配
        };
    };

    // 增强的空闲块结构
    struct FreeBlock {
        FreeBlock* next;
        FreeBlock* prev;
        usize order;
        MigrationType migration_type;  // 记录分配时的迁移类型

        // 性能优化：预取信息
        u64 last_access_time;         // 最后访问时间（用于LRU）
    };

    // 增强的页面元数据
    struct PageMetadata {
        moss::kernel::containers::AtomicU32 ref_count;
        moss::kernel::containers::AtomicU32 flags;
        MigrationType migration_type;        // 页面的迁移类型
        FreeBlock* free_list_node;

        // 页面标志
        enum Flags : u32 {
            ALLOCATED = (1 << 0),
            MOVABLE = (1 << 1),
            RECLAIMABLE = (1 << 2),
            COMPOUND_HEAD = (1 << 3),    // 复合页面头
            COMPOUND_TAIL = (1 << 4)     // 复合页面尾
        };
    };

    // Per-CPU页面缓存 - 优化小页面分配
    struct PerCpuPageCache {
        // 每种迁移类型的0阶页面缓存
        static constexpr usize CACHE_SIZE = 64;

        struct Cache {
            PhysAddr pages[CACHE_SIZE];
            moss::kernel::containers::AtomicU32 count;
            moss::kernel::containers::AtomicU64 alloc_count;
            moss::kernel::containers::AtomicU64 free_count;
        } caches[static_cast<u32>(MigrationType::TYPES_COUNT)];

        // 批量分配/释放
        BuddyResult<usize> refill_cache(MigrationType migration_type) noexcept;
        BuddyVoidResult drain_cache(MigrationType migration_type) noexcept;
    };

    // 静态数据成员
    static bool initialized_;
    static moss::kernel::containers::PerCpuData<PerCpuPageCache> per_cpu_caches_;

    // 按迁移类型分组的空闲列表 [migration_type][order]
    static FreeBlock* free_lists_[static_cast<u32>(MigrationType::TYPES_COUNT)][MAX_ORDER + 1];

    static PageMetadata* page_metadata_;
    static PageBlock* page_blocks_;          // 页面块元数据数组
    static usize total_pages_;
    static usize total_pageblocks_;

    // 原子统计计数器
    static moss::kernel::containers::AtomicSize free_pages_;
    static moss::kernel::containers::AtomicSize used_pages_;
    static moss::kernel::containers::AtomicU64 allocation_count_;
    static moss::kernel::containers::AtomicU64 steal_count_;      // fallback偷取计数

    // 水位线
    static usize low_watermark_;
    static usize min_watermark_;
    static usize high_watermark_;

    // 私有方法 - 核心反碎片化算法
    static BuddyResult<PhysAddr> allocate_from_migration_type(usize order, MigrationType migration_type) noexcept;
    static BuddyResult<PhysAddr> fallback_allocate(usize order, MigrationType preferred_type) noexcept;

    // 页面块管理
    static PageBlock* get_pageblock(PhysAddr addr) noexcept;
    static void set_pageblock_migration_type(PageBlock* block, MigrationType type) noexcept;
    static MigrationType get_pageblock_migration_type(PhysAddr addr) noexcept;
    static void mark_pageblock_mixed(PageBlock* block) noexcept;

    // 智能fallback策略
    static MigrationType get_fallback_migration_type(MigrationType original, usize order) noexcept;
    static bool can_steal_from_pageblock(PageBlock* block, MigrationType target_type, usize order) noexcept;
    static BuddyResult<PhysAddr> steal_pages(PageBlock* source_block, usize order, MigrationType target_type) noexcept;

    // Buddy算法增强版
    static FreeBlock* find_buddy_in_migration_type(FreeBlock* block, usize order, MigrationType migration_type) noexcept;
    static void merge_buddies_with_migration_check(FreeBlock* block, usize order) noexcept;
    static void add_to_free_list_typed(FreeBlock* block, usize order, MigrationType migration_type) noexcept;
    static FreeBlock* remove_from_free_list_typed(usize order, MigrationType migration_type) noexcept;

    // 内存碎片整理
    static BuddyVoidResult compact_pageblock(PageBlock* block) noexcept;
    static bool is_compaction_needed() noexcept;
    static usize calculate_fragmentation_index() noexcept;

    // Per-CPU缓存管理
    static BuddyResult<PhysAddr> allocate_from_percpu_cache(MigrationType migration_type) noexcept;
    static BuddyVoidResult free_to_percpu_cache(PhysAddr addr, MigrationType migration_type) noexcept;

    // 水位线管理
    static void update_watermarks() noexcept;
    static bool should_trigger_reclaim() noexcept;
    static void trigger_memory_reclaim() noexcept;

    // 统计和监控
    static void update_allocation_stats(usize order, MigrationType migration_type) noexcept;
    static void update_fragmentation_stats() noexcept;

    // 内存布局解析（继承自原版）
    static BuddyVoidResult parse_memory_layout() noexcept;
    static void initialize_pageblocks() noexcept;
    static void initialize_free_lists_typed() noexcept;
    static void initialize_watermarks() noexcept;

    // 诊断和调试
    #ifdef DEBUG
    static void validate_free_lists_typed() noexcept;
    static void dump_fragmentation_info() noexcept;
    static void verify_pageblock_consistency() noexcept;
    #endif
};

// 全局接口函数 - 向后兼容现有代码
[[nodiscard]] inline BuddyResult<PhysAddr> allocate_pages(usize order) noexcept {
    return BuddyAllocatorV2::allocate_pages(order, MigrationType::MOVABLE);
}

inline BuddyVoidResult free_pages(PhysAddr addr, usize order) noexcept {
    return BuddyAllocatorV2::free_pages(addr, order);
}

// 便利函数 - 按用途分配页面
namespace page_alloc {
    // 内核页面分配（不可移动）
    [[nodiscard]] inline BuddyResult<PhysAddr> alloc_kernel_pages(usize order) noexcept {
        return BuddyAllocatorV2::allocate_pages(order, MigrationType::UNMOVABLE);
    }

    // 用户页面分配（可移动）
    [[nodiscard]] inline BuddyResult<PhysAddr> alloc_user_pages(usize order) noexcept {
        return BuddyAllocatorV2::allocate_pages(order, MigrationType::MOVABLE);
    }

    // 缓存页面分配（可回收）
    [[nodiscard]] inline BuddyResult<PhysAddr> alloc_cache_pages(usize order) noexcept {
        return BuddyAllocatorV2::allocate_pages(order, MigrationType::RECLAIMABLE);
    }

    // 紧急分配（忽略反碎片化规则）
    [[nodiscard]] inline BuddyResult<PhysAddr> alloc_emergency_pages(usize order) noexcept {
        PageAllocRequest request(order, MigrationType::MOVABLE, BuddyAllocFlags::EMERGENCY);
        return BuddyAllocatorV2::allocate_pages(request);
    }
}

} // namespace moss::kernel::mm

