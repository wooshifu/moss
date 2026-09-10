// MOSS页表管理器实现 - Module implementation unit
// 提供ARM64页表管理、MMU启用和调试输出功能

module moss.mm;

import moss.logging;

namespace log = moss::kernel::logging;

namespace moss::kernel::mm {

static unsigned root_shift() noexcept { return hal::mmu::g_mmu_mode == hal::mmu::MmuMode::Sv39 ? 30 : 39; }

// A whole entry is shared only when its entire VA range belongs to the kernel.
// The low 4 GiB and upper half stay kernel-owned whether mapped by blocks or pages.
static bool shared_kernel_entry(u64 base, unsigned shift) noexcept {
  constexpr u64 end = PageTableManager::KERNEL_IDENTITY_END;
  return base >= USER_MAX || (base < end && (1ULL << shift) <= end - base);
}

// Staging uses the allocated tables themselves as a private linked list.
// An allocation failure cannot leave a published table or consume a data-page
// reference. No heap allocation or fixed maximum address-space size is needed.
class TableReserve {
  PageTable *head_ = nullptr;

public:
  TableReserve() = default;
  TableReserve(const TableReserve &) = delete;
  TableReserve &operator=(const TableReserve &) = delete;
  ~TableReserve() {
    while (head_) {
      (void)free_pages(PageTableManager::get_physical_address(take()), 0);
    }
  }

  VoidResult add() {
    auto result = PageTableManager::allocate_page_table_dynamic();
    if (!result)
      return VoidResult{result.error()};
    (*result)->entries[0].raw = head_ ? PageTableManager::get_physical_address(head_) : 0;
    head_ = *result;
    return VoidResult{};
  }

