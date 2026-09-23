#pragma once

#include "validation_internal.hpp"

namespace moss::test::validation {
constexpr usize page_size = moss::kernel::PAGE_SIZE;

// Inspect the real kernel/user tables without exposing kernel pointers to EL0.
struct KernelPermissions {
  // Four-level tables start at VA bit 39; Sv39 uses bit 30. Each 512-entry
  // level contributes nine bits, ending at bit 12 for a 4 KiB leaf.
  unsigned root_shift = 39;
  u64 kernel_bytes = 0;
  usize user_leaves = 0;
  bool check_wx = false;

  KernelPermissions() {
#if defined(MOSS_ARCH_RISCV64)
    if (hal::mmu::g_mmu_mode == hal::mmu::MmuMode::Sv39) {
      root_shift = 30;
    }
#endif
  }

  void walk(PhysAddr root, unsigned shift, u64 prefix, bool user_root, bool user_access = true) {
    using Tables = mm::PageTableManager;
    const auto *table = Tables::get_table_from_physical(root);
    if (!ut::expect(root && table && (root & (page_size - 1)) == 0)) {
      return;
    }
    for (usize i = 0; i < mm::PageTable::ENTRIES_PER_TABLE; ++i) {
      const auto &entry = table->entries[i];
      if (!entry.is_valid()) {
        continue;
      }
      const u64 base = prefix | (static_cast<u64>(i) << shift);
      // Bit 8 is the highest bit of the nine-bit root index: it selects the
      // sign-extended kernel half of the active virtual-address layout.
      const bool high_half = (base & (1ULL << (root_shift + 8))) != 0;
      bool descendant_user_access = user_access;
#if defined(MOSS_ARCH_X64)
      descendant_user_access = user_access && (entry.raw & mm::page_attr::USER) != 0;
#endif
      if (shift > 12 && entry.is_table()) {
#if defined(MOSS_ARCH_X64)
        // The user's low root can mix kernel and user descendants; its leaf
        // permissions are checked below. Kernel-only branches need no USER bit.
        if (!user_root || high_half) {
          ut::expect((entry.raw & mm::page_attr::USER) == 0);
        }
#endif
        walk(entry.get_phys_addr(), shift - 9, base, user_root, descendant_user_access);
      } else if (!user_root || high_half || base < 0x100000000ULL) {
        ut::expect((entry.raw & mm::page_attr::USER) == 0);
        if (check_wx) {
#if defined(MOSS_ARCH_ARM64)
          const bool writable = (entry.raw & mm::page_attr::READONLY) == 0;
          const bool executable = (entry.raw & mm::page_attr::PXN) == 0;
          ut::expect((entry.raw & mm::page_attr::XN) != 0);
#elif defined(MOSS_ARCH_X64)
          const bool writable = (entry.raw & mm::page_attr::WRITABLE) != 0;
          const bool executable = (entry.raw & mm::page_attr::XN) == 0;
#else
          const bool writable = (entry.raw & mm::page_attr::WRITE) != 0;
          const bool executable = (entry.raw & mm::page_attr::EXECUTE) != 0;
#endif
          namespace linker = moss::abi::linker;
          const PhysAddr pa = entry.get_phys_addr();
          const u64 end = pa + (1ULL << shift);
          ut::expect(!(writable && executable));
          ut::expect(!executable || (!high_half && pa >= linker::text_start() && end <= linker::text_end()));
          if ((pa < linker::text_end() && end > linker::text_start()) ||
              (pa < linker::rodata_end() && end > linker::rodata_start())) {
            ut::expect(!writable);
          }
        }
        kernel_bytes += 1ULL << shift;
      } else {
        ut::expect(descendant_user_access && (entry.raw & mm::page_attr::USER) != 0);
        ++user_leaves;
      }
    }
  }
};

inline u64 memory_hash(PhysAddr begin, usize size) {
  // FNV-1a's 64-bit offset basis and prime detect accidental byte changes in
  // allocator-owned metadata; this is an integrity regression check, not a
  // cryptographic guarantee. https://www.rfc-editor.org/rfc/rfc9923.html#section-5
  u64 hash = 14695981039346656037ULL;
  const auto *bytes = reinterpret_cast<volatile u8 *>(begin);
  for (usize i = 0; i < size; ++i) {
    hash = (hash ^ bytes[i]) * 1099511628211ULL;
  }
  return hash;
}

inline u64 page_table_hash(PhysAddr address) {
  // Hardware may set Accessed/Dirty while the mapping and its owner stay intact.
  // Keep address, U/S, R/W, NX, software ownership and all other bits in the hash.
  u64 hardware_bits = mm::page_attr::AF;
#if !defined(MOSS_ARCH_ARM64)
  hardware_bits |= mm::page_attr::DIRTY;
#endif
  // Reuse the FNV-64 constants for word-wise descriptor mixing (not byte-wise
  // FNV-1a), so a hardware-only A/D update does not count as mapping corruption.
  u64 hash = 14695981039346656037ULL;
  const auto *table = reinterpret_cast<const mm::PageTable *>(address);
  for (const auto &entry : table->entries) {
    hash = (hash ^ (entry.raw & ~hardware_bits)) * 1099511628211ULL;
  }
  return hash;
}

// Own real PFA blocks while leaving a precisely controlled number of pages
// available to the production allocator. No allocation failure hook or mock.
struct PagePressure {
  PhysAddr head = 0;
  // Retain up to 16 individual pages for controlled rollback tests. This is a
  // fixture budget, not a PFA limit; pressure records live in the owned blocks
  // themselves so exhausting memory does not require further heap allocation.
  PhysAddr allowance[16]{};
  usize count = 0;

  bool acquire(usize budget) {
    if (budget > 16) {
      return false;
    }
    while (count < budget) {
      auto page = mm::allocate_pages(0);
      if (!page) {
        return false;
      }
      allowance[count++] = *page;
    }
    for (usize next_order = MAX_ORDER + 1; next_order; --next_order) {
      const usize order = next_order - 1;
      for (;;) {
        auto block = mm::allocate_pages(order);
        if (!block) {
          break;
        }
        auto *record = reinterpret_cast<u64 *>(phys_to_virt(*block));
        record[0] = head;
        record[1] = order;
        head = *block;
      }
    }
    return mm::PageFrameAllocator::get_memory_stats().free_pages == 0;
  }

  void give_one() {
    if (!count) {
      ut::expect(false);
      return;
    }
    // Retire the slot before freeing, so release() cannot retry a failed free.
    --count;
    ut::expect(mm::free_pages(allowance[count], 0).has_value());
  }

  void release() {
    while (head) {
      const auto page = head;
      const auto *record = reinterpret_cast<const u64 *>(phys_to_virt(page));
      head = record[0];
      const auto order = static_cast<usize>(record[1]);
      ut::expect(mm::free_pages(page, order).has_value());
    }
    while (count) {
      give_one();
    }
  }

  ~PagePressure() { release(); }
};
} // namespace moss::test::validation
