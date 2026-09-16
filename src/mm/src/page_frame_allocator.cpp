// 物理页面分配器实现 - Buddy算法
// 为内核提供可靠的物理页面分配和释放功能
// Module implementation unit

module moss.mm;

import moss.abi;

namespace moss::kernel::mm {

// 静态成员定义
containers::IrqSpinLock PageFrameAllocator::lock_;
bool PageFrameAllocator::initialized_ = false;
PageFrameAllocator::MemoryRegion *PageFrameAllocator::memory_regions_ = nullptr;
PageFrameAllocator::FreeBlock *PageFrameAllocator::free_lists_[MAX_ORDER + 1] = {nullptr};
PageFrameAllocator::PageMetadata *PageFrameAllocator::page_metadata_ = nullptr;
usize PageFrameAllocator::total_pages_ = 0;
usize PageFrameAllocator::metadata_pages_ = 0;
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
  if (total_pages_ == 0) {
    return PageAllocVoidResult{PageAllocError::InitializationFailed};
  }

  initialized_ = true;
  log::klog::info("PFA layout: heap={:x}..{:x}, tables={:x}..{:x}, metadata={:x} ({} bytes), {} pages",
                  moss::abi::linker::heap_start(), moss::abi::linker::heap_end(), moss::abi::linker::pagetable_start(),
                  moss::abi::linker::pagetable_end(), reinterpret_cast<PhysAddr>(page_metadata_),
                  (metadata_pages_ * sizeof(PageMetadata) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1), total_pages_);

#ifdef DEBUG
  dump_memory_layout();
#endif

  return PageAllocVoidResult{};
}

// Private metadata encoding: bits 0/1 record allocation/head state; the order
// starts at bit 2 so free_pages can reject a tail page or a mismatched order.
static constexpr u32 PAGE_FLAG_ALLOCATED = 1U << 0;
static constexpr u32 PAGE_FLAG_HEAD = 1U << 1;
static constexpr u32 PAGE_ORDER_SHIFT = 2;

// 分配物理页面
PageAllocResult<PhysAddr> PageFrameAllocator::allocate_pages(usize order) noexcept {
  if (!initialized_) {
    return PageAllocResult<PhysAddr>{PageAllocError::InitializationFailed};
  }
  containers::LockGuard<containers::IrqSpinLock> guard(lock_);

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
  FreeBlock *block = remove_from_free_list(current_order);
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

  const usize page_idx = addr_to_page(allocated_addr - memory_regions_->start_addr);
  if ((allocated_addr & ((PAGE_SIZE << order) - 1)) != 0 || page_idx >= metadata_pages_ ||
      pages_allocated > metadata_pages_ - page_idx) {
    log::klog::panic("PFA: free-list block outside allocation boundaries");
  }
  // The allocator lock owns flags. Record one head and the original order;
  // allocated tail pages are not independently releasable allocations.
  for (usize pi = 0; pi < pages_allocated; ++pi) {
    auto &metadata = page_metadata_[page_idx + pi];
    if (metadata.flags.load(containers::MemoryOrder::Relaxed) & PAGE_FLAG_ALLOCATED) {
      log::klog::panic("PFA: double-alloc page 0x{:x} order={}", static_cast<u64>(allocated_addr + pi * PAGE_SIZE),
                       order);
    }
    metadata.ref_count.store(1, containers::MemoryOrder::Relaxed);
    metadata.flags.store(PAGE_FLAG_ALLOCATED |
                             (pi == 0 ? PAGE_FLAG_HEAD | static_cast<u32>(order) << PAGE_ORDER_SHIFT : 0),
                         containers::MemoryOrder::Relaxed);
  }

  return PageAllocResult<PhysAddr>{allocated_addr};
}

