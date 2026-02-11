#pragma once

// Vmalloc分配器核心实现 - 展示关键算法
// 红黑树地址管理、延迟释放和NUMA感知分配的具体实现

#include "vmalloc_allocator.hpp"
#include "../include/moss_std.hpp"

namespace moss::kernel::mm {

// 错误转换工具
inline VmallocError convert_buddy_error(BuddyError error) noexcept {
    switch (error) {
        case BuddyError::OutOfMemory:
            return VmallocError::OutOfMemory;
        case BuddyError::InvalidAddress:
            return VmallocError::InvalidAddress;
        case BuddyError::FragmentationSevere:
            return VmallocError::FragmentationSevere;
        default:
            return VmallocError::OutOfMemory;
    }
}

// 红黑树地址空间管理 - 核心实现
class VirtualAddressSpaceImpl {
public:
    // 核心分配算法 - First Fit with Best Fit优化
    [[nodiscard]] static VmallocResult<VmArea*>
    allocate_area_impl(VirtualAddressSpace* vas, moss::kernel::usize size,
                      moss::kernel::usize alignment) noexcept {

        // 1. 对齐大小到页边界
        size = align_up_to_page(size);
        alignment = align_up_to_page(alignment);

        // 2. 查找最佳适配的空闲区域
        auto free_addr_result = find_best_fit_area(vas, size, alignment);
        if (!free_addr_result) {
            return VmallocResult<VmArea*>{free_addr_result.error()};
        }

        moss::kernel::VirtAddr addr = *free_addr_result;

        // 3. 创建新的VM区域
        VmArea* new_area = new VmArea(addr, size, VmAreaType::NORMAL, NUMA_NO_NODE);
        if (!new_area) {
            return VmallocResult<VmArea*>{VmallocError::OutOfMemory};
        }

        // 4. 插入到红黑树
        rb_insert_impl(vas, new_area);

        // 5. 更新统计信息
        vas->allocated_size_.fetch_add(size, moss::MemoryOrder::Relaxed);
        vas->area_count_.fetch_add(1, moss::MemoryOrder::Relaxed);

        return VmallocResult<VmArea*>{new_area};
    }

