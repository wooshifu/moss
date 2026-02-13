#pragma once

// 大页支持系统 - 现代高性能内存管理
// 实现透明大页(THP)、HugeTLB和大页池管理，优化TLB性能

#include "core/types.hpp"
#include "core/result.hpp"
#include "containers/atomic_types.hpp"
#include "numa_policy.hpp"

namespace moss::kernel::mm {

// 大页错误类型
enum class HugePagesError : u32 {
    OutOfMemory = 1,
    InvalidSize = 2,
    InvalidAlignment = 3,
    PoolExhausted = 4,
    FragmentationSevere = 5,
    THPDisabled = 6,
    DefragmentationFailed = 7
};

// 大页结果类型
template<typename T>
using HugePagesResult = moss::kernel::Result<T, HugePagesError>;
using HugePagesVoidResult = moss::kernel::Result<void, HugePagesError>;

// 大页尺寸枚举 - 支持多种大页大小
enum class HugePageSize : u32 {
    SIZE_2MB = 0,    // 2MB页面 (ARM64: 2MB, x86_64: 2MB)
    SIZE_1GB = 1,    // 1GB页面 (ARM64: 1GB, x86_64: 1GB)
    SIZE_16MB = 2,   // 16MB页面 (ARM64特有)
    SIZE_32MB = 3,   // 32MB页面 (ARM64特有)
    COUNT = 4        // 大页尺寸总数
};

// 大页尺寸常量
constexpr moss::kernel::usize HUGE_PAGE_2MB = 2 * 1024 * 1024;     // 2MB
constexpr moss::kernel::usize HUGE_PAGE_1GB = 1024 * 1024 * 1024;  // 1GB
constexpr moss::kernel::usize HUGE_PAGE_16MB = 16 * 1024 * 1024;   // 16MB
constexpr moss::kernel::usize HUGE_PAGE_32MB = 32 * 1024 * 1024;   // 32MB

// 透明大页策略
enum class THPPolicy : u32 {
    ALWAYS = 0,      // 始终启用THP
    MADVISE = 1,     // 仅在madvise时启用
    NEVER = 2,       // 禁用THP
    DEFER = 3        // 延迟分配
};

// 大页分割策略
enum class SplitPolicy : u32 {
    IMMEDIATE = 0,   // 立即分割
    LAZY = 1,        // 延迟分割
    NEVER = 2        // 不分割
};

// 大页碎片整理策略
enum class DefragPolicy : u32 {
    ALWAYS = 0,      // 总是进行碎片整理
    DEFER = 1,       // 延迟碎片整理
    MADVISE = 2,     // 仅在建议时整理
    NEVER = 3        // 从不整理
};

// 大页元数据
struct HugePageInfo {
    moss::kernel::PhysAddr phys_addr;           // 物理地址
    moss::kernel::VirtAddr virt_addr;           // 虚拟地址
    HugePageSize size_class;                    // 大页尺寸类别
    moss::kernel::usize size;                   // 实际大小
    numa_node_t numa_node;                      // NUMA节点

    // 状态标志
    moss::kernel::containers::AtomicU32 flags;
    enum Flags : u32 {
        ALLOCATED = (1 << 0),    // 已分配
        MAPPED = (1 << 1),       // 已映射
        LOCKED = (1 << 2),       // 锁定状态
        COMPOUND = (1 << 3),     // 复合页面
        SPLITTING = (1 << 4),    // 正在分割
        MIGRATION = (1 << 5),    // 迁移中
        DIRTY = (1 << 6),        // 脏页面
        REFERENCED = (1 << 7)    // 最近被引用
    };

    // 引用计数和使用统计
    moss::kernel::containers::AtomicU32 ref_count;
    moss::kernel::u64 allocation_time;
    moss::kernel::u64 last_access_time;

    HugePageInfo() noexcept
        : phys_addr(0), virt_addr(0), size_class(HugePageSize::SIZE_2MB),
          size(HUGE_PAGE_2MB), numa_node(NUMA_NO_NODE), flags(0), ref_count(0),
          allocation_time(0), last_access_time(0) {}

