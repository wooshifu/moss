// 物理页面分配器实现 - Buddy算法
// 为内核提供可靠的物理页面分配和释放功能
// Module implementation unit

module;

// Linker symbols (must be in global module fragment)
extern "C" {
    extern char _kernel_end_addr[];
    extern char _heap_start_addr[];
    extern char _heap_end_addr[];
}

module moss.mm;

namespace moss::kernel::mm {

// 静态成员定义
bool PageFrameAllocator::initialized_ = false;
PageFrameAllocator::MemoryRegion* PageFrameAllocator::memory_regions_ = nullptr;
PageFrameAllocator::FreeBlock* PageFrameAllocator::free_lists_[MAX_ORDER + 1] = {nullptr};
PageFrameAllocator::PageMetadata* PageFrameAllocator::page_metadata_ = nullptr;
usize PageFrameAllocator::total_pages_ = 0;
moss::kernel::containers::AtomicSize PageFrameAllocator::free_pages_{0};
moss::kernel::containers::AtomicSize PageFrameAllocator::used_pages_{0};

// 初始化页面分配器
PageAllocVoidResult PageFrameAllocator::initialize() noexcept {
    if (initialized_) {
        return PageAllocVoidResult{};
    }

    // 解析内核内存布局
    auto parse_result = parse_memory_layout();
    if (!parse_result) {
        return PageAllocVoidResult{parse_result.error()};
    }

    // 初始化空闲列表
    initialize_free_lists();

    initialized_ = true;

#ifdef DEBUG
    dump_memory_layout();
#endif

    return PageAllocVoidResult{};
}

// 分配物理页面
PageAllocResult<PhysAddr> PageFrameAllocator::allocate_pages(usize order) noexcept {
    if (!initialized_) {
        return PageAllocResult<PhysAddr>{PageAllocError::InitializationFailed};
    }

    if (order > MAX_ORDER) {
        return PageAllocResult<PhysAddr>{PageAllocError::InvalidOrder};
    }

    // 查找合适的空闲块
    usize current_order = order;
    while (current_order <= MAX_ORDER && free_lists_[current_order] == nullptr) {
        current_order++;
    }

    // 没有足够大的空闲块
    if (current_order > MAX_ORDER) {
        return PageAllocResult<PhysAddr>{PageAllocError::OutOfMemory};
    }

    // 从空闲列表移除块
    FreeBlock* block = remove_from_free_list(current_order);
    if (block == nullptr) {
        return PageAllocResult<PhysAddr>{PageAllocError::OutOfMemory};
    }

    // 将大块分割为合适的大小
    while (current_order > order) {
        current_order--;
        split_block(block, current_order);
    }

    // 更新统计信息
    usize pages_allocated = 1UL << order;
    [[maybe_unused]] auto old_used = used_pages_.fetch_add(pages_allocated);
    [[maybe_unused]] auto old_free = free_pages_.fetch_sub(pages_allocated);

    PhysAddr allocated_addr = reinterpret_cast<PhysAddr>(block);
    return PageAllocResult<PhysAddr>{allocated_addr};
}

// 释放物理页面
PageAllocVoidResult PageFrameAllocator::free_pages(PhysAddr addr, usize order) noexcept {
    if (!initialized_) {
        return PageAllocVoidResult{PageAllocError::InitializationFailed};
    }

    if (order > MAX_ORDER) {
        return PageAllocVoidResult{PageAllocError::InvalidOrder};
    }

    if (!is_valid_page_address(addr)) {
        return PageAllocVoidResult{PageAllocError::InvalidAddress};
    }

    // 将地址转换为FreeBlock
    FreeBlock* block = reinterpret_cast<FreeBlock*>(addr);

    // 尝试与buddy块合并
    merge_buddies(block, order);

    // 更新统计信息
    usize pages_freed = 1UL << order;
    [[maybe_unused]] auto old_used = used_pages_.fetch_sub(pages_freed);
    [[maybe_unused]] auto old_free = free_pages_.fetch_add(pages_freed);

    return PageAllocVoidResult{};
}

// 获取内存统计信息
PageFrameAllocator::MemoryStats PageFrameAllocator::get_memory_stats() noexcept {
    return MemoryStats{
        .total_pages = total_pages_,
        .free_pages = free_pages_.load(),
        .used_pages = used_pages_.load(),
        .kernel_pages = total_pages_ - free_pages_.load() - used_pages_.load()
    };
}

// 解析内核内存布局
PageAllocVoidResult PageFrameAllocator::parse_memory_layout() noexcept {
    // QEMU virt 平台内存布局：
    // 0x40000000 - 内核起始
    // _kernel_end_addr - 内核结束，可用内存开始
    // 假设系统有256MB内存 (0x40000000 + 256MB = 0x50000000)

    PhysAddr kernel_end = reinterpret_cast<PhysAddr>(_kernel_end_addr);
    PhysAddr memory_end = static_cast<PhysAddr>(0x50000000);  // 256MB

    // 对齐到页面边界
    PhysAddr available_start = (kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    PhysAddr available_end = memory_end & ~(PAGE_SIZE - 1);

    if (available_start >= available_end) {
        return PageAllocVoidResult{PageAllocError::InitializationFailed};
    }

    // 计算可用页面数
    total_pages_ = (available_end - available_start) / PAGE_SIZE;
    free_pages_.store(total_pages_);
    used_pages_.store(0);

    // 创建单一内存区域 (简化实现)
    // 在实际系统中，这里应该解析设备树或UEFI内存映射
    static MemoryRegion main_region;
    main_region.start_addr = available_start;
    main_region.page_count = total_pages_;
    main_region.next = nullptr;

    memory_regions_ = &main_region;

    return PageAllocVoidResult{};
}

// 初始化空闲列表
void PageFrameAllocator::initialize_free_lists() noexcept {
    // 清空所有空闲列表
    for (usize i = 0; i <= MAX_ORDER; i++) {
        free_lists_[i] = nullptr;
    }

    // 将所有可用内存添加到最大阶的空闲列表
    MemoryRegion* region = memory_regions_;
    while (region != nullptr) {
        PhysAddr current_addr = region->start_addr;
        usize remaining_pages = region->page_count;

        while (remaining_pages > 0) {
            // 找到最大的可用块大小
            usize order = MAX_ORDER;
            while (order > 0 && (1UL << order) > remaining_pages) {
                order--;
            }

            // 确保地址对齐到块大小
            usize block_size = 1UL << order;
            usize addr_offset = addr_to_page(current_addr) & (block_size - 1);
            if (addr_offset != 0) {
                order--;
                block_size = 1UL << order;
            }

            // 添加块到空闲列表
            FreeBlock* block = reinterpret_cast<FreeBlock*>(current_addr);
            add_to_free_list(block, order);

            current_addr += block_size * PAGE_SIZE;
            remaining_pages -= block_size;
        }

        region = region->next;
    }
}

// 将块分割为两个小块
void PageFrameAllocator::split_block(FreeBlock* block, usize order) noexcept {
    usize block_size = PAGE_SIZE << order;
    FreeBlock* buddy = reinterpret_cast<FreeBlock*>(
        reinterpret_cast<usize>(block) + block_size
    );

    add_to_free_list(buddy, order);
}

// 合并buddy块
void PageFrameAllocator::merge_buddies(FreeBlock* block, usize order) noexcept {
    while (order < MAX_ORDER) {
        // 找到buddy块
        FreeBlock* buddy = find_buddy(block, order);
        if (buddy == nullptr) {
            break;  // buddy不可用，无法合并
        }

        // 从空闲列表移除buddy
        if (buddy->prev) {
            buddy->prev->next = buddy->next;
        } else {
            free_lists_[order] = buddy->next;
        }
        if (buddy->next) {
            buddy->next->prev = buddy->prev;
        }

        // 确保block是较低地址的块
        if (reinterpret_cast<usize>(buddy) < reinterpret_cast<usize>(block)) {
            block = buddy;
        }

        order++;
    }

    // 将合并后的块添加到空闲列表
    add_to_free_list(block, order);
}

// 查找buddy块
PageFrameAllocator::FreeBlock* PageFrameAllocator::find_buddy(FreeBlock* block, usize order) noexcept {
    usize block_size = PAGE_SIZE << order;
    usize block_addr = reinterpret_cast<usize>(block);
    usize buddy_addr = block_addr ^ block_size;

    // 检查buddy是否在空闲列表中
    FreeBlock* current = free_lists_[order];
    while (current != nullptr) {
        if (reinterpret_cast<usize>(current) == buddy_addr) {
            return current;
        }
        current = current->next;
    }

    return nullptr;
}

// 添加块到空闲列表
void PageFrameAllocator::add_to_free_list(FreeBlock* block, usize order) noexcept {
    block->order = order;
    block->next = free_lists_[order];
    block->prev = nullptr;

    if (free_lists_[order] != nullptr) {
        free_lists_[order]->prev = block;
    }

    free_lists_[order] = block;
}

// 从空闲列表移除块
PageFrameAllocator::FreeBlock* PageFrameAllocator::remove_from_free_list(usize order) noexcept {
    FreeBlock* block = free_lists_[order];
    if (block == nullptr) {
        return nullptr;
    }

    free_lists_[order] = block->next;
    if (block->next != nullptr) {
        block->next->prev = nullptr;
    }

    return block;
}

// 验证地址是否为有效的页面地址
bool PageFrameAllocator::is_valid_page_address(PhysAddr addr) noexcept {
    // 检查地址是否页面对齐
    if (static_cast<usize>(addr) & (PAGE_SIZE - 1)) {
        return false;
    }

    // 检查地址是否在可用内存区域内
    MemoryRegion* region = memory_regions_;
    while (region != nullptr) {
        PhysAddr region_start = region->start_addr;
        PhysAddr region_end = region_start + region->page_count * PAGE_SIZE;

        if (addr >= region_start && addr < region_end) {
            return true;
        }

        region = region->next;
    }

    return false;
}

#ifdef DEBUG
// 验证空闲列表的完整性
void PageFrameAllocator::validate_free_lists() noexcept {
    // 实现空闲列表完整性检查
    for (usize order = 0; order <= MAX_ORDER; order++) {
        FreeBlock* block = free_lists_[order];
        usize count = 0;

        while (block != nullptr && count < 1000) {  // 防止无限循环
            // 验证块的阶数
            if (block->order != order) {
                // ERROR: Block order mismatch
            }

            // 验证双向链表的一致性
            if (block->next && block->next->prev != block) {
                // ERROR: Free list corruption
            }

            block = block->next;
            count++;
        }
    }
}

// 输出内存布局信息
void PageFrameAllocator::dump_memory_layout() noexcept {
    // Simplified: debug output disabled for module compilation
    // In production, use unified logging system
}
#endif

} // namespace moss::kernel::mm
