import moss.std;
import moss.types;
import moss.arch;
import moss.abi;
import moss.hal.timer;
import moss.boot;
import moss.fdt;
import moss.mm;
import moss.vfs;
import moss.process;
import moss.timer;
import moss.containers;
import moss.smart_ptr;
import moss.hal.uart;
import moss.hal.mmu;
import moss.logging;
import moss.ipc;
import moss.drivers;
import moss.result;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "ipc_regression.hpp"
#include "queue_regression.hpp"
#include "scheduler_regression.hpp"
#include "validation/memory_internal.hpp"
#include "validation/memory_cases.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;
using moss::test::validation::HeapPressure;

namespace {
bool mm_publication_boundary_observed;
bool mm_ready_visible_at_publication_boundary;
bool mm_instance_visible_at_publication_boundary;
} // namespace

extern "C" void moss_validation_mm_before_ready(const void *published_instance) noexcept {
  mm_publication_boundary_observed = true;
  mm_ready_visible_at_publication_boundary = mm::UnifiedMemoryManager::is_system_initialized();
  // The hook receives the exact object published by the preceding release
  // store, avoiding a public raw-reference API that could outlive shutdown.
  mm_instance_visible_at_publication_boundary = published_instance != nullptr;
}

namespace moss::test::validation {
void mm_initialization_publication() {
  ut::expect(mm_publication_boundary_observed);
  ut::expect(!mm_ready_visible_at_publication_boundary);
  ut::expect(mm_instance_visible_at_publication_boundary);
  ut::expect(mm::UnifiedMemoryManager::is_system_initialized());
}

void mm_unsupported_contracts() {
  const auto pages_before = mm::PageFrameAllocator::get_memory_stats();
  const auto heap_before = mm::RuntimeHeapAllocator::get_heap_stats();

  const auto buddy_init = mm::BuddyAllocatorV2::initialize();
  const auto buddy_compaction = mm::BuddyAllocatorV2::compact_memory();
  const auto buddy_watermark = mm::BuddyAllocatorV2::get_water_mark();
  const auto buddy_pressure = mm::BuddyAllocatorV2::is_memory_pressure();
  const auto buddy_fragmentation = mm::BuddyAllocatorV2::get_fragmentation_stats();
  const auto buddy_stats = mm::BuddyAllocatorV2::get_memory_stats();
  // This page-aligned value is only a sentinel. Unsupported operations must
  // neither dereference it nor replace it with a fabricated allocation.
  VirtAddr address = page_size;
  const auto info = mm::UnifiedMemoryManager::query_memory_info(address);
  const auto allocated_size = mm::UnifiedMemoryManager::get_allocated_size(address);
  const auto reallocation = mm::UnifiedMemoryManager::reallocate(address, page_size, 2 * page_size);
  const auto prefault = mm::UnifiedMemoryManager::prefault_memory(address, page_size);
  const auto advice = mm::UnifiedMemoryManager::advise_usage_pattern(address, page_size, mm::UsagePattern::SEQUENTIAL);
  const auto reclaim = mm::UnifiedMemoryManager::trigger_memory_reclaim();
  const auto compaction = mm::UnifiedMemoryManager::trigger_memory_compaction();
  const auto pressure = mm::UnifiedMemoryManager::get_memory_pressure();
  const auto numa = mm::UnifiedMemoryManager::optimize_numa_placement(address, page_size);
  const auto huge = mm::UnifiedMemoryManager::promote_to_huge_pages(address, page_size);
  const auto region_compaction = mm::UnifiedMemoryManager::compact_memory_region(address, page_size);
  const auto performance = mm::UnifiedMemoryManager::get_performance_stats();
  const mm::UnifiedMemoryManager::SystemPerformanceStats empty_performance{};
  const auto counter_reset = mm::UnifiedMemoryManager::reset_performance_counters();
  const auto history = mm::UnifiedMemoryManager::dump_allocation_history();
  const auto leak_report = mm::UnifiedMemoryManager::generate_leak_report();

  ut::expect(!buddy_init && buddy_init.error() == mm::BuddyError::NotSupported);
  ut::expect(!buddy_compaction && buddy_compaction.error() == mm::BuddyError::NotSupported);
  ut::expect(!buddy_watermark && buddy_watermark.error() == mm::BuddyError::NotSupported);
  ut::expect(!buddy_pressure && buddy_pressure.error() == mm::BuddyError::NotSupported);
  ut::expect(!buddy_fragmentation && buddy_fragmentation.error() == mm::BuddyError::NotSupported);
  ut::expect(!buddy_stats && buddy_stats.error() == mm::BuddyError::NotSupported);
  ut::expect(!info && info.error() == mm::MMError::NotSupported);
  ut::expect(!allocated_size && allocated_size.error() == mm::MMError::NotSupported);
  ut::expect(!reallocation && reallocation.error() == mm::MMError::NotSupported);
  ut::expect(address == page_size);
  ut::expect(!prefault && prefault.error() == mm::MMError::NotSupported);
  ut::expect(!advice && advice.error() == mm::MMError::NotSupported);
  ut::expect(!reclaim && reclaim.error() == mm::MMError::NotSupported);
  ut::expect(!compaction && compaction.error() == mm::MMError::NotSupported);
  ut::expect(!pressure && pressure.error() == mm::MMError::NotSupported);
  ut::expect(!numa && numa.error() == mm::MMError::NotSupported);
  ut::expect(!huge && huge.error() == mm::MMError::NotSupported);
  ut::expect(!region_compaction && region_compaction.error() == mm::MMError::NotSupported);
  ut::expect(!performance && performance.error() == mm::MMError::NotSupported);
  ut::expect(empty_performance.overall_pressure == mm::MemoryPressure::UNKNOWN);
  ut::expect(!counter_reset && counter_reset.error() == mm::MMError::NotSupported);
  ut::expect(!history && history.error() == mm::MMError::NotSupported);
  ut::expect(!leak_report && leak_report.error() == mm::MMError::NotSupported);

  const auto pages_after = mm::PageFrameAllocator::get_memory_stats();
  const auto heap_after = mm::RuntimeHeapAllocator::get_heap_stats();
  ut::expect(pages_after.free_pages == pages_before.free_pages);
  ut::expect(pages_after.used_pages == pages_before.used_pages);
  ut::expect(heap_after.allocated_bytes == heap_before.allocated_bytes);
  ut::expect(heap_after.free_bytes == heap_before.free_bytes);
}

void table_permission_defaults() {
  using Tables = mm::PageTableManager;
  const auto root = Tables::get_physical_address(Tables::get_kernel_pgd());
  mm::PageTableEntry entry;
  entry.set_table(root);
  ut::expect(entry.is_valid() && entry.is_table() && entry.get_phys_addr() == root &&
             (entry.raw & mm::page_attr::USER) == 0);
  entry.set_table(root, true);
  ut::expect(entry.is_valid() && entry.is_table() && entry.get_phys_addr() == root);
#if defined(MOSS_ARCH_X64)
  ut::expect((entry.raw & mm::page_attr::USER) != 0);
#else
  // ARM64/RISC-V 64 user permission belongs to the leaf, not this table descriptor.
  ut::expect((entry.raw & mm::page_attr::USER) == 0);
#endif
  entry.set_block(0, mm::page_perms::KERNEL_RW);
  ut::expect(entry.is_valid() && entry.is_block() && (entry.raw & mm::page_attr::USER) == 0);
  entry.set_page(root, mm::page_perms::KERNEL_RW);
  ut::expect(entry.is_valid() && entry.get_phys_addr() == root && (entry.raw & mm::page_attr::USER) == 0);
  const auto rejected = Tables::map_page(0, 0, mm::page_perms::USER_RW);
  ut::expect(!rejected && rejected.error() == ErrorCode::InvalidParameter);
}

void kernel_mapping_permissions() {
  using Tables = mm::PageTableManager;
  KernelPermissions kernel;
  kernel.walk(Tables::get_physical_address(Tables::get_kernel_pgd()), kernel.root_shift, 0, false);
  // Bootstrap maps 4 GiB low on ARM64; x64/RISC-V count both the 4 GiB identity
  // window and its high direct-map alias. This checks boot tables, not board RAM size.
#if defined(MOSS_ARCH_ARM64)
  constexpr u64 kernel_bytes = 0x100000000ULL;
#else
  constexpr u64 kernel_bytes = 0x200000000ULL;
#endif
  ut::expect(kernel.kernel_bytes == kernel_bytes && kernel.user_leaves == 0);
  KernelPermissions high;
  high.walk(Tables::get_physical_address(Tables::get_kernel_high_pgd()), high.root_shift, 0, false);
#if defined(MOSS_ARCH_RISCV64)
  ut::expect(high.kernel_bytes == kernel_bytes);
#else
  ut::expect(high.kernel_bytes == 0x100000000ULL);
#endif
}

void active_user_mapping_permissions() {
  PhysAddr active_root = 0;
#if defined(MOSS_ARCH_ARM64)
  asm volatile("mrs %0, ttbr0_el1" : "=r"(active_root));
  active_root &= hal::mmu::PTE_ADDR_MASK;
#elif defined(MOSS_ARCH_X64)
  asm volatile("mov %%cr3, %0" : "=r"(active_root));
  active_root &= hal::mmu::PTE_ADDR_MASK;
#else
  asm volatile("csrr %0, satp" : "=r"(active_root));
  active_root = (active_root & 0x00000FFFFFFFFFFFULL) << 12;
#endif
  if (!ut::expect(active_root && active_root == moss::abi::bridge::get_current_pgd_phys())) {
    return;
  }
  KernelPermissions user;
  user.walk(active_root, user.root_shift, 0, true);
#if defined(MOSS_ARCH_ARM64)
  ut::expect(user.kernel_bytes == 0x100000000ULL && user.user_leaves > 0);
  PhysAddr high_root;
  asm volatile("mrs %0, ttbr1_el1" : "=r"(high_root));
  ut::expect((high_root & hal::mmu::PTE_ADDR_MASK) ==
             mm::PageTableManager::get_physical_address(mm::PageTableManager::get_kernel_high_pgd()));
#else
  ut::expect(user.kernel_bytes == 0x200000000ULL && user.user_leaves > 0);
#endif
}

void kernel_wx_permissions() {
  using Tables = mm::PageTableManager;
  KernelPermissions check;
  check.check_wx = true;
  check.walk(Tables::get_physical_address(Tables::get_kernel_pgd()), check.root_shift, 0, false);
  check.walk(Tables::get_physical_address(Tables::get_kernel_high_pgd()), check.root_shift, 0, false);
#if defined(MOSS_ARCH_X64)
  u64 cr0;
  asm volatile("mov %%cr0, %0" : "=r"(cr0));
  ut::expect((cr0 & (1ULL << 16)) != 0); // Supervisor writes must obey RO PTEs.
#endif
}

bool ram_contains(PhysAddr begin, PhysAddr end) {
  const auto &info = moss::fdt::get_platform_info();
  while (begin < end) {
    PhysAddr next = begin;
    for (u32 i = 0; i < info.memory_region_count; ++i) {
      const auto &region = info.memory_regions[i];
      if (region.base <= begin && region.base + region.size > next) {
        next = region.base + region.size;
      }
    }
    if (next == begin) {
      return false;
    }
    begin = next;
  }
  return true;
}

// Snapshot only immutable ownership data, not counters or arbitrary kernel BSS.
// The suite has one userspace worker; other CPUs are online and idle.
struct LayoutSnapshot {
  // Static budget of 128 unique table pages; capture fails if the mapping grows
  // beyond it. The low 4 GiB checks match the bootstrap identity-map window
  // because this fixture dereferences physical addresses directly.
  struct Table {
    PhysAddr address;
    u64 hash;
  } tables[128]{};
  usize count = 0;
  mm::PageFrameAllocator::MemoryStats pfa{};
  u64 metadata_hash = 0;
  u64 boot_tables_hash = 0;
  bool valid = true;

