#pragma once

// 物理页面分配器 - Buddy算法实现
// 管理内核可用的物理内存页面，支持4KB到2MB的分配

#include "../../../include/types.hpp"
#include "../../../include/result.hpp"
#include "containers/atomic_types.hpp"

namespace moss::kernel::mm {

// 页面分配器错误类型
enum class PageAllocError : u32 {
    OutOfMemory = 1,
    InvalidOrder = 2,
    InvalidAddress = 3,
    InitializationFailed = 4
};

// 页面分配结果类型
template<typename T>
using PageAllocResult = moss::kernel::Result<T, PageAllocError>;
using PageAllocVoidResult = moss::kernel::Result<void, PageAllocError>;

// 物理地址类型定义
using PhysAddr = moss::kernel::PhysAddr;

// 页面大小常量
constexpr usize PAGE_SIZE = 4096;  // 4KB
constexpr usize PAGE_SHIFT = 12;   // log2(PAGE_SIZE)

// Buddy算法最大阶数 (支持最大4MB分配 = 4KB * 2^10 = 4MB)
constexpr usize MAX_ORDER = 10;

// 将地址转换为页面号
constexpr usize addr_to_page(PhysAddr addr) noexcept {
    return static_cast<usize>(addr) >> PAGE_SHIFT;
}

// 将页面号转换为地址
constexpr PhysAddr page_to_addr(usize page) noexcept {
    return static_cast<PhysAddr>(page << PAGE_SHIFT);
}

// Buddy算法页面分配器
class PageFrameAllocator {
public:
    // 初始化页面分配器
    // 解析内核内存布局，设置可用内存区域
    static PageAllocVoidResult initialize() noexcept;

    // 分配连续的物理页面
    // order: 分配阶数 (0=4KB, 1=8KB, 2=16KB, ..., 10=4MB)
    [[nodiscard]] static PageAllocResult<PhysAddr> allocate_pages(usize order) noexcept;

    // 释放物理页面
    // addr: 页面起始地址，必须是分配时返回的地址
    // order: 分配时使用的阶数
    static PageAllocVoidResult free_pages(PhysAddr addr, usize order) noexcept;

    // 获取系统内存统计信息
    struct MemoryStats {
        usize total_pages;      // 总页面数
        usize free_pages;       // 空闲页面数
        usize used_pages;       // 已使用页面数
        usize kernel_pages;     // 内核占用页面数
    };

    [[nodiscard]] static MemoryStats get_memory_stats() noexcept;

private:
    // 内存区域描述
    struct MemoryRegion {
        PhysAddr start_addr;    // 起始物理地址
        usize page_count;       // 页面数量
        MemoryRegion* next;     // 链表指针
    };

    // Buddy空闲列表节点
    struct FreeBlock {
        FreeBlock* next;        // 链表指针
        FreeBlock* prev;        // 双向链表
        usize order;            // 块的阶数
    };

    // 页面元数据
    struct PageMetadata {
        moss::kernel::containers::AtomicU32 ref_count;  // 引用计数
        moss::kernel::containers::AtomicU32 flags;      // 页面标志
        FreeBlock* free_list_node;  // 空闲列表节点（当页面空闲时）
    };

    // 静态数据成员
    static bool initialized_;
    static MemoryRegion* memory_regions_;  // 可用内存区域链表
    static FreeBlock* free_lists_[MAX_ORDER + 1];  // 每个阶数的空闲列表
    static PageMetadata* page_metadata_;   // 页面元数据数组
    static usize total_pages_;
    static moss::kernel::containers::AtomicSize free_pages_;
    static moss::kernel::containers::AtomicSize used_pages_;

    // 私有辅助方法
    static PageAllocVoidResult parse_memory_layout() noexcept;
    static void initialize_free_lists() noexcept;
    static FreeBlock* find_buddy(FreeBlock* block, usize order) noexcept;
    static void merge_buddies(FreeBlock* block, usize order) noexcept;
    static void split_block(FreeBlock* block, usize order) noexcept;
    static void add_to_free_list(FreeBlock* block, usize order) noexcept;
    static FreeBlock* remove_from_free_list(usize order) noexcept;
    static bool is_valid_page_address(PhysAddr addr) noexcept;

    // 调试和诊断
#ifdef DEBUG
    static void validate_free_lists() noexcept;
    static void dump_memory_layout() noexcept;
#endif
};

} // namespace moss::kernel::mm
