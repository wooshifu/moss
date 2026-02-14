// 运行时堆分配器实现
// 提供动态内存分配功能，基于虚拟内存和物理页面管理
// Module implementation unit

module;

module moss.mm;

namespace moss::kernel::mm {

// 静态成员定义
bool RuntimeHeapAllocator::initialized_ = false;
VirtAddr RuntimeHeapAllocator::heap_start_ = 0;
VirtAddr RuntimeHeapAllocator::heap_end_ = 0;
VirtAddr RuntimeHeapAllocator::heap_limit_ = 0;
RuntimeHeapAllocator::FreeBlock* RuntimeHeapAllocator::free_list_head_ = nullptr;
usize RuntimeHeapAllocator::allocated_bytes_ = 0;
usize RuntimeHeapAllocator::total_allocations_ = 0;

// 初始化堆分配器
HeapAllocVoidResult RuntimeHeapAllocator::initialize_heap(VirtAddr heap_start, usize initial_size) noexcept {
    if (initialized_) {
        return HeapAllocVoidResult{};
    }

    // 对齐堆起始地址和大小到页面边界
    heap_start_ = heap_start & ~(PAGE_SIZE - 1);
    usize aligned_size = align_size(initial_size, PAGE_SIZE);
    heap_end_ = heap_start_ + aligned_size;
    heap_limit_ = heap_start_ + (256 * 1024 * 1024);  // 最大256MB堆空间

    // 映射初始堆页面
    auto map_result = map_heap_pages(heap_start_, aligned_size);
    if (!map_result) {
        return HeapAllocVoidResult{map_result.error()};
    }

    // 创建初始空闲块
    FreeBlock* initial_block = reinterpret_cast<FreeBlock*>(heap_start_);
    new (initial_block) FreeBlock(aligned_size);
    free_list_head_ = initial_block;

    allocated_bytes_ = 0;
    total_allocations_ = 0;
    initialized_ = true;

    return HeapAllocVoidResult{};
}

// 分配内存块
HeapAllocResult<void*> RuntimeHeapAllocator::allocate(usize size) noexcept {
    return allocate_aligned(size, BLOCK_ALIGN);
}

// 分配对齐内存块
HeapAllocResult<void*> RuntimeHeapAllocator::allocate_aligned(usize size, usize alignment) noexcept {
    if (!initialized_) {
        return HeapAllocResult<void*>{HeapAllocError::InitializationFailed};
    }

    if (size == 0) {
        return HeapAllocResult<void*>{HeapAllocError::InvalidSize};
    }

    // 计算实际需要的大小（包括头部和对齐）
    usize header_size = sizeof(AllocatedBlock);
    usize aligned_size = align_size(size + header_size, alignment);

    // 确保最小块大小
    if (aligned_size < MIN_BLOCK_SIZE) {
        aligned_size = MIN_BLOCK_SIZE;
    }

    // 查找合适的空闲块
    FreeBlock* suitable_block = find_suitable_block(aligned_size);
    if (suitable_block == nullptr) {
        // 尝试扩展堆
        usize expand_size = align_size(aligned_size * 2, PAGE_SIZE);
        auto expand_result = expand_heap(expand_size);
        if (!expand_result) {
            return HeapAllocResult<void*>{HeapAllocError::OutOfMemory};
        }

        // 再次尝试查找
        suitable_block = find_suitable_block(aligned_size);
        if (suitable_block == nullptr) {
            return HeapAllocResult<void*>{HeapAllocError::OutOfMemory};
        }
    }

    // 从空闲列表移除块
    remove_from_free_list(suitable_block);

    // 如果块太大，分割它
    if (suitable_block->size > aligned_size + MIN_BLOCK_SIZE) {
        split_block(suitable_block, aligned_size);
    }

    // 将空闲块转换为已分配块
    AllocatedBlock* alloc_block = reinterpret_cast<AllocatedBlock*>(suitable_block);
    new (alloc_block) AllocatedBlock(suitable_block->size);

    // 更新统计信息
    allocated_bytes_ += alloc_block->size;
    total_allocations_++;

    // 返回用户数据指针（跳过头部）
    void* user_ptr = reinterpret_cast<char*>(alloc_block) + sizeof(AllocatedBlock);
    return HeapAllocResult<void*>{user_ptr};
}

// 释放内存块
HeapAllocVoidResult RuntimeHeapAllocator::deallocate(void* ptr, [[maybe_unused]] usize size) noexcept {
    if (!initialized_) {
        return HeapAllocVoidResult{HeapAllocError::InitializationFailed};
    }

    if (ptr == nullptr) {
        return HeapAllocVoidResult{};  // 释放nullptr是合法的
    }

    if (!is_heap_address(ptr)) {
        return HeapAllocVoidResult{HeapAllocError::InvalidAddress};
    }

    // 获取已分配块头部
    AllocatedBlock* alloc_block = reinterpret_cast<AllocatedBlock*>(
        static_cast<char*>(ptr) - sizeof(AllocatedBlock)
    );

    // 验证块完整性
    if (!alloc_block->is_valid()) {
        return HeapAllocVoidResult{HeapAllocError::HeapCorruption};
    }

    // 更新统计信息
    allocated_bytes_ -= alloc_block->size;

    // 将已分配块转换为空闲块
    FreeBlock* free_block = reinterpret_cast<FreeBlock*>(alloc_block);
    new (free_block) FreeBlock(alloc_block->size);

    // 添加到空闲列表
    add_to_free_list(free_block);

    // 合并相邻的空闲块
    merge_free_blocks();

    return HeapAllocVoidResult{};
}

// 扩展堆空间
HeapAllocVoidResult RuntimeHeapAllocator::expand_heap(usize additional_size) noexcept {
    if (!initialized_) {
        return HeapAllocVoidResult{HeapAllocError::InitializationFailed};
    }

    // 对齐大小到页面边界
    usize aligned_size = align_size(additional_size, PAGE_SIZE);
    VirtAddr new_end = heap_end_ + aligned_size;

    // 检查是否超过堆限制
    if (new_end > heap_limit_) {
        return HeapAllocVoidResult{HeapAllocError::OutOfMemory};
    }

    // 映射新页面
    auto map_result = map_heap_pages(heap_end_, aligned_size);
    if (!map_result) {
        return HeapAllocVoidResult{map_result.error()};
    }

    // 创建新的空闲块
    FreeBlock* new_block = reinterpret_cast<FreeBlock*>(heap_end_);
    new (new_block) FreeBlock(aligned_size);

    // 更新堆边界
    heap_end_ = new_end;

    // 添加到空闲列表
    add_to_free_list(new_block);

    // 尝试与前一个块合并
    merge_free_blocks();

    return HeapAllocVoidResult{};
}

// 获取堆统计信息
RuntimeHeapAllocator::HeapStats RuntimeHeapAllocator::get_heap_stats() noexcept {
    usize total_size = get_heap_size();
    usize free_bytes = total_size - allocated_bytes_;

    // 计算碎片化比例（简化版本）
    usize fragmentation_ratio = 0;
    usize largest_free = 0;

    if (free_bytes > 0 && free_list_head_ != nullptr) {
        [[maybe_unused]] usize free_block_count = 0;
        FreeBlock* current = free_list_head_;

        while (current != nullptr) {
            if (current->size > largest_free) {
                largest_free = current->size;
            }
            free_block_count++;
            current = current->next;
        }

        // 碎片化 = (1 - 最大空闲块/总空闲) * 100
        if (free_bytes > 0) {
            fragmentation_ratio = 100 - (largest_free * 100 / free_bytes);
        }
    }

    return HeapStats{
        .total_heap_size = total_size,
        .allocated_bytes = allocated_bytes_,
        .free_bytes = free_bytes,
        .fragmentation_ratio = fragmentation_ratio,
        .largest_free_block = largest_free
    };
}

// 映射堆页面 - 简化实现，使用恒等映射
HeapAllocVoidResult RuntimeHeapAllocator::map_heap_pages(VirtAddr start, usize size) noexcept {
    // 在MOSS内核中，我们已经通过MMU设置了0-4GB的恒等映射
    // 因此虚拟地址直接对应物理地址，无需额外的页面映射操作

    // 验证地址范围合理（在0-4GB范围内）
    if (start >= 0x100000000ULL || (start + size) >= 0x100000000ULL) {
        return HeapAllocVoidResult{HeapAllocError::InvalidAddress};
    }

    // 确保大小是页面对齐的
    if (size == 0 || (size & (PAGE_SIZE - 1)) != 0) {
        return HeapAllocVoidResult{HeapAllocError::InvalidSize};
    }

    // 在恒等映射模式下，堆虚拟地址空间已经由MMU建立映射
    // 我们只需要确保这段内存区域在内核可用范围内（已通过地址检查确认）

    // 简化实现：直接返回成功，因为MMU已经建立了所需的映射
    return HeapAllocVoidResult{};
}

// 取消映射堆页面 - 简化实现
HeapAllocVoidResult RuntimeHeapAllocator::unmap_heap_pages(VirtAddr start, usize size) noexcept {
    // 简化实现：在恒等映射的情况下，我们只需要释放物理页面
    [[maybe_unused]] VirtAddr heap_addr = start; // 避免未使用参数警告

    usize page_count = size / PAGE_SIZE;

    for (usize i = 0; i < page_count; i++) {
        // 在简化实现中，我们假设虚拟地址直接对应物理地址
        PhysAddr phys_addr = static_cast<PhysAddr>(start + i * PAGE_SIZE);
        [[maybe_unused]] auto free_result = PageFrameAllocator::free_pages(phys_addr, 0);
    }

    return HeapAllocVoidResult{};
}

// 查找合适的空闲块
RuntimeHeapAllocator::FreeBlock* RuntimeHeapAllocator::find_suitable_block(usize required_size) noexcept {
    FreeBlock* current = free_list_head_;

    while (current != nullptr) {
        if (current->is_valid() && current->size >= required_size) {
            return current;
        }
        current = current->next;
    }

    return nullptr;
}

// 分割块
void RuntimeHeapAllocator::split_block(FreeBlock* block, usize required_size) noexcept {
    usize remaining_size = block->size - required_size;

    if (remaining_size >= MIN_BLOCK_SIZE) {
        // 创建新的空闲块
        FreeBlock* new_block = reinterpret_cast<FreeBlock*>(
            reinterpret_cast<char*>(block) + required_size
        );
        new (new_block) FreeBlock(remaining_size);

        // 更新原块大小
        block->size = required_size;

        // 添加新块到空闲列表
        add_to_free_list(new_block);
    }
}

// 添加到空闲列表
void RuntimeHeapAllocator::add_to_free_list(FreeBlock* block) noexcept {
    block->next = free_list_head_;
    block->prev = nullptr;

    if (free_list_head_ != nullptr) {
        free_list_head_->prev = block;
    }

    free_list_head_ = block;
}

// 从空闲列表移除
void RuntimeHeapAllocator::remove_from_free_list(FreeBlock* block) noexcept {
    if (block->prev != nullptr) {
        block->prev->next = block->next;
    } else {
        free_list_head_ = block->next;
    }

    if (block->next != nullptr) {
        block->next->prev = block->prev;
    }
}

// 合并相邻空闲块
void RuntimeHeapAllocator::merge_free_blocks() noexcept {
    FreeBlock* current = free_list_head_;

    while (current != nullptr) {
        // 检查是否可以与下一个块合并
        char* current_end = reinterpret_cast<char*>(current) + current->size;
        FreeBlock* next_block = reinterpret_cast<FreeBlock*>(current_end);

        // 验证下一个块是否是有效的空闲块
        bool can_merge = false;
        FreeBlock* search = free_list_head_;
        while (search != nullptr) {
            if (search == next_block && next_block->is_valid()) {
                can_merge = true;
                break;
            }
            search = search->next;
        }

        if (can_merge) {
            // 合并块
            current->size += next_block->size;
            remove_from_free_list(next_block);
            // 继续检查同一个块，可能还能合并更多
        } else {
            current = current->next;
        }
    }
}

// 检查地址是否在堆范围内
bool RuntimeHeapAllocator::is_heap_address(void* ptr) noexcept {
    VirtAddr addr = reinterpret_cast<VirtAddr>(ptr);
    return addr >= heap_start_ && addr < heap_end_;
}

#ifdef DEBUG
// 验证堆完整性 - 简化版本
void RuntimeHeapAllocator::validate_heap() noexcept {
    // 简化实现：只验证关键数据结构，不输出调试信息
    FreeBlock* current = free_list_head_;
    usize free_count = 0;

    while (current != nullptr && free_count < 1000) {  // 防止无限循环
        if (!current->is_valid()) {
            // 检测到损坏但不输出，在实际内核中应该触发panic
            return;
        }

        free_count++;
        current = current->next;
    }
    // 验证完成，无需输出
}

// 输出堆布局信息 - 简化版本
void RuntimeHeapAllocator::dump_heap_layout() noexcept {
    // 简化实现：不输出调试信息，在实际内核中应该使用统一的日志系统
}
#endif

} // namespace moss::kernel::mm