  static bool disjoint(PhysAddr begin, PhysAddr end, PhysAddr other, usize size) {
    return !size || end <= other || begin >= other + size;
  }

  bool protect(PhysAddr address) {
    for (usize i = 0; i < count; ++i) {
      if (tables[i].address == address) {
        return false; // Shared kernel tables are recorded only once.
      }
    }
    namespace linker = moss::abi::linker;
    if (!ut::expect(count < 128 && address && (address & (page_size - 1)) == 0 && address < 0x100000000ULL &&
                    ram_contains(address, address + page_size) &&
                    disjoint(address, address + page_size, linker::heap_start(), linker::heap_size()) &&
                    disjoint(address, address + page_size, pfa.metadata_start, pfa.metadata_size))) {
      valid = false;
      return false;
    }
    if (address >= linker::kernel_end() && !ut::expect(mm::PageFrameAllocator::page_ref_get(address) > 0)) {
      valid = false;
      return false;
    }
    tables[count++] = {.address = address, .hash = page_table_hash(address)};
    return true;
  }

  void walk(PhysAddr root, unsigned levels) {
    if (!protect(root)) {
      return;
    }
    if (levels > 1) {
      const auto *table = reinterpret_cast<const mm::PageTable *>(root);
      for (const auto &entry : table->entries) {
        if (entry.is_valid() && entry.is_table()) {
          walk(entry.get_phys_addr(), levels - 1);
        }
      }
    }
  }

  bool capture() {
    namespace linker = moss::abi::linker;
    using Tables = mm::PageTableManager;
    pfa = mm::PageFrameAllocator::get_memory_stats();
    if (!ut::expect(linker::bss_end() <= linker::heap_start() && linker::heap_start() < linker::heap_end() &&
                    linker::heap_end() <= linker::pagetable_start() &&
                    linker::pagetable_start() < linker::pagetable_end() &&
                    linker::pagetable_end() <= linker::kernel_end() && pfa.metadata_start >= linker::kernel_end() &&
                    pfa.metadata_start < 0x100000000ULL && pfa.metadata_size &&
                    pfa.metadata_size <= 0x100000000ULL - pfa.metadata_start &&
                    ((pfa.metadata_start | pfa.metadata_size) & (page_size - 1)) == 0 &&
                    ram_contains(pfa.metadata_start, pfa.metadata_start + pfa.metadata_size))) {
      return false;
    }
    const auto &info = moss::fdt::get_platform_info();
    valid = ut::expect(disjoint(pfa.metadata_start, pfa.metadata_start + pfa.metadata_size, info.initrd_start,
                                info.initrd_end - info.initrd_start));
    for (u32 i = 0; i < info.reserved_region_count; ++i) {
      const auto &region = info.reserved_regions[i];
      valid =
          ut::expect(disjoint(pfa.metadata_start, pfa.metadata_start + pfa.metadata_size, region.base, region.size)) &&
          valid;
    }
    const unsigned levels = hal::mmu::g_mmu_mode == hal::mmu::MmuMode::Sv39 ? 3 : 4;
    walk(Tables::get_physical_address(Tables::get_kernel_pgd()), levels);
    walk(Tables::get_physical_address(Tables::get_kernel_high_pgd()), levels);
    walk(moss::abi::bridge::get_current_pgd_phys(), levels);
    // Include unused early-table slots too; heap must not borrow their storage.
    for (usize i = 0;; ++i) {
      auto *table = Tables::get_table_by_index(i);
      if (!table) {
        break;
      }
      (void)protect(Tables::get_physical_address(table));
    }
    metadata_hash = memory_hash(pfa.metadata_start, pfa.metadata_size);
    boot_tables_hash = memory_hash(linker::pagetable_start(), linker::pagetable_size());
    return valid;
  }

  bool excludes(PhysAddr begin, PhysAddr end) const {
    if (!disjoint(begin, end, pfa.metadata_start, pfa.metadata_size)) {
      return false;
    }
    for (usize i = 0; i < count; ++i) {
      if (!disjoint(begin, end, tables[i].address, page_size)) {
        return false;
      }
    }
    return true;
  }