    HugePageInfo(moss::kernel::PhysAddr paddr, HugePageSize sz, numa_node_t node) noexcept
        : phys_addr(paddr), virt_addr(0), size_class(sz), numa_node(node),
          flags(Flags::ALLOCATED), ref_count(1), allocation_time(0), last_access_time(0) {

        switch (sz) {
            case HugePageSize::SIZE_2MB: size = HUGE_PAGE_2MB; break;
            case HugePageSize::SIZE_1GB: size = HUGE_PAGE_1GB; break;
            case HugePageSize::SIZE_16MB: size = HUGE_PAGE_16MB; break;
            case HugePageSize::SIZE_32MB: size = HUGE_PAGE_32MB; break;
            case HugePageSize::COUNT: size = HUGE_PAGE_2MB; break; // COUNT作为默认情况
            default: size = HUGE_PAGE_2MB; break; // 默认情况
        }
    }
};

// 前向声明
class HugePagePoolImpl;
class THPManagerImpl;
class HugeTLBImpl;

// 大页池管理 - 预分配大页池
class HugePagePool {
    friend class HugePagePoolImpl;

public:
    struct PoolConfig {
        moss::kernel::usize pool_sizes[static_cast<u32>(HugePageSize::COUNT)]; // 每种大页的池大小
        moss::kernel::usize min_free_ratio;    // 最小空闲比例 (%)
        moss::kernel::usize max_free_ratio;    // 最大空闲比例 (%)
        bool numa_aware;                       // NUMA感知
        bool dynamic_resizing;                 // 动态调整大小
    };

    struct PoolStats {
        moss::kernel::usize total_pages[static_cast<u32>(HugePageSize::COUNT)];
        moss::kernel::usize free_pages[static_cast<u32>(HugePageSize::COUNT)];
        moss::kernel::usize allocated_pages[static_cast<u32>(HugePageSize::COUNT)];
        moss::kernel::usize allocation_failures[static_cast<u32>(HugePageSize::COUNT)];
        double pool_utilization[static_cast<u32>(HugePageSize::COUNT)];
        moss::kernel::u64 total_allocations;
        moss::kernel::u64 total_frees;
    };

protected:
    PoolConfig config_;
    PoolStats stats_;

    // 每种大页尺寸的空闲列表
    struct FreeList {
        HugePageInfo* head;
        moss::kernel::containers::AtomicU32 count;
        moss::kernel::containers::AtomicU64 total_size;
    } free_lists_[static_cast<u32>(HugePageSize::COUNT)];

    // NUMA感知的Per-Node池
    static constexpr moss::kernel::u32 MAX_NUMA_POOLS = 16;
    struct NumaPool {
        FreeList free_lists[static_cast<u32>(HugePageSize::COUNT)];
        moss::kernel::containers::AtomicU32 pressure_level; // 内存压力 (0-100)
    } numa_pools_[MAX_NUMA_POOLS];

    moss::kernel::containers::AtomicBool pool_initialized_;
    moss::kernel::containers::AtomicU32 global_pressure_;

public:
    HugePagePool() noexcept;
    HugePagePool(const PoolConfig& config) noexcept;

    // 池管理
    HugePagesVoidResult initialize_pool() noexcept;
    HugePagesVoidResult resize_pool(HugePageSize size, moss::kernel::usize new_count) noexcept;
    void shutdown_pool() noexcept;

    // 分配和释放
    [[nodiscard]] HugePagesResult<HugePageInfo*> allocate_huge_page(
        HugePageSize size, numa_node_t preferred_node = NUMA_NO_NODE) noexcept;
    HugePagesVoidResult free_huge_page(HugePageInfo* page) noexcept;

    // 池状态查询
    [[nodiscard]] moss::kernel::usize get_free_count(HugePageSize size, numa_node_t node = NUMA_NO_NODE) const noexcept;
    [[nodiscard]] bool is_pool_available(HugePageSize size) const noexcept;
    [[nodiscard]] moss::kernel::u32 get_memory_pressure(numa_node_t node = NUMA_NO_NODE) const noexcept;