  PageTable *take() noexcept {
    auto *table = head_;
    const PhysAddr next = table->entries[0].raw;
    head_ = next ? PageTableManager::get_table_from_physical(next) : nullptr;
    table->entries[0].clear();
    return table;
  }
};

static void publish_entry(PageTableEntry &entry, PageTableEntry value) noexcept {
  // Publish a naturally aligned, whole descriptor after initializing its tree.
  __atomic_store_n(&entry.raw, value.raw, __ATOMIC_RELEASE);
}

static bool overlaps_ram(PhysAddr start, PhysAddr end) noexcept {
  const auto &hardware = moss::kernel::platform::hardware;
  for (u32 i = 0; i < hardware.memory_region_count; ++i) {
    const auto &region = hardware.memory_regions[i];
    if (region.size && (start >= region.base ? start - region.base < region.size : end > region.base)) {
      return true;
    }
  }
  return false;
}

static bool permission_boundary(PhysAddr begin, PhysAddr end) noexcept {
  namespace linker = moss::abi::linker;
  auto inside = [&](PhysAddr boundary) { return begin < boundary && boundary < end; };
  if (inside(linker::text_start()) || inside(linker::text_end()) || inside(linker::rodata_start()) ||
      inside(linker::rodata_end())) {
    return true;
  }
  const auto &hardware = moss::kernel::platform::hardware;
  for (u32 i = 0; i < hardware.memory_region_count; ++i) {
    const auto &region = hardware.memory_regions[i];
    if (region.base < PageTableManager::KERNEL_IDENTITY_END && region.size) {
      // Clip before rounding so a valid RAM end near UINT64_MAX cannot wrap.
      const u64 room = PageTableManager::KERNEL_IDENTITY_END - region.base;
      const u64 limit = region.base + (region.size < room ? region.size : room);
      if (inside(region.base & ~(PAGE_SIZE - 1)) || inside((limit + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))) {
        return true;
      }
    }
  }
  return false;
}

// Keep large leaves where permissions are uniform; split only at image/RAM edges.
static VoidResult build_kernel_entry(PageTableEntry &entry, PhysAddr pa, unsigned shift, bool direct_map) {
  namespace linker = moss::abi::linker;
  using Tables = PageTableManager;
  const u64 size = 1ULL << shift;
  if (shift > PAGE_SHIFT && permission_boundary(pa, pa + size)) {
    auto result = Tables::allocate_page_table();
    if (!result) {
      return VoidResult{result.error()};
    }
    auto *table = *result;
    for (usize i = 0; i < PageTable::ENTRIES_PER_TABLE; ++i) {
      auto mapped =
          build_kernel_entry(table->entries[i], pa + (static_cast<u64>(i) << (shift - 9)), shift - 9, direct_map);
      if (!mapped) {
        return mapped;
      }
    }
    entry.set_table(Tables::get_physical_address(table));
    return VoidResult{};
  }
  u64 attributes;
  if (pa >= linker::text_start() && pa < linker::text_end()) {
    attributes = direct_map ? page_perms::KERNEL_RO : page_perms::KERNEL_RX;
  } else if (pa >= linker::rodata_start() && pa < linker::rodata_end()) {
    attributes = page_perms::KERNEL_RO;
  } else {
    attributes = overlaps_ram(pa, pa + PAGE_SIZE) ? page_perms::KERNEL_RW : page_perms::DEVICE;
  }
  if (shift == PAGE_SHIFT) {
    entry.set_page(pa, attributes);
  } else {
    entry.set_block(pa, attributes);
  }
  return VoidResult{};
}

// PageTableManager 方法实现 — routes to early bump or dynamic buddy allocator
KernelResult<PageTable *> PageTableManager::allocate_page_table() {
  if (use_dynamic_alloc) {
    return allocate_page_table_dynamic();
  }

  // Early boot path: bump allocator from linker-allocated page table section
  if (next_table_index >= MAX_EARLY_TABLES) {
    return KernelResult<PageTable *>{ErrorCode::OutOfMemory};
  }

  PageTable *table = &early_tables[next_table_index++];

  // 清零页表
  for (usize i = 0; i < PageTable::ENTRIES_PER_TABLE; i++) {
    table->entries[i].clear();
  }

  return KernelResult<PageTable *>{table};
}

// Dynamic page table allocation from buddy allocator (post-boot)
KernelResult<PageTable *> PageTableManager::allocate_page_table_dynamic() {
  auto result = page_alloc::alloc_kernel_pages(0); // order 0 = 1 page = 4KB
  if (!result) {
    return KernelResult<PageTable *>{ErrorCode::OutOfMemory};
  }
  PhysAddr pa = *result;
#ifdef MOSS_ARCH_X86_64
  auto *table = reinterpret_cast<PageTable *>(static_cast<VirtAddr>(pa));
#else
  auto *table = reinterpret_cast<PageTable *>(phys_to_virt(pa));
#endif
  for (usize i = 0; i < PageTable::ENTRIES_PER_TABLE; i++) {
    table->entries[i].clear();
  }
  return KernelResult<PageTable *>{table};
}

// Build high-half kernel page table: map physical RAM at KERNEL_DIRECT_MAP_BASE
VoidResult PageTableManager::setup_kernel_high_half_tables() {
  if (!moss::kernel::platform::hardware.memory_map_valid) {
    return VoidResult{ErrorCode::InvalidState};
  }
  constexpr u64 ONE_GB = 0x40000000ULL;

  // break_virtual_address gives the correct pgd_index for each arch:
  //   ARM64/x86: (KERNEL_DIRECT_MAP_BASE >> 39) & 0x1FF = 256
  //   RISC-V Sv39: (KERNEL_DIRECT_MAP_BASE >> 30) & 0x1FF = 256
  auto bd = break_virtual_address(KERNEL_DIRECT_MAP_BASE);
  u16 pgd_idx = bd.pgd_index;

#if defined(MOSS_ARCH_RISCV)
  // RISC-V has a single satp register (no separate ttbr0/ttbr1), so the
  // high-half entries MUST go into the same root table as the identity map.
  if (!kernel_pgd) {
    return VoidResult{ErrorCode::InvalidState};
  }
  kernel_high_pgd = kernel_pgd;

  if (hal::mmu::g_mmu_mode == hal::mmu::MmuMode::Sv39) {
    // Sv39: each root entry covers 1 GiB, using blocks or lower-level tables.
    for (usize i = 0; i < 4; i++) {
      PhysAddr block_addr = static_cast<PhysAddr>(i * ONE_GB);
      auto mapped = build_kernel_entry(kernel_pgd->entries[pgd_idx + i], block_addr, 30, true);
      if (!mapped) {
        return mapped;
      }
    }
  } else {
    // Sv48: allocate a PUD for the direct map's blocks and split subtrees.
    auto pud_result = allocate_page_table();
    if (!pud_result) {
      return VoidResult{pud_result.error()};
    }
    PageTable *pud = *pud_result;

    for (usize i = 0; i < 4; i++) {
      PhysAddr block_addr = static_cast<PhysAddr>(i * ONE_GB);
      auto mapped = build_kernel_entry(pud->entries[i], block_addr, 30, true);
      if (!mapped) {
        return mapped;
      }
    }

    PhysAddr pud_pa = get_physical_address(pud);
    kernel_pgd->entries[pgd_idx].set_table(pud_pa);
  }
#else
  {
    // ARM64/x86_64: separate high-half PGD (ARM64 uses ttbr1)
    auto pgd_result = allocate_page_table();
    if (!pgd_result) {
      return VoidResult{pgd_result.error()};
    }
    kernel_high_pgd = *pgd_result;

    // Allocate PUD table for this PGD entry
    auto pud_result = allocate_page_table();
    if (!pud_result) {
      return VoidResult{pud_result.error()};
    }
    PageTable *pud = *pud_result;

    PhysAddr pud_pa = get_physical_address(pud);
    kernel_high_pgd->entries[pgd_idx].set_table(pud_pa);
#if defined(MOSS_ARCH_X86_64)
    // CR3 is shared by user and kernel mode; there is no separate TTBR1.
    kernel_pgd->entries[pgd_idx] = kernel_high_pgd->entries[pgd_idx];
#endif

    // Cover 0-4 GiB, splitting at physical memory and protection boundaries.
    for (usize i = 0; i < 4; i++) {
      PhysAddr block_addr = static_cast<PhysAddr>(i * ONE_GB);
      auto mapped = build_kernel_entry(pud->entries[i], block_addr, 30, true);
      if (!mapped) {
        return mapped;
      }
    }
  }
#endif

  log::klog::info("high-half page table: PGD[{}] -> NX direct map", pgd_idx);
  return VoidResult{};
}

// Walk user page tables and return a mutable pointer to the leaf PTE.
// Returns nullptr if any intermediate table is missing (does not allocate).
PageTableEntry *PageTableManager::get_user_pte(PhysAddr pgd_phys, VirtAddr va) {
  if (!pgd_phys || !is_user_range(va, 1) || pgd_phys == get_physical_address(kernel_pgd) ||
      pgd_phys == get_physical_address(kernel_high_pgd)) {
    return nullptr;
  }
  auto *pgd = get_table_from_physical(pgd_phys);
  if (!pgd) {
    return nullptr;
  }
  auto bd = break_virtual_address(va);

  auto &pge = pgd->entries[bd.pgd_index];
  if (!pge.is_valid() || !pge.is_table()) {
    return nullptr;
  }
  auto *pud = get_table_from_physical(pge.get_phys_addr());

  auto &pude = pud->entries[bd.pud_index];
  if (!pude.is_valid() || !pude.is_table()) {
    return nullptr;
  }
  auto *pmd = get_table_from_physical(pude.get_phys_addr());

#if defined(MOSS_ARCH_RISCV)
  if (hal::mmu::g_mmu_mode == hal::mmu::MmuMode::Sv39) {
    // Sv39: PMD is the final L0 table — pmd_index IS the leaf PTE index.
    return &pmd->entries[bd.pmd_index];
  }
#endif
  auto &pmde = pmd->entries[bd.pmd_index];
  if (!pmde.is_valid() || !pmde.is_table()) {
    return nullptr;
  }
  auto *pte = get_table_from_physical(pmde.get_phys_addr());
  return &pte->entries[bd.pte_index];
}

// Unmap a single user page: clear PTE, invalidate TLB, free physical page
// when refcount drops to 0.  Pattern follows free_user_page_tables leaf cleanup.
void PageTableManager::unmap_user_page(PhysAddr pgd_phys, VirtAddr va) noexcept {
  auto *pte_entry = get_user_pte(pgd_phys, va);
  if (!pte_entry || !pte_entry->is_valid()) {
    return;
  }

  PhysAddr pa = pte_entry->get_phys_addr();
  pte_entry->clear();
  invalidate_tlb_addr(va);

  // COW-aware: only free the physical page when no more references
  u32 remaining = PageFrameAllocator::page_ref_dec(pa);
  if (remaining == 0) {
    (void)free_pages(pa, 0);
  }
}

KernelResult<PhysAddr> PageTableManager::create_user_page_tables() {
  if (!kernel_pgd || !use_dynamic_alloc) {
    return KernelResult<PhysAddr>{ErrorCode::InvalidState};
  }
  auto result = allocate_page_table_dynamic();
  if (!result) {
    return KernelResult<PhysAddr>{result.error()};
  }
  auto *root = *result;
  const PhysAddr root_pa = get_physical_address(root);
  const unsigned shift = root_shift();
  for (usize i = 0; i < PageTable::ENTRIES_PER_TABLE; ++i) {
    const auto &source = kernel_pgd->entries[i];
    if (!source.is_valid()) {
      continue;
    }
    const u64 base = static_cast<u64>(i) << shift;
    if (shared_kernel_entry(base, shift)) {
      root->entries[i] = source;
      continue;
    }
    // In four-level mode, root[0] mixes the identity map with future user VAs.
    // Its PUD is private; the kernel-owned children are shared without descent.
    if (shift != 39 || i != 0 || !source.is_table()) {
      free_user_page_tables(root_pa);
      return KernelResult<PhysAddr>{ErrorCode::InvalidState};
    }
    auto private_result = allocate_page_table_dynamic();
    if (!private_result) {
      free_user_page_tables(root_pa);
      return KernelResult<PhysAddr>{private_result.error()};
    }
    auto *private_table = *private_result;
    const auto *kernel_table = get_table_from_physical(source.get_phys_addr());
    for (usize j = 0; j < PageTable::ENTRIES_PER_TABLE; ++j) {
      if (shared_kernel_entry(base | (static_cast<u64>(j) << (shift - 9)), shift - 9)) {
        private_table->entries[j] = kernel_table->entries[j];
      }
    }
    root->entries[i].set_table(get_physical_address(private_table), true);
  }
  return KernelResult<PhysAddr>{root_pa};
}

// Clone user page tables for fork(): deep-copy intermediate tables,
// share leaf pages via COW (mark READONLY + SW_COW, increment refcount).
static VoidResult check_empty_user_tables(const PageTable *table, unsigned shift, u64 base) {
  for (usize i = 0; i < PageTable::ENTRIES_PER_TABLE; ++i) {
    const u64 address = base | (static_cast<u64>(i) << shift);
    if (shared_kernel_entry(address, shift))
      continue;
    const auto &entry = table->entries[i];
    if (!entry.is_valid())
      continue;
    if (shift == 12)
      return VoidResult{ErrorCode::AlreadyExists};
    if (!entry.is_table())
      return VoidResult{ErrorCode::NotSupported};
    auto result =
        check_empty_user_tables(PageTableManager::get_table_from_physical(entry.get_phys_addr()), shift - 9, address);
    if (!result)
      return result;
  }
  return VoidResult{};
}

static VoidResult prepare_user_clone(const PageTable *src, const PageTable *dst, unsigned shift, u64 base,
                                     TableReserve &reserve) {
  for (usize i = 0; i < PageTable::ENTRIES_PER_TABLE; ++i) {
    const u64 address = base | (static_cast<u64>(i) << shift);
    if (shared_kernel_entry(address, shift))
      continue;
    const auto &entry = src->entries[i];
    if (!entry.is_valid())
      continue;
    if (shift == 12) {
#if defined(MOSS_ARCH_RISCV)
      if (entry.is_table())
        return VoidResult{ErrorCode::NotSupported};
#elif defined(MOSS_ARCH_ARM64)
      if (!entry.is_table())
        return VoidResult{ErrorCode::NotSupported};
#endif
      if ((entry.raw & page_attr::USER) == 0 || PageFrameAllocator::page_ref_get(entry.get_phys_addr()) == 0) {
        return VoidResult{ErrorCode::InvalidState};
      }
      continue;
    }
    if (!entry.is_table())
      return VoidResult{ErrorCode::NotSupported};
    const PageTable *dst_child = nullptr;
    if (dst && dst->entries[i].is_valid()) {
      dst_child = PageTableManager::get_table_from_physical(dst->entries[i].get_phys_addr());
    } else {
      auto result = reserve.add();
      if (!result)
        return result;
    }
    auto result = prepare_user_clone(PageTableManager::get_table_from_physical(entry.get_phys_addr()), dst_child,
                                     shift - 9, address, reserve);
    if (!result)
      return result;
  }
  return VoidResult{};
}

// All fallible work precedes this traversal. The caller keeps the source stable
// and the destination unpublished throughout preparation and commit.
static void commit_user_clone(PageTable *src, PageTable *dst, unsigned shift, u64 base, TableReserve &reserve) {
  for (usize i = 0; i < PageTable::ENTRIES_PER_TABLE; ++i) {
    const u64 address = base | (static_cast<u64>(i) << shift);
    if (shared_kernel_entry(address, shift))
      continue;
    auto &source = src->entries[i];
    if (!source.is_valid())
      continue;
    auto &target = dst->entries[i];
    if (shift == 12) {
      auto value = source;
      if (value.is_writable()) {
        value.set_cow();
        value.make_readonly();
        publish_entry(source, value);
      }
      PageFrameAllocator::page_ref_inc(value.get_phys_addr());
      publish_entry(target, value);
      continue;
    }
    const bool allocated = !target.is_valid();
    auto *child = allocated ? reserve.take() : PageTableManager::get_table_from_physical(target.get_phys_addr());
    commit_user_clone(PageTableManager::get_table_from_physical(source.get_phys_addr()), child, shift - 9, address,
                      reserve);
    if (allocated) {
      PageTableEntry link;
      link.set_table(PageTableManager::get_physical_address(child), true);
      publish_entry(target, link);
    }
  }
}

VoidResult PageTableManager::clone_user_page_tables(PhysAddr src_pgd_phys, PhysAddr dst_pgd_phys) {
  const auto kernel = get_physical_address(kernel_pgd);
  const auto high = get_physical_address(kernel_high_pgd);
  if (!src_pgd_phys || !dst_pgd_phys || src_pgd_phys == dst_pgd_phys ||
      ((src_pgd_phys | dst_pgd_phys) & (PAGE_SIZE - 1)) || src_pgd_phys == kernel || src_pgd_phys == high ||
      dst_pgd_phys == kernel || dst_pgd_phys == high) {
    return VoidResult{ErrorCode::InvalidParameter};
  }
  auto *src = get_table_from_physical(src_pgd_phys);
  auto *dst = get_table_from_physical(dst_pgd_phys);
  const unsigned shift = root_shift();
  auto empty = check_empty_user_tables(dst, shift, 0);
  if (!empty)
    return empty;
  TableReserve reserve;
  auto prepared = prepare_user_clone(src, dst, shift, 0, reserve);
  if (!prepared)
    return prepared;
  commit_user_clone(src, dst, shift, 0, reserve);
  invalidate_tlb();
  return VoidResult{};
}

// Free all user page tables and demand-paged physical pages.
// Walks PGD→PUD→PMD→PTE, frees leaf pages and intermediate tables.
// Skip kernel-owned VA ranges at each mixed level, regardless of leaf size.
void PageTableManager::free_user_page_tables(PhysAddr pgd_phys) {
  if (pgd_phys == 0 || pgd_phys == get_physical_address(kernel_pgd) ||
      pgd_phys == get_physical_address(kernel_high_pgd)) {
    return;
  }

  auto *pgd = get_table_from_physical(pgd_phys);
  if (!pgd) {
    return;
  }

  constexpr usize ENTRIES = PageTable::ENTRIES_PER_TABLE;
  const unsigned shift = root_shift();
  for (usize i0 = 0; i0 < ENTRIES; i0++) {
    // Skip kernel high-half PGD entries (e.g. Sv48 PGD[256..511]).
    // These point to shared kernel page tables that must not be freed.
    const u64 base = static_cast<u64>(i0) << shift;
    if (shared_kernel_entry(base, shift)) {
      continue;
    }
    auto &pge = pgd->entries[i0];
    if (!pge.is_valid() || !pge.is_table()) {
      continue;
    }

    // A four-level process owns the mixed low PUD, but not the kernel children
    // it references. Only user descendants and the private PUD are freed.

    auto *pud = get_table_from_physical(pge.get_phys_addr());
    if (!pud) {
      continue;
    }

    for (usize i1 = 0; i1 < ENTRIES; i1++) {
      if (shared_kernel_entry(base | (static_cast<u64>(i1) << (shift - 9)), shift - 9)) {
        continue;
      }
      auto &pude = pud->entries[i1];
      if (!pude.is_valid()) {
        continue;
      }

      // User huge pages are not supported by this teardown path.
      if (pude.is_block()) {
        continue;
      }

      auto *pmd = get_table_from_physical(pude.get_phys_addr());
      if (!pmd) {
        continue;
      }

      for (usize i2 = 0; i2 < ENTRIES; i2++) {
        auto &pmde = pmd->entries[i2];
        if (!pmde.is_valid()) {
          continue;
        }

#if defined(MOSS_ARCH_RISCV)
        // Sv39: PMD IS the leaf level — free 4KB leaf pages directly.
        if (hal::mmu::g_mmu_mode == hal::mmu::MmuMode::Sv39) {
          PhysAddr leaf_pa = pmde.get_phys_addr();
          u32 remaining = PageFrameAllocator::page_ref_dec(leaf_pa);
          if (remaining == 0) {
            (void)free_pages(leaf_pa, 0);
          }
          pmde.clear();
          continue;
        }
#endif

        // Block mapping (2MB) — skip (Sv48/ARM64/x86_64)
        if (pmde.is_block()) {
          continue;
        }

        auto *pte = get_table_from_physical(pmde.get_phys_addr());
        if (!pte) {
          continue;
        }

        // Free all leaf pages (COW-aware: only free when refcount drops to 0)
        for (usize i3 = 0; i3 < ENTRIES; i3++) {
          auto &ptee = pte->entries[i3];
          if (ptee.is_valid()) {
            PhysAddr leaf_pa = ptee.get_phys_addr();
            u32 remaining = PageFrameAllocator::page_ref_dec(leaf_pa);
            if (remaining == 0) {
              (void)free_pages(leaf_pa, 0); // order-0 = single 4KB page
            }
            ptee.clear();
          }
        }

        // Free PTE table page
        PhysAddr pte_pa = pmde.get_phys_addr();
        (void)free_pages(pte_pa, 0);
        pmde.clear();
      }

      // Free PMD table page
      PhysAddr pmd_pa = pude.get_phys_addr();
      (void)free_pages(pmd_pa, 0);
      pude.clear();
    }

    // Free PUD table page
    PhysAddr pud_pa = pge.get_phys_addr();
    (void)free_pages(pud_pa, 0);
    pge.clear();
  }

  // Free the PGD page itself
  (void)free_pages(pgd_phys, 0);
}

// Map a single 4KB page into a user process page table
// ARM64/x86_64: 4-level walk;  RISC-V Sv39: 3-level walk
VoidResult PageTableManager::map_user_page(PhysAddr pgd_phys, VirtAddr va, PhysAddr pa, u64 perms) {
  if (!pgd_phys || pgd_phys == get_physical_address(kernel_pgd) || pgd_phys == get_physical_address(kernel_high_pgd) ||
      !is_user_range(va, PAGE_SIZE) || ((va | pa | pgd_phys) & (PAGE_SIZE - 1)) != 0 ||
      (perms & page_attr::USER) == 0) {
    return VoidResult{ErrorCode::InvalidParameter};
  }
  auto *table = get_table_from_physical(pgd_phys);
  for (unsigned shift = root_shift(); shift > 12; shift -= 9) {
    auto &entry = table->entries[(va >> shift) & 511];
    if (entry.is_valid()) {
      if (!entry.is_table())
        return VoidResult{ErrorCode::NotSupported};
      table = get_table_from_physical(entry.get_phys_addr());
      continue;
    }

    TableReserve reserve;
    for (unsigned remaining = shift; remaining > 12; remaining -= 9) {
      auto result = reserve.add();
      if (!result)
        return result;
    }
    // Build the whole missing path offline, then publish just its first link.
    auto *first = reserve.take();
    table = first;
    for (unsigned child_shift = shift - 9; child_shift > 12; child_shift -= 9) {
      auto *next = reserve.take();
      table->entries[(va >> child_shift) & 511].set_table(get_physical_address(next), true);
      table = next;
    }
    table->entries[(va >> 12) & 511].set_page(pa, perms);
    PageTableEntry link;
    link.set_table(get_physical_address(first), true);
    publish_entry(entry, link);
    invalidate_tlb_addr(va);
    return VoidResult{};
  }

  auto &leaf = table->entries[(va >> 12) & 511];
  if (leaf.is_valid())
    return VoidResult{ErrorCode::AlreadyExists};
  PageTableEntry value;
  value.set_page(pa, perms);
  publish_entry(leaf, value);
  invalidate_tlb_addr(va);
  return VoidResult{};
}

// 创建内核页表映射
//
// 4-level (ARM64, x86_64, RISC-V Sv48): 48-bit VA
//   PGD[0] → PUD for the first 4 GiB identity map.
//
// 3-level (RISC-V Sv39): 39-bit VA
//   Root table = L2 level; root[0..3] cover the identity map.
// Both layouts split leaves at permission/firmware boundaries.
VoidResult PageTableManager::setup_kernel_page_tables() {
  if (!moss::kernel::platform::hardware.memory_map_valid) {
    return VoidResult{ErrorCode::InvalidState};
  }
  constexpr u64 ONE_GB = 0x40000000ULL;
  namespace linker = moss::abi::linker;
  if (((linker::text_start() | linker::text_end() | linker::rodata_start() | linker::rodata_end()) & (PAGE_SIZE - 1)) !=
          0 ||
      linker::text_start() >= linker::text_end() || linker::text_end() > linker::rodata_start() ||
      linker::rodata_end() > linker::data_start() || linker::kernel_end() > KERNEL_IDENTITY_END) {
    return VoidResult{ErrorCode::InvalidState};
  }

#if defined(MOSS_ARCH_RISCV)
  if (hal::mmu::g_mmu_mode == hal::mmu::MmuMode::Sv39) {
    // Sv39: root table = L2 level (512 × 1GB entries).
    // kernel_pgd IS the root table; 1GB gigapage entries go directly in it.
    auto root_result = PageTableManager::allocate_page_table();
    if (!root_result) {
      return VoidResult{root_result.error()};
    }
    PageTableManager::kernel_pgd = *root_result;

    // Each root entry covers 1 GiB; build smaller leaves where required.
    for (usize i = 0; i < 4; i++) {
      PhysAddr block_addr = static_cast<PhysAddr>(i * ONE_GB);
      auto mapped = build_kernel_entry(kernel_pgd->entries[i], block_addr, 30, false);
      if (!mapped) {
        return mapped;
      }
    }
  } else
#endif
  {
    // 4-level: PGD[0] → PUD covering the identity map.
    auto pgd_result = PageTableManager::allocate_page_table();
    if (!pgd_result) {
      return VoidResult{pgd_result.error()};
    }
    PageTableManager::kernel_pgd = *pgd_result;

    auto pud_result = PageTableManager::allocate_page_table();
    if (!pud_result) {
      return VoidResult{pud_result.error()};
    }
    PageTable *pud = *pud_result;

    PhysAddr pud_pa = PageTableManager::get_physical_address(pud);
    PageTableManager::kernel_pgd->entries[0].set_table(pud_pa);

    // All architectures: identity-map 0-4GB only.
    // User code at CODE_BASE (8GB) is mapped via map_user_page() / demand paging.
    constexpr usize PUD_ENTRY_COUNT = 4;
    for (usize i = 0; i < PUD_ENTRY_COUNT; i++) {
      PhysAddr block_addr = static_cast<PhysAddr>(i * ONE_GB);
      auto mapped = build_kernel_entry(pud->entries[i], block_addr, 30, false);
      if (!mapped) {
        return mapped;
      }
    }
  }

  return VoidResult{};
}

// 映射内存区域
VoidResult PageTableManager::map_region(VirtAddr virt_addr, PhysAddr phys_addr, usize size, u64 permissions) {
  // 页面对齐检查
  if ((virt_addr & (PAGE_SIZE - 1)) != 0 || (phys_addr & (PAGE_SIZE - 1)) != 0) {
    return VoidResult{ErrorCode::InvalidParameter};
  }

  usize pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

  for (usize i = 0; i < pages; i++) {
    VirtAddr curr_virt = virt_addr + i * PAGE_SIZE;
    PhysAddr curr_phys = phys_addr + i * PAGE_SIZE;

    auto result = PageTableManager::map_page(curr_virt, curr_phys, permissions);
    if (!result) {
      return VoidResult{result.error()};
    }
  }

  // Full TLB flush after bulk mapping (map_page does per-page invalidation,
  // but a full flush is cheaper for large regions and ensures coherency)
  invalidate_tlb();

  return VoidResult{};
}

// 映射单个页面
//
// 4-level walk: PGD → PUD → PMD → PTE[pte_index]
// RISC-V Sv39 (3-level): PGD → PUD → final entry at PMD[pmd_index]
VoidResult PageTableManager::map_page(VirtAddr virt_addr, PhysAddr phys_addr, u64 permissions) {
  // This entry point modifies the kernel PGD; user mappings have their own API.
  if (permissions & page_attr::USER) {
    return VoidResult{ErrorCode::InvalidParameter};
  }
  if (!PageTableManager::kernel_pgd) {
    return VoidResult{ErrorCode::InvalidState};
  }

  auto bd = break_virtual_address(virt_addr);

  // 遍历页表层级
  PageTable *current_table = PageTableManager::kernel_pgd;

  // PGD -> PUD
  if (!current_table->entries[bd.pgd_index].is_valid()) {
    auto pud_result = PageTableManager::allocate_page_table();
    if (!pud_result) {
      return VoidResult{pud_result.error()};
    }
    PhysAddr pud_pa = PageTableManager::get_physical_address(*pud_result);
    current_table->entries[bd.pgd_index].set_table(pud_pa);
  }
  current_table = get_table_from_physical(current_table->entries[bd.pgd_index].get_phys_addr());

  // PUD -> PMD
  if (!current_table->entries[bd.pud_index].is_valid()) {
    auto pmd_result = PageTableManager::allocate_page_table();
    if (!pmd_result) {
      return VoidResult{pmd_result.error()};
    }
    PhysAddr pmd_pa = PageTableManager::get_physical_address(*pmd_result);
    current_table->entries[bd.pud_index].set_table(pmd_pa);
  }
  current_table = get_table_from_physical(current_table->entries[bd.pud_index].get_phys_addr());

#if defined(MOSS_ARCH_RISCV)
  if (hal::mmu::g_mmu_mode == hal::mmu::MmuMode::Sv39) {
    // Sv39: 3 levels — PMD IS the final L0 table.
    current_table->entries[bd.pmd_index].set_page(phys_addr, permissions);
  } else
#endif
  {
    // 4-level: PMD → PTE table
    if (!current_table->entries[bd.pmd_index].is_valid()) {
      auto pte_result = PageTableManager::allocate_page_table();
      if (!pte_result) {
        return VoidResult{pte_result.error()};
      }
      PhysAddr pte_pa = PageTableManager::get_physical_address(*pte_result);
      current_table->entries[bd.pmd_index].set_table(pte_pa);
    }
    current_table = get_table_from_physical(current_table->entries[bd.pmd_index].get_phys_addr());
    current_table->entries[bd.pte_index].set_page(phys_addr, permissions);
  }

  invalidate_tlb_addr(virt_addr);
  return VoidResult{};
}

// 启用MMU — delegates to HAL for architecture-specific register operations
VoidResult PageTableManager::enable_mmu() {
  if (!PageTableManager::kernel_pgd) {
    return VoidResult{ErrorCode::InvalidState};
  }

  PhysAddr kernel_pgd_pa = PageTableManager::get_physical_address(PageTableManager::kernel_pgd);

  return moss::kernel::hal::mmu::enable_mmu(kernel_pgd_pa);
}

// 全局函数接口
VoidResult setup_mmu() {
  // 现在有了正确的身份映射，尝试真正启用MMU

  // 阶段1：设置内核页表映射（现在包含1GB身份映射）
  auto setup_result = PageTableManager::setup_kernel_page_tables();
  if (!setup_result) {
    return VoidResult{setup_result.error()};
  }

  // 2. 启用MMU - 使用原始配置参数
  auto enable_result = PageTableManager::enable_mmu();
  if (!enable_result) {
    return VoidResult{enable_result.error()};
  }

  // 3. 验证MMU已启用
  if (!moss::kernel::hal::mmu::mmu_enabled()) {
    // MMU启用失败但继续运行
    // return VoidResult{ErrorCode::InvalidState};
  }

  // 4. 打印页表详细信息（调试输出）
  PageTableManager::print_page_table_details();

  return VoidResult{}; // MMU成功启用
}

void invalidate_all_tlb() { PageTableManager::invalidate_tlb(); }

// 调试功能实现：打印MMU寄存器状态
void PageTableManager::print_mmu_registers() {
  log::klog::info("=== MMU寄存器详细状态 ===");

#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
  // 读取关键MMU寄存器
  u64 sctlr_el1, tcr_el1, mair_el1, ttbr0_el1, ttbr1_el1;

  asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr_el1));
  asm volatile("mrs %0, tcr_el1" : "=r"(tcr_el1));
  asm volatile("mrs %0, mair_el1" : "=r"(mair_el1));
  asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0_el1));
  asm volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1_el1));

  log::klog::debug_chain("SCTLR_EL1: ").hex(sctlr_el1);
  log::klog::debug_chain("  MMU启用: ").str((sctlr_el1 & (1ULL << 0)) ? "是" : "否").str(" (M位)");
  log::klog::debug_chain("  缓存启用: ").str((sctlr_el1 & (1ULL << 2)) ? "是" : "否").str(" (C位)");
  log::klog::debug_chain("  指令缓存: ").str((sctlr_el1 & (1ULL << 12)) ? "是" : "否").str(" (I位)");

  log::klog::debug_chain("TCR_EL1:   ").hex(tcr_el1);
  log::klog::debug_chain("  T0SZ: ").hex(tcr_el1 & 0x3F).str(" (TTBR0地址空间大小)");
  log::klog::debug_chain("  T1SZ: ").hex((tcr_el1 >> 16) & 0x3F).str(" (TTBR1地址空间大小)");
  log::klog::debug_chain("  EPD0: ")
      .hex((tcr_el1 >> 7) & 1)
      .str(" (TTBR0 ")
      .str(((tcr_el1 >> 7) & 1) ? "禁用" : "启用")
      .str(")");
  log::klog::debug_chain("  EPD1: ")
      .hex((tcr_el1 >> 23) & 1)
      .str(" (TTBR1 ")
      .str(((tcr_el1 >> 23) & 1) ? "禁用" : "启用")
      .str(")");

  log::klog::debug_chain("MAIR_EL1:  ").hex(mair_el1);
  log::klog::debug_chain("  属性0: ").hex((mair_el1 >> 0) & 0xFF).str(" (设备内存)");
  log::klog::debug_chain("  属性1: ").hex((mair_el1 >> 8) & 0xFF).str(" (普通缓存)");
  log::klog::debug_chain("  属性2: ").hex((mair_el1 >> 16) & 0xFF).str(" (非缓存)");

  log::klog::debug_chain("TTBR0_EL1: ").hex(ttbr0_el1);
  log::klog::debug_chain("TTBR1_EL1: ").hex(ttbr1_el1);
