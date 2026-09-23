#pragma once

#include "framework/ut_kernel.hpp"
#include "validation/memory_internal.hpp"

namespace moss::test::validation {
using namespace moss::kernel;
namespace ut = boost::ut;
inline bool ram_contains(PhysAddr begin, PhysAddr end) {
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

} // namespace moss::test::validation