    // 统计信息
    [[nodiscard]] PoolStats get_stats() const noexcept;
    void reset_stats() noexcept;

private:
    HugePagesVoidResult preallocate_pages(HugePageSize size, moss::kernel::usize count, numa_node_t node) noexcept;
    HugePagesVoidResult add_to_free_list(HugePageInfo* page, numa_node_t node) noexcept;
    [[nodiscard]] HugePageInfo* remove_from_free_list(HugePageSize size, numa_node_t node) noexcept;
    void update_pressure_level(numa_node_t node) noexcept;
};

// 透明大页管理器 (THP)
class THPManager {
    friend class THPManagerImpl;

public:
    struct THPConfig {
        THPPolicy policy;                       // THP策略
        SplitPolicy split_policy;               // 分割策略
        DefragPolicy defrag_policy;             // 碎片整理策略
        moss::kernel::usize scan_interval_ms;   // 扫描间隔
        moss::kernel::u32 defrag_threshold;     // 碎片整理阈值 (%)
        bool khugepaged_enabled;                // 启用khugepaged守护进程
    };

    struct THPStats {
        moss::kernel::usize thp_allocations;           // THP分配次数
        moss::kernel::usize thp_splits;                // THP分割次数
        moss::kernel::usize thp_collapses;             // THP合并次数
        moss::kernel::usize thp_defrag_attempts;       // 碎片整理尝试
        moss::kernel::usize thp_defrag_successes;      // 碎片整理成功
        moss::kernel::u64 pages_promoted_to_thp;       // 提升为THP的页面数
        moss::kernel::u64 pages_demoted_from_thp;      // 从THP降级的页面数
        double thp_utilization_ratio;                  // THP利用率
    };

protected:
    THPConfig config_;
    THPStats stats_;
    HugePagePool* pool_;
    moss::kernel::containers::AtomicBool manager_active_;
    moss::kernel::containers::AtomicU32 scan_generation_;

public:
    THPManager(const THPConfig& config, HugePagePool* pool) noexcept;

    // THP控制
    HugePagesVoidResult start_thp_manager() noexcept;
    void stop_thp_manager() noexcept;
    [[nodiscard]] bool is_thp_enabled() const noexcept;

    // THP分配
    [[nodiscard]] HugePagesResult<HugePageInfo*> allocate_transparent_huge_page(
        moss::kernel::usize size, numa_node_t preferred_node = NUMA_NO_NODE) noexcept;
    HugePagesVoidResult free_transparent_huge_page(HugePageInfo* thp) noexcept;

    // 页面提升和降级
    HugePagesVoidResult try_promote_to_huge_page(moss::kernel::VirtAddr addr, moss::kernel::usize size) noexcept;
    HugePagesVoidResult split_huge_page(HugePageInfo* huge_page) noexcept;
    HugePagesVoidResult collapse_to_huge_page(moss::kernel::VirtAddr addr, moss::kernel::usize size) noexcept;

    // 碎片整理
    HugePagesVoidResult defragment_memory() noexcept;
    [[nodiscard]] bool should_defragment() const noexcept;

    // 策略管理
    void set_thp_policy(THPPolicy policy) noexcept { config_.policy = policy; }
    [[nodiscard]] THPPolicy get_thp_policy() const noexcept { return config_.policy; }

    // 统计信息
    [[nodiscard]] THPStats get_stats() const noexcept { return stats_; }
    void reset_stats() noexcept;

private:
    HugePagesVoidResult khugepaged_daemon() noexcept;
    HugePagesVoidResult scan_for_thp_candidates() noexcept;
    [[nodiscard]] bool is_thp_candidate(moss::kernel::VirtAddr addr, moss::kernel::usize size) const noexcept;
    void update_thp_stats() noexcept;
};

// HugeTLB文件系统支持
class HugeTLBManager {
    friend class HugeTLBImpl;

public:
    struct HugeTLBConfig {
        moss::kernel::usize default_huge_page_size;    // 默认大页尺寸
        moss::kernel::usize max_huge_pages;            // 最大大页数量
        moss::kernel::usize reserve_huge_pages;        // 保留大页数量
        bool overcommit_enabled;                       // 启用超额分配
        bool shared_memory_enabled;                    // 启用共享内存支持
    };

