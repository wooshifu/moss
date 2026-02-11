#pragma once

// Vmalloc虚拟内存分配器 - 高性能虚拟地址空间管理
// 基于红黑树的地址分配，支持延迟释放、NUMA亲和性和大内存分配

#include "../include/types.hpp"
#include "../include/result.hpp"
#include "../include/arch/arch_abstraction.hpp"
#include "../containers/atomic_types.hpp"
#include "../containers/per_cpu_data.hpp"
#include "../mm/buddy_allocator_v2.hpp"


namespace moss::kernel::mm {

// Vmalloc分配器错误类型
enum class VmallocError : u32 {
    OutOfMemory = 1,
    InvalidSize = 2,
    InvalidAddress = 3,
    AddressSpaceExhausted = 4,
    AlignmentError = 5,
    FragmentationSevere = 6,
    NUMANodeInvalid = 7
};

// Vmalloc分配结果类型
template<typename T>
using VmallocResult = moss::kernel::Result<T, VmallocError>;
using VmallocVoidResult = moss::kernel::Result<void, VmallocError>;

// 虚拟内存区域类型
enum class VmAreaType : u32 {
    NORMAL = 0,        // 普通分配
    DMA = 1,           // DMA一致性内存
    PERCPU = 2,        // Per-CPU数据
    MODULE = 3,        // 内核模块代码
    IOREMAP = 4,       // I/O内存映射
    STACK = 5          // 内核栈
};

// NUMA节点ID类型前向声明
using numa_node_t = moss::kernel::u32;
// NUMA_NO_NODE定义在numa_policy.hpp中 - 需要包含该文件

// 虚拟内存分配请求
struct VmallocRequest {
    moss::kernel::usize size;           // 分配大小
    moss::kernel::usize alignment;      // 对齐要求（必须是2的幂）
    VmAreaType type;                   // 区域类型
    numa_node_t numa_node;             // NUMA节点偏好

    // 分配标志
    enum Flags : u32 {
        NONE = 0,
        ATOMIC = (1 << 0),             // 原子分配，不能睡眠
        NOWAIT = (1 << 1),             // 不等待，立即返回
        HIGH_PRIORITY = (1 << 2),       // 高优先级分配
        ZERO_MEMORY = (1 << 3),        // 零初始化内存
        EXEC = (1 << 4)                // 可执行内存
    } flags;

    VmallocRequest(moss::kernel::usize sz, moss::kernel::usize align = 0,
                  VmAreaType t = VmAreaType::NORMAL, numa_node_t node = NUMA_NO_NODE,
                  Flags f = Flags::NONE) noexcept
        : size(sz), alignment(align == 0 ? moss::kernel::PAGE_SIZE : align),
          type(t), numa_node(node), flags(f) {}
};

// 红黑树颜色
enum class RBColor : u8 {
    RED = 0,
    BLACK = 1
};

// 虚拟内存区域 - 红黑树节点
struct VmArea {
    // 红黑树节点结构
    VmArea* parent;
    VmArea* left;
    VmArea* right;
    RBColor color;

    // 虚拟内存区域信息
    moss::kernel::VirtAddr start;       // 虚拟地址起始
    moss::kernel::usize size;           // 区域大小
    VmAreaType type;                   // 区域类型
    numa_node_t numa_node;             // 所属NUMA节点

    // 物理页面映射
    moss::kernel::PhysAddr* phys_pages; // 物理页面数组
    moss::kernel::usize page_count;     // 页面数量

    // 引用计数和状态
    moss::kernel::containers::AtomicU32 ref_count;
    moss::kernel::containers::AtomicU32 flags;

    // 区域标志
    enum Flags : u32 {
        MAPPED = (1 << 0),             // 已映射到页表
        LAZY_FREE = (1 << 1),          // 延迟释放
        DMA_COHERENT = (1 << 2),       // DMA一致性
        EXECUTABLE = (1 << 3),         // 可执行
        USER_ACCESSIBLE = (1 << 4)     // 用户可访问
    };

    // 性能统计
    moss::kernel::u64 alloc_time;      // 分配时间戳
    moss::kernel::u64 access_count;    // 访问计数