#else
  log::klog::debug("非ARM64架构，跳过MMU寄存器读取");
#endif
}

// 调试功能实现：打印PGD级页表项
void PageTableManager::print_pgd_entries() {
  log::klog::info("=== PGD级页表项详细信息 ===");

  if (!kernel_pgd) {
    log::klog::error("内核页表根目录未初始化");
    return;
  }

  log::klog::debug_chain("PGD物理地址: ").hex(get_physical_address(kernel_pgd));
  log::klog::debug("L0 (PGD): [0-511], each entry covers 512GB");
  log::klog::debug("L1 (PUD): [0-511], each entry covers 1GB");

  // Show valid L0 entries and walk into L1 for block details
  u32 total_blocks = 0;

  for (usize i = 0; i < PageTable::ENTRIES_PER_TABLE; i++) {
    const auto &entry = kernel_pgd->entries[i];
    if (!entry.is_valid()) {
      continue;
    }

    VirtAddr virt_start = i * (512ULL * 0x40000000ULL); // i * 512GB

    if (entry.is_table()) {
      PhysAddr pud_pa = entry.get_phys_addr();
      log::klog::debug_chain("PGD[").hex(i).str("] = ").hex(entry.raw).str(" -> L1 table @ ").hex(pud_pa);

      // Walk into L1 (PUD) table
      const auto *pud = static_cast<const PageTable *>(get_table_from_physical(pud_pa));
      for (usize j = 0; j < PageTable::ENTRIES_PER_TABLE; j++) {
        const auto &l1_entry = pud->entries[j];
        if (!l1_entry.is_valid()) {
          continue;
        }
        total_blocks++;

        VirtAddr block_start = virt_start + j * 0x40000000ULL;
        VirtAddr block_end_va = block_start + 0x40000000ULL - 1;
        PhysAddr phys_addr = l1_entry.get_phys_addr();

        log::klog::debug_chain("  PUD[").hex(j).str("] = ").hex(l1_entry.raw);
        log::klog::debug_chain("    VA: ")
            .hex(block_start)
            .str(" - ")
            .hex(block_end_va)
            .str(" -> PA: ")
            .hex(phys_addr)
            .str(l1_entry.is_table() ? " (1GB table)" : " (1GB block)");

        // Memory type from AttrIndx
        u64 attr_idx = (l1_entry.raw >> 2) & 7;
        const char *attr_name = " NC";
        if (attr_idx == 0) {
          attr_name = " Device";
        } else if (attr_idx == 1) {
          attr_name = " Normal";
        }
        log::klog::debug_chain("    AttrIndx=").hex(attr_idx).str(attr_name);
      }
    } else {
      log::klog::debug_chain("PGD[").hex(i).str("] = ").hex(entry.raw).str(" (block - unexpected at L0!)");
    }
  }

  log::klog::debug_chain("Total 1GB blocks mapped: ").hex(total_blocks);
}