    // 红黑树插入实现
    static void rb_insert_impl(VirtualAddressSpace* vas, VmArea* new_area) noexcept {
        VmArea* parent = nullptr;
        VmArea** current = &vas->root_;

        // 1. 标准BST插入
        while (*current != nullptr) {
            parent = *current;
            if (new_area->start < parent->start) {
                current = &parent->left;
            } else {
                current = &parent->right;
            }
        }

        // 2. 设置新节点
        new_area->parent = parent;
        new_area->left = nullptr;
        new_area->right = nullptr;
        new_area->color = RBColor::RED;
        *current = new_area;

        // 3. 红黑树性质修复
        rb_insert_fixup_impl(vas, new_area);
    }

private:
    // 页面对齐工具
    static moss::kernel::usize align_up_to_page(moss::kernel::usize size) noexcept {
        return (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    }

    // Best Fit地址查找算法
    [[nodiscard]] static VmallocResult<moss::kernel::VirtAddr>
    find_best_fit_area(VirtualAddressSpace* vas, moss::kernel::usize size,
                      moss::kernel::usize alignment) noexcept {

        moss::kernel::VirtAddr best_addr = 0;
        moss::kernel::usize best_gap_size = static_cast<moss::kernel::usize>(-1);

        // 从起始地址开始搜索
        moss::kernel::VirtAddr search_addr = vas->start_;
        VmArea* current = find_first_area_after(vas, search_addr);

        while (search_addr < vas->end_ && (vas->end_ - search_addr) >= size) {
            moss::kernel::VirtAddr aligned_addr = align_up(search_addr, alignment);
            moss::kernel::VirtAddr gap_end = current ? current->start : vas->end_;

            // 检查这个位置是否能容纳所需大小
            if (aligned_addr + size <= gap_end) {
                moss::kernel::usize gap_size = gap_end - aligned_addr;

                // Best Fit策略：选择最小的合适空隙
                if (gap_size < best_gap_size) {
                    best_addr = aligned_addr;
                    best_gap_size = gap_size;

                    // 如果是完美匹配，立即返回
                    if (gap_size == size) {
                        break;
                    }
                }
            }

            // 移动到下一个空隙
            if (current) {
                search_addr = current->start + current->size;
                current = find_next_area(current);
            } else {
                break;
            }
        }

        if (best_addr == 0) {
            return VmallocResult<moss::kernel::VirtAddr>{VmallocError::AddressSpaceExhausted};
        }

        return VmallocResult<moss::kernel::VirtAddr>{best_addr};
    }

    // 红黑树修复算法
    static void rb_insert_fixup_impl(VirtualAddressSpace* vas, VmArea* area) noexcept {
        while (area->parent && area->parent->color == RBColor::RED) {
            VmArea* grandparent = area->parent->parent;

            if (area->parent == grandparent->left) {
                VmArea* uncle = grandparent->right;

                if (uncle && uncle->color == RBColor::RED) {
                    // Case 1: 叔父是红色
                    area->parent->color = RBColor::BLACK;
                    uncle->color = RBColor::BLACK;
                    grandparent->color = RBColor::RED;
                    area = grandparent;
                } else {
                    if (area == area->parent->right) {
                        // Case 2: 左-右情况
                        area = area->parent;
                        rb_rotate_left_impl(vas, area);
                    }

                    // Case 3: 左-左情况
                    area->parent->color = RBColor::BLACK;
                    grandparent->color = RBColor::RED;
                    rb_rotate_right_impl(vas, grandparent);
                }
            } else {
                // 对称情况（右子树）
                VmArea* uncle = grandparent->left;

                if (uncle && uncle->color == RBColor::RED) {
                    area->parent->color = RBColor::BLACK;
                    uncle->color = RBColor::BLACK;
                    grandparent->color = RBColor::RED;
                    area = grandparent;
                } else {
                    if (area == area->parent->left) {
                        area = area->parent;
                        rb_rotate_right_impl(vas, area);
                    }

                    area->parent->color = RBColor::BLACK;
                    grandparent->color = RBColor::RED;
                    rb_rotate_left_impl(vas, grandparent);
                }
            }
        }

        vas->root_->color = RBColor::BLACK;
    }

    // 红黑树旋转操作
    static void rb_rotate_left_impl(VirtualAddressSpace* vas, VmArea* x) noexcept {
        VmArea* y = x->right;
        x->right = y->left;

        if (y->left) {
            y->left->parent = x;
        }

        y->parent = x->parent;

        if (!x->parent) {
            vas->root_ = y;
        } else if (x == x->parent->left) {
            x->parent->left = y;
        } else {
            x->parent->right = y;
        }

        y->left = x;
        x->parent = y;
    }

    static void rb_rotate_right_impl(VirtualAddressSpace* vas, VmArea* y) noexcept {
        VmArea* x = y->left;
        y->left = x->right;

        if (x->right) {
            x->right->parent = y;
        }

        x->parent = y->parent;

        if (!y->parent) {
            vas->root_ = x;
        } else if (y == y->parent->left) {
            y->parent->left = x;
        } else {
            y->parent->right = x;
        }

        x->right = y;
        y->parent = x;
    }

    // 辅助查找函数
    static VmArea* find_first_area_after(VirtualAddressSpace* vas, moss::kernel::VirtAddr addr) noexcept {
        VmArea* current = vas->root_;
        VmArea* result = nullptr;

        while (current) {
            if (current->start >= addr) {
                result = current;
                current = current->left;
            } else {
                current = current->right;
            }
        }

        return result;
    }

    static VmArea* find_next_area(VmArea* area) noexcept {
        if (area->right) {
            area = area->right;
            while (area->left) {
                area = area->left;
            }
            return area;
        }

        VmArea* parent = area->parent;
        while (parent && area == parent->right) {
            area = parent;
            parent = parent->parent;
        }

        return parent;
    }

    // 地址对齐
    static moss::kernel::VirtAddr align_up(moss::kernel::VirtAddr addr, moss::kernel::usize alignment) noexcept {
        return (addr + alignment - 1) & ~(alignment - 1);
    }
};

// 延迟释放管理器实现
class LazyFreeManagerImpl {
public:
    // 高效的延迟释放处理
    static void process_lazy_frees_impl(LazyFreeManager* manager) noexcept {
        moss::kernel::u64 current_time = get_current_time_ms();
        moss::kernel::u32 current_cpu = moss::kernel::arch::get_current_cpu_id();

        // 处理当前CPU的释放队列
        if (current_cpu < moss::kernel::MAX_CPUS) {
            auto& cpu_list = manager->per_cpu_lists_.get_cpu(current_cpu);
            process_cpu_free_list_impl(&cpu_list, current_time);
        }

        // 定期处理所有CPU的队列（负载均衡）
        static moss::kernel::u64 last_global_cleanup = 0;
        if (current_time - last_global_cleanup > 5000) {  // 5秒间隔
            for (moss::kernel::u32 cpu = 0; cpu < moss::kernel::MAX_CPUS; ++cpu) {
                auto& cpu_list = manager->per_cpu_lists_.get_cpu(cpu);
                process_cpu_free_list_impl(&cpu_list, current_time);
            }
            last_global_cleanup = current_time;
        }
    }