// 释放物理页面
PageAllocVoidResult PageFrameAllocator::free_pages(PhysAddr addr, usize order) noexcept {
  if (!initialized_) {
    return PageAllocVoidResult{PageAllocError::InitializationFailed};
  }
  containers::LockGuard<containers::IrqSpinLock> guard(lock_);

  if (order > MAX_ORDER) {
    return PageAllocVoidResult{PageAllocError::InvalidOrder};
  }

  if (!is_valid_page_address(addr)) {
    return PageAllocVoidResult{PageAllocError::InvalidAddress};
  }

  const usize page_idx = addr_to_page(addr - memory_regions_->start_addr);
  const u32 head_flags = page_metadata_[page_idx].flags.load(containers::MemoryOrder::Relaxed);
  if ((head_flags & (PAGE_FLAG_ALLOCATED | PAGE_FLAG_HEAD)) != (PAGE_FLAG_ALLOCATED | PAGE_FLAG_HEAD)) {
    return PageAllocVoidResult{PageAllocError::InvalidAddress};
  }
  if ((head_flags >> PAGE_ORDER_SHIFT) != order) {
    return PageAllocVoidResult{PageAllocError::InvalidOrder};
  }
  const usize pages_freed = 1UL << order;
  if ((addr & ((PAGE_SIZE << order) - 1)) != 0 || pages_freed > metadata_pages_ - page_idx) {
    return PageAllocVoidResult{PageAllocError::InvalidAddress};
  }
  // Validate the complete allocation before mutating any flag, refcount or list.
  for (usize pi = 0; pi < pages_freed; ++pi) {
    const auto &metadata = page_metadata_[page_idx + pi];
    const u32 expected_flags = pi == 0 ? head_flags : PAGE_FLAG_ALLOCATED;
    if (metadata.flags.load(containers::MemoryOrder::Relaxed) != expected_flags) {
      return PageAllocVoidResult{PageAllocError::InvalidAddress};
    }
    if (metadata.ref_count.load(containers::MemoryOrder::Acquire) > 1) {
      return PageAllocVoidResult{PageAllocError::PageInUse};
    }
  }
  for (usize pi = 0; pi < pages_freed; ++pi) {
    page_metadata_[page_idx + pi].flags.store(0, containers::MemoryOrder::Relaxed);
    page_metadata_[page_idx + pi].ref_count.store(0, containers::MemoryOrder::Relaxed);
  }

  // 将地址转换为FreeBlock
  FreeBlock *block = reinterpret_cast<FreeBlock *>(addr);

  // 尝试与buddy块合并
  merge_buddies(block, order);

  // 更新统计信息
  [[maybe_unused]] auto old_used = used_pages_.fetch_sub(pages_freed);
  [[maybe_unused]] auto old_free = free_pages_.fetch_add(pages_freed);

  return PageAllocVoidResult{};
}

// 获取内存统计信息
PageFrameAllocator::MemoryStats PageFrameAllocator::get_memory_stats() noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(lock_);
  return MemoryStats{
      .total_pages = total_pages_,
      .free_pages = free_pages_.load(),
      .used_pages = used_pages_.load(),
      .kernel_pages = total_pages_ - free_pages_.load() - used_pages_.load(),
      .metadata_start = reinterpret_cast<PhysAddr>(page_metadata_),
      .metadata_size = initialized_ ? (metadata_pages_ * sizeof(PageMetadata) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1) : 0};
}

