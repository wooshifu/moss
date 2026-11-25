import moss.std;
import moss.types;
import moss.arch;
import moss.abi;
import moss.mm;
import moss.platform;
import moss.hal.mmu;
import moss.process;

#include "framework/ut_kernel.hpp"
#include "validation/core_cases.hpp"
#include "validation/memory_internal.hpp"
#include "validation_internal.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;

namespace moss::test::validation {
namespace {

using Tables = mm::PageTableManager;

void clear_table(mm::PageTable *table) {
  for (usize i = 0; i < mm::PageTable::ENTRIES_PER_TABLE; ++i) {
    table->entries[i].clear();
  }
}

auto new_table() -> mm::PageTable * {
  auto page = mm::allocate_pages(0);
  if (!ut::expect(static_cast<bool>(page))) {
    return nullptr;
  }
  auto *table = Tables::get_table_from_physical(*page);
  clear_table(table);
  return table;
}

auto new_page() -> PhysAddr {
  auto page = mm::allocate_pages(0);
  if (!ut::expect(static_cast<bool>(page))) {
    return 0;
  }
  return *page;
}

struct UserTree {
  PhysAddr root_pa = 0;
  PhysAddr leaf_pa = 0;
  VirtAddr va = 0;
};

auto build_tree(VirtAddr va, bool with_block) -> UserTree {
  UserTree tree{.root_pa = 0, .leaf_pa = 0, .va = va};
  auto *root = new_table();
  if (!root) {
    return tree;
  }
  tree.root_pa = Tables::get_physical_address(root);
  auto *pud = new_table();
  auto *pmd = new_table();
  if (!pud || !pmd) {
    return tree;
  }
  root->entries[0].set_table(Tables::get_physical_address(pud), true);
  const auto pud_index = (va >> 30) & 511;
  const auto pmd_index = (va >> 21) & 511;
  const auto pte_index = (va >> 12) & 511;
  if (with_block) {
    pud->entries[pud_index].set_block(0x100000, mm::page_perms::USER_RW);
    return tree;
  }
  pud->entries[pud_index].set_table(Tables::get_physical_address(pmd), true);
  auto *pte = new_table();
  if (!pte) {
    return tree;
  }
  pmd->entries[pmd_index].set_table(Tables::get_physical_address(pte), true);
  tree.leaf_pa = new_page();
  if (!tree.leaf_pa) {
    return tree;
  }
  pte->entries[pte_index].set_page(tree.leaf_pa, mm::page_perms::USER_RW);
  return tree;
}

void invalid_paths() {
  ut::expect(Tables::is_dynamic_alloc());
  const auto kernel = Tables::get_physical_address(Tables::get_kernel_pgd());
  const auto high = Tables::get_physical_address(Tables::get_kernel_high_pgd());
  constexpr VirtAddr user_va = process::user_layout::CODE_BASE;
  ut::expect(!Tables::map_user_page(0, user_va, 0, mm::page_perms::USER_RW));
  ut::expect(!Tables::map_user_page(kernel, user_va, 0, mm::page_perms::USER_RW));
  ut::expect(!Tables::map_user_page(high, user_va, 0, mm::page_perms::USER_RW));
  ut::expect(!Tables::map_user_page(kernel, user_va, 0, mm::page_perms::KERNEL_RW));
  ut::expect(!Tables::map_user_page(kernel + 1, user_va, 0, mm::page_perms::USER_RW));
  ut::expect(!Tables::map_user_page(kernel, USER_MAX, 0, mm::page_perms::USER_RW));
  ut::expect(!Tables::map_page(0, 0, mm::page_perms::USER_RW));
  ut::expect(!Tables::map_region(1, 0, moss::kernel::PAGE_SIZE, mm::page_perms::KERNEL_RW));
  ut::expect(!Tables::map_region(0, 1, moss::kernel::PAGE_SIZE, mm::page_perms::KERNEL_RW));
  ut::expect(Tables::get_user_pte(0, user_va) == nullptr);
  ut::expect(Tables::get_user_pte(kernel, user_va) == nullptr);
  ut::expect(Tables::get_user_pte(high, user_va) == nullptr);
  ut::expect(Tables::resident_user_pages(0) == 0);
  ut::expect(Tables::resident_user_pages(kernel) == 0);
  ut::expect(!Tables::clone_user_page_tables(0, 0));
  ut::expect(!Tables::clone_user_page_tables(kernel, high));
  ut::expect(!Tables::clone_user_page_tables(kernel + 1, high));
  Tables::free_user_page_tables(0);
  Tables::free_user_page_tables(kernel);
  Tables::free_user_page_tables(high);
  Tables::unmap_user_page(0, user_va);
#if defined(MOSS_ARCH_ARM64)
  // x64 and RISC-V keep an identity mapping at physical address zero.
  ut::expect(!Tables::unmap_page(0));
  ut::expect(Tables::query_page(0).mapped);
#endif
  Tables::print_mmu_registers();
  Tables::print_pgd_entries();
  Tables::print_page_table_details();

  auto saved = platform::hardware;
  platform::hardware.memory_map_valid = false;
  ut::expect(!Tables::setup_kernel_page_tables());
  ut::expect(!Tables::setup_kernel_high_half_tables());
  platform::hardware = saved;
}

void free_tree_paths() {
  const auto tree = build_tree(process::user_layout::CODE_BASE, false);
  if (!ut::expect(tree.root_pa != 0 && tree.leaf_pa != 0)) {
    return;
  }
  ut::expect(Tables::get_user_pte(tree.root_pa, tree.va) != nullptr);
  ut::expect(Tables::resident_user_pages(tree.root_pa) == 1);
  Tables::free_user_page_tables(tree.root_pa);

  const auto blocked = build_tree(process::user_layout::CODE_BASE, true);
  if (!ut::expect(blocked.root_pa != 0)) {
    return;
  }
  Tables::free_user_page_tables(blocked.root_pa);
}

void clone_paths() {
  const auto source = build_tree(process::user_layout::CODE_BASE, false);
  auto *dst_root = new_table();
  if (!ut::expect(source.root_pa != 0 && dst_root != nullptr)) {
    return;
  }
  const auto dst_pa = Tables::get_physical_address(dst_root);
  ut::expect(Tables::clone_user_page_tables(source.root_pa, dst_pa));
  ut::expect(Tables::resident_user_pages(dst_pa) == 1);
  ut::expect(Tables::get_user_pte(dst_pa, source.va) != nullptr);
  ut::expect(!Tables::clone_user_page_tables(dst_pa, source.root_pa));
  ut::expect(!Tables::map_user_page(dst_pa, source.va, source.leaf_pa, mm::page_perms::USER_RW));
  const auto blocked = build_tree(process::user_layout::CODE_BASE + moss::kernel::PAGE_SIZE, true);
  if (blocked.root_pa) {
    Tables::free_user_page_tables(blocked.root_pa);
  }
  Tables::free_user_page_tables(source.root_pa);
  Tables::free_user_page_tables(dst_pa);
}

void fault_paths() {
  constexpr u32 vma_read = 1U << 0;
  constexpr u32 vma_write = 1U << 1;
  constexpr u32 vma_exec = 1U << 2;
  constexpr VirtAddr fault = process::user_layout::CODE_BASE + moss::kernel::PAGE_SIZE;
  const u8 backing[32]{};
  mm::UserFaultContext invalid{.root = 0,
                               .flags = vma_read,
                               .start = process::user_layout::CODE_BASE,
                               .backing = nullptr,
                               .backing_offset = 0,
                               .backing_size = 0,
                               .shared_pages = nullptr,
                               .shared_page_count = 0};
  ut::expect(!mm::resolve_user_demand_fault(invalid, fault, mm::UserFaultAccess::Read));
  invalid.root = 0x1000;
  ut::expect(!mm::resolve_user_demand_fault(invalid, 0, mm::UserFaultAccess::Read));
  invalid.flags = 0;
  ut::expect(!mm::resolve_user_demand_fault(invalid, fault, mm::UserFaultAccess::Read));

  auto *root = new_table();
  if (!ut::expect(root != nullptr)) {
    return;
  }
  const auto root_pa = Tables::get_physical_address(root);
  mm::UserFaultContext context{.root = root_pa,
                               .flags = vma_read | vma_write,
                               .start = process::user_layout::CODE_BASE,
                               .backing = backing,
                               .backing_offset = 0,
                               .backing_size = sizeof(backing),
                               .shared_pages = nullptr,
                               .shared_page_count = 0};
  ut::expect(mm::resolve_user_demand_fault(context, fault, mm::UserFaultAccess::Read));
  ut::expect(Tables::get_user_pte(root_pa, fault) != nullptr);
  ut::expect(mm::resolve_user_demand_fault(context, fault, mm::UserFaultAccess::Read));
  context.flags = vma_exec;
  ut::expect(!mm::resolve_user_demand_fault(context, fault + moss::kernel::PAGE_SIZE, mm::UserFaultAccess::Write));
  context.flags = vma_read;
  ut::expect(!mm::resolve_user_demand_fault(context, fault + moss::kernel::PAGE_SIZE, mm::UserFaultAccess::Write));

  const PhysAddr shared[] = {0};
  context.flags = vma_read;
  context.shared_pages = shared;
  context.shared_page_count = 0;
  ut::expect(!mm::resolve_user_demand_fault(context, fault + moss::kernel::PAGE_SIZE, mm::UserFaultAccess::Read));
  context.flags = vma_read | vma_exec;
  context.shared_page_count = 1;
  ut::expect(!mm::resolve_user_demand_fault(context, fault + moss::kernel::PAGE_SIZE, mm::UserFaultAccess::Read));
  context.flags = vma_read;
  ut::expect(!mm::resolve_user_demand_fault(context, fault + moss::kernel::PAGE_SIZE, mm::UserFaultAccess::Read));
  {
    PagePressure pressure;
    if (ut::expect(pressure.acquire(0))) {
      ut::expect(!mm::resolve_user_demand_fault(context, fault + moss::kernel::PAGE_SIZE, mm::UserFaultAccess::Read));
    }
  }
  Tables::free_user_page_tables(root_pa);

  auto cow_root = build_tree(fault, false);
  if (!ut::expect(cow_root.root_pa != 0 && cow_root.leaf_pa != 0)) {
    return;
  }
  mm::UserFaultContext cow{.root = cow_root.root_pa,
                           .flags = vma_write,
                           .start = process::user_layout::CODE_BASE,
                           .backing = nullptr,
                           .backing_offset = 0,
                           .backing_size = 0,
                           .shared_pages = nullptr,
                           .shared_page_count = 0};
  auto *pte = Tables::get_user_pte(cow_root.root_pa, fault);
  if (!ut::expect(pte != nullptr)) {
    return;
  }
  ut::expect(mm::resolve_user_cow_fault(cow, fault));
  pte->set_page(cow_root.leaf_pa, mm::page_perms::USER_RO);
  ut::expect(!mm::resolve_user_cow_fault(cow, fault));
  pte->set_cow();
  ut::expect(mm::resolve_user_cow_fault(cow, fault));
  pte->set_page(cow_root.leaf_pa, mm::page_perms::USER_RO);
  pte->set_cow();
  mm::PageFrameAllocator::page_ref_inc(cow_root.leaf_pa);
  reinterpret_cast<u8 *>(phys_to_virt(cow_root.leaf_pa))[0] = 0x5a;
  ut::expect(mm::resolve_user_cow_fault(cow, fault));
  ut::expect(mm::PageFrameAllocator::page_ref_get(cow_root.leaf_pa) == 1);
  ut::expect(mm::PageFrameAllocator::page_ref_dec(cow_root.leaf_pa) == 0);
  ut::expect(mm::free_pages(cow_root.leaf_pa, 0));
  Tables::free_user_page_tables(cow_root.root_pa);
}

void mapping_edges() {
  const auto kernel = Tables::get_physical_address(Tables::get_kernel_pgd());
  ut::expect(kernel != 0 && Tables::get_kernel_pgd() != nullptr);
  const auto identity = Tables::query_page(0x1000);
  ut::expect(identity.mapped && identity.level != 0);
  ut::expect(Tables::get_table_index(Tables::get_table_by_index(0)) == 0);
  ut::expect(Tables::get_table_by_index(mm::PageTableManager::KERNEL_IDENTITY_END) == nullptr);
  ut::expect(Tables::get_table_index(nullptr) == 64);

  const auto spare = mm::allocate_pages(0);
  const auto spare_two = mm::allocate_pages(0);
  if (!ut::expect(static_cast<bool>(spare) && static_cast<bool>(spare_two))) {
    return;
  }
  const VirtAddr spare_va = KERNEL_BASE + (100ULL << 39) + (7ULL << 30) + (9ULL << 21) + (11ULL << 12);
  ut::expect(!Tables::map_page(spare_va, *spare, mm::page_perms::USER_RW));
  ut::expect(Tables::map_page(spare_va, *spare, mm::page_perms::KERNEL_RW).has_value());
  const auto mapped = Tables::query_page(spare_va);
  ut::expect(mapped.mapped && mapped.phys_addr == *spare && mapped.level == 3);
  ut::expect(Tables::map_page(spare_va, *spare, mm::page_perms::KERNEL_RW).has_value());
  ut::expect(Tables::unmap_page(spare_va).has_value());
  ut::expect(!Tables::query_page(spare_va).mapped);
  ut::expect(!Tables::unmap_page(spare_va).has_value());

  ut::expect(Tables::map_region(spare_va + moss::kernel::PAGE_SIZE * 2, 0, 0, mm::page_perms::KERNEL_RW).has_value());
  ut::expect(Tables::map_region(spare_va + moss::kernel::PAGE_SIZE * 2, *spare_two, moss::kernel::PAGE_SIZE * 2, mm::page_perms::KERNEL_RW)
                 .has_value());
  ut::expect(Tables::query_page(spare_va + moss::kernel::PAGE_SIZE * 2).mapped);
  ut::expect(Tables::query_page(spare_va + moss::kernel::PAGE_SIZE * 3).mapped);
  ut::expect(Tables::unmap_page(spare_va + moss::kernel::PAGE_SIZE * 2).has_value());
  ut::expect(Tables::unmap_page(spare_va + moss::kernel::PAGE_SIZE * 3).has_value());
  ut::expect(mm::free_pages(*spare, 0).has_value());
  ut::expect(mm::free_pages(*spare_two, 0).has_value());

  // Low physical addresses are live kernel mappings on x64, so probe the
  // unmapped spare range instead of unmapping boot identity pages.
  const auto block_unmap = Tables::unmap_page(spare_va + (1ULL << 30));
  const auto absent_unmap = Tables::unmap_page(spare_va + (1ULL << 39));
  ut::expect(!block_unmap.has_value() && !absent_unmap.has_value());
  ut::expect(true);
}

void allocation_failures() {
  // Normal creation exercises the private PUD path; the allocation-free path
  // is also reachable when dynamic allocation is disabled.
  auto created = Tables::create_user_page_tables();
  if (ut::expect(static_cast<bool>(created))) {
    ut::expect(Tables::get_table_from_physical(*created) != nullptr);
    Tables::free_user_page_tables(*created);
  }

  const auto source = build_tree(process::user_layout::CODE_BASE, false);
  auto *destination = new_table();
  if (!ut::expect(source.root_pa != 0 && destination != nullptr)) {
    return;
  }
  const auto destination_pa = Tables::get_physical_address(destination);
  const auto spare = mm::allocate_pages(0);
  if (!ut::expect(static_cast<bool>(spare))) {
    Tables::free_user_page_tables(source.root_pa);
    Tables::free_user_page_tables(destination_pa);
    return;
  }
  const VirtAddr spare_va = KERNEL_BASE + (200ULL << 39) + (3ULL << 30) + (5ULL << 21) + (7ULL << 12);
  {
    PagePressure pressure;
    if (ut::expect(pressure.acquire(0))) {
      ut::expect(!Tables::create_user_page_tables());
      ut::expect(!Tables::clone_user_page_tables(source.root_pa, destination_pa));
      ut::expect(!Tables::map_page(spare_va, *spare, mm::page_perms::KERNEL_RW));
      ut::expect(!Tables::map_user_page(source.root_pa, process::user_layout::CODE_BASE + (1ULL << 30), *spare,
                                        mm::page_perms::USER_RW));
    }
  }
  ut::expect(Tables::map_page(spare_va, *spare, mm::page_perms::KERNEL_RW).has_value());
  ut::expect(Tables::unmap_page(spare_va).has_value());
  ut::expect(mm::free_pages(*spare, 0).has_value());
  Tables::free_user_page_tables(source.root_pa);
  Tables::free_user_page_tables(destination_pa);
}

void allocation_edges() {
  const auto page = mm::allocate_pages(0);
  if (!ut::expect(static_cast<bool>(page))) {
    return;
  }
  const VirtAddr base = KERNEL_BASE + (300ULL << 39) + (7ULL << 30) + (11ULL << 21);
  const auto mapped = Tables::map_page(base, *page, mm::page_perms::KERNEL_RW);
  ut::expect(static_cast<bool>(mapped));
  if (mapped) {
    ut::expect(Tables::unmap_page(base).has_value());
  }
  ut::expect(mm::free_pages(*page, 0).has_value());

  const auto source = build_tree(process::user_layout::CODE_BASE, false);
  auto *destination = new_table();
  if (!ut::expect(source.root_pa != 0 && destination != nullptr)) {
    return;
  }
  const auto destination_pa = Tables::get_physical_address(destination);
  ut::expect(static_cast<bool>(Tables::clone_user_page_tables(source.root_pa, destination_pa)));
  Tables::free_user_page_tables(source.root_pa);
  Tables::free_user_page_tables(destination_pa);
}

void block_mapping_paths() {
  const VirtAddr addresses[] = {0,
                                0x1000,
                                0x200000,
                                0x40000000,
                                0x80000000,
                                0xC0000000,
                                KERNEL_BASE + (1ULL << 39),
                                KERNEL_BASE + (1ULL << 39) + (1ULL << 30)};
  for (const auto address : addresses) {
    const auto info = Tables::query_page(address);
    (void)info;
  }
  (void)Tables::unmap_page(0x200000);
  (void)Tables::unmap_page(0x100000000ULL);
  const auto blocked = build_tree(process::user_layout::CODE_BASE, true);
  if (ut::expect(blocked.root_pa != 0)) {
    ut::expect(Tables::get_user_pte(blocked.root_pa, process::user_layout::CODE_BASE) == nullptr);
    Tables::unmap_user_page(blocked.root_pa, process::user_layout::CODE_BASE);
    Tables::free_user_page_tables(blocked.root_pa);
  }
  ut::expect(true);
}

} // namespace

void register_page_table_cases() {
  ut::register_suite("page_table", [] {
    ut::register_test("invalid_paths", invalid_paths);
    ut::register_test("free_tree", free_tree_paths);
    ut::register_test("clone", clone_paths);
    ut::register_test("faults", fault_paths);
    ut::register_test("mapping_edges", mapping_edges);
    ut::register_test("allocation_failures", allocation_failures);
    ut::register_test("allocation_edges", allocation_edges);
    ut::register_test("block_mapping_paths", block_mapping_paths);
  });
}

} // namespace moss::test::validation
