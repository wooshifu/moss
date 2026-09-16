// 运行时堆分配器实现
// 从链接脚本预留且已映射的 arena 分配；不借用或释放 PFA 的页面。
// Module implementation unit

module;

module moss.mm;

namespace moss::kernel::mm {

// 静态成员定义
containers::IrqSpinLock RuntimeHeapAllocator::lock_;
bool RuntimeHeapAllocator::initialized_ = false;
VirtAddr RuntimeHeapAllocator::heap_start_ = 0;
VirtAddr RuntimeHeapAllocator::heap_end_ = 0;
VirtAddr RuntimeHeapAllocator::heap_limit_ = 0;
RuntimeHeapAllocator::FreeBlock *RuntimeHeapAllocator::free_list_head_ = nullptr;
usize RuntimeHeapAllocator::allocated_bytes_ = 0;
usize RuntimeHeapAllocator::total_allocations_ = 0;

// 初始化堆分配器
HeapAllocVoidResult RuntimeHeapAllocator::initialize_heap(VirtAddr heap_start, usize initial_size) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(lock_);
  if (initialized_) {
    return HeapAllocVoidResult{};
  }

  const auto limit = moss::abi::linker::heap_end();
  // 0x100000000 是当前 4 GiB 内核身份映射的独占上界；arena 的字节地址
  // 必须能直接解引用，不能仅因链接脚本预留了空间就接受未映射的高地址。
  if (heap_start != moss::abi::linker::heap_start() || (heap_start & (PAGE_SIZE - 1)) != 0 || limit <= heap_start ||
      limit > 0x100000000ULL || (limit & (PAGE_SIZE - 1)) != 0) {
    return HeapAllocVoidResult{HeapAllocError::InvalidAddress};
  }
  if (initial_size == 0) {
    return HeapAllocVoidResult{HeapAllocError::InvalidSize};
  }
  // Bound before rounding or addition. The reserved arena is below 4 GiB.
  if (initial_size > limit - heap_start) {
    return HeapAllocVoidResult{HeapAllocError::OutOfMemory};
  }
  const usize aligned_size = align_size(initial_size, PAGE_SIZE);
  heap_start_ = heap_start;
  heap_end_ = heap_start + aligned_size;
  heap_limit_ = limit;

  // 创建初始空闲块
  FreeBlock *initial_block = reinterpret_cast<FreeBlock *>(heap_start_);
  new (initial_block) FreeBlock(aligned_size);
  free_list_head_ = initial_block;

  allocated_bytes_ = 0;
  total_allocations_ = 0;
  initialized_ = true;

  return HeapAllocVoidResult{};
}

// 分配内存块
HeapAllocResult<void *> RuntimeHeapAllocator::allocate(usize size) noexcept {
  return allocate_aligned(size, BLOCK_ALIGN);
}