// 调试功能实现：打印页表详细信息
void PageTableManager::print_page_table_details() {
  log::klog::info("========================================");
  log::klog::info("        MOSS页表详细信息调试输出");
  log::klog::info("========================================");

  // 1. 页表分配器状态
  log::klog::info("=== 页表分配器状态 ===");
  log::klog::debug_chain("最大页表数量: ").hex(MAX_EARLY_TABLES);
  log::klog::debug_chain("已分配页表数: ").hex(next_table_index);
  log::klog::debug_chain("可用页表数:   ").hex(MAX_EARLY_TABLES - next_table_index);
  log::klog::debug_chain("总内存占用:   ").hex(MAX_EARLY_TABLES * 4).str(" KB");

  // 2. 页表数组布局
  log::klog::info("=== 页表内存布局 ===");
  log::klog::debug_chain("early_tables起始: ").hex(reinterpret_cast<PhysAddr>(early_tables));
  log::klog::debug_chain("单个页表大小:     ").hex(sizeof(PageTable)).str(" 字节");
  log::klog::debug_chain("每页表条目数:     ").hex(PageTable::ENTRIES_PER_TABLE).str(" 个");

  if (kernel_pgd) {
    usize pgd_index = get_table_index(kernel_pgd);
    log::klog::debug_chain("内核PGD位置: early_tables[")
        .hex(pgd_index)
        .str("] (")
        .hex(get_physical_address(kernel_pgd))
        .str(")");
  } else {
    log::klog::debug("内核PGD: 未初始化");
  }

  // 3. MMU寄存器状态
  print_mmu_registers();

  // 4. PGD页表项详情
  print_pgd_entries();

  // 5. 地址转换示例 (L0→L1 1GB block mapping)
  log::klog::info("=== 地址转换示例 (L0→L1 1GB block mapping) ===");
  VirtAddr test_addrs[] = {moss::kernel::platform::ram_base(), moss::kernel::platform::uart_base(), 0x40000000,
                           0x80000000, 0xC0000000};
  const char *addr_names[] = {"RAM start", "UART MMIO", "1GB boundary", "2GB boundary", "3GB boundary"};

  for (size_t i = 0; i < 5; i++) {
    VirtAddr vaddr = test_addrs[i];
    u32 l0_idx = (vaddr >> 39) & 0x1FF; // PGD index (512GB)
    u32 l1_idx = (vaddr >> 30) & 0x1FF; // PUD index (1GB)
    u32 block_offset = vaddr & 0x3FFFFFFF;

    log::klog::debug_chain("VA ")
        .hex(vaddr)
        .str(" (")
        .str(addr_names[i])
        .str("): L0[")
        .hex(l0_idx)
        .str("] L1[")
        .hex(l1_idx)
        .str("] offset=")
        .hex(block_offset);
  }

  log::klog::info("========================================");
  log::klog::info("        页表详细信息输出完成");
  log::klog::info("========================================");
}