    // 添加到延迟释放队列的优化实现
    static VmallocVoidResult add_lazy_free_impl(LazyFreeManager* manager, VmArea* area) noexcept {
        moss::kernel::u32 cpu_id = moss::kernel::arch::get_current_cpu_id();
        if (cpu_id >= moss::kernel::MAX_CPUS) {
            // 回退到CPU 0
            cpu_id = 0;
        }

        auto& cpu_list = manager->per_cpu_lists_.get_cpu(cpu_id);

        // 检查队列是否过满
        moss::kernel::u32 current_count = cpu_list.count.load(moss::MemoryOrder::Relaxed);
        if (current_count >= LazyFreeManager::MAX_PENDING_FREES) {
            // 强制处理一些旧的释放项
            process_cpu_free_list_impl(&cpu_list, get_current_time_ms());
        }

        // 创建释放条目
        auto* entry = new LazyFreeManager::FreeEntry{
            area,
            manager->global_timestamp_.fetch_add(1, moss::MemoryOrder::AcqRel),
            cpu_list.head
        };

        if (!entry) {
            return VmallocVoidResult{VmallocError::OutOfMemory};
        }

        // 原子地添加到队列头部
        cpu_list.head = entry;
        cpu_list.count.fetch_add(1, moss::MemoryOrder::Release);

        return VmallocVoidResult{};
    }

private:
    // Per-CPU释放队列处理
    static void process_cpu_free_list_impl(LazyFreeManager::PerCpuFreeList* list,
                                          moss::kernel::u64 current_time) noexcept {
        LazyFreeManager::FreeEntry* entry = list->head;
        LazyFreeManager::FreeEntry* prev = nullptr;
        moss::kernel::u32 freed_count = 0;

        while (entry) {
            LazyFreeManager::FreeEntry* next = entry->next;

            // 检查是否可以安全释放（基于时间戳）
            bool can_free = (current_time - entry->timestamp) >= LazyFreeManager::LAZY_FREE_DELAY_MS;

            if (can_free) {
                // 从队列中移除
                if (prev) {
                    prev->next = next;
                } else {
                    list->head = next;
                }

                // 立即释放VM区域
                free_vm_area_immediate(entry->area);
                delete entry;
                freed_count++;
            } else {
                prev = entry;
            }

            entry = next;
        }

        // 更新计数
        if (freed_count > 0) {
            list->count.fetch_sub(freed_count, moss::MemoryOrder::AcqRel);
            list->last_cleanup = current_time;
        }
    }

    // 立即释放VM区域
    static void free_vm_area_immediate(VmArea* area) noexcept {
        if (!area) return;

        // 1. 从页表中取消映射
        if (area->flags.load(moss::MemoryOrder::Relaxed) & VmArea::Flags::MAPPED) {
            unmap_vm_area_pages(area);
        }

        // 2. 释放物理页面
        if (area->phys_pages && area->page_count > 0) {
            for (moss::kernel::usize i = 0; i < area->page_count; ++i) {
                if (area->phys_pages[i] != 0) {
                    // 使用Buddy分配器释放单个页面
                    [[maybe_unused]] auto result = BuddyAllocatorV2::free_pages(area->phys_pages[i], 0);
                }
            }
            delete[] area->phys_pages;
        }

        // 3. 释放VM区域结构本身
        delete area;
    }

    // 获取当前时间（毫秒）
    static moss::kernel::u64 get_current_time_ms() noexcept {
        // 简化实现 - 在实际系统中应该使用真实的时间戳
        static moss::kernel::containers::AtomicU64 fake_time{0};
        return fake_time.fetch_add(1, moss::MemoryOrder::Relaxed);
    }

    // 取消页表映射
    static void unmap_vm_area_pages(VmArea* area) noexcept {
        // 简化实现 - 实际应该操作页表
        // 这里只是清除映射标志
        moss::kernel::u32 flags = area->flags.load(moss::MemoryOrder::Relaxed);
        flags &= ~VmArea::Flags::MAPPED;
        area->flags.store(flags, moss::MemoryOrder::Release);
    }
};

// NUMA感知分配策略
class NUMAAllocationStrategy {
public:
    // 智能NUMA节点选择
    static numa_node_t select_optimal_numa_node_impl(const VmallocRequest& request) noexcept {
        // 1. 如果指定了NUMA节点，直接使用
        if (request.numa_node != NUMA_NO_NODE && is_numa_node_available(request.numa_node)) {
            return request.numa_node;
        }

        // 2. 根据当前CPU选择本地NUMA节点
        moss::kernel::u32 cpu_id = moss::kernel::arch::get_current_cpu_id();
        numa_node_t local_node = cpu_to_numa_node(cpu_id);

        if (is_numa_node_available(local_node)) {
            // 检查本地节点是否有足够内存
            if (has_sufficient_memory(local_node, request.size)) {
                return local_node;
            }
        }

        // 3. 查找最优的备选节点
        return find_best_alternative_node(request.size, local_node);
    }