    struct HugeTLBStats {
        moss::kernel::usize total_huge_pages;
        moss::kernel::usize free_huge_pages;
        moss::kernel::usize reserved_huge_pages;
        moss::kernel::usize surplus_huge_pages;        // 超额大页
        moss::kernel::usize shared_huge_pages;         // 共享大页
        moss::kernel::u64 huge_page_faults;           // 大页缺页异常
    };

protected:
    HugeTLBConfig config_;
    HugeTLBStats stats_;
    HugePagePool* pool_;

    // HugeTLB页面列表
    struct HugeTLBPage {
        HugePageInfo* huge_page;
        moss::kernel::containers::AtomicU32 ref_count;
        moss::kernel::containers::AtomicU32 flags;
        HugeTLBPage* next;

        enum Flags : u32 {
            RESERVED = (1 << 0),
            SHARED = (1 << 1),
            SURPLUS = (1 << 2)
        };
    };

    HugeTLBPage* free_huge_pages_;
    HugeTLBPage* reserved_huge_pages_;
    moss::kernel::containers::AtomicU32 total_huge_pages_;

public:
    HugeTLBManager(const HugeTLBConfig& config, HugePagePool* pool) noexcept;

    // HugeTLB管理
    HugePagesVoidResult initialize_hugetlb() noexcept;
    HugePagesVoidResult set_huge_page_pool_size(moss::kernel::usize count) noexcept;
    HugePagesVoidResult reserve_huge_pages(moss::kernel::usize count) noexcept;

    // 大页分配
    [[nodiscard]] HugePagesResult<HugePageInfo*> allocate_hugetlb_page(
        moss::kernel::usize size = HUGE_PAGE_2MB) noexcept;
    HugePagesVoidResult free_hugetlb_page(HugePageInfo* page) noexcept;

    // 共享内存支持
    [[nodiscard]] HugePagesResult<HugePageInfo*> allocate_shared_huge_page(moss::kernel::usize size) noexcept;
    HugePagesVoidResult attach_shared_huge_page(HugePageInfo* page) noexcept;
    HugePagesVoidResult detach_shared_huge_page(HugePageInfo* page) noexcept;

    // 状态查询
    [[nodiscard]] moss::kernel::usize get_free_huge_pages() const noexcept;
    [[nodiscard]] moss::kernel::usize get_total_huge_pages() const noexcept;
    [[nodiscard]] bool can_allocate_huge_page(moss::kernel::usize size) const noexcept;

    // 统计信息
    [[nodiscard]] HugeTLBStats get_stats() const noexcept { return stats_; }

private:
    HugePagesVoidResult add_huge_page_to_pool(HugePageInfo* page, bool is_reserved) noexcept;
    [[nodiscard]] HugeTLBPage* remove_huge_page_from_pool(bool prefer_surplus = false) noexcept;
    void update_surplus_pages() noexcept;
};

// 大页支持系统主管理器
class HugePagesManager {
public:
    struct HugePagesConfig {
        HugePagePool::PoolConfig pool_config;
        THPManager::THPConfig thp_config;
        HugeTLBManager::HugeTLBConfig hugetlb_config;
        bool enable_thp;                    // 启用透明大页
        bool enable_hugetlb;                // 启用HugeTLB
        moss::kernel::usize default_huge_page_size;
    };

    struct SystemStats {
        HugePagePool::PoolStats pool_stats;
        THPManager::THPStats thp_stats;
        HugeTLBManager::HugeTLBStats hugetlb_stats;
        moss::kernel::usize total_huge_memory;     // 大页总内存
        moss::kernel::usize available_huge_memory; // 可用大页内存
        double huge_page_efficiency;              // 大页效率
    };

protected:
    HugePagesConfig config_;
    HugePagePool pool_;
    THPManager thp_manager_;
    HugeTLBManager hugetlb_manager_;
    moss::kernel::containers::AtomicBool system_initialized_;

public:
    static HugePagesVoidResult initialize(const HugePagesConfig& config) noexcept;