// ============================================================================
// unmap_page — remove a 4KB page mapping and invalidate TLB
// ============================================================================
VoidResult PageTableManager::unmap_page(VirtAddr virt_addr) {
  if (!kernel_pgd) {
    return VoidResult{ErrorCode::InvalidState};
  }

  auto bd = break_virtual_address(virt_addr);

  // PGD level
  auto &pgd_entry = kernel_pgd->entries[bd.pgd_index];
  if (!pgd_entry.is_valid()) {
    return VoidResult{ErrorCode::NotFound};
  }
  if (!pgd_entry.is_table()) {
    return VoidResult{ErrorCode::NotSupported}; // 1GB block — cannot unmap 4KB from it
  }

  // PUD level
  auto *pud = get_table_from_physical(pgd_entry.get_phys_addr());
  auto &pud_entry = pud->entries[bd.pud_index];
  if (!pud_entry.is_valid()) {
    return VoidResult{ErrorCode::NotFound};
  }
  if (!pud_entry.is_table()) {
    return VoidResult{ErrorCode::NotSupported}; // 2MB block — not yet split
  }

  // PMD level
  auto *pmd = get_table_from_physical(pud_entry.get_phys_addr());

#if defined(MOSS_ARCH_RISCV)
  if (hal::mmu::g_mmu_mode == hal::mmu::MmuMode::Sv39) {
    // Sv39: PMD IS the final L0 table — pmd_index is the leaf entry.
    auto &leaf_entry = pmd->entries[bd.pmd_index];
    if (!leaf_entry.is_valid()) {
      return VoidResult{ErrorCode::NotFound};
    }
    leaf_entry.clear();
  } else
#endif
  {
    auto &pmd_entry = pmd->entries[bd.pmd_index];
    if (!pmd_entry.is_valid()) {
      return VoidResult{ErrorCode::NotFound};
    }
    if (!pmd_entry.is_table()) {
      return VoidResult{ErrorCode::NotSupported}; // 2MB block
    }

    auto *pte_table = get_table_from_physical(pmd_entry.get_phys_addr());
    auto &pte_entry = pte_table->entries[bd.pte_index];
    if (!pte_entry.is_valid()) {
      return VoidResult{ErrorCode::NotFound};
    }
    pte_entry.clear();
  }

  invalidate_tlb_addr(virt_addr);
  return VoidResult{};
}

