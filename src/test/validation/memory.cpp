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
import moss.drivers;
import moss.result;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "queue_regression.hpp"
#include "scheduler_regression.hpp"
#include "validation/memory_cases.hpp"
#include "validation/memory_internal.hpp"
#include "validation/memory_layout.hpp"

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
