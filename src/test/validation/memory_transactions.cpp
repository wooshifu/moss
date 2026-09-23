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

namespace moss::test::validation {
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

} // namespace moss::test::validation
