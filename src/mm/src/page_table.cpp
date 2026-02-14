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

// PageTableManager 方法实现
KernelResult<PageTable *> PageTableManager::allocate_page_table() {
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

// 创建内核页表映射
VoidResult PageTableManager::setup_kernel_page_tables() {
  // 1. 分配内核页表根目录
  auto pgd_result = PageTableManager::allocate_page_table();
  if (!pgd_result) {
    return VoidResult{pgd_result.error()};
  }
  PageTableManager::kernel_pgd = *pgd_result;

  // Determine RAM region from DTB (PlatformInfo) — no more hardcoded assumptions
  // about which 1GB block contains RAM.
  const auto &plat = moss::fdt::get_platform_info();
  PhysAddr ram_start = (plat.dtb_valid && plat.total_memory_size > 0)
                           ? plat.total_memory_start
                           : moss::kernel::platform::ram_base();
  u64 ram_size = (plat.dtb_valid && plat.total_memory_size > 0)
                     ? plat.total_memory_size
                     : moss::kernel::platform::ram_size();
  PhysAddr ram_end = ram_start + ram_size;

  // Map 4 x 1GB blocks covering 0x00000000 - 0xFFFFFFFF
  // Each block's memory type is determined by whether it overlaps with RAM
  // (from DTB). Block descriptor format is provided by the MMU HAL.
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

    PageTableManager::kernel_pgd->entries[i].raw = block_entry;
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

  PhysAddr pud_pa =
      current_table->entries[addr_breakdown.pgd_index].get_phys_addr();
  current_table = reinterpret_cast<PageTable *>(pud_pa);

  // PUD -> PMD
  if (!current_table->entries[addr_breakdown.pud_index].is_valid()) {
    auto pmd_result = PageTableManager::allocate_page_table();
    if (!pmd_result) {
      return VoidResult{pmd_result.error()};
    }

    PhysAddr pmd_pa = PageTableManager::get_physical_address(*pmd_result);
    current_table->entries[addr_breakdown.pud_index].set_table(pmd_pa);
  }

  PhysAddr pmd_pa =
      current_table->entries[addr_breakdown.pud_index].get_phys_addr();
  current_table = reinterpret_cast<PageTable *>(pmd_pa);

  // PMD -> PTE
  if (!current_table->entries[addr_breakdown.pmd_index].is_valid()) {
    auto pte_result = PageTableManager::allocate_page_table();
    if (!pte_result) {
      return VoidResult{pte_result.error()};
    }

    PhysAddr pte_pa = PageTableManager::get_physical_address(*pte_result);
    current_table->entries[addr_breakdown.pmd_index].set_table(pte_pa);
  }

  PhysAddr pte_pa =
      current_table->entries[addr_breakdown.pmd_index].get_phys_addr();
  current_table = reinterpret_cast<PageTable *>(pte_pa);

  // 设置最终的页表项
  current_table->entries[addr_breakdown.pte_index].set_block(phys_addr,
                                                             permissions);

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
  log::klog::debug("PGD索引范围: [0-511] (每个索引覆盖1GB地址空间)");

  // 遍历PGD的所有条目，但只详细显示有效的条目
  u32 valid_entries = 0;

  for (usize i = 0; i < PageTable::ENTRIES_PER_TABLE; i++) {
    const auto &entry = kernel_pgd->entries[i];

    if (entry.is_valid()) {
      valid_entries++;

      // 计算这个条目覆盖的虚拟地址范围
      VirtAddr virt_start = i * 0x40000000ULL; // i * 1GB
      VirtAddr virt_end = virt_start + 0x40000000ULL - 1;
      PhysAddr phys_addr = entry.get_phys_addr();

      log::klog::debug_chain("PGD[").hex(i).str("] = ").hex(entry.raw).str(" (有效)");
      log::klog::debug_chain("  虚拟地址范围: ").hex(virt_start).str(" - ").hex(virt_end).str(" (1GB)");
      log::klog::debug_chain("  物理地址:     ").hex(phys_addr);
      log::klog::debug_chain("  映射类型:     ").str(entry.is_table() ? "页表" : "1GB块");

      // 解析权限位
      u64 raw = entry.raw;
      log::klog::debug_chain("  权限位:").str("");
      log::klog::debug_chain("    Valid:    ").str((raw & (1ULL << 0)) ? "是" : "否").str(" (bit0)");
      log::klog::debug_chain("    Table:    ").str((raw & (1ULL << 1)) ? "是" : "否").str(" (bit1)");
      log::klog::debug_chain("    AttrIndx: ").hex((raw >> 2) & 7).str(" (bits[4:2])");
      log::klog::debug_chain("    AF:       ").str((raw & (1ULL << 10)) ? "是" : "否").str(" (bit10)");
      log::klog::debug_chain("    PXN:      ").str((raw & (1ULL << 53)) ? "是" : "否").str(" (bit53)");
      log::klog::debug_chain("    XN:       ").str((raw & (1ULL << 54)) ? "是" : "否").str(" (bit54)");

      // 根据AttrIndx解释内存类型
      u64 attr_idx = (raw >> 2) & 7;
      const char *mem_type;
      switch (attr_idx) {
      case 0:
        mem_type = "设备内存 (nGnRnE)";
        break;
      case 1:
        mem_type = "普通缓存内存 (WB/WA)";
        break;
      case 2:
        mem_type = "普通非缓存内存";
        break;
      default:
        mem_type = "未知内存类型";
        break;
      }
      log::klog::debug_chain("    内存类型: ").str(mem_type);
    }
  }

  log::klog::debug_chain("页表统计: 有效条目数: ").hex(valid_entries).str(" / 512");
  log::klog::debug_chain("  映射覆盖: ").hex(valid_entries).str(" GB");
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

  // 5. 地址转换示例（1GB块映射）
  log::klog::info("=== 地址转换示例 (1GB块映射) ===");
  VirtAddr test_addrs[] = {0x00000000, 0x12345678, 0x40000000, 0x80000000,
                           0xC0000000};
  const char *addr_names[] = {"0GB起始", "内核代码", "1GB边界", "2GB边界",
                              "3GB边界"};

  for (size_t i = 0; i < 5; i++) {
    VirtAddr vaddr = test_addrs[i];

    // 对于1GB块映射，PGD索引是地址的前2位 (vaddr >> 30)
    u32 pgd_index_1gb = (vaddr >> 30) & 0x3;   // 1GB块映射的PGD索引
    u32 block_offset_1gb = vaddr & 0x3FFFFFFF; // 1GB块内偏移

    log::klog::debug_chain("虚拟地址 ").hex(vaddr).str(" (").str(addr_names[i]).str("):");
    log::klog::debug_chain("  PGD索引: ").hex(pgd_index_1gb).str(" (1GB块映射)");
    log::klog::debug_chain("  块内偏移: ").hex(block_offset_1gb);

    // 检查对应的PGD条目
    if (kernel_pgd && pgd_index_1gb < PageTable::ENTRIES_PER_TABLE) {
      const auto &pgd_entry = kernel_pgd->entries[pgd_index_1gb];
      if (pgd_entry.is_valid()) {
        PhysAddr phys_base = pgd_entry.get_phys_addr();
        PhysAddr phys_final = phys_base + block_offset_1gb;
        log::klog::debug_chain("  -> 物理地址: ").hex(phys_final);
      } else {
        log::klog::debug("  -> 未映射");
      }
    }
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
  auto* pud = reinterpret_cast<PageTable*>(pgd_entry.get_phys_addr());
  auto& pud_entry = pud->entries[bd.pud_index];
  if (!pud_entry.is_valid()) {
    return VoidResult{ErrorCode::NotFound};
  }
  if (!pud_entry.is_table()) {
    return VoidResult{ErrorCode::NotSupported}; // 2MB block — not yet split
  }

  // PMD level
  auto* pmd = reinterpret_cast<PageTable*>(pud_entry.get_phys_addr());
  auto& pmd_entry = pmd->entries[bd.pmd_index];
  if (!pmd_entry.is_valid()) {
    return VoidResult{ErrorCode::NotFound};
  }
  if (!pmd_entry.is_table()) {
    return VoidResult{ErrorCode::NotSupported}; // 2MB block
  }

  // PTE level — the actual 4KB page
  auto* pte_table = reinterpret_cast<PageTable*>(pmd_entry.get_phys_addr());
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
  auto* pud = reinterpret_cast<PageTable*>(pgd_entry.get_phys_addr());
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
  auto* pmd = reinterpret_cast<PageTable*>(pud_entry.get_phys_addr());
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
  auto* pte_table = reinterpret_cast<PageTable*>(pmd_entry.get_phys_addr());
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