  void verify() const {
    namespace linker = moss::abi::linker;
    ut::expect(memory_hash(pfa.metadata_start, pfa.metadata_size) == metadata_hash);
    ut::expect(memory_hash(linker::pagetable_start(), linker::pagetable_size()) == boot_tables_hash);
    for (usize i = 0; i < count; ++i) {
      ut::expect(page_table_hash(tables[i].address) == tables[i].hash);
    }
  }
};

void address_space_ownership() {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  LayoutSnapshot layout;
  if (!layout.capture()) {
    return;
  }
  const auto free_before = Pfa::get_memory_stats().free_pages;
  {
    auto parent = process::user_space::create_user_address_space();
    auto child = process::user_space::create_user_address_space();
    if (!ut::expect(parent && child)) {
      return;
    }
    const auto parent_root = (*parent)->pgd_phys;
    const auto child_root = (*child)->pgd_phys;
    auto page = mm::allocate_pages(0);
    if (!ut::expect(page.has_value())) {
      return;
    }
    constexpr VirtAddr user_va = process::user_layout::CODE_BASE;
    auto mapped = Tables::map_user_page(parent_root, user_va, *page, mm::page_perms::USER_RW);
    if (!ut::expect(mapped.has_value())) {
      (void)mm::free_pages(*page, 0);
      return;
    }
    // Rejected requests cannot descend into shared kernel page-table storage.
    ut::expect(!Tables::map_user_page(parent_root, moss::abi::linker::text_start(), *page, mm::page_perms::USER_RW));
    ut::expect(!Tables::map_user_page(parent_root, USER_MAX, *page, mm::page_perms::USER_RW));
    ut::expect(Tables::get_user_pte(parent_root, moss::abi::linker::text_start()) == nullptr);
    if (!ut::expect(Tables::clone_user_page_tables(parent_root, child_root).has_value())) {
      return;
    }
    const auto *parent_pte = Tables::get_user_pte(parent_root, user_va);
    const auto *child_pte = Tables::get_user_pte(child_root, user_va);
    ut::expect(parent_pte && child_pte && parent_pte->is_cow() && child_pte->is_cow() &&
               parent_pte->get_phys_addr() == *page && child_pte->get_phys_addr() == *page &&
               Pfa::page_ref_get(*page) == 2);
    KernelPermissions inherited;
    inherited.check_wx = true;
    inherited.walk(child_root, inherited.root_shift, 0, true);
#if defined(MOSS_ARCH_ARM64)
    ut::expect(inherited.kernel_bytes == 0x100000000ULL);
#else
    ut::expect(inherited.kernel_bytes == 0x200000000ULL);
#endif
    (*child).reset();
    ut::expect(Pfa::page_ref_get(*page) == 1);
  }
  ut::expect(Pfa::get_memory_stats().free_pages == free_before);
  layout.verify();
}

void cow_clone_permissions() {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  const auto free_before = Pfa::get_memory_stats().free_pages;
  {
    auto parent = process::user_space::create_user_address_space();
    auto child = process::user_space::create_user_address_space();
    auto grandchild = process::user_space::create_user_address_space();
    if (!ut::expect(parent && child && grandchild)) {
      return;
    }
    const PhysAddr roots[] = {(*parent)->pgd_phys, (*child)->pgd_phys, (*grandchild)->pgd_phys};
    constexpr u64 permissions[] = {mm::page_perms::USER_RO | mm::page_attr::XN, mm::page_perms::USER_RX,
                                   mm::page_perms::USER_RW | mm::page_attr::XN};
    constexpr VirtAddr base = process::user_layout::CODE_BASE;
    PhysAddr physical[3]{};
    mm::PageTableEntry expected[3];
    for (usize i = 0; i < 3; ++i) {
      auto page = mm::allocate_pages(0);
      if (!ut::expect(page.has_value())) {
        return;
      }
      physical[i] = *page;
      if (!ut::expect(Tables::map_user_page(roots[0], base + i * page_size, *page, permissions[i]).has_value())) {
        (void)mm::free_pages(*page, 0);
        return;
      }
      expected[i].set_page(*page, permissions[i]);
      if (i == 2) { // Only the originally writable private page is COW-eligible.
        expected[i].set_cow();
        expected[i].make_readonly();
      }
    }
    if (!ut::expect(Tables::clone_user_page_tables(roots[0], roots[1]).has_value())) {
      return;
    }
    if (!ut::expect(Tables::clone_user_page_tables(roots[1], roots[2]).has_value())) {
      return;
    }
    for (usize i = 0; i < 3; ++i) {
      for (const PhysAddr root : roots) {
        const auto *pte = Tables::get_user_pte(root, base + i * page_size);
        ut::expect(pte && pte->raw == expected[i].raw && pte->get_phys_addr() == physical[i]);
      }
      ut::expect(Pfa::page_ref_get(physical[i]) == 3);
    }
    (*parent).reset();
    (*child).reset();
    for (const PhysAddr page : physical) {
      ut::expect(Pfa::page_ref_get(page) == 1);
    }
  }
  ut::expect(Pfa::get_memory_stats().free_pages == free_before);
}

void map_preserves_existing() {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  const auto free_before = Pfa::get_memory_stats().free_pages;
  {
    auto space = process::user_space::create_user_address_space();
    if (!ut::expect(space.has_value())) {
      return;
    }
    auto original = mm::allocate_pages(0);
    auto replacement = mm::allocate_pages(0);
    if (!ut::expect(original && replacement)) {
      if (original) {
        (void)mm::free_pages(*original, 0);
      }
      if (replacement) {
        (void)mm::free_pages(*replacement, 0);
      }
      return;
    }
    constexpr VirtAddr address = process::user_layout::CODE_BASE;
    const PhysAddr root = (*space)->pgd_phys;
    if (!ut::expect(Tables::map_user_page(root, address, *original, mm::page_perms::USER_RO).has_value())) {
      (void)mm::free_pages(*original, 0);
      (void)mm::free_pages(*replacement, 0);
      return;
    }
    auto *pte = Tables::get_user_pte(root, address);
    const auto before = *pte;
    const auto free_mapped = Pfa::get_memory_stats().free_pages;
    const auto result = Tables::map_user_page(root, address, *replacement, mm::page_perms::USER_RW);
    ut::expect(!result && result.error() == ErrorCode::AlreadyExists);
    ut::expect(pte->raw == before.raw);
    ut::expect(Pfa::page_ref_get(*original) == 1 && Pfa::page_ref_get(*replacement) == 1);
    ut::expect(Pfa::get_memory_stats().free_pages == free_mapped);
    // This address space was never installed. Restore ownership for cleanup
    // even on the old, broken implementation that overwrites the leaf.
    *pte = before;
    ut::expect(mm::free_pages(*replacement, 0).has_value());
  }
  ut::expect(Pfa::get_memory_stats().free_pages == free_before);
}

void unmap_reclaims_tables() {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  auto space = process::user_space::create_user_address_space();
  if (!ut::expect(space.has_value())) {
    return;
  }
  const PhysAddr root = (*space)->pgd_phys;
  const auto baseline = Pfa::get_memory_stats().free_pages;
  constexpr VirtAddr base = process::user_layout::CODE_BASE;
  // Adjacent leaves share a table; distant leaves also exercise parent-table
  // reclamation. Unmapping one must preserve the other and all kernel entries.
  constexpr VirtAddr offsets[] = {page_size, 1ULL << 21, 1ULL << 30};
  for (const auto offset : offsets) {
    auto first = mm::allocate_pages(0);
    auto second = mm::allocate_pages(0);
    if (!ut::expect(first && second)) {
      if (first) {
        (void)mm::free_pages(*first, 0);
      }
      if (second) {
        (void)mm::free_pages(*second, 0);
      }
      return;
    }
    const auto a = Tables::map_user_page(root, base, *first, mm::page_perms::USER_RW);
    const auto b = Tables::map_user_page(root, base + offset, *second, mm::page_perms::USER_RO);
    if (!ut::expect(a && b)) {
      if (!a) {
        (void)mm::free_pages(*first, 0);
      }
      if (!b) {
        (void)mm::free_pages(*second, 0);
      }
      return;
    }
    Tables::unmap_user_page(root, base);
    auto *neighbor = Tables::get_user_pte(root, base + offset);
    ut::expect(neighbor && neighbor->is_valid() && neighbor->get_phys_addr() == *second);
    ut::expect(Pfa::page_ref_get(*first) == 0 && Pfa::page_ref_get(*second) == 1);
    Tables::unmap_user_page(root, base + offset);
    Tables::unmap_user_page(root, base + offset); // Repeated unmap is harmless.
    if (!ut::expect(Pfa::get_memory_stats().free_pages == baseline)) {
      return;
    }
  }
}

void map_allocation_rollback() {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  const auto free_before = Pfa::get_memory_stats().free_pages;
  {
    auto space = process::user_space::create_user_address_space();
    if (!ut::expect(space.has_value())) {
      return;
    }
    auto page = mm::allocate_pages(0);
    if (!ut::expect(page.has_value())) {
      return;
    }
    // Select a wholly absent root branch in either three- or four-level mode.
    const bool four_levels = Tables::is_user_range(1ULL << 39, page_size);
    const VirtAddr address = four_levels ? 1ULL << 39 : process::user_layout::CODE_BASE;
    const usize needed = four_levels ? 3 : 2;
    PagePressure pressure;
    if (!ut::expect(pressure.acquire(needed))) {
      (void)mm::free_pages(*page, 0);
      return;
    }
    for (usize budget = 0; budget < needed; ++budget) {
      const PhysAddr root = (*space)->pgd_phys;
      const u64 root_before = page_table_hash(root);
      const auto result = Tables::map_user_page(root, address, *page, mm::page_perms::USER_RW);
      ut::expect(!result && result.error() == ErrorCode::OutOfMemory);
      ut::expect(Pfa::get_memory_stats().free_pages == budget);
      ut::expect(page_table_hash(root) == root_before);
      auto *leaf = Tables::get_user_pte(root, address);
      ut::expect(!leaf || !leaf->is_valid());
      ut::expect(Pfa::page_ref_get(*page) == 1);
      // Recover even a partially modified, inactive tree on the failing baseline.
      if (leaf && leaf->is_valid()) {
        leaf->clear();
      }
      (*space)->pgd_phys = 0;
      Tables::free_user_page_tables(root);
      auto fresh = Tables::create_user_page_tables();
      if (!ut::expect(fresh.has_value())) {
        (void)mm::free_pages(*page, 0);
        return;
      }
      (*space)->pgd_phys = *fresh;
      pressure.give_one();
    }
    pressure.release();
    auto mapped = Tables::map_user_page((*space)->pgd_phys, address, *page, mm::page_perms::USER_RW);
    if (!ut::expect(mapped.has_value())) {
      (void)mm::free_pages(*page, 0);
    }
  }
  ut::expect(Pfa::get_memory_stats().free_pages == free_before);
}

void map_rejects_blocks() {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  const auto free_before = Pfa::get_memory_stats().free_pages;
  {
    auto space = process::user_space::create_user_address_space();
    auto destination = process::user_space::create_user_address_space();
    if (!ut::expect(space && destination)) {
      return;
    }
    auto page = mm::allocate_pages(0);
    if (!ut::expect(page.has_value())) {
      return;
    }
    constexpr VirtAddr address = process::user_layout::CODE_BASE;
    const PhysAddr root = (*space)->pgd_phys;
    if (!ut::expect(Tables::map_user_page(root, address, *page, mm::page_perms::USER_RW).has_value())) {
      (void)mm::free_pages(*page, 0);
      return;
    }
    const auto free_mapped = Pfa::get_memory_stats().free_pages;
    auto *table = Tables::get_table_from_physical(root);
    const unsigned first_shift = Tables::is_user_range(1ULL << 39, page_size) ? 39 : 30;
    for (unsigned shift = first_shift; shift > 12; shift -= 9) {
      auto &entry = table->entries[(address >> shift) & 511];
      const auto original = entry;
      const auto child_hash = page_table_hash(original.get_phys_addr());
      // Only this inactive address space is changed. Keep backing storage owned
      // and accessible so a broken software walker cannot access arbitrary RAM.
      entry.set_block(original.get_phys_addr(), mm::page_perms::USER_RW & ~mm::page_attr::TABLE);
      const auto block = entry;
      const auto result = Tables::map_user_page(root, address, *page, mm::page_perms::USER_RO);
      ut::expect(!result && result.error() == ErrorCode::NotSupported);
      ut::expect(entry.raw == block.raw);
      ut::expect(page_table_hash(original.get_phys_addr()) == child_hash);
      ut::expect(Pfa::get_memory_stats().free_pages == free_mapped);
      const auto destination_hash = page_table_hash((*destination)->pgd_phys);
      const auto cloned = Tables::clone_user_page_tables(root, (*destination)->pgd_phys);
      ut::expect(!cloned && cloned.error() == ErrorCode::NotSupported);
      ut::expect(page_table_hash((*destination)->pgd_phys) == destination_hash);
      ut::expect(Pfa::get_memory_stats().free_pages == free_mapped);
      ut::expect(entry.raw == block.raw);
      entry = original;
      table = Tables::get_table_from_physical(original.get_phys_addr());
    }
  }
  ut::expect(Pfa::get_memory_stats().free_pages == free_before);
}

void clone_preserves_destination() {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  const auto free_before = Pfa::get_memory_stats().free_pages;
  {
    auto source = process::user_space::create_user_address_space();
    auto destination = process::user_space::create_user_address_space();
    if (!ut::expect(source && destination)) {
      return;
    }
    const PhysAddr roots[] = {(*source)->pgd_phys, (*destination)->pgd_phys};
    constexpr VirtAddr address = process::user_layout::CODE_BASE;
    mm::PageTableEntry original[2];
    for (usize i = 0; i < 2; ++i) {
      auto page = mm::allocate_pages(0);
      if (!ut::expect(page.has_value())) {
        return;
      }
      if (!ut::expect(Tables::map_user_page(roots[i], address, *page, mm::page_perms::USER_RW).has_value())) {
        (void)mm::free_pages(*page, 0);
        return;
      }
      original[i] = *Tables::get_user_pte(roots[i], address);
    }
    const auto free_mapped = Pfa::get_memory_stats().free_pages;
    auto cloned = Tables::clone_user_page_tables(roots[0], roots[1]);
    ut::expect(!cloned && cloned.error() == ErrorCode::AlreadyExists);
    auto alias = Tables::clone_user_page_tables(roots[0], roots[0]);
    ut::expect(!alias && alias.error() == ErrorCode::InvalidParameter);
    for (usize i = 0; i < 2; ++i) {
      ut::expect(Tables::get_user_pte(roots[i], address)->raw == original[i].raw);
      ut::expect(Pfa::page_ref_get(original[i].get_phys_addr()) == 1);
    }
    ut::expect(Pfa::get_memory_stats().free_pages == free_mapped);
  }
  ut::expect(Pfa::get_memory_stats().free_pages == free_before);
}

void clone_allocation_rollback() {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  const auto free_before = Pfa::get_memory_stats().free_pages;
  {
    auto parent = process::user_space::create_user_address_space();
    auto child = process::user_space::create_user_address_space();
    if (!ut::expect(parent && child)) {
      return;
    }
    const PhysAddr parent_root = (*parent)->pgd_phys;
    const bool four_levels = Tables::is_user_range(1ULL << 39, page_size);
    constexpr VirtAddr base = process::user_layout::CODE_BASE;
    const VirtAddr addresses[] = {base, base + (1ULL << 21), base + (1ULL << 30),
                                  four_levels ? 1ULL << 39 : base + (1ULL << 37)};
    mm::PageTableEntry originals[4];
    for (usize i = 0; i < 4; ++i) {
      auto page = mm::allocate_pages(0);
      if (!ut::expect(page.has_value())) {
        return;
      }
      if (!ut::expect(Tables::map_user_page(parent_root, addresses[i], *page, mm::page_perms::USER_RW).has_value())) {
        (void)mm::free_pages(*page, 0);
        return;
      }
      originals[i] = *Tables::get_user_pte(parent_root, addresses[i]);
      *reinterpret_cast<u64 *>(phys_to_virt(*page)) = 0xc0ffeeULL + i;
    }
    auto recreate_child = [&]() {
      Tables::free_user_page_tables((*child)->pgd_phys);
      (*child)->pgd_phys = 0;
      auto fresh = Tables::create_user_page_tables();
      if (!ut::expect(fresh.has_value())) {
        return false;
      }
      (*child)->pgd_phys = *fresh;
      return true;
    };
    // Measure the real table-page requirement with ample memory, then undo this
    // inactive calibration clone. No parent user instruction can run here.
    const auto before_clone = Pfa::get_memory_stats().free_pages;
    if (!ut::expect(Tables::clone_user_page_tables(parent_root, (*child)->pgd_phys).has_value())) {
      return;
    }
    const usize needed = before_clone - Pfa::get_memory_stats().free_pages;
    if (!ut::expect(needed > 0 && needed <= 16) || !recreate_child()) {
      return;
    }
    for (usize i = 0; i < 4; ++i) {
      *Tables::get_user_pte(parent_root, addresses[i]) = originals[i];
    }
    PagePressure pressure;
    if (!ut::expect(pressure.acquire(needed))) {
      return;
    }
    for (usize budget = 0; budget < needed; ++budget) {
      const PhysAddr child_root = (*child)->pgd_phys;
      const auto root_hash = page_table_hash(child_root);
      const auto *root = Tables::get_table_from_physical(child_root);
      const PhysAddr mixed_pud = four_levels ? root->entries[0].get_phys_addr() : 0;
      const auto pud_hash = mixed_pud ? page_table_hash(mixed_pud) : 0;
      const auto cloned = Tables::clone_user_page_tables(parent_root, child_root);
      ut::expect(!cloned && cloned.error() == ErrorCode::OutOfMemory);
      ut::expect(Pfa::get_memory_stats().free_pages == budget);
      ut::expect(page_table_hash(child_root) == root_hash);
      ut::expect(!mixed_pud || page_table_hash(mixed_pud) == pud_hash);
      for (usize i = 0; i < 4; ++i) {
        const auto *pte = Tables::get_user_pte(parent_root, addresses[i]);
        ut::expect(pte && pte->raw == originals[i].raw);
        ut::expect(Pfa::page_ref_get(originals[i].get_phys_addr()) == 1);
        ut::expect(*reinterpret_cast<const u64 *>(phys_to_virt(originals[i].get_phys_addr())) == 0xc0ffeeULL + i);
      }
      if (!recreate_child()) {
        return;
      }
      for (usize i = 0; i < 4; ++i) {
        *Tables::get_user_pte(parent_root, addresses[i]) = originals[i];
      }
      pressure.give_one();
    }
    // Exactly the required number of pages must suffice, including after all
    // preceding failed attempts; there is no extra reserve hidden in a mock.
    if (!ut::expect(Tables::clone_user_page_tables(parent_root, (*child)->pgd_phys).has_value())) {
      return;
    }
    ut::expect(Pfa::get_memory_stats().free_pages == 0);
    for (usize i = 0; i < 4; ++i) {
      const auto *pte = Tables::get_user_pte((*child)->pgd_phys, addresses[i]);
      ut::expect(pte && pte->is_cow() && pte->get_phys_addr() == originals[i].get_phys_addr());
      ut::expect(Pfa::page_ref_get(originals[i].get_phys_addr()) == 2);
    }
  }
  ut::expect(Pfa::get_memory_stats().free_pages == free_before);
}

void asid_leases() {
  using namespace process;
  const auto before = mm::PageFrameAllocator::get_memory_stats().free_pages;
  auto *thread = CfsScheduler::get_current_task();
  auto proc = g_process_manager->find_process(thread->owner_pid);
  const auto active_asid = proc->address_space()->asid;
  {
    // The software allocator has 256 tags: zero is kernel-only, leaving 255
    // user leases. This worker already owns one, so exhaustion occurs at 254
    // new spaces; releasing alternate leases tests reuse without a full reset.
    shared_ptr<AddressSpace> spaces[255];
    bool used[256]{};
    used[active_asid] = true;
    unsigned count = 0;
    for (; count < 255; ++count) {
      auto next = user_space::create_user_address_space();
      if (!next) {
        ut::expect(next.error() == ErrorCode::ResourceExhausted);
        break;
      }
      const auto tag = (*next)->asid;
      if (!ut::expect(tag > 0 && tag < 256 && !used[tag])) {
        return;
      }
      used[tag] = true;
      spaces[count] = moss::move(*next);
    }
    ut::expect(count == 254); // One live lease belongs to this worker.
    if (count != 254) {
      return;
    }
    for (unsigned i = 0; i < count; i += 2) {
      used[spaces[i]->asid] = false;
      spaces[i].reset();
    }
    for (unsigned i = 0; i < count; i += 2) {
      auto next = user_space::create_user_address_space();
      if (!ut::expect(next.has_value())) {
        return;
      }
      const auto tag = (*next)->asid;
      if (!ut::expect(tag > 0 && tag < 256 && !used[tag])) {
        return;
      }
      used[tag] = true;
      spaces[i] = moss::move(*next);
    }
  }
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == before);
}