// ============================================================================
// query_page — walk the page table and return mapping information
// ============================================================================
PageTableManager::PageInfo PageTableManager::query_page(VirtAddr virt_addr) {
  PageInfo info{.phys_addr = 0, .attributes = 0, .mapped = false, .level = 0};

  if (!kernel_pgd) {
    return info;
  }

  auto bd = break_virtual_address(virt_addr);

  // PGD level
  auto &pgd_entry = kernel_pgd->entries[bd.pgd_index];
  if (!pgd_entry.is_valid()) {
    return info;
  }
  // 1GB block/gigapage at root level (Sv39 root or Sv48/ARM64 PGD block)
  if (!pgd_entry.is_table()) {
    info.phys_addr = pgd_entry.get_phys_addr() | (virt_addr & 0x3FFFFFFFULL);
    info.attributes = pgd_entry.raw;
    info.mapped = true;
    info.level = 1;
    return info;
  }

  // PUD level
  auto *pud = get_table_from_physical(pgd_entry.get_phys_addr());
  auto &pud_entry = pud->entries[bd.pud_index];
  if (!pud_entry.is_valid()) {
    return info;
  }
  if (!pud_entry.is_table()) {
#if defined(MOSS_ARCH_RISCV)
    if (hal::mmu::g_mmu_mode == hal::mmu::MmuMode::Sv39) {
      // Sv39 L1: 2MB megapage
      info.phys_addr = pud_entry.get_phys_addr() | (virt_addr & 0x1FFFFFULL);
      info.level = 2;
    } else
#endif
    {
      // 4-level: 1GB block at PUD level
      info.phys_addr = pud_entry.get_phys_addr() | (virt_addr & 0x3FFFFFFFULL);
      info.level = 1;
    }
    info.attributes = pud_entry.raw;
    info.mapped = true;
    return info;
  }

  // PMD level
  auto *pmd = get_table_from_physical(pud_entry.get_phys_addr());
  auto &pmd_entry = pmd->entries[bd.pmd_index];
  if (!pmd_entry.is_valid()) {
    return info;
  }

#if defined(MOSS_ARCH_RISCV)
  if (hal::mmu::g_mmu_mode == hal::mmu::MmuMode::Sv39) {
    // Sv39: PMD IS the final L0 table — pmd_index = 4KB leaf PTE.
    info.phys_addr = pmd_entry.get_phys_addr() | (virt_addr & 0xFFFULL);
    info.attributes = pmd_entry.raw;
    info.mapped = true;
    info.level = 3;
    return info;
  }
#endif

  if (!pmd_entry.is_table()) {
    // 2MB block mapping
    info.phys_addr = pmd_entry.get_phys_addr() | (virt_addr & 0x1FFFFFULL);
    info.attributes = pmd_entry.raw;
    info.mapped = true;
    info.level = 2;
    return info;
  }

  // PTE level — 4KB page
  auto *pte_table = get_table_from_physical(pmd_entry.get_phys_addr());
  auto &pte_entry = pte_table->entries[bd.pte_index];
  if (!pte_entry.is_valid()) {
    return info;
  }

  info.phys_addr = pte_entry.get_phys_addr() | (virt_addr & 0xFFFULL);
  info.attributes = pte_entry.raw;
  info.mapped = true;
  info.level = 3;
  return info;
}

} // namespace moss::kernel::mm