// 解析内核内存布局
PageAllocVoidResult PageFrameAllocator::parse_memory_layout() noexcept {
  // Current identity/direct maps cover only [0, 4 GiB). Every 0x100000000
  // bound below excludes RAM/kernel addresses the allocator cannot dereference.
  // Firmware RAM discovery must complete before allocator initialization.
  const auto &plat = ::moss::fdt::get_platform_info();

  PhysAddr kernel_end = moss::abi::linker::kernel_end();
  namespace linker = moss::abi::linker;
  if (linker::bss_end() > linker::heap_start() || linker::heap_start() >= linker::heap_end() ||
      linker::heap_end() > linker::pagetable_start() || linker::pagetable_start() >= linker::pagetable_end() ||
      linker::pagetable_end() > kernel_end) {
    log::klog::error("PFA: invalid linker memory layout");
    return PageAllocVoidResult{PageAllocError::InitializationFailed};
  }
  if (!plat.memory_map_valid || plat.memory_region_count == 0 ||
      plat.memory_region_count > sizeof(plat.memory_regions) / sizeof(plat.memory_regions[0]) ||
      plat.reserved_region_count > sizeof(plat.reserved_regions) / sizeof(plat.reserved_regions[0]) ||
      plat.initrd_end < plat.initrd_start || kernel_end >= 0x100000000ULL) {
    return PageAllocVoidResult{PageAllocError::InitializationFailed};
  }
  // Firmware entries are inputs, including maps supplied by non-DTB boot paths.
  for (u32 i = 0; i < plat.memory_region_count; ++i) {
    const auto &region = plat.memory_regions[i];
    if (!region.size || region.size > ~u64{0} - region.base) {
      return PageAllocVoidResult{PageAllocError::InitializationFailed};
    }
  }
  for (u32 i = 0; i < plat.reserved_region_count; ++i) {
    const auto &region = plat.reserved_regions[i];
    if (region.size > ~u64{0} - region.base) {
      return PageAllocVoidResult{PageAllocError::InitializationFailed};
    }
  }
  constexpr usize capacity = sizeof(plat.memory_regions) / sizeof(plat.memory_regions[0]);
  struct Range {
    PhysAddr begin;
    PhysAddr end;
  };
  Range ranges[capacity]{};
  usize count = 0;
  bool contains_kernel_end = false;
  for (u32 i = 0; i < plat.memory_region_count; ++i) {
    const auto &region = plat.memory_regions[i];
    const PhysAddr end = region.base + region.size;
    if (end > 0x100000000ULL) {
      return PageAllocVoidResult{PageAllocError::InitializationFailed};
    }
    contains_kernel_end = contains_kernel_end || (region.base <= kernel_end && kernel_end < end);
    usize at = count++;
    while (at && ranges[at - 1].begin > region.base) {
      ranges[at] = ranges[at - 1];
      --at;
    }
    ranges[at] = {.begin = region.base, .end = end};
  }
  if (!contains_kernel_end) {
    return PageAllocVoidResult{PageAllocError::InitializationFailed};
  }
  // Adjacent firmware entries form one bank, including partial edge pages.
  // Reject overlapping usable entries consistently with the DTB parser.
  usize merged = 0;
  for (usize i = 0; i < count; ++i) {
    if (merged && ranges[i].begin < ranges[merged - 1].end) {
      return PageAllocVoidResult{PageAllocError::InitializationFailed};
    }
    if (merged && ranges[i].begin == ranges[merged - 1].end) {
      ranges[merged - 1].end = ranges[i].end;
    } else {
      ranges[merged++] = ranges[i];
    }
  }
  static MemoryRegion regions[capacity];
  count = 0;
  for (usize i = 0; i < merged; ++i) {
    // ponytail: retain the boot prefix below kernel_end until every boot
    // protocol explicitly reserves its live low-memory buffers/trampolines.
    const auto base = ranges[i].begin > kernel_end ? ranges[i].begin : kernel_end;
    const PhysAddr begin = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    const PhysAddr end = ranges[i].end & ~(PAGE_SIZE - 1);
    if (begin < end) {
      regions[count++] = {.start_addr = begin, .page_count = (end - begin) / PAGE_SIZE, .next = nullptr};
    }
  }
  if (!count) {
    return PageAllocVoidResult{PageAllocError::InitializationFailed};
  }
  for (usize i = 1; i < count; ++i) {
    regions[i - 1].next = &regions[i];
  }
  memory_regions_ = regions;
  const PhysAddr span_end = regions[count - 1].start_addr + regions[count - 1].page_count * PAGE_SIZE;
  // ponytail: dense PFN metadata spans holes, bounded to 16 MiB by the current
  // 4-GiB physical mapping. Use per-bank metadata when high/sparse RAM is supported.
  metadata_pages_ = (span_end - regions[0].start_addr) / PAGE_SIZE;
  const usize metadata_bytes = (metadata_pages_ * sizeof(PageMetadata) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  // Metadata must fit wholly in a real bank, not merely in the total RAM span.
  auto reserved_end = [&](PhysAddr begin, PhysAddr end, PhysAddr bank_end) -> PhysAddr {
    PhysAddr skip_to = begin;
    auto check = [&](PhysAddr base, u64 size) {
      if (size && begin < base + size && end > base && base + size > skip_to) {
        // Clip before rounding: a valid exclusive end of UINT64_MAX must not
        // wrap the metadata placement to address zero.
        const auto clipped_end = base + size < bank_end ? base + size : bank_end;
        skip_to = (clipped_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
      }
    };
    check(plat.initrd_start, plat.initrd_end - plat.initrd_start);
    for (u32 i = 0; i < plat.reserved_region_count; ++i) {
      check(plat.reserved_regions[i].base, plat.reserved_regions[i].size);
    }
    return skip_to;
  };
  page_metadata_ = nullptr;
  for (usize i = 0; i < count; ++i) {
    PhysAddr begin = regions[i].start_addr;
    const PhysAddr end = begin + regions[i].page_count * PAGE_SIZE;
    while (begin < end && metadata_bytes <= end - begin) {
      const auto next = reserved_end(begin, begin + metadata_bytes, end);
      if (next == begin) {
        page_metadata_ = reinterpret_cast<PageMetadata *>(begin);
        auto *bytes = reinterpret_cast<u8 *>(begin);
        for (usize byte = 0; byte < metadata_bytes; ++byte) {
          bytes[byte] = 0;
        }
        used_pages_.store(0);
        return PageAllocVoidResult{};
      }
      begin = next;
    }
  }
  return PageAllocVoidResult{PageAllocError::InitializationFailed};
}

// 初始化空闲列表
void PageFrameAllocator::initialize_free_lists() noexcept {
  // 清空所有空闲列表
  for (usize i = 0; i <= MAX_ORDER; i++) {
    free_lists_[i] = nullptr;
  }

  const auto &plat = ::moss::fdt::get_platform_info();
  const PhysAddr metadata_start = reinterpret_cast<PhysAddr>(page_metadata_);
  const PhysAddr metadata_end =
      metadata_start + ((metadata_pages_ * sizeof(PageMetadata) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
  auto reserved = [&](PhysAddr begin, PhysAddr end) {
    if (begin < metadata_end && end > metadata_start) {
      return true;
    }
    if (plat.initrd_end > plat.initrd_start && begin < plat.initrd_end && end > plat.initrd_start) {
      return true;
    }
    for (u32 i = 0; i < plat.reserved_region_count; ++i) {
      const auto &r = plat.reserved_regions[i];
      if (begin < r.base + r.size && end > r.base) {
        return true;
      }
    }
    return false;
  };
  usize usable_pages = 0;
  // Do not publish any block spanning reserved data or misaligned to its order.
  MemoryRegion *region = memory_regions_;
  while (region != nullptr) {
    PhysAddr current_addr = region->start_addr;
    usize remaining_pages = region->page_count;

    while (remaining_pages > 0) {
      if (reserved(current_addr, current_addr + PAGE_SIZE)) {
        current_addr += PAGE_SIZE;
        --remaining_pages;
        continue;
      }
      // 找到最大的可用块大小
      usize order = MAX_ORDER;
      while (order > 0 && ((1UL << order) > remaining_pages || (current_addr & ((PAGE_SIZE << order) - 1)) != 0 ||
                           reserved(current_addr, current_addr + (PAGE_SIZE << order)))) {
        order--;
      }

      // 确保地址对齐到块大小
      usize block_size = 1UL << order;

      // 添加块到空闲列表
      FreeBlock *block = reinterpret_cast<FreeBlock *>(current_addr);
      add_to_free_list(block, order);

      current_addr += block_size * PAGE_SIZE;
      remaining_pages -= block_size;
      usable_pages += block_size;
    }

    region = region->next;
  }
  total_pages_ = usable_pages;
  free_pages_.store(usable_pages);
}

// 将块分割为两个小块
void PageFrameAllocator::split_block(FreeBlock *block, usize order) noexcept {
  usize block_size = PAGE_SIZE << order;
  FreeBlock *buddy = reinterpret_cast<FreeBlock *>(reinterpret_cast<usize>(block) + block_size);

  add_to_free_list(buddy, order);
}

// 合并buddy块
void PageFrameAllocator::merge_buddies(FreeBlock *block, usize order) noexcept {
  while (order < MAX_ORDER) {
    // 找到buddy块
    FreeBlock *buddy = find_buddy(block, order);
    if (buddy == nullptr) {
      break; // buddy不可用，无法合并
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
PageFrameAllocator::FreeBlock *PageFrameAllocator::find_buddy(FreeBlock *block, usize order) noexcept {
  usize block_size = PAGE_SIZE << order;
  usize block_addr = reinterpret_cast<usize>(block);
  // Flipping the order's size bit selects the other half of the same aligned
  // parent. Relative-to-bank XOR would pair incorrectly at an unaligned bank.
  usize buddy_addr = block_addr ^ block_size;

  // 检查buddy是否在空闲列表中
  FreeBlock *current = free_lists_[order];
  while (current != nullptr) {
    if (reinterpret_cast<usize>(current) == buddy_addr) {
      return current;
    }
    current = current->next;
  }

  return nullptr;
}

// 添加块到空闲列表
void PageFrameAllocator::add_to_free_list(FreeBlock *block, usize order) noexcept {
  block->order = order;
  block->next = free_lists_[order];
  block->prev = nullptr;

  if (free_lists_[order] != nullptr) {
    free_lists_[order]->prev = block;
  }

  free_lists_[order] = block;
}

// 从空闲列表移除块
PageFrameAllocator::FreeBlock *PageFrameAllocator::remove_from_free_list(usize order) noexcept {
  FreeBlock *block = free_lists_[order];
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
  MemoryRegion *region = memory_regions_;
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
    FreeBlock *block = free_lists_[order];
    usize count = 0;

    // 调试扫描最多 1000 个节点，避免损坏链表使诊断挂死；选值依据尚未记录，
    // 因此到达上限不代表完整验证了该阶的全部空闲块。
    while (block != nullptr && count < 1000) { // 防止无限循环
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

// ============================================================================
// Page reference counting for COW (Copy-on-Write)
// ============================================================================
// Counts describe frame ownership, not VMA/PTE lifetime. Callers must already
// own a live frame and serialize mapping changes; atomics alone cannot make a
// refcount==1 COW fast path safe against a concurrent clone or unmap.

void PageFrameAllocator::page_ref_inc(PhysAddr addr) noexcept {
  if (!page_metadata_ || !memory_regions_) {
    return;
  }
  usize idx = addr_to_page(addr - memory_regions_->start_addr);
  if (idx < metadata_pages_) {
    // AcqRel orders local setup/access around the ownership update; the caller
    // must publish the new mapping and prevent increments of a released frame.
    (void)page_metadata_[idx].ref_count.fetch_add(1, containers::MemoryOrder::AcqRel);
  }
}

u32 PageFrameAllocator::page_ref_dec(PhysAddr addr) noexcept {
  if (!page_metadata_ || !memory_regions_) {
    return 0;
  }
  usize idx = addr_to_page(addr - memory_regions_->start_addr);
  if (idx < metadata_pages_) {
    // Release orders this owner's page accesses before relinquishing it. The
    // final decrement acquires earlier releases before the caller reclaims it.
    return page_metadata_[idx].ref_count.fetch_sub(1, containers::MemoryOrder::AcqRel) - 1;
  }
  return 0;
}

u32 PageFrameAllocator::page_ref_get(PhysAddr addr) noexcept {
  if (!page_metadata_ || !memory_regions_) {
    return 0;
  }
  usize idx = addr_to_page(addr - memory_regions_->start_addr);
  if (idx < metadata_pages_) {
    // Acquire pairs with published ownership updates. The caller's mapping
    // protocol must still prevent a new owner between this read and a COW write.
    return page_metadata_[idx].ref_count.load(containers::MemoryOrder::Acquire);
  }
  return 0;
}

void PageFrameAllocator::page_ref_set(PhysAddr addr, u32 count) noexcept {
  if (!page_metadata_ || !memory_regions_) {
    return;
  }
  usize idx = addr_to_page(addr - memory_regions_->start_addr);
  if (idx < metadata_pages_) {
    page_metadata_[idx].ref_count.store(count, containers::MemoryOrder::Relaxed);
  }
}

} // namespace moss::kernel::mm