void vma_boundaries() {
  using namespace process;
  const auto before = mm::PageFrameAllocator::get_memory_stats().free_pages;
  {
    auto created = user_space::create_user_address_space();
    if (!ut::expect(created.has_value())) {
      return;
    }
    auto &space = **created;
    constexpr auto base = user_layout::MMAP_BASE;
    const VirtAddr invalid[][2] = {
        {0, page_size},
        {mm::PageTableManager::KERNEL_IDENTITY_END - page_size, mm::PageTableManager::KERNEL_IDENTITY_END},
        {USER_MAX, USER_MAX + page_size},
        {USER_MAX - page_size, USER_MAX + page_size},
        {~VirtAddr{0} - page_size + 1, page_size},
        {base, base},
        {base + page_size, base},
        {base + 1, base + page_size},
        {user_layout::SIGRETURN_PAGE, user_layout::SIGRETURN_PAGE + page_size},
    };
    for (const auto &range : invalid) {
      const bool added = space.add_vma(range[0], range[1], vma_flags::READ, VmaType::MMAP);
      ut::expect(!added);
      if (added) {
        ut::expect(space.remove_vma(range[0], range[1]));
      }
    }
    ut::expect(space.add_vma(base, base + page_size, vma_flags::READ, VmaType::MMAP));
    ut::expect(!space.add_vma(base, base + page_size, vma_flags::WRITE, VmaType::MMAP));
    ut::expect(space.remove_vma(base, base + page_size));
    ut::expect(!space.add_vma(base, base + page_size, ~u32{0}, VmaType::MMAP));
    ut::expect(
        !space.add_vma(base, base + page_size, vma_flags::READ | vma_flags::WRITE | vma_flags::EXEC, VmaType::MMAP));
    ut::expect(space.add_vma(base, base + page_size, vma_flags::READ | vma_flags::WRITE, VmaType::MMAP));
    ut::expect(space.add_vma(base + page_size, base + 2 * page_size, vma_flags::READ, VmaType::MMAP));
    ut::expect(space.allows_user_access(base + page_size - 8, 16, vma_flags::READ));
    ut::expect(!space.allows_user_access(base + page_size - 8, 16, vma_flags::WRITE));
    ut::expect(!space.allows_user_access(base + 2 * page_size - 8, 16, vma_flags::READ));
    ut::expect(!space.allows_user_access(0, 1, vma_flags::READ));
    ut::expect(!space.allows_user_access(USER_MAX - 1, 2, vma_flags::READ));
    ut::expect(space.allows_user_access(0, 0, vma_flags::READ));

    constexpr auto heap = user_layout::HEAP_START;
    constexpr u32 heap_flags = vma_flags::READ | vma_flags::WRITE | vma_flags::DEMAND_ZERO;
    ut::expect(!space.add_vma(heap + page_size, heap + 2 * page_size, heap_flags, VmaType::HEAP));
    ut::expect(space.add_vma(heap, heap, heap_flags, VmaType::HEAP));
    ut::expect(!space.add_vma(heap, heap, heap_flags, VmaType::HEAP));
    bool resized = false;
    ut::expect(space.resize_vma(heap, heap, heap + page_size, VmaType::HEAP,
                                [&](const VmaRegion &old) { resized = old.end_addr == heap; }));
    ut::expect(resized && space.find_vma(heap) && space.find_vma(heap)->end_addr == heap + page_size);
    const auto collision_start = heap + 2 * page_size;
    const auto collision_end = collision_start + page_size;
    ut::expect(space.add_vma(collision_start, collision_end, vma_flags::READ, VmaType::MMAP));
    resized = false;
    ut::expect(!space.resize_vma(heap, heap + page_size, collision_end, VmaType::HEAP,
                                 [&](const VmaRegion &) { resized = true; }));
    ut::expect(!resized && space.find_vma(heap) && space.find_vma(heap)->end_addr == heap + page_size);
    ut::expect(space.resize_vma(heap, heap + page_size, heap, VmaType::HEAP,
                                [&](const VmaRegion &old) { resized = old.end_addr == heap + page_size; }));
    ut::expect(resized && !space.find_vma(heap));
    ut::expect(space.remove_vma(heap, heap));
    ut::expect(space.remove_vma(collision_start, collision_end));

    const auto stub = user_layout::SIGRETURN_PAGE;
    ut::expect(!space.add_vma(stub, stub + page_size, vma_flags::WRITE, VmaType::SIGRETURN));
    ut::expect(space.add_vma(stub, stub + page_size, vma_flags::READ | vma_flags::EXEC, VmaType::SIGRETURN));
  }
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == before);
}

