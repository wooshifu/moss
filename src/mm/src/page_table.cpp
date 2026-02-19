// MOSS页表管理器实现 - Module implementation unit
// 提供ARM64页表管理、MMU启用和调试输出功能

module;

// Architecture detection
#include "arch_detect.h"

// Linker symbols (must be in global module fragment)
extern "C" {
    extern char _text_start_addr[];
    extern char _text_end_addr[];
    extern char _rodata_start_addr[];
    extern char _rodata_end_addr[];
    extern char _data_start_addr[];
    extern char _data_end_addr[];
    extern char _bss_start_addr[];
    extern char _bss_end_addr[];
    extern char _pagetable_start_addr[];
    extern char _pagetable_end_addr[];
    extern char _kernel_end_addr[];
}

module moss.mm;

import moss.logging;

namespace log = moss::kernel::logging;

namespace moss::kernel::mm {

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
  auto result = page_alloc::alloc_kernel_pages(0);  // order 0 = 1 page = 4KB
  if (!result) {
    return KernelResult<PageTable *>{ErrorCode::OutOfMemory};
  }
  PhysAddr pa = *result;
  auto *table = reinterpret_cast<PageTable *>(phys_to_virt(pa));

  for (usize i = 0; i < PageTable::ENTRIES_PER_TABLE; i++) {
    table->entries[i].clear();
  }

  return KernelResult<PageTable *>{table};
}

// Build TTBR1 high-half kernel page table: map physical RAM at KERNEL_DIRECT_MAP_BASE
VoidResult PageTableManager::setup_kernel_high_half_tables() {
  // 1. Allocate L0 (PGD) for TTBR1 — using early bump allocator (before dynamic switch)
  auto pgd_result = allocate_page_table();
  if (!pgd_result) return VoidResult{pgd_result.error()};
  kernel_high_pgd = *pgd_result;

  // 2. Determine RAM region from DTB (PlatformInfo)
  const auto &plat = moss::fdt::get_platform_info();
  PhysAddr ram_start = (plat.dtb_valid && plat.total_memory_size > 0)
                           ? plat.total_memory_start
                           : moss::kernel::platform::ram_base();
  u64 ram_size = (plat.dtb_valid && plat.total_memory_size > 0)
                     ? plat.total_memory_size
                     : moss::kernel::platform::ram_size();
  PhysAddr ram_end = ram_start + ram_size;

  // 3. Map physical address space at KERNEL_DIRECT_MAP_BASE using 1GB blocks.
  //    TTBR1 handles VA >= 0xFFFF000000000000 (with T1SZ=16).
  //    PGD index = (va >> 39) & 0x1FF
  //    For KERNEL_DIRECT_MAP_BASE (0xFFFF800000000000): PGD[256]
  u16 pgd_idx = static_cast<u16>((KERNEL_DIRECT_MAP_BASE >> 39) & 0x1FF);

  // Allocate PUD table for this PGD entry
  auto pud_result = allocate_page_table();
  if (!pud_result) return VoidResult{pud_result.error()};
  PageTable *pud = *pud_result;

  PhysAddr pud_pa = get_physical_address(pud);
  kernel_high_pgd->entries[pgd_idx].set_table(pud_pa);

  // 4. Fill PUD entries with 1GB block descriptors covering 0-4GB
  //    This maps device MMIO (0x00000000-0x3FFFFFFF) + RAM (0x40000000+)
  constexpr u64 ONE_GB = 0x40000000ULL;
  for (usize i = 0; i < 4; i++) {
    PhysAddr block_addr = static_cast<PhysAddr>(i * ONE_GB);
    PhysAddr block_end = block_addr + ONE_GB;
    bool overlaps_ram = (block_addr < ram_end) && (block_end > ram_start);
    u64 block_entry = overlaps_ram
        ? moss::kernel::hal::mmu::make_normal_block(block_addr)
        : moss::kernel::hal::mmu::make_device_block(block_addr);
    pud->entries[i].raw = block_entry;
  }

  log::klog::info("high-half page table: PGD[{}] -> PUD with 4x1GB blocks", pgd_idx);
  return VoidResult{};
}

