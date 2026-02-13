#pragma once

// 运行时堆分配器 - 管理内核动态堆分配
// 基于虚拟内存管理，使用 PageFrameAllocator 作为物理页面后端

#include "core/types.hpp"
#include "core/result.hpp"
#include "page_frame_allocator.hpp"

namespace moss::kernel::mm {

// 堆分配错误类型
enum class HeapAllocError : u32 {
    OutOfMemory = 1,
    InvalidAddress = 2,
    InvalidSize = 3,
    InitializationFailed = 4,
    HeapCorruption = 5
};

// 堆分配结果类型
template<typename T>
using HeapAllocResult = moss::kernel::Result<T, HeapAllocError>;
using HeapAllocVoidResult = moss::kernel::Result<void, HeapAllocError>;

// 虚拟地址类型
using VirtAddr = moss::kernel::VirtAddr;

// 运行时堆分配器
class RuntimeHeapAllocator {
public:
    // 初始化堆分配器
    // heap_start: 堆起始虚拟地址
    // initial_size: 初始堆大小（字节）
    static HeapAllocVoidResult initialize_heap(VirtAddr heap_start, usize initial_size) noexcept;

    // 分配指定大小的内存块
    [[nodiscard]] static HeapAllocResult<void*> allocate(usize size) noexcept;

    // 分配对齐的内存块
    [[nodiscard]] static HeapAllocResult<void*> allocate_aligned(usize size, usize alignment) noexcept;

    // 释放内存块
    static HeapAllocVoidResult deallocate(void* ptr, usize size) noexcept;

    // 扩展堆空间
    static HeapAllocVoidResult expand_heap(usize additional_size) noexcept;

    // 收缩堆空间（释放未使用的尾部页面）
    static HeapAllocVoidResult shrink_heap() noexcept;

    // 堆统计信息
    struct HeapStats {
        usize total_heap_size;      // 总堆大小
        usize allocated_bytes;      // 已分配字节数
        usize free_bytes;           // 空闲字节数
        usize fragmentation_ratio;  // 碎片化比例 (0-100)
        usize largest_free_block;   // 最大空闲块大小
    };

    [[nodiscard]] static HeapStats get_heap_stats() noexcept;

    // 获取堆边界信息
    [[nodiscard]] static VirtAddr get_heap_start() noexcept { return heap_start_; }
    [[nodiscard]] static VirtAddr get_heap_end() noexcept { return heap_end_; }
    [[nodiscard]] static usize get_heap_size() noexcept {
        return static_cast<usize>(heap_end_ - heap_start_);
    }

private:
    // 空闲块结构 - 使用链表管理空闲内存块
    struct FreeBlock {
        usize size;             // 块大小（包括头部）
        FreeBlock* next;        // 下一个空闲块
        FreeBlock* prev;        // 上一个空闲块

        // 块魔数，用于检测堆损坏
        static constexpr u32 MAGIC = 0xDEADC0DE;
        u32 magic;

        // 构造函数
        FreeBlock(usize block_size) noexcept
            : size(block_size), next(nullptr), prev(nullptr), magic(MAGIC) {}

        // 验证块完整性
        [[nodiscard]] bool is_valid() const noexcept {
            return magic == MAGIC;
        }
    };

    // 已分配块头部 - 用于释放时确定块大小
    struct AllocatedBlock {
        usize size;             // 块大小（包括头部）
        u32 magic;              // 魔数

        static constexpr u32 MAGIC = 0xBEEFF00D;

        AllocatedBlock(usize block_size) noexcept
            : size(block_size), magic(MAGIC) {}

        [[nodiscard]] bool is_valid() const noexcept {
            return magic == MAGIC;
        }
    };

    // 静态数据成员
    static bool initialized_;
    static VirtAddr heap_start_;        // 堆起始虚拟地址
    static VirtAddr heap_end_;          // 堆结束虚拟地址
    static VirtAddr heap_limit_;        // 堆最大虚拟地址
    static FreeBlock* free_list_head_;  // 空闲块链表头
    static usize allocated_bytes_;      // 已分配字节统计
    static usize total_allocations_;    // 总分配次数统计

    // 内存块大小对齐
    static constexpr usize BLOCK_ALIGN = 16;  // 16字节对齐
    static constexpr usize MIN_BLOCK_SIZE = sizeof(FreeBlock);

    // 将大小对齐到指定边界
    [[nodiscard]] static constexpr usize align_size(usize size, usize alignment) noexcept {
        return (size + alignment - 1) & ~(alignment - 1);
    }

    // 私有辅助方法
    static HeapAllocVoidResult map_heap_pages(VirtAddr start, usize size) noexcept;
    static HeapAllocVoidResult unmap_heap_pages(VirtAddr start, usize size) noexcept;
    static FreeBlock* find_suitable_block(usize required_size) noexcept;
    static void split_block(FreeBlock* block, usize required_size) noexcept;
    static void merge_free_blocks() noexcept;
    static void add_to_free_list(FreeBlock* block) noexcept;
    static void remove_from_free_list(FreeBlock* block) noexcept;
    static bool is_heap_address(void* ptr) noexcept;

    // 调试和诊断
#ifdef DEBUG
    static void validate_heap() noexcept;
    static void dump_heap_layout() noexcept;
    static void check_block_corruption() noexcept;
#endif
};

} // namespace moss::kernel::mm