void pages() {
  // Orders 0..7 cover 1..128-page blocks held concurrently. An arbitrary
  // nonzero base pattern plus order distinguishes each block after allocation.
  auto before = mm::PageFrameAllocator::get_memory_stats().free_pages;
  PhysAddr live[8]{};
  for (usize order = 0; order < 8; ++order) {
    auto page = mm::PageFrameAllocator::allocate_pages(order);
    if (!ut::expect(static_cast<bool>(page))) {
      break;
    }
    live[order] = *page;
    ut::expect((*page & ((page_size << order) - 1)) == 0);
    for (usize other = 0; other < order; ++other) {
      ut::expect(live[other] + (page_size << other) <= *page || *page + (page_size << order) <= live[other]);
    }
    *reinterpret_cast<volatile u64 *>(*page) = 0x12345678 + order;
  }
  for (usize order = 0; order < 8; ++order) {
    if (live[order]) {
      ut::expect(*reinterpret_cast<volatile u64 *>(live[order]) == 0x12345678 + order);
      ut::expect(static_cast<bool>(mm::PageFrameAllocator::free_pages(live[order], order)));
    }
  }
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == before);
}

void reuse_pages() {
  // Cycle through orders 0..3 (1..8 pages) for 128 bounded reuse operations;
  // the count is a regression workload budget, not a production limit.
  auto before = mm::PageFrameAllocator::get_memory_stats().free_pages;
  for (usize i = 0; i < 128; ++i) {
    auto page = mm::PageFrameAllocator::allocate_pages(i % 4);
    if (!ut::expect(static_cast<bool>(page))) {
      return;
    }
    ut::expect(static_cast<bool>(mm::PageFrameAllocator::free_pages(*page, i % 4)));
  }
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == before);
  auto invalid_order = mm::PageFrameAllocator::allocate_pages(MAX_ORDER + 1);
  ut::expect(!invalid_order && invalid_order.error() == mm::PageAllocError::InvalidOrder);
  auto invalid_address = mm::PageFrameAllocator::free_pages(1, 0);
  ut::expect(!invalid_address && invalid_address.error() == mm::PageAllocError::InvalidAddress);
}