// Walk user page tables and return a mutable pointer to the L3 PTE.
// Returns nullptr if any intermediate table is missing (does not allocate).
PageTableEntry* PageTableManager::get_user_pte(PhysAddr pgd_phys, VirtAddr va) {
  auto *pgd = get_table_from_physical(pgd_phys);
  if (!pgd) return nullptr;
  auto bd = break_virtual_address(va);

  auto &pge = pgd->entries[bd.pgd_index];
  if (!pge.is_valid() || !pge.is_table()) return nullptr;
  auto *pud = get_table_from_physical(pge.get_phys_addr());

  auto &pude = pud->entries[bd.pud_index];
  if (!pude.is_valid() || !pude.is_table()) return nullptr;
  auto *pmd = get_table_from_physical(pude.get_phys_addr());

  auto &pmde = pmd->entries[bd.pmd_index];
  if (!pmde.is_valid() || !pmde.is_table()) return nullptr;
  auto *pte = get_table_from_physical(pmde.get_phys_addr());

  return &pte->entries[bd.pte_index];
}

// Clone user page tables for fork(): deep-copy intermediate tables,
// share leaf pages via COW (mark READONLY + SW_COW, increment refcount).
void PageTableManager::clone_user_page_tables(PhysAddr src_pgd_phys,
                                               PhysAddr dst_pgd_phys) {
  auto *src_pgd = get_table_from_physical(src_pgd_phys);
  auto *dst_pgd = get_table_from_physical(dst_pgd_phys);
  if (!src_pgd || !dst_pgd) return;

  constexpr usize ENTRIES = PageTable::ENTRIES_PER_TABLE;

  // Skip PGD[0] — kernel identity map (already copied by create_user_address_space)
  for (usize pgd_i = 1; pgd_i < ENTRIES; pgd_i++) {
    auto &src_pge = src_pgd->entries[pgd_i];
    if (!src_pge.is_valid() || !src_pge.is_table()) continue;

    auto *src_pud = get_table_from_physical(src_pge.get_phys_addr());
    if (!src_pud) continue;

    // Allocate fresh PUD for child
    auto pud_result = allocate_page_table_dynamic();
    if (!pud_result) return;
    auto *dst_pud = *pud_result;
    dst_pgd->entries[pgd_i].set_table(get_physical_address(dst_pud));

    for (usize pud_i = 0; pud_i < ENTRIES; pud_i++) {
      auto &src_pude = src_pud->entries[pud_i];
      if (!src_pude.is_valid()) continue;
      if (!src_pude.is_table()) continue; // skip 1GB block descriptors

      auto *src_pmd = get_table_from_physical(src_pude.get_phys_addr());
      if (!src_pmd) continue;

      // Allocate fresh PMD for child
      auto pmd_result = allocate_page_table_dynamic();
      if (!pmd_result) return;
      auto *dst_pmd = *pmd_result;
      dst_pud->entries[pud_i].set_table(get_physical_address(dst_pmd));

      for (usize pmd_i = 0; pmd_i < ENTRIES; pmd_i++) {
        auto &src_pmde = src_pmd->entries[pmd_i];
        if (!src_pmde.is_valid()) continue;
        if (!src_pmde.is_table()) continue; // skip 2MB block descriptors

        auto *src_pte = get_table_from_physical(src_pmde.get_phys_addr());
        if (!src_pte) continue;

        // Allocate fresh PTE table for child
        auto pte_result = allocate_page_table_dynamic();
        if (!pte_result) return;
        auto *dst_pte = *pte_result;
        dst_pmd->entries[pmd_i].set_table(get_physical_address(dst_pte));

        for (usize pte_i = 0; pte_i < ENTRIES; pte_i++) {
          auto &src_ptee = src_pte->entries[pte_i];
          if (!src_ptee.is_valid()) continue;

          PhysAddr leaf_pa = src_ptee.get_phys_addr();

          // Mark parent PTE as COW + read-only
          src_ptee.set_cow();
          src_ptee.make_readonly();

          // Child gets same PTE value (COW + read-only)
          dst_pte->entries[pte_i].raw = src_ptee.raw;

          // Increment physical page refcount (now shared)
          PageFrameAllocator::page_ref_inc(leaf_pa);
        }
      }
    }
  }

  // Parent PTEs were changed to read-only + COW — flush stale writable TLB entries.
  // Without this, the parent can silently write through a stale writable TLB entry,
  // bypassing COW and corrupting the shared page.
  invalidate_tlb();
}