    // 系统控制
    HugePagesVoidResult start_huge_pages_system() noexcept;
    void stop_huge_pages_system() noexcept;
    [[nodiscard]] bool is_huge_pages_enabled() const noexcept;

    // 大页分配接口 (统一入口)
    [[nodiscard]] HugePagesResult<moss::kernel::PhysAddr> allocate_huge_pages(
        moss::kernel::usize size, numa_node_t preferred_node = NUMA_NO_NODE) noexcept;
    HugePagesVoidResult free_huge_pages(moss::kernel::PhysAddr addr, moss::kernel::usize size) noexcept;

    // THP接口
    [[nodiscard]] HugePagesResult<moss::kernel::PhysAddr> allocate_thp(moss::kernel::usize size) noexcept;
    HugePagesVoidResult promote_to_thp(moss::kernel::VirtAddr addr, moss::kernel::usize size) noexcept;
    HugePagesVoidResult split_thp(moss::kernel::VirtAddr addr) noexcept;

    // HugeTLB接口
    [[nodiscard]] HugePagesResult<moss::kernel::PhysAddr> allocate_hugetlb(moss::kernel::usize size) noexcept;
    HugePagesVoidResult set_hugetlb_pool_size(moss::kernel::usize count) noexcept;

    // 大页尺寸工具函数
    [[nodiscard]] static HugePageSize get_huge_page_size_class(moss::kernel::usize size) noexcept;
    [[nodiscard]] static moss::kernel::usize get_huge_page_size(HugePageSize size_class) noexcept;
    [[nodiscard]] static bool is_huge_page_aligned(moss::kernel::usize addr, HugePageSize size_class) noexcept;

    // 统计和监控
    [[nodiscard]] SystemStats get_system_stats() const noexcept;
    [[nodiscard]] double get_huge_page_efficiency() const noexcept;
    [[nodiscard]] static HugePagesManager& get_instance() noexcept;

private:
    HugePagesManager(const HugePagesConfig& config) noexcept;

    // 单例实例
    static bool initialized_;
    static HugePagesManager* instance_;
};

// 全局便利接口
namespace huge_pages {
    // 初始化大页系统
    inline HugePagesVoidResult initialize(const HugePagesManager::HugePagesConfig& config) noexcept {
        return HugePagesManager::initialize(config);
    }

    // 大页分配
    inline HugePagesResult<moss::kernel::PhysAddr> alloc(moss::kernel::usize size) noexcept {
        return HugePagesManager::get_instance().allocate_huge_pages(size);
    }

    // 大页释放
    inline HugePagesVoidResult free(moss::kernel::PhysAddr addr, moss::kernel::usize size) noexcept {
        return HugePagesManager::get_instance().free_huge_pages(addr, size);
    }

    // THP分配
    inline HugePagesResult<moss::kernel::PhysAddr> alloc_thp(moss::kernel::usize size) noexcept {
        return HugePagesManager::get_instance().allocate_thp(size);
    }

    // 获取大页效率
    inline double get_efficiency() noexcept {
        return HugePagesManager::get_instance().get_huge_page_efficiency();
    }

    // 检查大页可用性
    inline bool is_available() noexcept {
        return HugePagesManager::get_instance().is_huge_pages_enabled();
    }

    // 大页尺寸工具
    inline bool is_huge_size(moss::kernel::usize size) noexcept {
        return size >= HUGE_PAGE_2MB && (size % HUGE_PAGE_2MB == 0 ||
                                        size % HUGE_PAGE_1GB == 0 ||
                                        size % HUGE_PAGE_16MB == 0 ||
                                        size % HUGE_PAGE_32MB == 0);
    }
}

} // namespace moss::kernel::mm