void page_release_contract() {
  using Pfa = mm::PageFrameAllocator;
  const auto before = Pfa::get_memory_stats();
  for (usize order = 0; order <= MAX_ORDER; ++order) {
    auto allocation = Pfa::allocate_pages(order);
    if (!ut::expect(static_cast<bool>(allocation))) {
      return;
    }
    const usize pages_owned = usize{1} << order;
    const auto live = Pfa::get_memory_stats();
    ut::expect((*allocation & ((page_size << order) - 1)) == 0);
    ut::expect(live.used_pages == before.used_pages + pages_owned);
    ut::expect(live.free_pages + pages_owned == before.free_pages);
    for (usize i = 0; i < pages_owned; ++i) {
      *reinterpret_cast<volatile u64 *>(*allocation + i * page_size) = *allocation ^ i;
      ut::expect(Pfa::page_ref_get(*allocation + i * page_size) == 1);
    }
    const usize wrong = order == 0 ? 1 : order - 1;
    auto wrong_order = Pfa::free_pages(*allocation, wrong);
    if (!ut::expect(!wrong_order)) {
      return; // A broken free may already have mutated part of the live block.
    }
    ut::expect(wrong_order.error() == mm::PageAllocError::InvalidOrder);
    ut::expect(!Pfa::free_pages(*allocation, MAX_ORDER + 1));
    ut::expect(!Pfa::free_pages(*allocation + 1, order));
    if (order != 0) {
      ut::expect(!Pfa::free_pages(*allocation + page_size, 0));
      // The last page being shared must reject the whole free, not partially
      // release preceding pages before discovering the outstanding reference.
      const auto last = *allocation + (pages_owned - 1) * page_size;
      Pfa::page_ref_inc(last);
      auto shared = Pfa::free_pages(*allocation, order);
      if (!ut::expect(!shared)) {
        return;
      }
      ut::expect(shared.error() == mm::PageAllocError::PageInUse);
      ut::expect(Pfa::page_ref_dec(last) == 1);
    }
    ut::expect(Pfa::get_memory_stats().free_pages == live.free_pages);
    ut::expect(Pfa::get_memory_stats().used_pages == live.used_pages);
    for (usize i = 0; i < pages_owned; ++i) {
      ut::expect(*reinterpret_cast<volatile u64 *>(*allocation + i * page_size) == (*allocation ^ i));
      ut::expect(Pfa::page_ref_get(*allocation + i * page_size) == 1);
    }
    ut::expect(static_cast<bool>(Pfa::free_pages(*allocation, order)));
    ut::expect(!Pfa::free_pages(*allocation, order));
    ut::expect(Pfa::page_ref_get(*allocation) == 0);
    ut::expect(Pfa::get_memory_stats().free_pages == before.free_pages);
    ut::expect(Pfa::get_memory_stats().used_pages == before.used_pages);
  }
  ut::expect(!Pfa::free_pages(moss::abi::linker::heap_start(), 0));
  // kernel_end is exclusive and may now be usable when metadata is elsewhere.
  ut::expect(!Pfa::free_pages(moss::abi::linker::kernel_end() - page_size, 0));
  ut::expect(!Pfa::free_pages(~PhysAddr{0} & ~(page_size - 1), 0));
  const auto &info = moss::fdt::get_platform_info();
  if (info.initrd_end > info.initrd_start) {
    ut::expect(!Pfa::free_pages(info.initrd_start & ~(page_size - 1), 0));
  }
  for (u32 i = 0; i < info.reserved_region_count; ++i) {
    ut::expect(!Pfa::free_pages(info.reserved_regions[i].base & ~(page_size - 1), 0));
  }
  ut::expect(Pfa::get_memory_stats().free_pages == before.free_pages);
  ut::expect(Pfa::get_memory_stats().used_pages == before.used_pages);
}

void page_exhaustion() {
  using Pfa = mm::PageFrameAllocator;
  LayoutSnapshot layout;
  if (!layout.capture()) {
    return;
  }
  struct Owned {
    PhysAddr address;
    usize order;
  };
  // Store block descriptors off the worker stack. The fixed 2048-entry budget
  // is not a PFA capacity: overflow fails this fixture after freeing the extra
  // block. Its sizing basis for fragmented memory maps is unrecorded.
  static Owned owned[2048]{};
  const auto &info = moss::fdt::get_platform_info();
  auto initrd_hash = [&] {
    // FNV-1a's 64-bit offset basis and prime detect accidental initrd writes;
    // this is a regression checksum, not an integrity/authentication check.
    // Constant definitions: https://www.rfc-editor.org/rfc/rfc9923.html#section-5
    u64 hash = 14695981039346656037ULL;
    for (PhysAddr address = info.initrd_start; address < info.initrd_end; ++address) {
      hash = (hash ^ *reinterpret_cast<volatile u8 *>(address)) * 1099511628211ULL;
    }
    return hash;
  };
  const auto before = Pfa::get_memory_stats();
  const u64 original_hash = initrd_hash();
  usize count = 0;
  usize pages_owned = 0;
  bool valid = true;
  for (usize order_plus_one = MAX_ORDER + 1; order_plus_one != 0 && valid; --order_plus_one) {
    const usize order = order_plus_one - 1;
    for (;;) {
      auto allocation = Pfa::allocate_pages(order);
      if (!allocation) {
        valid = allocation.error() == mm::PageAllocError::OutOfMemory;
        break;
      }
      if (count == 2048) {
        ut::expect(static_cast<bool>(Pfa::free_pages(*allocation, order)));
        valid = false;
        break;
      }
      const PhysAddr begin = *allocation;
      const PhysAddr end = begin + (page_size << order);
      valid = valid && (begin & ((page_size << order) - 1)) == 0 && begin >= moss::abi::linker::kernel_end();
      valid = valid && ram_contains(begin, end) && layout.excludes(begin, end) &&
              (end <= info.initrd_start || begin >= info.initrd_end);
      for (u32 i = 0; i < info.reserved_region_count; ++i) {
        const auto &region = info.reserved_regions[i];
        valid = valid && (end <= region.base || begin >= region.base + region.size);
      }
      for (usize i = 0; i < count; ++i) {
        valid = valid && (end <= owned[i].address || begin >= owned[i].address + (page_size << owned[i].order));
      }
      owned[count++] = {.address = begin, .order = order};
      if (!valid) {
        break; // Never write into a block found to overlap reserved or live memory.
      }
      pages_owned += usize{1} << order;
      // An arbitrary nonzero XOR mask and a complemented end word distinguish
      // each page's endpoints, exposing overlap or writes beyond owned storage.
      for (PhysAddr page = begin; page < end; page += page_size) {
        *reinterpret_cast<volatile u64 *>(page) = page ^ 0x5eed1234ULL;
        *reinterpret_cast<volatile u64 *>(page + page_size - sizeof(u64)) = ~page;
      }
    }
  }
  ut::expect(valid && pages_owned == before.free_pages);
  ut::expect(Pfa::get_memory_stats().free_pages == 0);
  ut::expect(Pfa::get_memory_stats().used_pages == before.used_pages + pages_owned);
  ut::expect(!Pfa::allocate_pages(0));
  ut::expect(initrd_hash() == original_hash);
  while (count) {
    const auto &block = owned[--count];
    if (valid) {
      for (PhysAddr page = block.address; page < block.address + (page_size << block.order); page += page_size) {
        valid = valid && *reinterpret_cast<volatile u64 *>(page) == (page ^ 0x5eed1234ULL) &&
                *reinterpret_cast<volatile u64 *>(page + page_size - sizeof(u64)) == ~page;
      }
    }
    ut::expect(static_cast<bool>(Pfa::free_pages(block.address, block.order)));
  }
  ut::expect(valid);
  ut::expect(Pfa::get_memory_stats().free_pages == before.free_pages);
  ut::expect(Pfa::get_memory_stats().used_pages == before.used_pages);
  auto merged = Pfa::allocate_pages(MAX_ORDER);
  if (ut::expect(static_cast<bool>(merged))) {
    ut::expect(static_cast<bool>(Pfa::free_pages(*merged, MAX_ORDER)));
  }
  ut::expect(Pfa::get_memory_stats().free_pages == before.free_pages);
  layout.verify();
}