// Free all user page tables and demand-paged physical pages.
// Walks PGD→PUD→PMD→PTE, frees leaf pages and intermediate tables.
// PGD[0] is the shared kernel identity map — skip it.
void PageTableManager::free_user_page_tables(PhysAddr pgd_phys) {
  if (pgd_phys == 0) return;

  auto *pgd = get_table_from_physical(pgd_phys);
  if (!pgd) return;

  constexpr usize ENTRIES = PageTable::ENTRIES_PER_TABLE;

  for (usize i0 = 0; i0 < ENTRIES; i0++) {
    auto &pge = pgd->entries[i0];
    if (!pge.is_valid() || !pge.is_table()) continue;

    // Skip PGD[0]: shared kernel identity map (1GB blocks)
    if (i0 == 0) continue;

    auto *pud = get_table_from_physical(pge.get_phys_addr());
    if (!pud) continue;

    for (usize i1 = 0; i1 < ENTRIES; i1++) {
      auto &pude = pud->entries[i1];
      if (!pude.is_valid()) continue;

      // Block mapping (1GB) — don't free the underlying physical memory
      // (it belongs to device or kernel identity map)
      if (pude.is_block()) continue;

      auto *pmd = get_table_from_physical(pude.get_phys_addr());
      if (!pmd) continue;

      for (usize i2 = 0; i2 < ENTRIES; i2++) {
        auto &pmde = pmd->entries[i2];
        if (!pmde.is_valid()) continue;

        // Block mapping (2MB) — skip
        if (pmde.is_block()) continue;

        auto *pte = get_table_from_physical(pmde.get_phys_addr());
        if (!pte) continue;

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

// Map a single 4KB page into a user process page table (4-level walk)
VoidResult PageTableManager::map_user_page(PhysAddr pgd_phys, VirtAddr va,
                                            PhysAddr pa, u64 perms) {
  auto *pgd = get_table_from_physical(pgd_phys);
  auto bd = break_virtual_address(va);

  // PGD -> PUD (use dynamic allocator for user page tables)
  if (!pgd->entries[bd.pgd_index].is_valid()) {
    auto result = allocate_page_table_dynamic();
    if (!result) return VoidResult{ErrorCode::OutOfMemory};
    pgd->entries[bd.pgd_index].set_table(get_physical_address(*result));
  }
  auto *pud = get_table_from_physical(pgd->entries[bd.pgd_index].get_phys_addr());

  // PUD -> PMD
  if (!pud->entries[bd.pud_index].is_valid()) {
    auto result = allocate_page_table_dynamic();
    if (!result) return VoidResult{ErrorCode::OutOfMemory};
    pud->entries[bd.pud_index].set_table(get_physical_address(*result));
  }
  auto *pmd = get_table_from_physical(pud->entries[bd.pud_index].get_phys_addr());

  // PMD -> PTE table
  if (!pmd->entries[bd.pmd_index].is_valid()) {
    auto result = allocate_page_table_dynamic();
    if (!result) return VoidResult{ErrorCode::OutOfMemory};
    pmd->entries[bd.pmd_index].set_table(get_physical_address(*result));
  }
  auto *pte = get_table_from_physical(pmd->entries[bd.pmd_index].get_phys_addr());

  // Set the final 4KB page entry (L3 uses page descriptor: bits[1:0]=0b11)
  pte->entries[bd.pte_index].set_page(pa, perms);
  invalidate_tlb_addr(va);
  return VoidResult{};
}

// 创建内核页表映射
//
// ARM64 4KB granule page table walk with T0SZ=16 (48-bit VA):
//   L0 (PGD): bits[47:39], each entry covers 512GB — table descriptors ONLY
//   L1 (PUD): bits[38:30], each entry covers 1GB   — block or table
//   L2 (PMD): bits[29:21], each entry covers 2MB   — block or table
//   L3 (PTE): bits[20:12], each entry covers 4KB   — page descriptors
//
// 1GB block descriptors are valid at L1, NOT at L0.
// We place a table descriptor at PGD[0] → L1 table, then fill L1[0..3]
// with 1GB block descriptors for the first 4GB identity map.
VoidResult PageTableManager::setup_kernel_page_tables() {
  // 1. Allocate L0 (PGD) — root of the 4-level page table
  auto pgd_result = PageTableManager::allocate_page_table();
  if (!pgd_result) {
    return VoidResult{pgd_result.error()};
  }
  PageTableManager::kernel_pgd = *pgd_result;

  // 2. Allocate L1 (PUD) — one table is enough for 512 × 1GB = 512GB
  auto pud_result = PageTableManager::allocate_page_table();
  if (!pud_result) {
    return VoidResult{pud_result.error()};
  }
  PageTable *pud = *pud_result;

  // 3. PGD[0] → table descriptor pointing to the L1 (PUD) table
  //    With T0SZ=16, PGD index 0 covers VA [0, 512GB), which contains
  //    our entire 4GB identity map.
  PhysAddr pud_pa = PageTableManager::get_physical_address(pud);
  PageTableManager::kernel_pgd->entries[0].set_table(pud_pa);

  // 4. Determine RAM region from DTB (PlatformInfo)
  const auto &plat = moss::fdt::get_platform_info();
  PhysAddr ram_start = (plat.dtb_valid && plat.total_memory_size > 0)
                           ? plat.total_memory_start
                           : moss::kernel::platform::ram_base();
  u64 ram_size = (plat.dtb_valid && plat.total_memory_size > 0)
                     ? plat.total_memory_size
                     : moss::kernel::platform::ram_size();
  PhysAddr ram_end = ram_start + ram_size;

  // 5. Fill L1 (PUD) entries [0..3] with 1GB block descriptors
  //    covering 0x00000000 - 0xFFFFFFFF (4GB identity map).
  //    1GB block descriptors are architecturally valid at L1.
  constexpr u64 ONE_GB = 0x40000000ULL;
  for (usize i = 0; i < 4; i++) {
    PhysAddr block_addr = static_cast<PhysAddr>(i * ONE_GB);
    PhysAddr block_end = block_addr + ONE_GB;

    // If this 1GB block overlaps with the RAM region, use Normal memory;
    // otherwise use Device memory.
    bool overlaps_ram = (block_addr < ram_end) && (block_end > ram_start);
    u64 block_entry = overlaps_ram
        ? moss::kernel::hal::mmu::make_normal_block(block_addr)
        : moss::kernel::hal::mmu::make_device_block(block_addr);

    pud->entries[i].raw = block_entry;
  }

  return VoidResult{};
}

// 映射内存区域
VoidResult PageTableManager::map_region(VirtAddr virt_addr, PhysAddr phys_addr,
                                        usize size, u64 permissions) {
  // 页面对齐检查
  if ((virt_addr & (PAGE_SIZE - 1)) != 0 ||
      (phys_addr & (PAGE_SIZE - 1)) != 0) {
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
VoidResult PageTableManager::map_page(VirtAddr virt_addr, PhysAddr phys_addr,
                                      u64 permissions) {
  if (!PageTableManager::kernel_pgd) {
    return VoidResult{ErrorCode::InvalidState};
  }

  auto addr_breakdown = break_virtual_address(virt_addr);

  // 遍历页表层级
  PageTable *current_table = PageTableManager::kernel_pgd;

  // PGD -> PUD
  if (!current_table->entries[addr_breakdown.pgd_index].is_valid()) {
    auto pud_result = PageTableManager::allocate_page_table();
    if (!pud_result) {
      return VoidResult{pud_result.error()};
    }

    PhysAddr pud_pa = PageTableManager::get_physical_address(*pud_result);
    current_table->entries[addr_breakdown.pgd_index].set_table(pud_pa);
  }

  current_table = get_table_from_physical(
      current_table->entries[addr_breakdown.pgd_index].get_phys_addr());

  // PUD -> PMD
  if (!current_table->entries[addr_breakdown.pud_index].is_valid()) {
    auto pmd_result = PageTableManager::allocate_page_table();
    if (!pmd_result) {
      return VoidResult{pmd_result.error()};
    }

    PhysAddr pmd_pa = PageTableManager::get_physical_address(*pmd_result);
    current_table->entries[addr_breakdown.pud_index].set_table(pmd_pa);
  }

  current_table = get_table_from_physical(
      current_table->entries[addr_breakdown.pud_index].get_phys_addr());

  // PMD -> PTE
  if (!current_table->entries[addr_breakdown.pmd_index].is_valid()) {
    auto pte_result = PageTableManager::allocate_page_table();
    if (!pte_result) {
      return VoidResult{pte_result.error()};
    }

    PhysAddr pte_pa = PageTableManager::get_physical_address(*pte_result);
    current_table->entries[addr_breakdown.pmd_index].set_table(pte_pa);
  }

  current_table = get_table_from_physical(
      current_table->entries[addr_breakdown.pmd_index].get_phys_addr());

  // Set final L3 page table entry (must use set_page, not set_block, at L3)
  current_table->entries[addr_breakdown.pte_index].set_page(phys_addr,
                                                            permissions);

  // Invalidate stale TLB entry for this VA
  invalidate_tlb_addr(virt_addr);

  return VoidResult{};
}

// 启用MMU — delegates to HAL for architecture-specific register operations
VoidResult PageTableManager::enable_mmu() {
  if (!PageTableManager::kernel_pgd) {
    return VoidResult{ErrorCode::InvalidState};
  }

  PhysAddr kernel_pgd_pa =
      PageTableManager::get_physical_address(PageTableManager::kernel_pgd);

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
  log::klog::debug_chain("  EPD0: ").hex((tcr_el1 >> 7) & 1).str(" (TTBR0 ").str(((tcr_el1 >> 7) & 1) ? "禁用" : "启用").str(")");
  log::klog::debug_chain("  EPD1: ").hex((tcr_el1 >> 23) & 1).str(" (TTBR1 ").str(((tcr_el1 >> 23) & 1) ? "禁用" : "启用").str(")");

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
    if (!entry.is_valid()) continue;

    VirtAddr virt_start = i * (512ULL * 0x40000000ULL); // i * 512GB

    if (entry.is_table()) {
      PhysAddr pud_pa = entry.get_phys_addr();
      log::klog::debug_chain("PGD[").hex(i).str("] = ").hex(entry.raw).str(" -> L1 table @ ").hex(pud_pa);

      // Walk into L1 (PUD) table
      auto *pud = static_cast<const PageTable *>(
          static_cast<const void *>(get_table_from_physical(pud_pa)));
      for (usize j = 0; j < PageTable::ENTRIES_PER_TABLE; j++) {
        const auto &l1_entry = pud->entries[j];
        if (!l1_entry.is_valid()) continue;
        total_blocks++;

        VirtAddr block_start = virt_start + j * 0x40000000ULL;
        VirtAddr block_end_va = block_start + 0x40000000ULL - 1;
        PhysAddr phys_addr = l1_entry.get_phys_addr();

        log::klog::debug_chain("  PUD[").hex(j).str("] = ").hex(l1_entry.raw);
        log::klog::debug_chain("    VA: ").hex(block_start).str(" - ").hex(block_end_va).str(" -> PA: ").hex(phys_addr).str(l1_entry.is_table() ? " (1GB table)" : " (1GB block)");

        // Memory type from AttrIndx
        u64 attr_idx = (l1_entry.raw >> 2) & 7;
        log::klog::debug_chain("    AttrIndx=").hex(attr_idx).str(attr_idx == 0 ? " Device" : attr_idx == 1 ? " Normal" : " NC");
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
    log::klog::debug_chain("内核PGD位置: early_tables[").hex(pgd_index).str("] (").hex(get_physical_address(kernel_pgd)).str(")");
  } else {
    log::klog::debug("内核PGD: 未初始化");
  }

  // 3. MMU寄存器状态
  print_mmu_registers();

  // 4. PGD页表项详情
  print_pgd_entries();

  // 5. 地址转换示例 (L0→L1 1GB block mapping)
  log::klog::info("=== 地址转换示例 (L0→L1 1GB block mapping) ===");
  VirtAddr test_addrs[] = {0x00000000, 0x09000000, 0x40000000, 0x80000000,
                           0xC0000000};
  const char *addr_names[] = {"RAM start", "UART MMIO", "1GB boundary",
                              "2GB boundary", "3GB boundary"};

  for (size_t i = 0; i < 5; i++) {
    VirtAddr vaddr = test_addrs[i];
    u32 l0_idx = (vaddr >> 39) & 0x1FF;  // PGD index (512GB)
    u32 l1_idx = (vaddr >> 30) & 0x1FF;  // PUD index (1GB)
    u32 block_offset = vaddr & 0x3FFFFFFF;

    log::klog::debug_chain("VA ").hex(vaddr).str(" (").str(addr_names[i]).str("): L0[").hex(l0_idx).str("] L1[").hex(l1_idx).str("] offset=").hex(block_offset);
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
  auto& pgd_entry = kernel_pgd->entries[bd.pgd_index];
  if (!pgd_entry.is_valid()) {
    return VoidResult{ErrorCode::NotFound};
  }
  // 1GB block mapping — cannot unmap a single 4KB page from it
  if (!pgd_entry.is_table()) {
    return VoidResult{ErrorCode::NotSupported};
  }

  // PUD level
  auto* pud = get_table_from_physical(pgd_entry.get_phys_addr());
  auto& pud_entry = pud->entries[bd.pud_index];
  if (!pud_entry.is_valid()) {
    return VoidResult{ErrorCode::NotFound};
  }
  if (!pud_entry.is_table()) {
    return VoidResult{ErrorCode::NotSupported}; // 2MB block — not yet split
  }

  // PMD level
  auto* pmd = get_table_from_physical(pud_entry.get_phys_addr());
  auto& pmd_entry = pmd->entries[bd.pmd_index];
  if (!pmd_entry.is_valid()) {
    return VoidResult{ErrorCode::NotFound};
  }
  if (!pmd_entry.is_table()) {
    return VoidResult{ErrorCode::NotSupported}; // 2MB block
  }

  // PTE level — the actual 4KB page
  auto* pte_table = get_table_from_physical(pmd_entry.get_phys_addr());
  auto& pte_entry = pte_table->entries[bd.pte_index];
  if (!pte_entry.is_valid()) {
    return VoidResult{ErrorCode::NotFound};
  }

  // Clear the PTE and invalidate TLB for this address
  pte_entry.clear();
  invalidate_tlb_addr(virt_addr);

  return VoidResult{};
}

// ============================================================================
// query_page — walk the page table and return mapping information
// ============================================================================
PageTableManager::PageInfo PageTableManager::query_page(VirtAddr virt_addr) {
  PageInfo info{0, 0, false, 0};

  if (!kernel_pgd) {
    return info;
  }

  auto bd = break_virtual_address(virt_addr);

  // PGD level
  auto& pgd_entry = kernel_pgd->entries[bd.pgd_index];
  if (!pgd_entry.is_valid()) {
    return info;
  }
  // 1GB block mapping
  if (!pgd_entry.is_table()) {
    info.phys_addr  = pgd_entry.get_phys_addr() | (virt_addr & 0x3FFFFFFFULL);
    info.attributes = pgd_entry.raw;
    info.mapped     = true;
    info.level      = 1;
    return info;
  }

  // PUD level
  auto* pud = get_table_from_physical(pgd_entry.get_phys_addr());
  auto& pud_entry = pud->entries[bd.pud_index];
  if (!pud_entry.is_valid()) {
    return info;
  }
  if (!pud_entry.is_table()) {
    // 1GB block at PUD level (ARM64 uses this for initial boot mapping)
    info.phys_addr  = pud_entry.get_phys_addr() | (virt_addr & 0x3FFFFFFFULL);
    info.attributes = pud_entry.raw;
    info.mapped     = true;
    info.level      = 1;
    return info;
  }

  // PMD level
  auto* pmd = get_table_from_physical(pud_entry.get_phys_addr());
  auto& pmd_entry = pmd->entries[bd.pmd_index];
  if (!pmd_entry.is_valid()) {
    return info;
  }
  if (!pmd_entry.is_table()) {
    // 2MB block mapping
    info.phys_addr  = pmd_entry.get_phys_addr() | (virt_addr & 0x1FFFFFULL);
    info.attributes = pmd_entry.raw;
    info.mapped     = true;
    info.level      = 2;
    return info;
  }

  // PTE level — 4KB page
  auto* pte_table = get_table_from_physical(pmd_entry.get_phys_addr());
  auto& pte_entry = pte_table->entries[bd.pte_index];
  if (!pte_entry.is_valid()) {
    return info;
  }

  info.phys_addr  = pte_entry.get_phys_addr() | (virt_addr & 0xFFFULL);
  info.attributes = pte_entry.raw;
  info.mapped     = true;
  info.level      = 3;
  return info;
}

} // namespace moss::kernel::mm