    VmArea(moss::kernel::VirtAddr addr, moss::kernel::usize sz, VmAreaType t, numa_node_t node) noexcept
        : parent(nullptr), left(nullptr), right(nullptr), color(RBColor::RED),
          start(addr), size(sz), type(t), numa_node(node),
          phys_pages(nullptr), page_count(0),
          ref_count(1), flags(0), alloc_time(0), access_count(0) {}
};

// 前向声明
class VirtualAddressSpaceImpl;

// 地址空间管理 - 红黑树实现
class VirtualAddressSpace {
    friend class VirtualAddressSpaceImpl;

protected:
    VmArea* root_;                     // 红黑树根节点
    moss::kernel::VirtAddr start_;     // 地址空间起始地址
    moss::kernel::VirtAddr end_;       // 地址空间结束地址
    moss::kernel::containers::AtomicSize allocated_size_;
    moss::kernel::containers::AtomicU32 area_count_;

public:
    VirtualAddressSpace(moss::kernel::VirtAddr start, moss::kernel::VirtAddr end) noexcept
        : root_(nullptr), start_(start), end_(end), allocated_size_(0), area_count_(0) {}

    // 分配虚拟地址区域
    [[nodiscard]] VmallocResult<VmArea*> allocate_area(moss::kernel::usize size,
                                                       moss::kernel::usize alignment) noexcept;

    // 释放虚拟地址区域
    VmallocVoidResult free_area(VmArea* area) noexcept;

    // 查找虚拟地址对应的区域
    [[nodiscard]] VmArea* find_area(moss::kernel::VirtAddr addr) noexcept;

    // 获取统计信息
    [[nodiscard]] moss::kernel::usize get_allocated_size() const noexcept {
        return allocated_size_.load(moss::MemoryOrder::Relaxed);
    }

    [[nodiscard]] moss::kernel::u32 get_area_count() const noexcept {
        return area_count_.load(moss::MemoryOrder::Relaxed);
    }

private:
    // 红黑树操作
    void rb_insert(VmArea* area) noexcept;
    void rb_delete(VmArea* area) noexcept;
    void rb_insert_fixup(VmArea* area) noexcept;
    void rb_delete_fixup(VmArea* area) noexcept;
    void rb_rotate_left(VmArea* area) noexcept;
    void rb_rotate_right(VmArea* area) noexcept;
    [[nodiscard]] VmArea* rb_minimum(VmArea* area) noexcept;
    [[nodiscard]] VmArea* rb_successor(VmArea* area) noexcept;

    // 地址分配算法
    [[nodiscard]] moss::kernel::VirtAddr find_free_area(moss::kernel::usize size,
                                                        moss::kernel::usize alignment) noexcept;
    [[nodiscard]] bool is_area_free(moss::kernel::VirtAddr start, moss::kernel::usize size) noexcept;
};

// 前向声明
class LazyFreeManagerImpl;

// 延迟释放管理器 - RCU风格的安全释放
class LazyFreeManager {
    friend class LazyFreeManagerImpl;

public:
    struct FreeEntry {
        VmArea* area;
        moss::kernel::u64 timestamp;
        FreeEntry* next;
    };

    // Per-CPU延迟释放队列
    struct PerCpuFreeList {
        FreeEntry* head;
        moss::kernel::containers::AtomicU32 count;
        moss::kernel::u64 last_cleanup;
    };

    // 延迟释放参数
    static constexpr moss::kernel::u64 LAZY_FREE_DELAY_MS = 1000;  // 1秒延迟
    static constexpr moss::kernel::u32 MAX_PENDING_FREES = 1024;   // 最大待释放数量

protected:
    moss::kernel::containers::PerCpuData<PerCpuFreeList> per_cpu_lists_;
    moss::kernel::containers::AtomicU64 global_timestamp_;

public:
    LazyFreeManager() noexcept : per_cpu_lists_{}, global_timestamp_(0) {}

    // 添加到延迟释放队列
    VmallocVoidResult add_lazy_free(VmArea* area) noexcept;

    // 处理延迟释放（定期调用）
    void process_lazy_frees() noexcept;