void address_space_heap_rollback() {
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
  {
    HeapPressure pressure;
    if (!ut::expect(pressure.acquire(sizeof(process::AddressSpace)))) {
      return;
    }
    // More attempts than the ASID capacity: even a one-lease-per-failure leak
    // must fail here, before pressure is removed and normal allocation resumes.
    for (unsigned attempt = 0; attempt < 256; ++attempt) {
      auto space = process::user_space::create_user_address_space();
      if (!ut::expect(!space && space.error() == ErrorCode::OutOfMemory)) {
        break;
      }
      ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == pages);
    }
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  // Reuse the existing public-boundary capacity check to require all 254 free
  // leases, not merely one successful retry after the failures.
  asid_leases();
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == pages);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
}

HeapPressure *address_space_control_pressure = nullptr;
bool address_space_control_exhausted = false;

void address_space_control_rollback() {
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
  HeapPressure pressure;
  // Repeated control-block failures must preserve the exact resource baseline.
  // Eight rounds exercise reuse without repeating the expensive full heap fill
  // for every ASID; the capacity check below independently detects one lost tag.
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    address_space_control_exhausted = false;
    address_space_control_pressure = &pressure;
    auto created = process::user_space::create_user_address_space();
    address_space_control_pressure = nullptr;
    ut::expect(address_space_control_exhausted && !created && created.error() == ErrorCode::OutOfMemory);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == pages);
  }
  asid_leases();
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == pages);
}

void vma_heap_rollback() {
  using namespace process;
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
  {
    auto created = user_space::create_user_address_space();
    if (!ut::expect(created.has_value())) {
      return;
    }
    auto &space = **created;
    constexpr auto base = user_layout::MMAP_BASE;
    if (!ut::expect(space.add_vma(base, base + page_size, vma_flags::READ, VmaType::MMAP))) {
      return;
    }
    const auto populated = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    {
      HeapPressure pressure;
      if (!ut::expect(pressure.acquire(sizeof(VmaRegion)))) {
        return;
      }
      ut::expect(!space.add_vma(base + page_size, base + 2 * page_size, vma_flags::WRITE, VmaType::MMAP));
      ut::expect(!space.find_vma(base + page_size) && space.vmas.size() == 1);
      ut::expect(space.allows_user_access(base, page_size, vma_flags::READ) &&
                 !space.allows_user_access(base, page_size, vma_flags::WRITE));
    }
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated);
    ut::expect(space.add_vma(base + page_size, base + 2 * page_size, vma_flags::WRITE, VmaType::MMAP));
    ut::expect(space.allows_user_access(base + page_size, page_size, vma_flags::WRITE));
    ut::expect(space.remove_vma(base + page_size, base + 2 * page_size) && space.vmas.size() == 1);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated);
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == pages);
}