// 分配对齐内存块
HeapAllocResult<void *> RuntimeHeapAllocator::allocate_aligned(usize size, usize alignment) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(lock_);
  if (!initialized_) {
    return HeapAllocResult<void *>{HeapAllocError::InitializationFailed};
  }
  if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
    return HeapAllocResult<void *>{HeapAllocError::InvalidSize};
  }

  const usize capacity = heap_limit_ - heap_start_;
  if (size > capacity - sizeof(AllocatedBlock) || alignment > heap_limit_) {
    return HeapAllocResult<void *>{HeapAllocError::OutOfMemory};
  }
  if (alignment < BLOCK_ALIGN) {
    alignment = BLOCK_ALIGN;
  }
  const usize payload_size = align_size(size, BLOCK_ALIGN);
  const auto first_payload = align_size(heap_start_ + sizeof(AllocatedBlock), alignment);
  if (first_payload > heap_limit_ || payload_size > heap_limit_ - first_payload) {
    return HeapAllocResult<void *>{HeapAllocError::OutOfMemory};
  }

  // 查找合适的空闲块
  FreeBlock *suitable_block = find_suitable_block(payload_size, alignment);
  if (suitable_block == nullptr) {
    // Include worst-case leading padding, but never grow outside the arena.
    const usize remaining = heap_limit_ - heap_end_;
    usize expand_size = align_size(payload_size + sizeof(AllocatedBlock) + alignment - BLOCK_ALIGN, PAGE_SIZE);
    if (expand_size > remaining) {
      expand_size = remaining;
    }
    if (expand_size == 0) {
      return HeapAllocResult<void *>{HeapAllocError::OutOfMemory};
    }
    auto expand_result = expand_heap_locked(expand_size);
    if (!expand_result) {
      return HeapAllocResult<void *>{HeapAllocError::OutOfMemory};
    }

    // 再次尝试查找
    suitable_block = find_suitable_block(payload_size, alignment);
    if (suitable_block == nullptr) {
      return HeapAllocResult<void *>{HeapAllocError::OutOfMemory};
    }
  }

  const auto block_start = reinterpret_cast<VirtAddr>(suitable_block);
  const auto user_address = align_size(block_start + sizeof(AllocatedBlock), alignment);
  const usize required_size = user_address - block_start + payload_size;
  remove_from_free_list(suitable_block);
  split_block(suitable_block, required_size);

  // The header immediately precedes the aligned payload and owns its padding.
  const usize block_size = suitable_block->size;
  auto *alloc_block = reinterpret_cast<AllocatedBlock *>(user_address - sizeof(AllocatedBlock));
  new (alloc_block) AllocatedBlock(block_size, block_start, size);

  // 更新统计信息
  allocated_bytes_ += alloc_block->size;
  total_allocations_++;

  return HeapAllocResult<void *>{reinterpret_cast<void *>(user_address)};
}

// 释放内存块
HeapAllocVoidResult RuntimeHeapAllocator::deallocate(void *ptr, usize size) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(lock_);
  if (!initialized_) {
    return HeapAllocVoidResult{HeapAllocError::InitializationFailed};
  }
  if (ptr == nullptr) {
    return HeapAllocVoidResult{}; // 释放nullptr是合法的
  }

  const auto address = reinterpret_cast<VirtAddr>(ptr);
  if (address < heap_start_ + sizeof(AllocatedBlock) || address >= heap_end_ || (address & (BLOCK_ALIGN - 1)) != 0) {
    return HeapAllocVoidResult{HeapAllocError::InvalidAddress};
  }

  // 获取已分配块头部
  AllocatedBlock *alloc_block = reinterpret_cast<AllocatedBlock *>(static_cast<char *>(ptr) - sizeof(AllocatedBlock));

  // 验证块完整性
  if (!alloc_block->is_valid()) {
    return HeapAllocVoidResult{HeapAllocError::HeapCorruption};
  }
  const auto start = alloc_block->block_start;
  const usize block_size = alloc_block->size;
  if (start < heap_start_ || start > address - sizeof(AllocatedBlock) || (start & (BLOCK_ALIGN - 1)) != 0 ||
      block_size > heap_end_ - start || (block_size & (BLOCK_ALIGN - 1)) != 0 || block_size < address - start ||
      alloc_block->requested_size == 0 || alloc_block->requested_size > block_size - (address - start) ||
      block_size > allocated_bytes_) {
    return HeapAllocVoidResult{HeapAllocError::HeapCorruption};
  }
  // A zero size is the existing unsized-delete contract.
  if (size != 0 && size != alloc_block->requested_size) {
    return HeapAllocVoidResult{HeapAllocError::InvalidSize};
  }

  // 更新统计信息
  allocated_bytes_ -= block_size;
  alloc_block->magic = 0; // Also invalidate a header placed inside alignment padding.

  // 将已分配块转换为空闲块
  FreeBlock *free_block = reinterpret_cast<FreeBlock *>(start);
  new (free_block) FreeBlock(block_size);

  // 添加到空闲列表
  add_to_free_list(free_block);

  // 合并相邻的空闲块
  merge_free_blocks();

  return HeapAllocVoidResult{};
}

// 扩展堆空间
HeapAllocVoidResult RuntimeHeapAllocator::expand_heap(usize additional_size) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(lock_);
  return expand_heap_locked(additional_size);
}