    // 强制释放所有待释放项（关机时调用）
    void flush_all_lazy_frees() noexcept;

private:
    void process_cpu_free_list(moss::kernel::u32 cpu_id) noexcept;
    bool is_safe_to_free(moss::kernel::u64 timestamp) noexcept;
    void free_area_immediate(VmArea* area) noexcept;
};

// 高性能Vmalloc分配器
class VmallocAllocator {
public:
    // Vmalloc配置结构
    struct VmallocConfig {
        bool enable_lazy_free;                       // 启用延迟释放
        moss::kernel::usize lazy_free_threshold;     // 延迟释放阈值
        moss::kernel::usize max_lazy_free_memory;    // 最大延迟释放内存
        bool enable_numa_awareness;                  // 启用NUMA感知
        moss::kernel::u32 default_numa_policy;      // 默认NUMA策略
    };

    // Vmalloc分配统计
    struct AllocationStats {
        moss::kernel::u64 total_allocations;        // 总分配次数
        moss::kernel::u64 failed_allocations;       // 失败分配次数
        moss::kernel::u64 total_allocated_bytes;    // 总分配字节数
        moss::kernel::u64 total_freed_bytes;        // 总释放字节数
        moss::kernel::u64 peak_allocated_memory;    // 峰值分配内存
        moss::kernel::u64 avg_allocation_latency_us; // 平均分配延迟
        double fragmentation_ratio;                 // 碎片化比例
        moss::kernel::u32 active_areas;             // 活跃区域数
    };

    // 初始化Vmalloc分配器
    static VmallocVoidResult initialize() noexcept;

    // 主分配接口
    [[nodiscard]] static VmallocResult<void*> vmalloc(const VmallocRequest& request) noexcept;

    // 简化接口
    [[nodiscard]] static VmallocResult<void*> vmalloc(moss::kernel::usize size) noexcept {
        return vmalloc(VmallocRequest(size));
    }

    // 类型化分配
    template<typename T>
    [[nodiscard]] static VmallocResult<T*> vmalloc_typed(moss::kernel::usize count = 1) noexcept {
        auto result = vmalloc(VmallocRequest(sizeof(T) * count, alignof(T)));
        if (!result) {
            return VmallocResult<T*>{result.error()};
        }
        return VmallocResult<T*>{static_cast<T*>(*result)};
    }

    // 释放虚拟内存
    static VmallocVoidResult vfree(void* ptr) noexcept;

    // NUMA感知分配
    [[nodiscard]] static VmallocResult<void*> vmalloc_numa(moss::kernel::usize size,
                                                           numa_node_t node) noexcept {
        return vmalloc(VmallocRequest(size, moss::kernel::PAGE_SIZE, VmAreaType::NORMAL, node));
    }

    // DMA一致性内存分配
    [[nodiscard]] static VmallocResult<void*> vmalloc_dma(moss::kernel::usize size,
                                                          moss::kernel::usize alignment = moss::kernel::PAGE_SIZE) noexcept {
        VmallocRequest request(size, alignment, VmAreaType::DMA);
        request.flags = static_cast<VmallocRequest::Flags>(
            request.flags | VmallocRequest::Flags::ZERO_MEMORY);
        return vmalloc(request);
    }

    // 获取虚拟地址对应的物理地址
    [[nodiscard]] static VmallocResult<moss::kernel::PhysAddr> virt_to_phys(void* virt_addr) noexcept;

    // 内存统计信息
    struct VmallocStats {
        moss::kernel::usize total_allocated;     // 总分配大小
        moss::kernel::usize total_free;          // 总空闲大小
        moss::kernel::u32 active_areas;          // 活跃区域数
        moss::kernel::u32 lazy_free_pending;     // 待延迟释放数
        moss::kernel::u64 allocation_count;      // 分配次数
        moss::kernel::u64 free_count;            // 释放次数
        double fragmentation_ratio;             // 碎片率

        // 按类型统计
        moss::kernel::usize size_by_type[static_cast<u32>(VmAreaType::STACK) + 1];

        // 按NUMA节点统计
        moss::kernel::usize size_by_numa_node[16];  // 支持最多16个NUMA节点
    };

    [[nodiscard]] static VmallocStats get_statistics() noexcept;

    // 内存压缩 - 减少碎片化
    static VmallocVoidResult compact_address_space() noexcept;