void heap_alignment() {
  // Power-of-two alignments cover byte through page granularity. A 73-byte
  // request is deliberately unaligned; offsets 0/72 touch its exact endpoints,
  // and distinct arbitrary sentinels reveal aliasing or truncated usable storage.
  const usize alignments[] = {1, 2, 4, 8, 16, 32, 64, 256, 4096};
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  for (usize alignment : alignments) {
    auto allocation = mm::RuntimeHeapAllocator::allocate_aligned(73, alignment);
    if (!ut::expect(static_cast<bool>(allocation))) {
      return;
    }
    auto address = reinterpret_cast<usize>(*allocation);
    const bool aligned = ut::expect(address % alignment == 0);
    auto *bytes = static_cast<volatile u8 *>(*allocation);
    bytes[0] = 0x35;
    bytes[72] = 0x79;
    ut::expect(bytes[0] == 0x35 && bytes[72] == 0x79);
    ut::expect(static_cast<bool>(mm::RuntimeHeapAllocator::deallocate(*allocation, 73)));
    if (!aligned) {
      return;
    }
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}
void heap_invalid_requests() {
  using Heap = mm::RuntimeHeapAllocator;
  const auto before = Heap::get_heap_stats();
  const usize maximum = ~usize{0};
  ut::expect(!Heap::allocate(0));
  ut::expect(!Heap::allocate_aligned(16, 0));
  ut::expect(!Heap::allocate_aligned(16, 3));
  ut::expect(!Heap::allocate(maximum));
  ut::expect(!Heap::allocate(maximum - 31));
  ut::expect(!Heap::allocate_aligned(16, maximum));
  ut::expect(!Heap::allocate_aligned(16, usize{1} << 63));
  ut::expect(!Heap::expand_heap(0));
  ut::expect(!Heap::expand_heap(maximum));
  ut::expect(!Heap::expand_heap(maximum - page_size));
  const auto after = Heap::get_heap_stats();
  ut::expect(after.total_heap_size == before.total_heap_size);
  ut::expect(after.allocated_bytes == before.allocated_bytes);
  ut::expect(after.largest_free_block == before.largest_free_block);
}
void heap_release_contract() {
  // Allocate 73 bytes and reject a sized free of 72; endpoint sentinels must
  // survive that failure. The values distinguish mismatch from a valid release.
  using Heap = mm::RuntimeHeapAllocator;
  const auto before = Heap::get_heap_stats().allocated_bytes;
  auto allocation = Heap::allocate_aligned(73, page_size);
  if (!ut::expect(static_cast<bool>(allocation))) {
    return;
  }
  auto *bytes = static_cast<volatile u8 *>(*allocation);
  bytes[0] = 0x39;
  bytes[72] = 0x81;
  const auto live = Heap::get_heap_stats().allocated_bytes;
  ut::expect(static_cast<bool>(Heap::deallocate(nullptr, 0)));
  ut::expect(!Heap::deallocate(reinterpret_cast<void *>(Heap::get_heap_start()), 0));
  ut::expect(!Heap::deallocate(reinterpret_cast<void *>(Heap::get_heap_end()), 0));
  ut::expect(!Heap::deallocate(static_cast<u8 *>(*allocation) + 1, 0));
  ut::expect(!Heap::deallocate(static_cast<u8 *>(*allocation) + 16, 0));
  if (!ut::expect(!Heap::deallocate(*allocation, 72))) {
    return; // A broken sized free may already have released the allocation.
  }
  ut::expect(Heap::get_heap_stats().allocated_bytes == live);
  ut::expect(bytes[0] == 0x39 && bytes[72] == 0x81);
  ut::expect(static_cast<bool>(Heap::deallocate(*allocation, 73)));
  ut::expect(!Heap::deallocate(*allocation, 0));
  ut::expect(Heap::get_heap_stats().allocated_bytes == before);
}
void heap_reuse() {
  using Heap = mm::RuntimeHeapAllocator;
  struct Slot {
    void *pointer = nullptr;
    usize size = 0;
    u8 pattern = 0;
  };
  Slot slots[32]{};
  // Fixed seed and a modulo-2^32 linear-congruential recurrence make this
  // workload repeatable; these multiplier/increment values define the fixture,
  // not a source of cryptographic randomness or measured allocator tuning.
  const auto before = Heap::get_heap_stats().allocated_bytes;
  u32 seed = 0x5eed;
  bool valid = true;
  for (usize step = 0; step < 4096 && valid; ++step) {
    // Mix 32 live slots over 4096 bounded steps, with 1..1024-byte requests and
    // power-of-two alignments 2^3..2^12 (8 bytes..4 KiB). Bit slices vary choices
    // independently of the recurrence's weakest low bits.
    seed = seed * 1664525U + 1013904223U;
    auto &slot = slots[(seed >> 16) % 32];
    if (slot.pointer) {
      auto *bytes = static_cast<volatile u8 *>(slot.pointer);
      for (usize i = 0; i < slot.size; ++i) {
        valid = valid && bytes[i] == slot.pattern;
      }
      valid = valid && static_cast<bool>(Heap::deallocate(slot.pointer, slot.size));
      slot.pointer = nullptr;
    } else {
      slot.size = 1 + (seed >> 8) % 1024;
      const usize alignment = usize{1} << (3 + (seed >> 24) % 10);
      auto allocation = Heap::allocate_aligned(slot.size, alignment);
      if (!ut::expect(static_cast<bool>(allocation))) {
        valid = false;
        break;
      }
      slot.pointer = *allocation;
      slot.pattern = static_cast<u8>(step);
      const auto address = reinterpret_cast<usize>(slot.pointer);
      valid = valid && address % alignment == 0;
      for (const auto &other : slots) {
        if (other.pointer && &other != &slot) {
          const auto other_address = reinterpret_cast<usize>(other.pointer);
          valid = valid && (address + slot.size <= other_address || other_address + other.size <= address);
        }
      }
      auto *bytes = static_cast<volatile u8 *>(slot.pointer);
      for (usize i = 0; i < slot.size; ++i) {
        bytes[i] = slot.pattern;
      }
    }
  }
  for (const auto &slot : slots) {
    if (slot.pointer) {
      auto *bytes = static_cast<volatile u8 *>(slot.pointer);
      for (usize i = 0; i < slot.size; ++i) {
        valid = valid && bytes[i] == slot.pattern;
      }
      ut::expect(static_cast<bool>(Heap::deallocate(slot.pointer, slot.size)));
    }
  }
  ut::expect(valid);
  ut::expect(Heap::get_heap_stats().allocated_bytes == before);
}
void heap_exhaustion() {
  // 128 pointers track up to 8 MiB in 64 KiB chunks, then the fixture tests
  // smaller remainder allocations. These bound bookkeeping and stack usage.
  using Heap = mm::RuntimeHeapAllocator;
  constexpr usize chunk_size = static_cast<const usize>(64 * 1024);
  void *owned[128]{};
  auto sentinel = mm::PageFrameAllocator::allocate_pages(0);
  if (!ut::expect(static_cast<bool>(sentinel))) {
    return;
  }
  auto *page = reinterpret_cast<volatile u64 *>(*sentinel);
  // Arbitrary nonzero poison, varied by word index, detects allocator writes
  // escaping the heap into a separately owned physical page.
  for (usize i = 0; i < page_size / sizeof(u64); ++i) {
    page[i] = 0x123456789abcdef0ULL ^ i;
  }
  LayoutSnapshot layout;
  if (!layout.capture()) {
    ut::expect(static_cast<bool>(mm::PageFrameAllocator::free_pages(*sentinel, 0)));
    return;
  }
  const auto before = Heap::get_heap_stats().allocated_bytes;
  const auto free_pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
  // Hold these together to write beyond the old 4/64/256-KiB boundaries.
  constexpr usize probes[] = {static_cast<const usize>(4 * 1024), static_cast<const usize>(64 * 1024),
                              static_cast<const usize>(256 * 1024)};
  usize probe_count = 0;
  for (usize size : probes) {
    auto allocation = Heap::allocate(size);
    if (!ut::expect(static_cast<bool>(allocation))) {
      break;
    }
    owned[probe_count++] = *allocation;
    auto *bytes = static_cast<volatile u8 *>(*allocation);
    for (usize i = 0; i < size; ++i) {
      bytes[i] = 0xa5;
    }
    layout.verify();
  }
  while (probe_count) {
    --probe_count;
    ut::expect(static_cast<bool>(Heap::deallocate(owned[probe_count], probes[probe_count])));
  }
  usize count = 0;
  bool exhausted = false;
  bool valid = true;
  while (count < 128) {
    auto allocation = Heap::allocate(chunk_size);
    if (!allocation) {
      exhausted = allocation.error() == mm::HeapAllocError::OutOfMemory;
      break;
    }
    owned[count++] = *allocation;
    const auto address = reinterpret_cast<usize>(*allocation);
    valid =
        valid && address >= moss::abi::linker::heap_start() && address + chunk_size <= moss::abi::linker::heap_end();
    auto *bytes = static_cast<volatile u8 *>(*allocation);
    for (usize i = 0; i < chunk_size; ++i) {
      bytes[i] = static_cast<u8>(count);
    }
  }
  ut::expect(exhausted && count > 0);
  ut::expect(Heap::get_heap_end() == moss::abi::linker::heap_end());
  ut::expect(!Heap::expand_heap(page_size));
  layout.verify();
  while (count) {
    auto *bytes = static_cast<volatile u8 *>(owned[count - 1]);
    for (usize i = 0; i < chunk_size; ++i) {
      valid = valid && bytes[i] == static_cast<u8>(count);
    }
    ut::expect(static_cast<bool>(Heap::deallocate(owned[--count], chunk_size)));
  }
  for (usize i = 0; i < page_size / sizeof(u64); ++i) {
    valid = valid && page[i] == (0x123456789abcdef0ULL ^ i);
  }
  ut::expect(valid);
  ut::expect(Heap::get_heap_stats().allocated_bytes == before);
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == free_pages);
  layout.verify();
  ut::expect(static_cast<bool>(mm::PageFrameAllocator::free_pages(*sentinel, 0)));
  // A large allocation after release requires the split blocks to coalesce.
  auto reused = Heap::allocate(static_cast<usize>(1024 * 1024));
  if (ut::expect(static_cast<bool>(reused))) {
    ut::expect(static_cast<bool>(Heap::deallocate(*reused, 0)));
  }
  ut::expect(Heap::get_heap_stats().allocated_bytes == before);
}
void register_mm_cases() {
  ut::register_suite("mm", [] {
    ut::register_test("initialization_publication", mm_initialization_publication);
    ut::register_test("unsupported_contracts", mm_unsupported_contracts);
    ut::register_test("pageblock_units", moss::test::scheduler_regression::pageblock_units);
    ut::register_test("mmu_granule", [] { ut::expect(moss::test::hardware::arm64_mmu_granule_regression()); });
    ut::register_test("orders_alignment", pages);
    ut::register_test("reuse", reuse_pages);
  });
}

void register_pfa_cases() {
  ut::register_suite("pfa", [] {
    ut::register_test("release_contract", page_release_contract);
    ut::register_test("exhaustion", page_exhaustion);
  });
}

void register_heap_cases() {
  ut::register_suite("heap", [] {
    ut::register_test("alignment", heap_alignment);
    ut::register_test("invalid_requests", heap_invalid_requests);
    ut::register_test("release_contract", heap_release_contract);
    ut::register_test("reuse", heap_reuse);
    ut::register_test("exhaustion", heap_exhaustion);
  });
}

void register_mm_permissions_cases() {
  ut::register_suite("mm.permissions", [] {
    ut::register_test("table_defaults", table_permission_defaults);
    ut::register_test("kernel_mappings", kernel_mapping_permissions);
    ut::register_test("active_user_mappings", active_user_mapping_permissions);
    ut::register_test("kernel_wx", kernel_wx_permissions);
    ut::register_test("address_space_ownership", address_space_ownership);
    ut::register_test("cow_clone_permissions", cow_clone_permissions);
    ut::register_test("vma_boundaries", vma_boundaries);
  });
}

} // namespace moss::test::validation