HeapAllocVoidResult RuntimeHeapAllocator::expand_heap_locked(usize additional_size) noexcept {
  // allocate_aligned already owns lock_; reacquiring the public entry's lock
  // here would deadlock. Growth only exposes another part of the mapped arena.
  if (!initialized_) {
    return HeapAllocVoidResult{HeapAllocError::InitializationFailed};
  }

  if (additional_size == 0) {
    return HeapAllocVoidResult{HeapAllocError::InvalidSize};
  }
  if (additional_size > heap_limit_ - heap_end_) {
    return HeapAllocVoidResult{HeapAllocError::OutOfMemory};
  }

  const usize aligned_size = align_size(additional_size, PAGE_SIZE);
  const VirtAddr new_end = heap_end_ + aligned_size;

  // 创建新的空闲块
  FreeBlock *new_block = reinterpret_cast<FreeBlock *>(heap_end_);
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
  containers::LockGuard<containers::IrqSpinLock> guard(lock_);
  usize total_size = heap_end_ - heap_start_;
  usize free_bytes = total_size - allocated_bytes_;

  // 计算碎片化比例（简化版本）
  usize fragmentation_ratio = 0;
  usize largest_free = 0;

  if (free_bytes > 0 && free_list_head_ != nullptr) {
    FreeBlock *current = free_list_head_;

    while (current != nullptr) {
      if (current->size > largest_free) {
        largest_free = current->size;
      }
      current = current->next;
    }

    // 碎片化 = (1 - 最大空闲块/总空闲) * 100
    if (free_bytes > 0) {
      fragmentation_ratio = 100 - (largest_free * 100 / free_bytes);
    }
  }

  return HeapStats{.total_heap_size = total_size,
                   .allocated_bytes = allocated_bytes_,
                   .free_bytes = free_bytes,
                   .fragmentation_ratio = fragmentation_ratio,
                   .largest_free_block = largest_free};
}

VirtAddr RuntimeHeapAllocator::get_heap_start() noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(lock_);
  return heap_start_;
}

VirtAddr RuntimeHeapAllocator::get_heap_end() noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(lock_);
  return heap_end_;
}

usize RuntimeHeapAllocator::get_heap_size() noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(lock_);
  return heap_end_ - heap_start_;
}

// 查找合适的空闲块
RuntimeHeapAllocator::FreeBlock *RuntimeHeapAllocator::find_suitable_block(usize payload_size,
                                                                           usize alignment) noexcept {
  FreeBlock *current = free_list_head_;

  while (current != nullptr) {
    const auto start = reinterpret_cast<VirtAddr>(current);
    const usize prefix = align_size(start + sizeof(AllocatedBlock), alignment) - start;
    if (current->is_valid() && prefix <= current->size && payload_size <= current->size - prefix) {
      return current;
    }
    current = current->next;
  }

  return nullptr;
}

// 分割块
void RuntimeHeapAllocator::split_block(FreeBlock *block, usize required_size) noexcept {
  usize remaining_size = block->size - required_size;

  // Smaller tails cannot hold FreeBlock; keep them owned by this allocation
  // so neither a later free nor the list traversal uses an undersized header.
  if (remaining_size >= MIN_BLOCK_SIZE) {
    // 创建新的空闲块
    FreeBlock *new_block = reinterpret_cast<FreeBlock *>(reinterpret_cast<char *>(block) + required_size);
    new (new_block) FreeBlock(remaining_size);

    // 更新原块大小
    block->size = required_size;

    // 添加新块到空闲列表
    add_to_free_list(new_block);
  }
}

// 添加到空闲列表
void RuntimeHeapAllocator::add_to_free_list(FreeBlock *block) noexcept {
  block->next = free_list_head_;
  block->prev = nullptr;

  if (free_list_head_ != nullptr) {
    free_list_head_->prev = block;
  }

  free_list_head_ = block;
}

// 从空闲列表移除
void RuntimeHeapAllocator::remove_from_free_list(FreeBlock *block) noexcept {
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
  // ponytail: O(n^2) coalescing in the bounded arena; use an address-ordered list if measured contention warrants it.
  FreeBlock *current = free_list_head_;

  while (current != nullptr) {
    // 检查是否可以与下一个块合并
    char *current_end = reinterpret_cast<char *>(current) + current->size;
    FreeBlock *next_block = reinterpret_cast<FreeBlock *>(current_end);

    // 验证下一个块是否是有效的空闲块
    bool can_merge = false;
    FreeBlock *search = free_list_head_;
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

} // namespace moss::kernel::mm