    // 预分配地址空间 - 优化性能
    static VmallocVoidResult preallocate_areas(moss::kernel::usize total_size,
                                              moss::kernel::usize area_size) noexcept;

private:
    // 静态数据成员
    static bool initialized_;
    static VirtualAddressSpace* address_space_;
    static LazyFreeManager* lazy_free_manager_;

    // Vmalloc地址空间范围
    static constexpr moss::kernel::VirtAddr VMALLOC_START = 0xFFFFC00000000000UL;  // -16TB
    static constexpr moss::kernel::VirtAddr VMALLOC_END   = 0xFFFFE00000000000UL;  // -8TB
    static constexpr moss::kernel::usize VMALLOC_SIZE = VMALLOC_END - VMALLOC_START;

    // 统计计数器
    static moss::kernel::containers::AtomicU64 allocation_count_;
    static moss::kernel::containers::AtomicU64 free_count_;
    static moss::kernel::containers::AtomicSize total_allocated_;

    // 分配和映射
    static VmallocResult<VmArea*> allocate_vm_area(const VmallocRequest& request) noexcept;
    static VmallocVoidResult map_vm_area(VmArea* area, const VmallocRequest& request) noexcept;
    static VmallocVoidResult unmap_vm_area(VmArea* area) noexcept;

    // 物理页面分配 - 考虑NUMA亲和性
    static VmallocResult<moss::kernel::PhysAddr*> allocate_physical_pages(moss::kernel::usize page_count,
                                                                          numa_node_t numa_node) noexcept;
    static VmallocVoidResult free_physical_pages(moss::kernel::PhysAddr* pages,
                                                 moss::kernel::usize page_count) noexcept;

    // NUMA节点管理
    static numa_node_t select_optimal_numa_node(const VmallocRequest& request) noexcept;
    static bool is_numa_node_available(numa_node_t node) noexcept;

    // 页表管理
    static VmallocVoidResult map_pages_to_area(VmArea* area, moss::kernel::PhysAddr* phys_pages) noexcept;
    static VmallocVoidResult unmap_pages_from_area(VmArea* area) noexcept;

    // 内存管理优化
    static void update_statistics(const VmallocRequest& request, VmArea* area) noexcept;
    static void trigger_memory_compaction_if_needed() noexcept;
    static bool should_use_lazy_free(const VmArea* area) noexcept;

    // 调试和监控
    #ifdef DEBUG
    static void validate_address_space_integrity() noexcept;
    static void dump_address_space_layout() noexcept;
    #endif
};

// 全局便利函数
namespace vmalloc {
    // 标准分配
    [[nodiscard]] inline VmallocResult<void*> alloc(moss::kernel::usize size) noexcept {
        return VmallocAllocator::vmalloc(size);
    }

    // 零初始化分配
    [[nodiscard]] inline VmallocResult<void*> zalloc(moss::kernel::usize size) noexcept {
        VmallocRequest request(size);
        request.flags = VmallocRequest::Flags::ZERO_MEMORY;
        return VmallocAllocator::vmalloc(request);
    }

    // 对齐分配
    [[nodiscard]] inline VmallocResult<void*> alloc_aligned(moss::kernel::usize size,
                                                            moss::kernel::usize alignment) noexcept {
        return VmallocAllocator::vmalloc(VmallocRequest(size, alignment));
    }

    // 释放
    inline VmallocVoidResult free(void* ptr) noexcept {
        return VmallocAllocator::vfree(ptr);
    }

    // 类型化便利函数
    template<typename T>
    [[nodiscard]] inline VmallocResult<T*> alloc_array(moss::kernel::usize count) noexcept {
        return VmallocAllocator::vmalloc_typed<T>(count);
    }

    template<typename T>
    [[nodiscard]] inline VmallocResult<T*> zalloc_array(moss::kernel::usize count) noexcept {
        VmallocRequest request(sizeof(T) * count, alignof(T));
        request.flags = VmallocRequest::Flags::ZERO_MEMORY;
        auto result = VmallocAllocator::vmalloc(request);
        if (!result) {
            return VmallocResult<T*>{result.error()};
        }
        return VmallocResult<T*>{static_cast<T*>(*result)};
    }
}

} // namespace moss::kernel::mm