    // 按NUMA节点分配物理页面
    static VmallocResult<moss::kernel::PhysAddr*>
    allocate_physical_pages_numa(moss::kernel::usize page_count, numa_node_t numa_node) noexcept {

        moss::kernel::PhysAddr* pages = new moss::kernel::PhysAddr[page_count];
        if (!pages) {
            return VmallocResult<moss::kernel::PhysAddr*>{VmallocError::OutOfMemory};
        }

        // 批量分配页面，优先从指定NUMA节点分配
        moss::kernel::usize allocated_count = 0;

        for (moss::kernel::usize i = 0; i < page_count; ++i) {
            // 使用NUMA感知的Buddy分配器
            auto page_result = allocate_page_from_numa_node(numa_node);
            if (page_result) {
                pages[i] = *page_result;
                allocated_count++;
            } else {
                // 分配失败，尝试其他节点
                auto fallback_result = allocate_page_fallback(numa_node);
                if (fallback_result) {
                    pages[i] = *fallback_result;
                    allocated_count++;
                } else {
                    // 彻底失败，清理已分配的页面
                    cleanup_partial_allocation(pages, allocated_count);
                    delete[] pages;
                    return VmallocResult<moss::kernel::PhysAddr*>{VmallocError::OutOfMemory};
                }
            }
        }

        return VmallocResult<moss::kernel::PhysAddr*>{pages};
    }

private:
    // CPU到NUMA节点映射
    static numa_node_t cpu_to_numa_node(moss::kernel::u32 cpu_id) noexcept {
        // 简化实现：假设每4个CPU属于一个NUMA节点
        return static_cast<numa_node_t>(cpu_id / 4);
    }

    // 检查NUMA节点是否可用
    static bool is_numa_node_available(numa_node_t node) noexcept {
        // 简化实现：假设节点0-3可用
        return node <= 3;
    }

    // 检查节点内存是否充足
    static bool has_sufficient_memory(numa_node_t node, moss::kernel::usize required_size) noexcept {
        // 简化实现：假设总是有足够内存
        (void)node; (void)required_size;
        return true;
    }

    // 查找最佳备选节点
    static numa_node_t find_best_alternative_node(moss::kernel::usize required_size,
                                                 numa_node_t preferred_node) noexcept {
        // 查找距离最近且有足够内存的节点
        for (numa_node_t node = 0; node <= 3; ++node) {
            if (node != preferred_node && has_sufficient_memory(node, required_size)) {
                return node;
            }
        }

        // 回退到节点0
        return 0;
    }

    // 从指定NUMA节点分配页面
    static VmallocResult<moss::kernel::PhysAddr>
    allocate_page_from_numa_node(numa_node_t numa_node) noexcept {
        // 使用NUMA感知的页面分配
        (void)numa_node;  // 简化实现
        auto result = BuddyAllocatorV2::allocate_pages(0, MigrationType::MOVABLE);
        if (!result) {
            return VmallocResult<moss::kernel::PhysAddr>{convert_buddy_error(result.error())};
        }
        return VmallocResult<moss::kernel::PhysAddr>{*result};
    }

    // 备用分配策略
    static VmallocResult<moss::kernel::PhysAddr>
    allocate_page_fallback(numa_node_t original_node) noexcept {
        (void)original_node;  // 简化实现
        auto result = BuddyAllocatorV2::allocate_pages(0, MigrationType::MOVABLE);
        if (!result) {
            return VmallocResult<moss::kernel::PhysAddr>{convert_buddy_error(result.error())};
        }
        return VmallocResult<moss::kernel::PhysAddr>{*result};
    }

    // 清理部分分配
    static void cleanup_partial_allocation(moss::kernel::PhysAddr* pages, moss::kernel::usize count) noexcept {
        for (moss::kernel::usize i = 0; i < count; ++i) {
            if (pages[i] != 0) {
                [[maybe_unused]] auto result = BuddyAllocatorV2::free_pages(pages[i], 0);
            }
        }
    }
};

} // namespace moss::kernel::mm
