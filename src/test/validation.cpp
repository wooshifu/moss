import moss.std;
import moss.types;
import moss.arch;
import moss.abi;
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

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "ipc_regression.hpp"
#include "queue_regression.hpp"
#include "scheduler_regression.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;

namespace moss::kernel {
void kernel_uart_puts(const char *str) noexcept { hal::uart::puts(str); }
[[noreturn]] void kernel_test_exit([[maybe_unused]] int code) noexcept {
  // The host observes the completed serial protocol and owns process termination.
  arch::disable_interrupts();
  for (;;) {
    arch::cpu_halt();
  }
}
} // namespace moss::kernel

namespace moss::bench {
Tick read_counter() noexcept {
#if defined(MOSS_ARCH_ARM64)
  Tick value;
  asm volatile("dsb ish; isb; mrs %0, cntvct_el0; isb" : "=r"(value) : : "memory");
  return value;
#elif defined(MOSS_ARCH_X64)
  unsigned lo, hi;
  asm volatile("mfence; lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) : : "memory");
  return (static_cast<Tick>(hi) << 32) | lo;
#else
  Tick value;
  asm volatile("fence iorw,iorw; rdtime %0; fence iorw,iorw" : "=r"(value) : : "memory");
  return value;
#endif
}
unsigned current_cpu() noexcept { return arch::get_current_cpu_id(); }
} // namespace moss::bench

namespace {
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

u64 memory_hash(PhysAddr begin, usize size) {
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

u64 page_table_hash(PhysAddr address) {
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

PagePressure *uaccess_pressure = nullptr;
usize uaccess_free_before = 0;
constexpr const char *uaccess_cases[] = {"allocation_fault", "write_fault",     "read_fault",
                                         "partial_read",     "partial_write",   "partial_pipe_read",
                                         "sigframe_fault",   "sigreturn_fault", "devices"};

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
    unique_ptr<AddressSpace> spaces[255];
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
    ut::expect(space.add_vma(base, base + page_size, vma_flags::READ | vma_flags::WRITE, VmaType::MMAP));
    ut::expect(space.add_vma(base + page_size, base + 2 * page_size, vma_flags::READ, VmaType::MMAP));
    ut::expect(space.allows_user_access(base + page_size - 8, 16, vma_flags::READ));
    ut::expect(!space.allows_user_access(base + page_size - 8, 16, vma_flags::WRITE));
    ut::expect(!space.allows_user_access(base + 2 * page_size - 8, 16, vma_flags::READ));
    ut::expect(!space.allows_user_access(0, 1, vma_flags::READ));
    ut::expect(!space.allows_user_access(USER_MAX - 1, 2, vma_flags::READ));
    ut::expect(space.allows_user_access(0, 0, vma_flags::READ));
    const auto stub = user_layout::SIGRETURN_PAGE;
    ut::expect(!space.add_vma(stub, stub + page_size, vma_flags::WRITE, VmaType::SIGRETURN));
    ut::expect(space.add_vma(stub, stub + page_size, vma_flags::READ | vma_flags::EXEC, VmaType::SIGRETURN));
  }
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == before);
}

// Match valid_id's 80-character limit and reserve one byte for the terminator.
char selection[81]{};
const char *active_case = nullptr;
bool failed = false;
unsigned completed = 0;
unsigned selected_count = 0;
unsigned sample_index = 0;
unsigned warmup_count = 5;
unsigned sample_count = 30;
usize fixed_iterations = 0;
bool stability = false;
bool is_lifecycle() {
  return ut::same_id(selection, "users.lifecycle") || ut::same_id(selection, "users.applications");
}
bench::Clock clock_info;
bench::Scenario *selected_benchmark = nullptr;
usize syscall_iterations = 1;
bool syscall_pilot = true;
u64 syscall_overhead = 0;
usize syscall_capacity = 65536;
const char *const user_benchmark_names[] = {"bench.fault",     "bench.cow",    "bench.switch",
                                            "bench.lifecycle", "bench.signal", "bench.pipe"};

// Private userspace dispatch modes: 2 is getpid and 11..16 follow this six-name
// catalog's order. Keep these numbers synchronized with userspace/validation.c.
long user_benchmark_mode() {
  if (ut::same_id(selection, "bench.getpid")) {
    return 2;
  }
  for (long i = 0; i < 6; ++i) {
    if (ut::same_id(selection, user_benchmark_names[i])) {
      return 11 + i;
    }
  }
  return 0;
}

// All strings come from the bounded catalog or fixed diagnostic identifiers.
// One complete record per UART write avoids allocating a formatting buffer.
class Event {
  // Fixed 1 KiB serial-record budget avoids heap allocation. Overflow marks the
  // run failed instead of emitting a truncated record that could parse as success.
  char buffer_[1024]{};
  usize used_ = 0;
  bool overflow_ = false;
  void append(char c) {
    if (used_ + 1 >= sizeof(buffer_)) {
      overflow_ = true;
      return;
    }
    buffer_[used_++] = c;
    buffer_[used_] = 0;
  }
  void append(const char *s) {
    while (*s) {
      append(*s++);
    }
  }

public:
  explicit Event(const char *type) {
    append("@@MOSS {\"v\":1");
    str("event", type);
    str("workload", selection);
  }
  Event &str(const char *key, const char *value) {
    append(",\"");
    append(key);
    append("\":\"");
    for (; *value; ++value) {
      if (*value == '\\' || *value == '"') {
        append('\\');
      }
      append(*value);
    }
    append('"');
    return *this;
  }
  Event &number(const char *key, u64 value) {
    append(",\"");
    append(key);
    append("\":");
    // A u64 needs at most 20 decimal digits; append adds them individually, so
    // this reverse-order scratch array needs no null terminator.
    char digits[20];
    unsigned n = 0;
    do {
      digits[n++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value);
    while (n) {
      append(digits[--n]);
    }
    return *this;
  }
  void send() {
    append("}\n");
    if (overflow_) {
      kernel_test_exit(2);
    }
    hal::uart::puts(buffer_);
  }
};

[[noreturn]] void finish(const char *reason = "complete") {
  Event("end")
      .number("completed", completed)
      .number("selected", selected_count)
      .number("failed", failed ? 1 : 0)
      .str("reason", reason)
      .send();
  kernel_test_exit(failed ? 1 : 0);
}

bool option(const char *key, char *out, usize capacity) {
  const char *args = moss::fdt::get_platform_info().bootargs;
  if (!args) {
    return false;
  }
  while (*args) {
    while (*args == ' ') {
      ++args;
    }
    const char *token = args;
    while (*args && *args != ' ') {
      ++args;
    }
    const char *p = token;
    const char *k = key;
    while (*k && p < args && *p == *k) {
      ++k;
      ++p;
    }
    if (*k == 0 && p < args && *p == '=') {
      ++p;
      usize n = static_cast<usize>(args - p);
      if (!n || n >= capacity) {
        return false;
      }
      for (usize i = 0; i < n; ++i) {
        out[i] = p[i];
      }
      out[n] = 0;
      return true;
    }
  }
  return false;
}

u64 numeric_option(const char *key, u64 fallback) {
  // Twenty-four bytes fit a u64's 20 decimal digits and terminator. The parser
  // additionally rejects a pre-digit accumulator above 100,000,000, bounding
  // subsequent multiply/add; selection-specific limits are checked at boot.
  char value[24];
  if (!option(key, value, sizeof(value))) {
    return fallback;
  }
  u64 parsed = 0;
  for (const char *p = value; *p; ++p) {
    if (*p < '0' || *p > '9' || parsed > 100000000) {
      failed = true;
      return 0;
    }
    parsed = parsed * 10 + static_cast<unsigned>(*p - '0');
  }
  return parsed;
}

bool affinity_valid() {
  auto *thread = process::CfsScheduler::get_current_task();
  return thread && thread->cpu_affinity_mask.low_word() == 1 && arch::get_current_cpu_id() == 0;
}

void resources() {
  const auto &info = moss::fdt::get_platform_info();
  // Match the host validation runner's default four vCPUs and 2048 MiB; the
  // boot options override these fixture expectations. Convert MiB to bytes.
  u64 cpus = numeric_option("moss.cpus", 4);
  u64 memory = numeric_option("moss.memory", 2048) * 1024 * 1024;
  u64 mask = (1ULL << cpus) - 1;
  ut::expect(info.cpu_count == cpus);
  ut::expect(__atomic_load_n(&moss::boot::online_cpu_mask, __ATOMIC_ACQUIRE) == mask);
  ut::expect(__atomic_load_n(&moss::boot::cpu_work_mask, __ATOMIC_ACQUIRE) == mask);
  ut::expect(info.memory_map_valid);
  // Allow the same 2 MiB RAM-reporting shortfall as the host protocol parser.
  // This is an acceptance tolerance; its original tuning basis is unrecorded.
  ut::expect(info.total_memory_size <= memory && info.total_memory_size + 0x200000 >= memory);
  auto stats = mm::PageFrameAllocator::get_memory_stats();
  // Probe beyond 256 MiB (half of RAM for smaller fixtures), catching an
  // allocator that only serves the low part of the advertised memory range.
  const u64 probe_offset = memory > 256UL * 1024 * 1024 ? 256UL * 1024 * 1024 : memory / 2;
  ut::expect(stats.total_pages * page_size > probe_offset);
  // Order 9 is 512 * 4 KiB = 2 MiB. Holding up to 129 disjoint blocks exceeds
  // the 128 blocks fitting below the 256 MiB probe, forcing a high allocation
  // in the default fixture. Fragmented or smaller fixtures may fail earlier.
  PhysAddr owned[129]{};
  usize count = 0;
  bool high = false;
  while (count < 129 && !high) {
    auto page = mm::PageFrameAllocator::allocate_pages(9);
    if (!ut::expect(static_cast<bool>(page))) {
      break;
    }
    owned[count++] = *page;
    ut::expect((*page & ((page_size << 9) - 1)) == 0);
    high = *page >= info.total_memory_start + probe_offset;
    auto *values = reinterpret_cast<volatile u64 *>(*page);
    values[0] = *page;
    values[(page_size << 9) / sizeof(u64) - 1] = ~*page;
    ut::expect(values[0] == *page && values[(page_size << 9) / sizeof(u64) - 1] == ~*page);
  }
  ut::expect(high);
  while (count) {
    ut::expect(static_cast<bool>(mm::PageFrameAllocator::free_pages(owned[--count], 9)));
  }
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == stats.free_pages);
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

void *fd_table() {
  auto *thread = process::CfsScheduler::get_current_task();
  auto proc = process::g_process_manager->find_process(thread->owner_pid);
  return proc->fd_table();
}

struct LifecycleResources {
  u64 heap_bytes = 0, free_pages = 0, processes = 0, threads = 0, descriptors = 0, file_refs = 0;
  u64 user_pages = 0, stack_pages = 0;
  vfs::PoolUsage vfs_pools;

  static LifecycleResources capture() {
    LifecycleResources result;
    process::g_process_manager->for_each_process([&](auto, process::Process *proc) {
      ++result.processes;
      result.threads += proc->thread_count();
      if (auto *as = proc->address_space()) {
        as->vmas.for_each([&](const process::VmaRegion &vma) {
          for (VirtAddr va = vma.start_addr; va < vma.end_addr; va += page_size) {
            auto *pte = mm::PageTableManager::get_user_pte(as->pgd_phys, va);
            if (pte && pte->is_valid()) {
              ++result.user_pages;
              result.stack_pages += vma.type == process::VmaType::STACK;
            }
          }
        });
      }
    });
    auto *table = static_cast<vfs::FdTable *>(fd_table());
    for (u32 fd = 0; table && fd < vfs::MAX_FDS; ++fd) {
      if (auto *file = table->get_file(fd)) {
        ++result.descriptors;
        result.file_refs += file->ref_count;
      }
    }
    // Snapshot iteration owns temporary storage. Account only after it is freed.
    result.heap_bytes = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    result.free_pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
    result.vfs_pools = vfs::pool_usage();
    return result;
  }
  bool operator==(const LifecycleResources &) const = default;
};

// Store ownership in the real allocations themselves: exhausting the heap
// must not need another allocation to remember how to release it.
struct HeapPressure {
  void *head = nullptr;

  bool acquire(usize size) {
    for (;;) {
      auto allocation = mm::RuntimeHeapAllocator::allocate(size);
      if (!allocation) {
        return allocation.error() == mm::HeapAllocError::OutOfMemory;
      }
      *static_cast<void **>(*allocation) = head;
      head = *allocation;
    }
  }
  bool release_one() {
    if (!head) {
      return false;
    }
    void *block = head;
    head = *static_cast<void **>(block);
    return ut::expect(mm::RuntimeHeapAllocator::deallocate(block, 0).has_value());
  }
  void release() {
    while (head) {
      release_one();
    }
  }
  ~HeapPressure() { release(); }
};

void ipc_heap_rollback() {
  const auto heap_baseline = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto page_baseline = mm::PageFrameAllocator::get_memory_stats().free_pages;
  ipc::SharedMemoryManager manager;
  bool recovered = false;
  unsigned failures = 0;
  {
    HeapPressure pressure;
    if (!ut::expect(pressure.acquire(sizeof(void *)))) {
      return;
    }
    // Restore heap capacity one real allocation at a time. Descriptor,
    // control-block and tracking-node failures must all return the backing.
    for (;;) {
      const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
      auto created = manager.create_region(0, page_size);
      if (created) {
        recovered = true;
        ut::expect(manager.destroy_region(*created).has_value());
      } else {
        ++failures;
        ut::expect(created.error() == KernelError::OutOfMemory);
      }
      ut::expect(manager.get_statistics().total_regions == 0);
      ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
      ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == page_baseline);
      if (recovered || !ut::expect(pressure.release_one())) {
        break;
      }
    }
  }
  ut::expect(failures > 0 && recovered);
  {
    HeapPressure pressure;
    usize exhausted_pages = 0;
    {
      ipc::SharedMemoryManager teardown;
      auto created = teardown.create_region(0, page_size);
      if (!ut::expect(created.has_value()) || !ut::expect(pressure.acquire(sizeof(void *)))) {
        return;
      }
      exhausted_pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
      // The manager leaves scope while every heap allocation still fails.
      // Teardown must not allocate a snapshot just to release existing regions.
    }
    ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == exhausted_pages + 1);
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap_baseline);
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == page_baseline);
}

void process_heap_rollback() {
  auto &manager = *process::g_process_manager;
  const auto baseline = LifecycleResources::capture();
  const auto forks = manager.total_forks();
  const auto exits = manager.total_exits();
  bool recovered = false;
  unsigned failures = 0;
  {
    HeapPressure pressure;
    if (!ut::expect(pressure.acquire(sizeof(void *)))) {
      return;
    }
    // Gradually restore real capacity through the public creation boundary;
    // intermediate failures must release every partially constructed object.
    for (usize attempt = 0; attempt < sizeof(process::Process) / sizeof(void *) + 16; ++attempt) {
      const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
      {
        auto created = manager.create_process();
        if (!created) {
          ++failures;
          ut::expect(created.error() == ErrorCode::OutOfMemory);
          ut::expect(manager.total_forks() == forks && manager.total_exits() == exits);
        } else {
          recovered = true;
          auto &proc = *created;
          ut::expect(manager.total_processes() == baseline.processes + 1);
          ut::expect(manager.find_process(proc->pid()).get() == proc.get());
          ut::expect(manager.terminate_process(proc->pid(), 37).has_value());
          ut::expect(!manager.process_exists(proc->pid()));
        }
      }
      ut::expect(manager.total_processes() == baseline.processes);
      ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
      ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == baseline.free_pages);
      if (recovered || !ut::expect(pressure.release_one())) {
        break;
      }
    }
  }
  ut::expect(failures > 0 && recovered);
  ut::expect(manager.total_forks() == forks + 1 && manager.total_exits() == exits + 1);
  ut::expect(LifecycleResources::capture() == baseline);
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

LifecycleResources lifecycle_baseline;
struct ForkMetadataPressure {
  LifecycleResources baseline;
  HeapPressure heap;
  unsigned stage = 0, vmas = 0;
  bool holding = false, exhausted = false;
};
ForkMetadataPressure *fork_metadata_pressure = nullptr;
HeapPressure *fork_clone_pressure = nullptr;
LifecycleResources fork_clone_baseline;
bool fork_clone_exhausted = false;
HeapPressure *user_heap_pressure = nullptr;
LifecycleResources user_heap_baseline;
PagePressure *exec_pressure = nullptr;
LifecycleResources exec_baseline;
process::AddressSpace *exec_original = nullptr;
PhysAddr exec_root = 0;
u64 exec_root_hash = 0;
char exec_name[16]{};
LifecycleResources benchmark_resources;
bool benchmark_warmed = false;
u64 benchmark_switches = 0;
u64 lifecycle_checkpoint = 0;
u64 lifecycle_started_ns = 0;
bool lifecycle_started = false;
bool lifecycle_complete = false;
bool lifecycle_host_released = false;

void file_read() {
  void *table = fd_table();
  long fd = vfs::syscall::do_open(table, "/fixture.bin", 0, 0);
  if (!ut::expect(fd >= 0)) {
    return;
  }
  u8 bytes[256]{};
  ut::expect(vfs::syscall::do_read(table, fd, vfs::OutputBuffer::kernel(bytes, sizeof(bytes))) == 256);
  bool content = true;
  for (unsigned i = 0; i < 256; ++i) {
    content = content && bytes[i] == i;
  }
  ut::expect(content);
  ut::expect(vfs::syscall::do_lseek(table, fd, 0, 1) == 256);
  ut::expect(vfs::syscall::do_lseek(table, fd, 65536, 0) == 65536);
  ut::expect(vfs::syscall::do_read(table, fd, vfs::OutputBuffer::kernel(bytes, sizeof(bytes))) == 0);
  ut::expect(vfs::syscall::do_close(table, fd) == 0);
  ut::expect(vfs::syscall::do_close(table, fd) == -static_cast<long>(vfs::VfsError::BadFd));
}

void file_errors() {
  void *table = fd_table();
  ut::expect(vfs::syscall::do_open(table, "/missing.fixture", 0, 0) == -static_cast<long>(vfs::VfsError::NoEntry));
  long fd = vfs::syscall::do_open(table, "/fixture.bin", 2, 0);
  if (!ut::expect(fd >= 0)) {
    return;
  }
  const u8 byte = 7;
  ut::expect(vfs::syscall::do_write(table, fd, vfs::InputBuffer::kernel(&byte, 1)) ==
             -static_cast<long>(vfs::VfsError::PermDenied));
  ut::expect(vfs::syscall::do_close(table, fd) == 0);
}

void fd_boundaries() {
  vfs::FdTable table;
  long fd = vfs::syscall::do_open(&table, "/fixture.bin", 0, 0);
  if (!ut::expect(fd >= 0)) {
    return;
  }
  constexpr long bad_fd = 1L << 32;
  auto *file = table.get_file(fd);
  bool valid = ut::expect(table.get_file(bad_fd + fd) == nullptr);
  valid = ut::expect(table.close_fd(bad_fd + fd) == -static_cast<long>(vfs::VfsError::BadFd)) && valid;
  if (valid) {
    ut::expect(vfs::syscall::do_dup2(&table, fd, bad_fd + fd) == -static_cast<long>(vfs::VfsError::BadFd));
    ut::expect(table.get_file(fd) == file && file->ref_count == 1);
  }
  {
    vfs::FdTable::Reservation pending(table);
    ut::expect(pending.fd() == 1 && table.get_file(1) == nullptr);
    ut::expect(table.close_fd(1) == -static_cast<long>(vfs::VfsError::BadFd));
    ut::expect(table.descriptor_flags(1) == -static_cast<long>(vfs::VfsError::BadFd));
    ut::expect(vfs::syscall::do_dup2(&table, fd, 1) == -static_cast<long>(vfs::VfsError::Busy));
    table.close_on_exec();
    ut::expect(vfs::syscall::do_dup(&table, fd) == 2);
    ut::expect(table.close_fd(2) == 0);
    long ends[2] = {-1, -1};
    if (ut::expect(vfs::syscall::do_pipe(&table, ends) == 0)) {
      ut::expect(ends[0] == 2 && ends[1] == 3 && table.get_file(1) == nullptr);
      ut::expect(table.close_fd(ends[0]) == 0);
      ut::expect(table.close_fd(ends[1]) == 0);
    }
    auto *copy = table.clone();
    ut::expect(copy->get_file(1) == nullptr && vfs::syscall::do_dup(copy, fd) == 1);
    copy->close_all();
    delete copy;
  }
  ut::expect(file->ref_count == 1 && vfs::syscall::do_dup(&table, fd) == 1);
  ut::expect(table.close_fd(1) == 0);
  {
    vfs::FdTable::Reservation pending(table);
    ut::expect(pending.fd() == 1 && pending.install(file, true) == 1);
  }
  ut::expect(table.get_file(1) == file && table.descriptor_flags(1) == 1);
  table.close_on_exec();
  ut::expect(table.get_file(1) == nullptr && file->ref_count == 1);
  for (unsigned attempt = 0; attempt < vfs::MAX_FDS; ++attempt) {
    ut::expect(vfs::syscall::do_open(&table, "/missing-open-reservation", 0, 0) ==
               -static_cast<long>(vfs::VfsError::NoEntry));
  }
  const u32 occupied = vfs::file_pool_usage();
  auto **held = new vfs::File *[vfs::MAX_FILES] {};
  u32 count = 0;
  while (count < vfs::MAX_FILES && (held[count] = vfs::alloc_file()) != nullptr) {
    ++count;
  }
  ut::expect(count == vfs::MAX_FILES - occupied);
  long pipe_ends[2] = {-37, -73};
  ut::expect(vfs::syscall::do_pipe(&table, pipe_ends) == -static_cast<long>(vfs::VfsError::NoMemory));
  ut::expect(pipe_ends[0] == -37 && pipe_ends[1] == -73);
  // One available File cannot build both endpoints; return that temporary
  // File as well as both reserved descriptors on the second allocation error.
  if (count) {
    vfs::release_file(held[--count]);
    ut::expect(vfs::syscall::do_pipe(&table, pipe_ends) == -static_cast<long>(vfs::VfsError::NoMemory));
    ut::expect(pipe_ends[0] == -37 && pipe_ends[1] == -73);
    ut::expect(vfs::file_pool_usage() == vfs::MAX_FILES - 1);
    held[count] = vfs::alloc_file();
    if (ut::expect(held[count] != nullptr)) {
      ++count;
    }
  }
  ut::expect(vfs::syscall::do_open(&table, "/uncreated-open-reservation", vfs::O_RDWR | vfs::O_CREAT, 0600) ==
             -static_cast<long>(vfs::VfsError::NoMemory));
  vfs::Stat missing{};
  ut::expect(vfs::syscall::do_stat("/uncreated-open-reservation", &missing) ==
             -static_cast<long>(vfs::VfsError::NoEntry));
  for (u32 i = 0; i < count; ++i) {
    vfs::release_file(held[i]);
  }
  delete[] held;
  ut::expect(vfs::file_pool_usage() == occupied);
  if (ut::expect(vfs::syscall::do_pipe(&table, pipe_ends) == 0)) {
    ut::expect(pipe_ends[0] == 1 && pipe_ends[1] == 2);
    ut::expect(table.close_fd(pipe_ends[0]) == 0);
    ut::expect(table.close_fd(pipe_ends[1]) == 0);
  }
  ut::expect(vfs::syscall::do_dup(&table, fd) == 1);
  table.close_all();

  // A failed fork-table allocation must not acquire any file/CWD references
  // or modify the source. Once memory is returned, the same clone can succeed.
  const auto pools = vfs::pool_usage();
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  fd = vfs::syscall::do_open(&table, "/fixture.bin", vfs::O_CLOEXEC, 0);
  if (!ut::expect(fd == 0 && vfs::syscall::do_chdir(&table, "/") == 0)) {
    return;
  }
  auto *source = table.get_file(fd);
  const auto populated = vfs::pool_usage();
  const auto populated_heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto cwd_refs = table.working_directory()->ref_count;
  {
    HeapPressure pressure;
    if (ut::expect(pressure.acquire(sizeof(vfs::FdTable)))) {
      auto *copy = table.clone();
      ut::expect(copy == nullptr);
      if (copy) {
        copy->close_all();
      }
      delete copy;
      ut::expect(table.get_file(fd) == source && source->ref_count == 1 && table.descriptor_flags(fd) == 1);
      ut::expect(table.working_directory()->ref_count == cwd_refs && vfs::pool_usage() == populated);
    }
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated_heap);
  auto *copy = table.clone();
  if (ut::expect(copy != nullptr)) {
    ut::expect(copy->get_file(fd) == source && source->ref_count == 2 && copy->descriptor_flags(fd) == 1);
    ut::expect(copy->working_directory() == table.working_directory() &&
               table.working_directory()->ref_count == cwd_refs + 1);
    copy->close_all();
    delete copy;
  }
  ut::expect(source->ref_count == 1 && table.working_directory()->ref_count == cwd_refs);
  u8 byte = 255;
  ut::expect(vfs::syscall::do_read(&table, fd, vfs::OutputBuffer::kernel(&byte, 1)) == 1 && byte == 0);
  table.close_all();
  ut::expect(vfs::pool_usage() == pools && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
}

void pipe_reuse() {
  vfs::FdTable table;
  const u8 sent[] = {0, 1, 2, 255};
  u8 received[sizeof(sent)]{};
  for (unsigned cycle = 0; cycle < 1000; ++cycle) {
    long ends[2] = {-1, -1};
    if (!ut::expect(vfs::syscall::do_pipe(&table, ends) == 0)) {
      break;
    }
    bool valid = ut::expect(vfs::syscall::do_write(&table, ends[1], vfs::InputBuffer::kernel(sent, sizeof(sent))) ==
                            sizeof(sent));
    valid = ut::expect(vfs::syscall::do_close(&table, ends[1]) == 0) && valid;
    valid = ut::expect(vfs::syscall::do_read(&table, ends[0], vfs::OutputBuffer::kernel(received, sizeof(received))) ==
                       sizeof(received)) &&
            valid;
    valid = ut::expect(__builtin_memcmp(sent, received, sizeof(sent)) == 0) && valid;
    valid = ut::expect(vfs::syscall::do_read(&table, ends[0], vfs::OutputBuffer::kernel(received, 1)) == 0) && valid;
    table.close_all();
    if (!valid) {
      break;
    }
  }
  table.close_all();
}

void pipe_fd_rollback() {
  for (unsigned free_slots = 0; free_slots < 2; ++free_slots) {
    vfs::FdTable table;
    long file = vfs::syscall::do_open(&table, "/fixture.bin", 0, 0);
    if (!ut::expect(file == 0)) {
      table.close_all();
      return;
    }
    for (u32 fd = 1; fd < vfs::MAX_FDS - free_slots; ++fd) {
      ut::expect(vfs::syscall::do_dup(&table, file) == static_cast<long>(fd));
    }
    bool valid = true;
    // Exceed the pipe-state capacity while every failed attempt must roll back.
    for (unsigned attempt = 0; valid && attempt < 1000; ++attempt) {
      long ends[2] = {-1, -1};
      valid = ut::expect(vfs::syscall::do_pipe(&table, ends) == -static_cast<long>(vfs::VfsError::TooManyFiles));
      valid = ut::expect(ends[0] == -1 && ends[1] == -1) && valid;
    }
    table.close_all();
    long ends[2] = {-1, -1};
    ut::expect(vfs::syscall::do_pipe(&table, ends) == 0);
    table.close_all();
    if (!valid) {
      return;
    }
  }
}

void writable_lifecycle() {
  const auto baseline = vfs::pool_usage();
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  vfs::FdTable table;
  constexpr u32 create = vfs::O_RDWR | vfs::O_CREAT | vfs::O_EXCL;
  constexpr const char *path = "/writable";
  constexpr u8 payload[] = {0, 1, 2, 0xff};
  long fd = vfs::syscall::do_open(&table, path, create, 0600, 0, 43);
  if (!ut::expect(fd == 0)) {
    return;
  }
  vfs::Stat original{}, status{};
  ut::expect(vfs::syscall::do_fstat(&table, fd, &original) == 0 && original.st_size == 0 && original.st_uid == 0 &&
             original.st_gid == 43 && (original.st_mode & 0777) == 0600);
  ut::expect(vfs::syscall::do_open(&table, path, create, 0600) == -static_cast<long>(vfs::VfsError::FileExists));
  ut::expect(vfs::syscall::do_write(&table, fd, vfs::InputBuffer::kernel(payload, sizeof(payload))) == 4);
  long duplicate = vfs::syscall::do_dup(&table, fd);
  ut::expect(duplicate == 1 && vfs::syscall::do_lseek(&table, duplicate, 0, 0) == 0);
  u8 bytes[5]{};
  ut::expect(vfs::syscall::do_read(&table, fd, vfs::OutputBuffer::kernel(bytes, 4)) == 4 &&
             __builtin_memcmp(bytes, payload, 4) == 0 && vfs::syscall::do_lseek(&table, duplicate, 0, 1) == 4);
  ut::expect(vfs::syscall::do_lseek(&table, fd, 4096, 0) == 4096);
  ut::expect(vfs::syscall::do_write(&table, fd, vfs::InputBuffer::kernel(payload, 4)) == 4);
  ut::expect(vfs::syscall::do_lseek(&table, fd, 4095, 0) == 4095);
  ut::expect(vfs::syscall::do_read(&table, fd, vfs::OutputBuffer::kernel(bytes, 5)) == 5 && !bytes[0] &&
             __builtin_memcmp(bytes + 1, payload, 4) == 0);
  const auto populated_heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto populated_pool = vfs::pool_usage();
  ut::expect(vfs::syscall::do_lseek(&table, fd, 16384, 0) == 16384);
  auto rejected = vfs::InputBuffer::user(1, 4, [](void *, u64, usize size) noexcept { return size; });
  ut::expect(vfs::syscall::do_write(&table, fd, rejected) == -static_cast<long>(vfs::VfsError::BadAddress));
  ut::expect(vfs::syscall::do_lseek(&table, fd, 0, 1) == 16384 && vfs::syscall::do_fstat(&table, fd, &status) == 0 &&
             status.st_size == 4100);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated_heap &&
             vfs::pool_usage() == populated_pool);
  ut::expect(vfs::syscall::do_lseek(&table, fd, 1LL << 40, 0) == (1LL << 40));
  ut::expect(vfs::syscall::do_write(&table, fd, vfs::InputBuffer::kernel(payload, 1)) ==
             -static_cast<long>(vfs::VfsError::NoMemory));
  ut::expect(vfs::syscall::do_lseek(&table, fd, 0, 1) == (1LL << 40) &&
             mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated_heap &&
             vfs::pool_usage() == populated_pool);
  constexpr i64 max_offset = 0x7fffffffffffffffLL;
  ut::expect(vfs::syscall::do_lseek(&table, fd, max_offset, 0) == max_offset);
  ut::expect(vfs::syscall::do_lseek(&table, fd, 1, 1) == -static_cast<long>(vfs::VfsError::Overflow));
  ut::expect(vfs::syscall::do_lseek(&table, fd, max_offset, 2) == -static_cast<long>(vfs::VfsError::Overflow));
  ut::expect(vfs::syscall::do_lseek(&table, fd, 0, 1) == max_offset);
  ut::expect(vfs::syscall::do_write(&table, fd, vfs::InputBuffer::kernel(payload, 1)) ==
             -static_cast<long>(vfs::VfsError::FileTooLarge));
  ut::expect(vfs::syscall::do_lseek(&table, fd, -1, 0) == -static_cast<long>(vfs::VfsError::InvalidArg));
  for (u32 next = 2; next < vfs::MAX_FDS; ++next) {
    ut::expect(vfs::syscall::do_dup(&table, fd) == static_cast<long>(next));
  }
  ut::expect(vfs::syscall::do_open(&table, "/uncreated", create, 0600) ==
             -static_cast<long>(vfs::VfsError::TooManyFiles));
  ut::expect(vfs::syscall::do_stat("/uncreated", &status) == -static_cast<long>(vfs::VfsError::NoEntry));
  ut::expect(vfs::syscall::do_open(&table, path, vfs::O_WRONLY | vfs::O_TRUNC, 0) ==
             -static_cast<long>(vfs::VfsError::TooManyFiles));
  ut::expect(vfs::syscall::do_fstat(&table, fd, &status) == 0 && status.st_size == 4100 &&
             mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated_heap);
  table.close_all();
  fd = vfs::syscall::do_open(&table, path, vfs::O_RDONLY, 0);
  long append = vfs::syscall::do_open(&table, path, vfs::O_WRONLY | vfs::O_APPEND, 0);
  ut::expect(vfs::syscall::do_write(&table, fd, vfs::InputBuffer::kernel(payload, 4)) ==
             -static_cast<long>(vfs::VfsError::BadFd));
  ut::expect(vfs::syscall::do_read(&table, append, vfs::OutputBuffer::kernel(bytes, 4)) ==
             -static_cast<long>(vfs::VfsError::BadFd));
  ut::expect(vfs::syscall::do_lseek(&table, append, 0, 0) == 0 &&
             vfs::syscall::do_write(&table, append, vfs::InputBuffer::kernel(payload, 4)) == 4 &&
             vfs::syscall::do_lseek(&table, append, 0, 1) == 4104);
  ut::expect(vfs::syscall::do_unlink(path) == 0);
  ut::expect(vfs::syscall::do_stat(path, &status) == -static_cast<long>(vfs::VfsError::NoEntry));
  ut::expect(vfs::syscall::do_fstat(&table, fd, &status) == 0 && status.st_ino == original.st_ino &&
             status.st_nlink == 0 && status.st_size == 4104);
  ut::expect(vfs::syscall::do_read(&table, fd, vfs::OutputBuffer::kernel(bytes, 4)) == 4 &&
             __builtin_memcmp(bytes, payload, 4) == 0);
  long replacement = vfs::syscall::do_open(&table, path, create, 0644);
  ut::expect(replacement >= 0 && vfs::syscall::do_fstat(&table, replacement, &status) == 0 &&
             status.st_ino != original.st_ino && status.st_size == 0);
  auto partial =
      vfs::InputBuffer::user(reinterpret_cast<u64>(payload), 4, [](void *target, u64 source, usize count) noexcept {
        __builtin_memcpy(target, reinterpret_cast<const void *>(source), 2);
        return count - 2;
      });
  ut::expect(vfs::syscall::do_write(&table, replacement, partial) == 2 &&
             vfs::syscall::do_fstat(&table, replacement, &status) == 0 && status.st_size == 2);
  ut::expect(vfs::syscall::do_lseek(&table, replacement, 0, 0) == 0 &&
             vfs::syscall::do_read(&table, replacement, vfs::OutputBuffer::kernel(bytes, 4)) == 2 &&
             __builtin_memcmp(bytes, payload, 2) == 0);
  long truncate = vfs::syscall::do_open(&table, path, vfs::O_WRONLY | vfs::O_TRUNC, 0);
  ut::expect(truncate >= 0 && vfs::syscall::do_fstat(&table, replacement, &status) == 0 && status.st_size == 0 &&
             vfs::syscall::do_read(&table, replacement, vfs::OutputBuffer::kernel(bytes, 4)) == 0);
  ut::expect(vfs::syscall::do_unlink(path) == 0);
  table.close_all();
  ut::expect(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  for (unsigned cycle = 0; cycle < 1000; ++cycle) {
    fd = vfs::syscall::do_open(&table, path, create, 0600);
    ut::expect(fd == 0 && vfs::syscall::do_write(&table, fd, vfs::InputBuffer::kernel(payload, 4)) == 4);
    ut::expect(vfs::syscall::do_unlink(path) == 0 && vfs::syscall::do_lseek(&table, fd, 0, 0) == 0);
    ut::expect(vfs::syscall::do_read(&table, fd, vfs::OutputBuffer::kernel(bytes, 4)) == 4 &&
               __builtin_memcmp(bytes, payload, 4) == 0);
    table.close_all();
    if (!ut::expect(vfs::pool_usage() == baseline &&
                    mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap)) {
      break;
    }
  }
}

void rename_lifecycle() {
  // Octal modes exercise owner-only versus public access; UID/GID 42/43 are
  // arbitrary non-root identities. Distinct byte fixtures include '\0'/0xff
  // so replacement checks exercise binary content rather than C strings.
  // One thousand cycles is a bounded reuse/leak workload, not a proof of stability.
  const auto baseline = vfs::pool_usage();
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  vfs::FdTable table;
  ut::expect(vfs::syscall::do_mkdir("/rename-a", 0777, 0, 0) == 0);
  ut::expect(vfs::syscall::do_mkdir("/rename-b", 0700, 0, 0) == 0);
  const auto directories = vfs::pool_usage();
  constexpr u32 create = vfs::O_RDWR | vfs::O_CREAT | vfs::O_EXCL;
  constexpr u8 source_bytes[] = {0, 37, 0xff}, target_bytes[] = {91, 0, 17};
  for (unsigned cycle = 0; cycle < 1000; ++cycle) {
    long source = vfs::syscall::do_open(&table, "/rename-a/source", create, 0600, 42, 43);
    long target = vfs::syscall::do_open(&table, "/rename-b/target", create, 0644);
    ut::expect(source == 0 && target == 1);
    ut::expect(vfs::syscall::do_write(&table, source, vfs::InputBuffer::kernel(source_bytes, 3)) == 3);
    ut::expect(vfs::syscall::do_write(&table, target, vfs::InputBuffer::kernel(target_bytes, 3)) == 3);
    vfs::Stat before{}, replaced{}, current{};
    ut::expect(vfs::syscall::do_stat("/rename-a/source", &before) == 0 &&
               vfs::syscall::do_stat("/rename-b/target", &replaced) == 0);
    const auto populated = vfs::pool_usage();
    const auto populated_heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    ut::expect(vfs::syscall::do_rename("/rename-a/source", "/rename-b/target") == 0);
    ut::expect(vfs::pool_usage() == populated &&
               mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated_heap);
    ut::expect(vfs::syscall::do_stat("/rename-a/source", &current) == -static_cast<long>(vfs::VfsError::NoEntry));
    ut::expect(vfs::syscall::do_stat("/rename-b/target", &current) == 0 && current.st_ino == before.st_ino &&
               current.st_nlink == 1 && current.st_size == 3 && current.st_uid == 42 && current.st_gid == 43);
    ut::expect(vfs::syscall::do_fstat(&table, target, &current) == 0 && current.st_ino == replaced.st_ino &&
               current.st_nlink == 0 && current.st_size == 3);
    u8 bytes[3]{};
    ut::expect(vfs::syscall::do_lseek(&table, source, 0, 0) == 0 &&
               vfs::syscall::do_read(&table, source, vfs::OutputBuffer::kernel(bytes, 3)) == 3 &&
               __builtin_memcmp(bytes, source_bytes, 3) == 0);
    ut::expect(vfs::syscall::do_lseek(&table, target, 0, 0) == 0 &&
               vfs::syscall::do_read(&table, target, vfs::OutputBuffer::kernel(bytes, 3)) == 3 &&
               __builtin_memcmp(bytes, target_bytes, 3) == 0);
    ut::expect(vfs::syscall::do_rename("/rename-b/target", "/rename-b/target") == 0);
    ut::expect(vfs::syscall::do_unlink("/rename-b/target") == 0);
    table.close_all();
    if (!ut::expect(vfs::pool_usage() == directories &&
                    mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap)) {
      break;
    }
  }
  ut::expect(vfs::syscall::do_rmdir("/rename-a") == 0 && vfs::syscall::do_rmdir("/rename-b") == 0);
  ut::expect(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
}

void rename_boundaries() {
  const auto baseline = vfs::pool_usage();
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  vfs::FdTable table;
  constexpr const char *paths[] = {"/rename-a", "/rename-b", "/rename-a/source", "/rename-a/source/leaf",
                                   "/rename-b/target"};
  for (const auto *path : paths) {
    ut::expect(vfs::syscall::do_mkdir(path, 0700, 0, 0) == 0);
  }
  long held = vfs::syscall::do_open(&table, "/rename-b/target", vfs::O_RDONLY, 0);
  long file = vfs::syscall::do_open(&table, "/rename-a/file", vfs::O_RDWR | vfs::O_CREAT, 0600);
  vfs::Stat source{}, target{}, current{}, parent{};
  ut::expect(held >= 0 && file >= 0 && vfs::syscall::do_stat("/rename-a/source", &source) == 0 &&
             vfs::syscall::do_stat("/rename-b/target", &target) == 0);
  ut::expect(vfs::syscall::do_rename("/rename-a/source", "/rename-b/target") == 0);
  ut::expect(vfs::syscall::do_stat("/rename-b/target", &current) == 0 && current.st_ino == source.st_ino &&
             current.st_nlink == 3 && vfs::syscall::do_stat("/rename-b/target/leaf", &current) == 0);
  ut::expect(vfs::syscall::do_fstat(&table, held, &current) == 0 && current.st_ino == target.st_ino &&
             current.st_nlink == 0);
  ut::expect(vfs::syscall::do_stat("/rename-a", &parent) == 0 && parent.st_nlink == 2);
  ut::expect(vfs::syscall::do_stat("/rename-b", &parent) == 0 && parent.st_nlink == 3);
  struct Rejection {
    const char *old_path;
    const char *new_path;
    vfs::VfsError error;
  };
  const Rejection rejections[] = {
      {.old_path = "/rename-b/target", .new_path = "/rename-b/target/leaf/cycle", .error = vfs::VfsError::InvalidArg},
      {.old_path = "/rename-a/file", .new_path = "/rename-b/target", .error = vfs::VfsError::IsDirectory},
      {.old_path = "/rename-b/target", .new_path = "/rename-a/file", .error = vfs::VfsError::NotDirectory},
      {.old_path = "/rename-a", .new_path = "/rename-b", .error = vfs::VfsError::NotEmpty},
      {.old_path = "/rename-a/file", .new_path = "/dev/null", .error = vfs::VfsError::CrossDevice},
      {.old_path = "/", .new_path = "/rename-a/root", .error = vfs::VfsError::Busy},
      {.old_path = "/rename-a/file", .new_path = "/", .error = vfs::VfsError::Busy},
      {.old_path = "/dev", .new_path = "/rename-a/device", .error = vfs::VfsError::Busy},
      {.old_path = "/rename-a/file", .new_path = "/dev", .error = vfs::VfsError::Busy},
      {.old_path = "/rename-a/file/", .new_path = "/rename-a/new", .error = vfs::VfsError::NotDirectory},
      {.old_path = "/rename-a/file", .new_path = "/rename-a/new/", .error = vfs::VfsError::NotDirectory},
      {.old_path = "/rename-a/file", .new_path = "/rename-a/file/child/new", .error = vfs::VfsError::NotDirectory},
      {.old_path = "/rename-a/.", .new_path = "/rename-a/new", .error = vfs::VfsError::InvalidArg},
      {.old_path = "/rename-a/file", .new_path = "/rename-a/..", .error = vfs::VfsError::InvalidArg},
      {.old_path = "/rename-a/missing", .new_path = "/rename-a/new", .error = vfs::VfsError::NoEntry},
      {.old_path = "/rename-a/file", .new_path = "", .error = vfs::VfsError::NoEntry},
  };
  const auto populated = vfs::pool_usage();
  for (const auto &rejection : rejections) {
    ut::expect(vfs::syscall::do_rename(rejection.old_path, rejection.new_path) == -static_cast<long>(rejection.error));
    ut::expect(vfs::pool_usage() == populated && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    ut::expect(vfs::syscall::do_stat("/rename-b/target", &current) == 0 && current.st_ino == source.st_ino &&
               current.st_nlink == 3 && vfs::syscall::do_stat("/rename-a/file", &current) == 0);
  }
  // A full destination must reject a cross-directory insertion without losing
  // the source. Same-directory renaming and replacement need no extra slot.
  ut::expect(vfs::syscall::do_mkdir("/rename-full", 0700, 0, 0) == 0);
  char path[] = "/rename-full/00";
  for (u32 i = 0; i < vfs::Inode::MAX_CHILDREN; ++i) {
    path[13] = static_cast<char>('0' + i / 10);
    path[14] = static_cast<char>('0' + i % 10);
    ut::expect(vfs::syscall::do_mkdir(path, 0700, 0, 0) == 0);
  }
  const auto full = vfs::pool_usage();
  for (unsigned attempt = 0; attempt < 300; ++attempt) {
    ut::expect(vfs::syscall::do_rename("/rename-b/target", "/rename-full/overflow") ==
               -static_cast<long>(vfs::VfsError::NoMemory));
    ut::expect(vfs::pool_usage() == full && vfs::syscall::do_stat("/rename-b/target/leaf", &current) == 0);
  }
  ut::expect(vfs::syscall::do_rename("/rename-full/00", "/rename-full/renamed") == 0);
  ut::expect(vfs::syscall::do_rename("/rename-full/renamed", "/rename-full/00") == 0);
  ut::expect(vfs::syscall::do_rename("/rename-b/target", "/rename-full/00") == 0);
  ut::expect(vfs::syscall::do_stat("/rename-full/00", &current) == 0 && current.st_ino == source.st_ino &&
             vfs::syscall::do_stat("/rename-full/00/leaf", &current) == 0);
  ut::expect(vfs::syscall::do_rmdir("/rename-full/00/leaf") == 0);
  for (u32 i = 0; i < vfs::Inode::MAX_CHILDREN; ++i) {
    path[13] = static_cast<char>('0' + i / 10);
    path[14] = static_cast<char>('0' + i % 10);
    ut::expect(vfs::syscall::do_rmdir(path) == 0);
  }
  table.close_all();
  ut::expect(vfs::syscall::do_unlink("/rename-a/file") == 0);
  constexpr const char *directories[] = {"/rename-a", "/rename-b", "/rename-full"};
  for (const auto *directory : directories) {
    ut::expect(vfs::syscall::do_rmdir(directory) == 0);
  }
  ut::expect(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
}

void access_permissions() {
  // Identity 42 owns files, group 43 matches them, and 99 is an unrelated user
  // or group. Octal modes isolate owner/group/other and execute permissions;
  // access masks use R_OK=4, W_OK=2, X_OK=1, F_OK=0 (combinations are bitwise OR).
  using namespace vfs;
  const auto baseline = pool_usage();
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  constexpr long denied = -static_cast<long>(VfsError::PermDenied);
  for (unsigned cycle = 0; cycle < 64; ++cycle) {
    {
      FdTable table;
      // Root provides a writable arena; unprivileged creation must not rely on
      // bypassing the namespace root's 0755 permissions.
      if (!ut::expect(vfs::syscall::do_mkdir("/access-arena", 0777, 0, 0) == 0 &&
                      vfs::syscall::do_chdir(&table, "/access-arena") == 0)) {
        return;
      }
      auto create = [&](const char *path, u32 mode) {
        const long fd = vfs::syscall::do_open(&table, path, O_CREAT | O_EXCL | O_RDWR, mode, 42, 43);
        return fd >= 0 && vfs::syscall::do_close(&table, fd) == 0;
      };
      if (!ut::expect(create("owner", 0640) && create("group", 0040) && create("exec", 0010) &&
                      create("others", 0066) && vfs::syscall::do_mkdir("tree", 0710, 42, 43, &table) == 0 &&
                      create("tree/leaf", 0600) && vfs::syscall::do_mkdir("tree/empty", 0700, 42, 43, &table) == 0)) {
        return;
      }
      ut::expect(vfs::syscall::do_access(&table, "owner", 6, 42, 99) == 0);
      ut::expect(vfs::syscall::do_access(&table, "owner", 4, 99, 43) == 0);
      ut::expect(vfs::syscall::do_access(&table, "owner", 2, 99, 43) == denied);
      ut::expect(vfs::syscall::do_access(&table, "owner", 4, 99, 99) == denied);
      auto open_as = [&](const char *path, u32 flags, u32 uid, u32 gid, bool permitted) {
        const long fd = vfs::syscall::do_open(&table, path, flags, 0, uid, gid);
        ut::expect(permitted ? fd >= 0 : fd == denied);
        if (fd >= 0) {
          ut::expect(vfs::syscall::do_close(&table, fd) == 0);
        }
      };
      const long owner = vfs::syscall::do_open(&table, "owner", O_RDWR, 0, 42, 43);
      constexpr u8 payload[] = {37, 0, 0xff};
      if (!ut::expect(owner >= 0 && vfs::syscall::do_write(&table, owner, InputBuffer::kernel(payload, 3)) == 3)) {
        return;
      }
      Stat original{}, current{}, original_leaf{};
      ut::expect(vfs::syscall::do_fstat(&table, owner, &original) == 0 && original.st_uid == 42 &&
                 original.st_gid == 43 && vfs::syscall::do_stat("tree", &current, &table) == 0 &&
                 current.st_uid == 42 && current.st_gid == 43 &&
                 vfs::syscall::do_stat("tree/leaf", &original_leaf, &table) == 0);
      const auto populated = pool_usage();
      const auto populated_heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
      open_as("owner", O_RDONLY, 99, 99, false);
      open_as("owner", O_RDONLY, 99, 43, true);
      open_as("owner", O_RDWR, 42, 99, true);
      open_as("owner", O_WRONLY | O_TRUNC, 99, 43, false);
      open_as("owner", O_RDWR, 99, 43, false);
      open_as("group", O_RDONLY, 42, 43, false); // Owner bits take precedence over group.
      open_as("group", O_RDONLY, 99, 43, true);
      open_as("others", O_RDWR, 99, 99, true);
      open_as("others", O_RDONLY, 42, 99, false);
      open_as("group", O_RDWR, 0, 99, true);
      open_as("tree/../others", O_RDONLY, 99, 99, false); // Do not normalize away a denied search.
      open_as("tree/../others", O_RDONLY, 99, 43, true);
      open_as("/access-uncreated", O_CREAT | O_WRONLY, 99, 99, false);
      if (vfs::syscall::do_stat("/access-uncreated", &current) == 0) {
        ut::expect(vfs::syscall::do_unlink("/access-uncreated") == 0);
      }
      // Group search alone cannot authorize namespace changes. Rejections must
      // preserve both rename endpoints and the populated resource baseline.
      open_as("tree/uncreated", O_CREAT | O_WRONLY, 99, 43, false);
      ut::expect(vfs::syscall::do_mkdir("tree/uncreated", 0700, 99, 43, &table) == denied);
      ut::expect(vfs::syscall::do_unlink("tree/leaf", &table, 99, 43) == denied);
      ut::expect(vfs::syscall::do_rmdir("tree/empty", &table, 99, 43) == denied);
      ut::expect(vfs::syscall::do_rename("tree/leaf", "owner", &table, 99, 43) == denied);
      ut::expect(vfs::syscall::do_rename("owner", "tree/leaf", &table, 99, 43) == denied);
      ut::expect(vfs::syscall::do_stat("tree/../owner", &current, &table, 99, 99) == denied);
      ut::expect(vfs::syscall::do_stat("tree/leaf", &current, &table, 99, 43) == 0 &&
                 current.st_ino == original_leaf.st_ino && current.st_size == original_leaf.st_size);
      u8 bytes[3]{};
      ut::expect(vfs::syscall::do_fstat(&table, owner, &current) == 0 && current.st_ino == original.st_ino &&
                 current.st_size == 3 && vfs::syscall::do_lseek(&table, owner, 0, 0) == 0 &&
                 vfs::syscall::do_read(&table, owner, OutputBuffer::kernel(bytes, 3)) == 3 &&
                 __builtin_memcmp(bytes, payload, 3) == 0);
      ut::expect(pool_usage() == populated &&
                 mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated_heap);
      ut::expect(vfs::syscall::do_close(&table, owner) == 0);
      ut::expect(vfs::syscall::do_access(&table, "group", 4, 42, 43) == denied);
      ut::expect(vfs::syscall::do_access(&table, "group", 4, 99, 43) == 0);
      ut::expect(vfs::syscall::do_access(&table, "owner", 6, 0, 0) == 0);
      ut::expect(vfs::syscall::do_access(&table, "owner", 1, 0, 0) == denied);
      ut::expect(vfs::syscall::do_access(&table, "exec", 1, 0, 0) == 0);
      ut::expect(vfs::syscall::do_access(&table, "/fixture.bin", 2, 0, 0) == denied);
      ut::expect(vfs::syscall::do_access(&table, "tree", 0, 99, 99) == 0);
      ut::expect(vfs::syscall::do_access(&table, "tree", 1, 99, 99) == denied);
      ut::expect(vfs::syscall::do_access(&table, "tree/leaf", 0, 99, 99) == denied);
      ut::expect(vfs::syscall::do_access(&table, "tree/leaf", 0, 99, 43) == 0);
      ut::expect(vfs::syscall::do_access(&table, "tree/leaf", 4, 99, 43) == denied);
      ut::expect(vfs::syscall::do_access(&table, "owner", 8, 42, 43) == -static_cast<long>(VfsError::InvalidArg));
      ut::expect(vfs::syscall::do_chdir(&table, "tree", 42, 43) == 0 &&
                 vfs::syscall::do_access(&table, "leaf", 6, 42, 43) == 0 && vfs::syscall::do_chdir(&table, "..") == 0);
      ut::expect(vfs::syscall::do_unlink("owner", &table) == 0 && vfs::syscall::do_unlink("group", &table) == 0 &&
                 vfs::syscall::do_unlink("exec", &table) == 0 && vfs::syscall::do_unlink("others", &table) == 0 &&
                 vfs::syscall::do_unlink("tree/leaf", &table) == 0 &&
                 vfs::syscall::do_rmdir("tree/empty", &table) == 0 && vfs::syscall::do_rmdir("tree", &table) == 0 &&
                 vfs::syscall::do_chdir(&table, "/") == 0 && vfs::syscall::do_rmdir("/access-arena") == 0);
    }
    if (!ut::expect(pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap)) {
      return;
    }
  }
}

void working_directory_lifecycle() {
  const auto baseline = vfs::pool_usage();
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  auto root_references = [] {
    containers::LockGuard<containers::IrqSpinLock> guard(vfs::namespace_lock);
    auto *root = vfs::resolve_path_locked("/");
    return root ? root->ref_count : 0;
  };
  const auto root_refs = root_references();
  for (unsigned cycle = 0; cycle < 1000; ++cycle) {
    {
      vfs::FdTable table;
      if (!ut::expect(vfs::syscall::do_mkdir("/cwd-tree", 0710, 0, 43) == 0 &&
                      vfs::syscall::do_mkdir("/cwd-tree/leaf", 0755, 0, 43) == 0)) {
        return;
      }
      // Both the final directory and each searched ancestor require access.
      ut::expect(vfs::syscall::do_chdir(&table, "/cwd-tree", 99, 44) == -static_cast<long>(vfs::VfsError::PermDenied));
      ut::expect(vfs::syscall::do_chdir(&table, "/cwd-tree/leaf", 99, 44) ==
                 -static_cast<long>(vfs::VfsError::PermDenied));
      if (!ut::expect(vfs::syscall::do_chdir(&table, "/cwd-tree/leaf", 99, 43) == 0)) {
        return;
      }
      auto *child = table.clone();
      if (!ut::expect(child != nullptr)) {
        return;
      }
      char path[vfs::MAX_PATH_LEN];
      auto output = vfs::OutputBuffer::kernel(path, sizeof(path));
      ut::expect(vfs::syscall::do_getcwd(child, output) == 0 && ut::same_id(path, "/cwd-tree/leaf"));
      ut::expect(vfs::syscall::do_rename("/cwd-tree", "/cwd-moved") == 0 &&
                 vfs::syscall::do_getcwd(child, output) == 0 && ut::same_id(path, "/cwd-moved/leaf"));
      ut::expect(vfs::syscall::do_rmdir("/cwd-moved/leaf") == 0 && vfs::syscall::do_rmdir("/cwd-moved") == 0);
      vfs::Stat status{};
      ut::expect(vfs::syscall::do_getcwd(child, output) == -static_cast<long>(vfs::VfsError::NoEntry) &&
                 vfs::syscall::do_stat(".", &status, child) == 0 && status.st_nlink == 0);
      ut::expect(vfs::syscall::do_open(child, "ghost", vfs::O_CREAT | vfs::O_WRONLY, 0600) ==
                 -static_cast<long>(vfs::VfsError::NoEntry));
      ut::expect(vfs::syscall::do_chdir(&table, "/") == 0 && vfs::pool_usage().dentries == baseline.dentries + 2);
      // The child still owns both detached directories, even after its parent
      // leaves. Moving upward must not read a freed/reused parent pointer.
      ut::expect(vfs::syscall::do_chdir(child, "..") == 0 &&
                 vfs::syscall::do_getcwd(child, output) == -static_cast<long>(vfs::VfsError::NoEntry));
      delete child;
    }
    if (!ut::expect(root_references() == root_refs && vfs::pool_usage() == baseline &&
                    mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap)) {
      return;
    }
  }
}

void directory_capacity() {
  const auto baseline = vfs::pool_usage();
  vfs::FdTable table;
  ut::expect(vfs::syscall::do_open(&table, "/", vfs::O_WRONLY, 0) == -static_cast<long>(vfs::VfsError::IsDirectory));
  ut::expect(vfs::syscall::do_open(&table, "/", vfs::O_RDWR, 0) == -static_cast<long>(vfs::VfsError::IsDirectory));
  ut::expect(vfs::syscall::do_open(&table, "/", 3, 0) == -static_cast<long>(vfs::VfsError::InvalidArg));
  table.close_all();
  ut::expect(vfs::pool_usage() == baseline);
  vfs::Stat root{}, full{}, current{};
  ut::expect(vfs::syscall::do_stat("/", &root) == 0);
  if (!ut::expect(vfs::syscall::do_mkdir("/capacity", 0700, 0, 43) == 0)) {
    return;
  }
  char path[] = "/capacity/00";
  u32 created = 0;
  for (; created < vfs::Inode::MAX_CHILDREN; ++created) {
    path[10] = static_cast<char>('0' + created / 10);
    path[11] = static_cast<char>('0' + created % 10);
    if (!ut::expect(vfs::syscall::do_mkdir(path, 0700, 0, 43) == 0)) {
      break;
    }
  }
  ut::expect(vfs::syscall::do_stat("/capacity", &full) == 0);
  ut::expect(full.st_nlink == created + 2 && full.st_uid == 0 && full.st_gid == 43);
  if (created == vfs::Inode::MAX_CHILDREN) {
    const auto capacity = vfs::pool_usage();
    for (unsigned attempt = 0; attempt < 300; ++attempt) {
      ut::expect(vfs::syscall::do_mkdir("/capacity/overflow", 0700, 0, 0) ==
                 -static_cast<long>(vfs::VfsError::NoMemory));
      ut::expect(vfs::pool_usage() == capacity);
    }
    ut::expect(vfs::syscall::do_mkdir(path, 0700, 0, 0) == -static_cast<long>(vfs::VfsError::FileExists));
    ut::expect(vfs::syscall::do_rmdir("/capacity") == -static_cast<long>(vfs::VfsError::NotEmpty));
    ut::expect(vfs::syscall::do_stat("/capacity", &current) == 0 && current.st_nlink == full.st_nlink);
    ut::expect(vfs::pool_usage() == capacity);
  }
  while (created) {
    --created;
    path[10] = static_cast<char>('0' + created / 10);
    path[11] = static_cast<char>('0' + created % 10);
    ut::expect(vfs::syscall::do_rmdir(path) == 0);
  }
  long held = vfs::syscall::do_open(&table, "/capacity", 0, 0);
  if (ut::expect(held == 0)) {
    auto *file = table.get_file(held);
    for (u32 fd = 1; fd < vfs::MAX_FDS; ++fd) {
      ut::expect(vfs::syscall::do_dup(&table, held) == static_cast<long>(fd));
    }
    const auto installed = vfs::pool_usage();
    const auto inode_refs = file->inode->ref_count, dentry_refs = file->dentry->ref_count;
    for (unsigned attempt = 0; attempt < 300; ++attempt) {
      ut::expect(vfs::syscall::do_open(&table, "/capacity", 0, 0) == -static_cast<long>(vfs::VfsError::TooManyFiles));
      ut::expect(vfs::pool_usage() == installed);
      ut::expect(file->inode->ref_count == inode_refs && file->dentry->ref_count == dentry_refs);
    }
    ut::expect(vfs::syscall::do_rmdir("/capacity") == 0);
    ut::expect(vfs::syscall::do_fstat(&table, held, &current) == 0 && current.st_ino == full.st_ino &&
               current.st_nlink == 0);
    ut::expect(vfs::syscall::do_mkdir("/capacity", 0700, 0, 0) == 0);
    ut::expect(vfs::syscall::do_stat("/capacity", &current) == 0 && current.st_ino != full.st_ino);
    ut::expect(vfs::pool_usage().inodes == baseline.inodes + 2 && vfs::pool_usage().dentries == baseline.dentries + 2);
  }
  table.close_all();
  ut::expect(vfs::syscall::do_rmdir("/capacity") == 0);
  ut::expect(vfs::pool_usage() == baseline);
  ut::expect(vfs::syscall::do_stat("/", &current) == 0 && current.st_nlink == root.st_nlink);
}

void timer_contracts() {
  ut::expect(timer::Clocksource{}.deadline_counter(0) == 0);
  const auto &clock = timer::TimerSubsystem::instance().clocksource();
  for (unsigned i = 0; i < 16; ++i) {
    const auto before = bench::read_counter();
    const auto at_deadline = clock.deadline_counter(clock.now_ns());
    const auto after = bench::read_counter();
    ut::expect(at_deadline >= before && at_deadline <= after);
  }
  timer::HrTimer test;
  test.init(timer::TimerMode::OneShot, nullptr);
  auto result = test.start_relative(1);
  ut::expect(!result && result.error() == ErrorCode::InvalidParameter && !test.is_active());
  test.init(timer::TimerMode::OneShot, [](void *) noexcept {});
  result = test.start_relative(~u64{0});
  ut::expect(!result && result.error() == ErrorCode::InvalidParameter && !test.is_active());
  // One second in nanoseconds keeps these state/cancel checks armed in the
  // future; it is a fixture delay rather than a timer precision requirement.
  const auto expiry = timer::TimerSubsystem::instance().now_ns() + 1000000000ULL;
  ut::expect(static_cast<bool>(test.start(expiry)));
  result = test.start_relative(1);
  ut::expect(!result && result.error() == ErrorCode::InvalidState && test.is_active() && test.expires_ns() == expiry);
  test.cancel();
  test.cancel();
  ut::expect(!test.is_active());
  test.init(timer::TimerMode::Periodic, [](void *) noexcept {});
  result = test.start_relative(0);
  ut::expect(!result && result.error() == ErrorCode::InvalidParameter && !test.is_active());
  result = test.start(expiry);
  ut::expect(!result && result.error() == ErrorCode::InvalidParameter && !test.is_active());
  ut::expect(static_cast<bool>(test.start_relative(1000000000ULL)));
  test.cancel();
}

struct TimerObservation {
  unsigned count = 0;
  u64 first_ns = 0;
  static void fired(void *data) noexcept {
    auto *self = static_cast<TimerObservation *>(data);
    if (__atomic_load_n(&self->count, __ATOMIC_RELAXED) == 0) {
      self->first_ns = timer::TimerSubsystem::instance().now_ns();
    }
    __atomic_add_fetch(&self->count, 1U, __ATOMIC_RELEASE);
  }
  unsigned calls() const { return __atomic_load_n(&count, __ATOMIC_ACQUIRE); }
};

void timer_dispatch() {
  // Use 2 ms one-shot / 1 ms periodic delays and a 100 ms observation budget.
  // At least three callbacks distinguishes repetition from a one-shot; the
  // 5 ms post-cancel window spans multiple periods. These are test budgets,
  // not measured dispatch-latency guarantees.
  auto &subsystem = timer::TimerSubsystem::instance();
  for (unsigned phase = 0; phase != 2; ++phase) {
    TimerObservation once, periodic;
    timer::HrTimer one_shot, repeating;
    one_shot.init(timer::TimerMode::OneShot, TimerObservation::fired, &once);
    repeating.init(timer::TimerMode::Periodic, TimerObservation::fired, &periodic);
    const auto deadline = subsystem.now_ns() + 2000000ULL;
    ut::expect(static_cast<bool>(one_shot.start(deadline)));
    if (phase != 0) {
      // Model preemption between arming the two independent timers.
      while (subsystem.now_ns() < deadline + 100000000ULL) {
        arch::cpu_yield();
      }
    }
    const auto first_period = subsystem.now_ns() + 1000000ULL;
    ut::expect(static_cast<bool>(repeating.start_relative(1000000ULL)));
    // Each timer gets the same observation window, even after preemption
    // between registrations. Setup time is not periodic dispatch time.
    const auto observation_end = subsystem.now_ns() + 100000000ULL;
    while ((once.calls() == 0 || periodic.calls() < 3) && subsystem.now_ns() < observation_end) {
      arch::cpu_yield();
    }
    one_shot.cancel();
    repeating.cancel();
    ut::expect(ut::eq(once.calls(), 1U));
    ut::expect(ut::ge(once.first_ns, deadline));
    const auto stopped_count = periodic.calls();
    ut::expect(ut::ge(stopped_count, 3U));
    ut::expect(ut::ge(periodic.first_ns, first_period));
    const auto after_cancel = subsystem.now_ns() + 5000000ULL;
    while (subsystem.now_ns() < after_cancel) {
      arch::cpu_yield();
    }
    ut::expect(once.calls() == 1 && periodic.calls() == stopped_count);
  }
}

struct TimerCapacity {
  // One more than the production timer heap's 256 slots forces exhaustion;
  // the 1000-second delay prevents test timers from expiring during setup.
  timer::HrTimer *timers = new timer::HrTimer[257];
  bool full = false;

  TimerCapacity() {
    if (!ut::expect(timers != nullptr)) {
      return;
    }
    unsigned active = 0;
    for (unsigned i = 0; i < 257; ++i) {
      timers[i].init(timer::TimerMode::OneShot, [](void *) noexcept {});
      auto started = timers[i].start_relative(1000000000000ULL);
      ut::expect(started ? timers[i].is_active()
                         : started.error() == ErrorCode::ResourceExhausted && !timers[i].is_active());
      full = full || (!started && started.error() == ErrorCode::ResourceExhausted);
      active += timers[i].is_active();
    }
    // The production heap has 256 slots, including its scheduler tick.
    ut::expect(full && active < 256);
  }

  ~TimerCapacity() {
    if (!timers) {
      return;
    }
    for (unsigned i = 0; i < 257; ++i) {
      timers[i].cancel_sync();
      ut::expect(!timers[i].is_active());
    }
    delete[] timers;
  }
};
TimerCapacity *sleep_capacity = nullptr;
u64 sleep_capacity_heap_before = 0;

void timer_capacity() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  {
    TimerCapacity fixture;
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}

void empty_case() {}
unsigned deferred_calls = 0;
void deferred_case() { ++deferred_calls; }
void accounting() {
  unsigned evaluations = 0;
  int before = ut::test_result::assertions_passed;
  if (!ut::expect(++evaluations == 1)) {
    return;
  }
  ut::expect(evaluations == 1 && ut::test_result::assertions_passed == before + 1);
  ut::Registry local;
  ut::expect(local.begin_suite("sample"));
  local.add_test("deferred", deferred_case);
  ut::expect(local.case_count == 1 && !local.error && deferred_calls == 0);
  local.cases[0].test_function();
  ut::expect(deferred_calls == 1);
  local.add_test("deferred", empty_case);
  ut::expect(ut::same_id(local.error, "duplicate_case"));
}
void registry_limits() {
  static ut::Registry local;
  static const ut::Registry empty; // Avoid reset temporaries on the 16 KiB kernel stack.
  static_assert(ut::Registry::suite_capacity <= ut::Registry::case_capacity);
  // One extra entry triggers overflow; each ID is 'c', three decimal digits,
  // and the zero-initialized terminator. Capacities here must stay below 1000.
  static char ids[ut::Registry::case_capacity + 1][5];
  for (unsigned i = 0; i <= ut::Registry::case_capacity; ++i) {
    ids[i][0] = 'c';
    ids[i][1] = static_cast<char>('0' + i / 100);
    ids[i][2] = static_cast<char>('0' + i / 10 % 10);
    ids[i][3] = static_cast<char>('0' + i % 10);
  }
  local.begin_suite("limit");
  for (unsigned i = 0; i <= ut::Registry::case_capacity; ++i) {
    local.add_test(ids[i], empty_case);
  }
  ut::expect(local.case_count == ut::Registry::case_capacity && ut::same_id(local.error, "case_capacity"));
  local = empty;
  for (unsigned i = 0; i <= ut::Registry::suite_capacity; ++i) {
    local.begin_suite(ids[i]);
    local.active_suite = nullptr;
  }
  ut::expect(local.suite_count == ut::Registry::suite_capacity && ut::same_id(local.error, "suite_capacity"));
  local = empty;
  local.begin_suite("duplicate");
  local.active_suite = nullptr;
  local.begin_suite("duplicate");
  ut::expect(ut::same_id(local.error, "duplicate_suite"));
  local = empty;
  local.begin_suite("invalid id");
  ut::expect(ut::same_id(local.error, "invalid_suite"));
  bench::Registry benchmarks;
  // The benchmark registry holds 16 entries; the seventeenth must report
  // exhaustion rather than silently dropping a registered workload.
  for (unsigned i = 0; i < 17; ++i) {
    benchmarks.add(ids[i], [](bench::Context &) {});
  }
  ut::expect(benchmarks.count == 16 && ut::same_id(benchmarks.error, "benchmark_capacity"));
}
void cleanup_guards() {
  // An arbitrary valid 1 MHz fixture clock: only cleanup/error accounting is
  // under test, so one iteration/sample and no warmup avoid unrelated work.
  bench::Clock clock{.frequency = 1000000};
  bench::Context context{.clock = clock, .iterations = 1, .capacity = 1, .warmup = 0, .samples = 1};
  unsigned cleaned = 0, called = 0;
  context.measure_batches([](usize) { return false; }, [&](usize) { ++called; },
                          [&](usize) {
                            ++cleaned;
                            return true;
                          });
  ut::expect(!context.valid && called == 0 && cleaned == 1);
  context.valid = true;
  context.measure_batches([](usize) { return true; }, [&](usize) { ++called; },
                          [&](usize) {
                            ++cleaned;
                            return false;
                          });
  ut::expect(!context.valid && called == 1 && cleaned == 2);

  // An uninstalled descriptor checks the snapshot filter, not MMU behavior.
  mm::PageTable table;
  auto &entry = table.entries[0];
  entry.set_page(page_size, mm::page_perms::USER_RW);
  const auto address = reinterpret_cast<PhysAddr>(&table);
  const auto original = entry.raw;
  const auto hash = page_table_hash(address);
  entry.raw ^= mm::page_attr::AF;
#if !defined(MOSS_ARCH_ARM64)
  entry.raw ^= mm::page_attr::DIRTY;
#endif
  ut::expect(page_table_hash(address) == hash);
  entry.raw = original ^ mm::page_attr::USER;
  ut::expect(page_table_hash(address) != hash);
  entry.raw = original ^ mm::page_attr::SW_COW;
  ut::expect(page_table_hash(address) != hash);
  entry.raw = original;
  entry.make_readonly();
  ut::expect(page_table_hash(address) != hash);
#if defined(MOSS_ARCH_RISCV64)
  entry.raw = original ^ mm::page_attr::EXECUTE;
#else
  entry.raw = original ^ mm::page_attr::XN;
#endif
  ut::expect(page_table_hash(address) != hash);
  entry.set_page(2 * page_size, mm::page_perms::USER_RW);
  ut::expect(page_table_hash(address) != hash);
}
void heap_bounds() {
  // One TiB is intentionally beyond the linker-reserved heap, testing rejection
  // without requiring allocation or iteration proportional to the request.
  auto before = mm::RuntimeHeapAllocator::get_heap_end();
  ut::expect(before <= moss::abi::linker::heap_end());
  ut::expect(!mm::RuntimeHeapAllocator::expand_heap(1ULL << 40));
  ut::expect(before == mm::RuntimeHeapAllocator::get_heap_end());
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
struct ContainerValue {
  u32 value;
  u32 *destroyed;
  ContainerValue(u32 v, u32 *counter) : value(v), destroyed(counter) {}
  ~ContainerValue() { ++*destroyed; }
};

void container_ownership() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  u32 destroyed = 0;
  // If reachable nodes have already been reclaimed, do not walk them again
  // during failed-case cleanup. The host discards this suite's kernel.
  auto *list = new containers::LockedList<ContainerValue>();
  for (u32 value = 1; value <= 3; ++value) {
    list->push_front(value, &destroyed);
  }
  logging::klog::info("Container ownership: {} reachable values destroyed after insertion", destroyed);
  if (!ut::expect(destroyed == 0)) {
    return;
  }
  u32 count = 0;
  u32 sum = 0;
  list->for_each([&](const ContainerValue &value) {
    ++count;
    sum += value.value;
  });
  ut::expect(count == 3 && sum == 6);
  delete list;
  ut::expect(destroyed == 3);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}

void container_release_reuse() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  u32 destroyed = 0;
  auto *list = new containers::LockedList<ContainerValue>();
  constexpr u32 length = 1024;
  for (u32 i = 0; i < length; ++i) {
    list->push_front(i, &destroyed);
    if (!ut::expect(destroyed == 0)) {
      return;
    }
  }
  if (!ut::expect(destroyed == 0 && list->size() == length)) {
    return;
  }
  constexpr u32 removed[] = {0, 511, length - 1}; // Tail, interior, head.
  u32 deleted = 0;
  for (u32 id : removed) {
    if (!ut::expect(list->remove_if([&](const ContainerValue &item) { return item.value == id; }))) {
      return;
    }
    if (!ut::expect(destroyed == ++deleted)) {
      return;
    }
    list->for_each([&](const ContainerValue &item) { ut::expect(item.value != id); });
  }
  u32 count = 0;
  u32 sum = 0;
  list->for_each([&](const ContainerValue &value) {
    ++count;
    sum += value.value;
  });
  ut::expect(count == length - 3 && sum == length * (length - 1) / 2 - 511 - (length - 1));
  list->clear(); // No bounded retirement queue remains.
  if (!ut::expect(destroyed == length && list->empty() && list->size() == 0)) {
    return;
  }
  list->push_front(length, &destroyed);
  ut::expect(list->size() == 1 && destroyed == length);
  delete list;
  ut::expect(destroyed == length + 1);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}

void container_map_ownership() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  u32 destroyed = 0;
  // Force collisions and use real owned values, as the IPC channel map does.
  auto *map = new containers::LockedHashMap<u32, shared_ptr<ContainerValue>, 1>();
  for (u32 key = 1; key <= 3; ++key) {
    map->insert_or_update(key, make_shared<ContainerValue>(key, &destroyed));
  }
  if (!ut::expect(destroyed == 0 && map->size() == 3)) {
    return;
  }
  map->insert_or_update(u32{2}, make_shared<ContainerValue>(u32{20}, &destroyed));
  if (!ut::expect(destroyed == 1 && map->size() == 3)) {
    return;
  }
  for (u32 key = 1; key <= 3; ++key) {
    auto value = map->find(key);
    ut::expect(value && *value && (*value)->value == (key == 2 ? 20 : key));
  }
  u32 deleted = 1;
  constexpr u32 keys[] = {1, 3, 2};
  for (u32 key : keys) {
    ut::expect(map->remove(key));
    ut::expect(!map->remove(key));
    if (!ut::expect(destroyed == ++deleted && !map->find(key))) {
      return;
    }
  }
  ut::expect(map->empty());
  delete map;
  ut::expect(destroyed == 4);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}
void container_held_reader() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  u32 destroyed = 0;
  auto *map = new containers::LockedHashMap<u32, shared_ptr<ContainerValue>, 1>();
  map->insert_or_update(u32{1}, make_shared<ContainerValue>(u32{1}, &destroyed));
  {
    auto borrowed = map->find(u32{1});
    if (!ut::expect(borrowed && *borrowed)) {
      return;
    }
    ut::expect(map->remove(u32{1}));
    logging::klog::info("Container held reader: {} values destroyed before reader release", destroyed);
    if (!ut::expect(destroyed == 0)) {
      return; // Do not dereference reclaimed storage; discard this failed suite.
    }
    ut::expect((*borrowed)->value == 1);
  }
  delete map;
  ut::expect(destroyed == 1);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}

struct ReentrantValue {
  using Map = containers::LockedHashMap<u32, shared_ptr<ReentrantValue>, 1>;
  Map *owner;
  u32 *destroyed;
  ReentrantValue(Map *map, u32 *counter) : owner(map), destroyed(counter) {}
  ~ReentrantValue() {
    // This would deadlock if remove/replacement invoked destructors under the map lock.
    ut::expect(owner->size() <= 1);
    ++*destroyed;
  }
};

void container_reentry() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  {
    containers::LockedList<u32> list;
    list.push_front(u32{1});
    auto copy = list.find(u32{1});
    ut::expect(list.update_if([](u32 value) { return value == 1; }, [](u32 &value) { value = 2; }));
    ut::expect(copy && *copy == 1 && !list.find(u32{1}));
    ut::expect(!list.push_front_unless([](u32 value) { return value == 2; }, u32{3}));
    list.push_front(u32{3});
    u32 count = 0;
    list.for_each_snapshot([&](u32 value) {
      ut::expect(list.remove(value));
      ++count;
    });
    ut::expect(count == 2 && list.empty());

    containers::LockedHashMap<u32, u32, 1> values;
    ut::expect(values.get_or_insert(u32{1}, [] { return u32{10}; }) == 10);
    ut::expect(values.get_or_insert(u32{1}, [] { return u32{20}; }) == 10);
    values.insert_or_update(u32{2}, u32{20});
    auto wider_key = values.find(u64{1});
    ut::expect(wider_key && *wider_key == 10);
    count = 0;
    values.for_each_snapshot([&](const auto &entry) {
      ut::expect(values.remove(entry.key));
      ++count;
    });
    ut::expect(count == 2 && values.empty());

    ReentrantValue::Map map;
    u32 destroyed = 0;
    map.insert_or_update(u32{1}, make_shared<ReentrantValue>(&map, &destroyed));
    map.insert_or_update(u32{1}, make_shared<ReentrantValue>(&map, &destroyed));
    ut::expect(destroyed == 1);
    auto held = map.find(u32{1});
    map.clear();
    ut::expect(held && destroyed == 1);
    held.reset();
    ut::expect(destroyed == 2);
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}

struct ConcurrentValue {
  u32 *destroyed;
  explicit ConcurrentValue(u32 *counter) : destroyed(counter) {}
  ~ConcurrentValue() { __atomic_fetch_add(destroyed, 1U, __ATOMIC_RELEASE); }
};

// Two real userspace threads enter this test through the validation syscall.
// Only CPU0 records assertions; acquire/release handshakes publish peer results.
struct ContainerInterleaving {
  containers::LockedHashMap<u32, shared_ptr<ConcurrentValue>, 1> map;
  containers::LockedList<u32> list;
  u32 phase = 0;
  u32 factories = 0;
  u32 inserted = 0;
  u32 destroyed = 0;
  u32 peer_cpu = 0;
  bool peer_ok = false;
  bool peer_inserted = false;
  bool peer_removed = false;
  bool peer_unlinked = false;
  VirtAddr peer_value = 0;

  static void wait_for(const u32 &value, u32 expected) {
    // These milestones only increase without wrapping. A later phase also
    // satisfies the barrier; equality could miss a fast owner's transition.
    // The host's case deadline bounds a stuck peer; no guest-clock dependency.
    while (__atomic_load_n(&value, __ATOMIC_ACQUIRE) < expected) {
    }
  }

  shared_ptr<ConcurrentValue> race_insert(bool &list_inserted, u32 actor) {
    // 42 is an arbitrary list value, and map key 2 differs from the earlier
    // key 1. Actor bits 1/2 identify owner/peer; their OR (3) waits for both
    // contenders, while factories==2 forces both through the lookup race.
    list_inserted = list.push_front_unless([](u32 value) { return value == 42; }, u32{42});
    auto result = map.get_or_insert(u32{2}, [&] {
      auto candidate = make_shared<ConcurrentValue>(&destroyed);
      __atomic_fetch_add(&factories, 1U, __ATOMIC_RELEASE);
      wait_for(factories, 2); // Force both creators past the initial lookup.
      return candidate;
    });
    if (actor == 2) {
      peer_value = reinterpret_cast<VirtAddr>(result.get());
    }
    __atomic_fetch_or(&inserted, actor, __ATOMIC_RELEASE);
    wait_for(inserted, 3);
    return result;
  }

  bool peer() {
    peer_cpu = arch::get_current_cpu_id();
    auto *thread = process::CfsScheduler::get_current_task();
    // The peer is pinned to logical CPU1: its affinity bitmap is bit 1 (2),
    // rather than the numeric CPU ID itself.
    peer_ok = peer_cpu == 1 && thread && thread->cpu_affinity_mask.low_word() == 2;
    wait_for(phase, 1);
    auto held = map.find(u32{1});
    peer_ok = peer_ok && held && *held;
    __atomic_store_n(&phase, 2U, __ATOMIC_RELEASE);
    wait_for(phase, 3);
    const bool alive = __atomic_load_n(&destroyed, __ATOMIC_ACQUIRE) == 0;
    peer_ok = peer_ok && alive;
    if (held && alive) {
      peer_ok = peer_ok && (*held)->destroyed == &destroyed;
    }
    held.reset();
    __atomic_store_n(&phase, 4U, __ATOMIC_RELEASE);
    wait_for(phase, 5);
    auto winner = race_insert(peer_inserted, 2);
    peer_removed = map.remove(u32{2});
    peer_unlinked = list.remove(u32{42});
    winner.reset();
    __atomic_store_n(&phase, 6U, __ATOMIC_RELEASE);
    return peer_ok;
  }

  void owner() {
    __atomic_store_n(&phase, 1U, __ATOMIC_RELEASE);
    wait_for(phase, 2);
    ut::expect(map.remove(u32{1}));
    ut::expect(__atomic_load_n(&destroyed, __ATOMIC_ACQUIRE) == 0);
    __atomic_store_n(&phase, 3U, __ATOMIC_RELEASE);
    wait_for(phase, 4);
    ut::expect(__atomic_load_n(&destroyed, __ATOMIC_ACQUIRE) == 1);
    __atomic_store_n(&phase, 5U, __ATOMIC_RELEASE);
    bool list_inserted = false;
    auto winner = race_insert(list_inserted, 1);
    ut::expect(reinterpret_cast<VirtAddr>(winner.get()) == peer_value);
    const bool removed = map.remove(u32{2});
    const bool unlinked = list.remove(u32{42});
    winner.reset();
    wait_for(phase, 6);
    ut::expect(peer_ok && peer_cpu == 1 && affinity_valid());
    ut::expect(list_inserted != peer_inserted && removed != peer_removed && unlinked != peer_unlinked);
    ut::expect(map.empty() && list.empty());
    ut::expect(__atomic_load_n(&destroyed, __ATOMIC_ACQUIRE) == 3);
    logging::klog::info("Container interleaving: owner CPU0, reader CPU{}, {} values destroyed", peer_cpu,
                        __atomic_load_n(&destroyed, __ATOMIC_ACQUIRE));
  }
};
ContainerInterleaving *container_interleaving = nullptr;

void end_case();

// Independent fork-style tables share a real endpoint, not a synthetic counter.
// Barriers align contention and make ownership checks quiescent; they do not
// force a particular instruction-level interleaving inside ref()/release_file().
struct FileReferences {
  vfs::FdTable reader, writers[2];
  // phase is the owner's release-published milestone; arrived is the peer's
  // acknowledgement. Each ownership cycle uses two milestones (clone/release),
  // so subsequent scenarios start after 2 * cycles. Acquire waits make the
  // associated fixture fields visible for quiescent reference-count checks.
  u32 phase = 0, arrived = 0;
  u32 peer_cpu = ~0U;
  process::Thread *peer_thread = nullptr;
  long shared_read_fd = -1, shared_read_result = -1;
  long shared_write_fd = -1, shared_write_result = -1;
  long shared_open_result = -1;
  // Distinct arbitrary negative sentinels expose unexpected output mutation;
  // successful descriptors are nonnegative, so neither can look like success.
  long shared_pipe_result = -1, shared_pipe_fds[2] = {-37, -73};
  u32 pipe_published = 0;
  vfs::FdTable *native_table = nullptr;
  long native_result = 0, native_fds[2] = {-1, -1};
  u32 copyout_ready = 0;
  u32 dup_ready = 0;
  long dup_result = -1;
  u8 shared_byte = 0;
  // Bounded stress dimensions: repeat 1000 ownership cycles and hold 64 copies
  // per cycle to exercise refcounts and pool reuse, not an API capacity limit.
  static constexpr u32 cycles = 1000, copies = 64;

  template <typename Condition> static void require(Condition valid) {
    if (!ut::expect(static_cast<Condition &&>(valid))) {
      // Ownership may already be corrupt. Preserve the failure and discard this
      // guest without releasing suspect pointers or leaving a peer unbounded.
      end_case();
      finish("file_ownership");
    }
  }

  static void require_refs(vfs::File *file, u32 expected, u32 cycle) {
    const u32 actual = file->ref_count;
    if (actual != expected) {
      logging::klog::error("File references cycle {}: expected {}, observed {}", cycle, expected, actual);
    }
    require(actual == expected);
  }

  long peer() {
    peer_cpu = arch::get_current_cpu_id();
    __atomic_store_n(&arrived, 1U, __ATOMIC_RELEASE);
    for (u32 cycle = 0; cycle < cycles; ++cycle) {
      const u32 start = 2 * cycle + 1;
      ContainerInterleaving::wait_for(phase, start);
      auto *copy = writers[1].clone();
      __atomic_store_n(&arrived, start + 1, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(phase, start + 1);
      copy->close_all();
      delete copy;
      __atomic_store_n(&arrived, start + 2, __ATOMIC_RELEASE);
    }
    ContainerInterleaving::wait_for(phase, 2 * cycles + 1);
    writers[1].close_all();
    __atomic_store_n(&arrived, 2 * cycles + 2, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(phase, 2 * cycles + 2);
    peer_thread = process::CfsScheduler::get_current_task();
    __atomic_store_n(&arrived, 2 * cycles + 3, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(phase, 2 * cycles + 3);
    shared_read_result = vfs::syscall::do_read(&reader, shared_read_fd, vfs::OutputBuffer::kernel(&shared_byte, 1));
    __atomic_store_n(&arrived, 2 * cycles + 4, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(phase, 2 * cycles + 4);
    ContainerInterleaving::wait_for(phase, 2 * cycles + 5);
    // Arbitrary one-byte payload, checked at the receiving endpoint; it is
    // independent of the negative -73 descriptor sentinel above.
    const u8 sent = 73;
    shared_write_result = vfs::syscall::do_write(&reader, shared_write_fd, vfs::InputBuffer::kernel(&sent, 1));
    __atomic_store_n(&arrived, 2 * cycles + 6, __ATOMIC_RELEASE);
    // Force the valid interleaving where this peer resumes only after the owner
    // advances past its intermediate release. The earlier milestone is not lost.
    ContainerInterleaving::wait_for(phase, 2 * cycles + 7);
    ContainerInterleaving::wait_for(phase, 2 * cycles + 6);
    shared_open_result = vfs::syscall::do_open(&reader, "/shared-open", vfs::O_WRONLY | vfs::O_TRUNC, 0);
    __atomic_store_n(&arrived, 2 * cycles + 8, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(phase, 2 * cycles + 8);
    ContainerInterleaving::wait_for(phase, 2 * cycles + 9);
    shared_pipe_result = vfs::syscall::do_pipe(&reader, shared_pipe_fds);
    __atomic_store_n(&arrived, 2 * cycles + 11, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(phase, 2 * cycles + 11);
    ContainerInterleaving::wait_for(phase, 2 * cycles + 12);
    shared_pipe_result = vfs::syscall::do_pipe(&reader, shared_pipe_fds);
    __atomic_store_n(&arrived, 2 * cycles + 14, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(phase, 2 * cycles + 14);
    auto proc = process::current_process();
    require(proc && proc->fd_table());
    native_table = static_cast<vfs::FdTable *>(proc->fd_table());
    __atomic_store_n(&arrived, 2 * cycles + 15, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(phase, 2 * cycles + 15);
    return 2; // Continue through the real userspace pipe syscall, not a mock.
  }

  bool dup_peer() {
    // Four variants share the same fixture: dup, dup2, F_DUPFD (0), and
    // F_DUPFD_CLOEXEC (1030), using the project's Linux fcntl ABI constants.
    // After the earlier 17 milestones, each variant reserves three more:
    // enter the syscall, coordinate its copy, then acknowledge completion.
    for (u32 kind = 0; kind < 4; ++kind) {
      const u32 start = 2 * cycles + 18 + 3 * kind;
      ContainerInterleaving::wait_for(phase, start);
      if (kind == 0) {
        dup_result = vfs::syscall::do_dup(&reader, 0);
      } else if (kind == 1) {
        dup_result = vfs::syscall::do_dup2(&reader, 0, 2);
      } else {
        dup_result = vfs::syscall::do_fcntl(&reader, 0, kind == 2 ? 0 : 1030, 0);
      }
      __atomic_store_n(&arrived, start + 2, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(phase, start + 2);
    }
    return arch::get_current_cpu_id() == 1;
  }

  void owner() {
    ContainerInterleaving::wait_for(arrived, 1);
    require(peer_cpu == 1 && affinity_valid());
    const auto baseline = vfs::pool_usage();
    const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    long ends[2] = {-1, -1};
    require(vfs::syscall::do_pipe(&reader, ends) == 0);
    auto *file = reader.get_file(ends[1]);
    for (auto &table : writers) {
      for (u32 fd = 0; fd < copies; ++fd) {
        require(table.alloc_fd(file) == static_cast<long>(fd));
      }
    }
    require(reader.close_fd(ends[1]) == 0);
    const auto populated = vfs::pool_usage();
    for (u32 cycle = 0; cycle < cycles; ++cycle) {
      const u32 start = 2 * cycle + 1;
      __atomic_store_n(&phase, start, __ATOMIC_RELEASE);
      auto *copy = writers[0].clone();
      ContainerInterleaving::wait_for(arrived, start + 1);
      // Two original tables plus two clones each hold 'copies' writer refs;
      // after both clones close, only the two original tables remain.
      require_refs(file, 4 * copies, cycle);
      __atomic_store_n(&phase, start + 1, __ATOMIC_RELEASE);
      copy->close_all();
      delete copy;
      ContainerInterleaving::wait_for(arrived, start + 2);
      require_refs(file, 2 * copies, cycle);
      const u8 sent = static_cast<u8>(cycle);
      u8 received = 0;
      require(vfs::syscall::do_write(&writers[0], 0, vfs::InputBuffer::kernel(&sent, 1)) == 1);
      require(vfs::syscall::do_read(&reader, ends[0], vfs::OutputBuffer::kernel(&received, 1)) == 1 &&
              received == sent);
      require(vfs::pool_usage() == populated && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    }
    __atomic_store_n(&phase, 2 * cycles + 1, __ATOMIC_RELEASE);
    writers[0].close_all();
    ContainerInterleaving::wait_for(arrived, 2 * cycles + 2);
    u8 byte = 0;
    require(vfs::syscall::do_read(&reader, ends[0], vfs::OutputBuffer::kernel(&byte, 1)) == 0);
    reader.close_all();
    require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    logging::klog::info("File references: CPU0/CPU{}, {} clone/close cycles, EOF and pools restored", peer_cpu, cycles);

    // A blocked I/O must retain its endpoint after another CPU closes and
    // reuses that descriptor in the very same table.
    require(vfs::syscall::do_pipe(&reader, ends) == 0);
    shared_read_fd = ends[0];
    __atomic_store_n(&phase, 2 * cycles + 2, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(arrived, 2 * cycles + 3);
    require(peer_thread != nullptr);
    __atomic_store_n(&phase, 2 * cycles + 3, __ATOMIC_RELEASE);
    for (;;) {
      {
        containers::LockGuard<containers::IrqSpinLock> guard(peer_thread->sleep_lock);
        if (peer_thread->state == process::ProcessState::Sleeping && peer_thread->sleep_handoff.load() == 0) {
          break;
        }
      }
      if (__atomic_load_n(&arrived, __ATOMIC_ACQUIRE) == 2 * cycles + 4) {
        require(false); // Returning before data or EOF is available is a failure.
      }
      arch::cpu_yield();
    }
    require(ut::eq(reader.close_fd(ends[0]), 0L));
    require(ut::eq(vfs::syscall::do_open(&reader, "/dev/null", 0, 0), ends[0]));
    const u8 sent = 37;
    require(ut::eq(vfs::syscall::do_write(&reader, ends[1], vfs::InputBuffer::kernel(&sent, 1)), 1L));
    ContainerInterleaving::wait_for(arrived, 2 * cycles + 4);
    require(ut::eq(shared_read_result, 1L));
    require(ut::eq(shared_byte, sent));
    require(vfs::syscall::do_read(&reader, ends[0], vfs::OutputBuffer::kernel(&byte, 1)) == 0);
    reader.close_all();
    require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    __atomic_store_n(&phase, 2 * cycles + 4, __ATOMIC_RELEASE);

    require(vfs::syscall::do_pipe(&reader, ends) == 0);
    // Fill the real 4096-byte pipe without a page-sized kernel stack temporary.
    u8 bytes[64] = {};
    for (unsigned i = 0; i < 64; ++i) {
      require(ut::eq(vfs::syscall::do_write(&reader, ends[1], vfs::InputBuffer::kernel(bytes, sizeof(bytes))), 64L));
    }
    shared_write_fd = ends[1];
    __atomic_store_n(&phase, 2 * cycles + 5, __ATOMIC_RELEASE);
    for (;;) {
      {
        containers::LockGuard<containers::IrqSpinLock> guard(peer_thread->sleep_lock);
        if (peer_thread->state == process::ProcessState::Sleeping && peer_thread->sleep_handoff.load() == 0) {
          break;
        }
      }
      if (__atomic_load_n(&arrived, __ATOMIC_ACQUIRE) == 2 * cycles + 6) {
        require(false); // A full pipe cannot accept the peer's byte yet.
      }
      arch::cpu_yield();
    }
    require(ut::eq(reader.close_fd(ends[1]), 0L));
    require(ut::eq(vfs::syscall::do_open(&reader, "/dev/null", vfs::O_WRONLY, 0), ends[1]));
    // Reader, active writer and replacement descriptor each still own a File.
    require(ut::eq(vfs::pool_usage().files, baseline.files + 3U));
    for (unsigned i = 0; i < 64; ++i) {
      require(ut::eq(vfs::syscall::do_read(&reader, ends[0], vfs::OutputBuffer::kernel(bytes, sizeof(bytes))), 64L));
      for (u8 value : bytes) {
        require(ut::eq(value, u8{0}));
      }
    }
    ContainerInterleaving::wait_for(arrived, 2 * cycles + 6);
    require(ut::eq(shared_write_result, 1L));
    require(ut::eq(vfs::syscall::do_read(&reader, ends[0], vfs::OutputBuffer::kernel(&byte, 1)), 1L));
    require(ut::eq(byte, u8{73}));
    require(ut::eq(vfs::syscall::do_read(&reader, ends[0], vfs::OutputBuffer::kernel(&byte, 1)), 0L));
    require(ut::eq(vfs::syscall::do_write(&reader, ends[1], vfs::InputBuffer::kernel(&byte, 1)), 1L));
    reader.close_all();
    require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    __atomic_store_n(&phase, 2 * cycles + 6, __ATOMIC_RELEASE);

    require(ut::eq(vfs::syscall::do_open(&reader, "/shared-open", vfs::O_RDWR | vfs::O_CREAT | vfs::O_EXCL, 0600), 0L));
    require(ut::eq(vfs::syscall::do_write(&reader, 0, vfs::InputBuffer::kernel(&sent, 1)), 1L));
    vfs::Stat original{}, after{};
    require(ut::eq(vfs::syscall::do_fstat(&reader, 0, &original), 0L));
    require(ut::eq(original.st_size, u64{1}));
    for (u32 fd = 1; fd + 1 < vfs::MAX_FDS; ++fd) {
      require(ut::eq(vfs::syscall::do_dup(&reader, 0), static_cast<long>(fd)));
    }
    const auto files_before_open = vfs::file_pool_usage();
    long competitor = -1;
    {
      // Hold mutation before launching the real open. File allocation makes
      // its progress observable without borrowing an unpublished object.
      containers::LockGuard<containers::IrqSpinLock> guard(vfs::namespace_lock);
      __atomic_store_n(&phase, 2 * cycles + 7, __ATOMIC_RELEASE);
      while (vfs::file_pool_usage() == files_before_open) {
        if (__atomic_load_n(&arrived, __ATOMIC_ACQUIRE) == 2 * cycles + 8) {
          require(false);
        }
        arch::cpu_yield();
      }
      require(ut::eq(vfs::file_pool_usage(), files_before_open + 1U));
      competitor = vfs::syscall::do_dup(&reader, 0);
    }
    ContainerInterleaving::wait_for(arrived, 2 * cycles + 8);
    const long full = -static_cast<long>(vfs::VfsError::TooManyFiles);
    const long last = vfs::MAX_FDS - 1;
    require((competitor == last && shared_open_result == full) || (competitor == full && shared_open_result == last));
    require(ut::eq(vfs::syscall::do_lseek(&reader, 0, 0, 0), 0L));
    const long expected_bytes = shared_open_result == full ? 1 : 0;
    require(ut::eq(vfs::syscall::do_fstat(&reader, 0, &after), 0L));
    require(ut::eq(after.st_ino, original.st_ino));
    require(ut::eq(after.st_size, static_cast<u64>(expected_bytes)));
    require(ut::eq(vfs::syscall::do_read(&reader, 0, vfs::OutputBuffer::kernel(&byte, 1)), expected_bytes));
    if (expected_bytes) {
      require(ut::eq(byte, sent));
    }
    reader.close_all();
    require(ut::eq(vfs::syscall::do_unlink("/shared-open"), 0L));
    require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    __atomic_store_n(&phase, 2 * cycles + 8, __ATOMIC_RELEASE);

    require(ut::eq(vfs::syscall::do_open(&reader, "/dev/null", vfs::O_RDWR, 0), 0L));
    for (u32 fd = 1; fd + 1 < vfs::MAX_FDS; ++fd) {
      require(ut::eq(vfs::syscall::do_dup(&reader, 0), static_cast<long>(fd)));
    }
    __atomic_store_n(&phase, 2 * cycles + 9, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&pipe_published, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&arrived, __ATOMIC_ACQUIRE) != 2 * cycles + 11) {
      arch::cpu_yield();
    }
    const bool published = __atomic_load_n(&pipe_published, __ATOMIC_ACQUIRE) != 0;
    if (published) {
      require(ut::eq(reader.close_fd(last), 0L));
    }
    require(ut::eq(vfs::syscall::do_dup(&reader, 0), last));
    require(ut::eq(vfs::syscall::do_write(&reader, last, vfs::InputBuffer::kernel(&sent, 1)), 1L));
    __atomic_store_n(&phase, 2 * cycles + 10, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(arrived, 2 * cycles + 11);
    require(ut::eq(shared_pipe_result, full));
    require(ut::eq(shared_pipe_fds[0], -37L));
    require(ut::eq(shared_pipe_fds[1], -73L));
    require(ut::eq(vfs::syscall::do_write(&reader, 0, vfs::InputBuffer::kernel(&sent, 1)), 1L));
    // A failed pipe must not close a descriptor installed by the other CPU,
    // nor transiently expose just one endpoint of the unsuccessful pair.
    require(ut::eq(vfs::syscall::do_write(&reader, last, vfs::InputBuffer::kernel(&sent, 1)), 1L));
    require(!published);
    reader.close_all();
    require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    __atomic_store_n(&phase, 2 * cycles + 11, __ATOMIC_RELEASE);

    // With two slots available, another CPU must see and clone both endpoints
    // together, even before the publishing syscall returns.
    __atomic_store_n(&pipe_published, 0U, __ATOMIC_RELEASE);
    require(ut::eq(vfs::syscall::do_open(&reader, "/dev/null", vfs::O_RDWR, 0), 0L));
    for (u32 fd = 1; fd + 2 < vfs::MAX_FDS; ++fd) {
      require(ut::eq(vfs::syscall::do_dup(&reader, 0), static_cast<long>(fd)));
    }
    __atomic_store_n(&phase, 2 * cycles + 12, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&pipe_published, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&arrived, __ATOMIC_ACQUIRE) != 2 * cycles + 14) {
      arch::cpu_yield();
    }
    require(__atomic_load_n(&pipe_published, __ATOMIC_ACQUIRE) != 0);
    require(reader.get_file(last - 1) != nullptr && reader.get_file(last) != nullptr);
    require(ut::eq(vfs::syscall::do_fcntl(&reader, last - 1, 3, 0), static_cast<long>(vfs::O_RDONLY)));
    require(ut::eq(vfs::syscall::do_fcntl(&reader, last, 3, 0), static_cast<long>(vfs::O_WRONLY)));
    require(reader.descriptor_flags(last - 1) == 0 && reader.descriptor_flags(last) == 0);
    auto *copy = reader.clone();
    require(copy->get_file(last - 1) != nullptr && copy->get_file(last) != nullptr);
    require(ut::eq(vfs::syscall::do_write(&reader, last, vfs::InputBuffer::kernel(&sent, 1)), 1L));
    require(ut::eq(vfs::syscall::do_read(copy, last - 1, vfs::OutputBuffer::kernel(&byte, 1)), 1L));
    require(ut::eq(byte, sent));
    copy->close_all();
    delete copy;
    __atomic_store_n(&phase, 2 * cycles + 13, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(arrived, 2 * cycles + 14);
    require(ut::eq(shared_pipe_result, 0L));
    require(ut::eq(shared_pipe_fds[0], last - 1));
    require(ut::eq(shared_pipe_fds[1], last));
    require(ut::eq(reader.close_fd(last), 0L));
    require(ut::eq(vfs::syscall::do_read(&reader, last - 1, vfs::OutputBuffer::kernel(&byte, 1)), 0L));
    reader.close_all();
    require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    __atomic_store_n(&phase, 2 * cycles + 14, __ATOMIC_RELEASE);

    ContainerInterleaving::wait_for(arrived, 2 * cycles + 15);
    require(native_table != nullptr);
    const long anchor = vfs::syscall::do_open(native_table, "/dev/null", vfs::O_RDWR, 0);
    require(ut::eq(anchor, 3L));
    __atomic_store_n(&phase, 2 * cycles + 15, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&copyout_ready, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&arrived, __ATOMIC_ACQUIRE) != 2 * cycles + 17) {
      arch::cpu_yield();
    }
    require(__atomic_load_n(&copyout_ready, __ATOMIC_ACQUIRE) != 0);
    require(native_fds[0] == 4 && native_fds[1] == 5);
    const bool visible = native_table->get_file(native_fds[0]) != nullptr;
    for (const long fd : native_fds) {
      if (visible) {
        require(ut::eq(native_table->close_fd(fd), 0L));
        require(ut::eq(vfs::syscall::do_dup2(native_table, anchor, fd), fd));
        require(ut::eq(vfs::syscall::do_write(native_table, fd, vfs::InputBuffer::kernel(&sent, 1)), 1L));
      } else {
        require(native_table->get_file(fd) == nullptr);
        require(ut::eq(native_table->close_fd(fd), -static_cast<long>(vfs::VfsError::BadFd)));
        require(ut::eq(vfs::syscall::do_dup2(native_table, anchor, fd), -static_cast<long>(vfs::VfsError::Busy)));
      }
    }
    if (!visible) {
      auto *pending_copy = native_table->clone();
      require(pending_copy->get_file(native_fds[0]) == nullptr && pending_copy->get_file(native_fds[1]) == nullptr);
      require(ut::eq(vfs::syscall::do_dup(pending_copy, anchor), native_fds[0]));
      pending_copy->close_all();
      delete pending_copy;
    }
    __atomic_store_n(&phase, 2 * cycles + 16, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(arrived, 2 * cycles + 17);
    require(ut::eq(native_result, -14L)); // The native copy into read-only user memory must fail.
    require(ut::eq(vfs::syscall::do_write(native_table, anchor, vfs::InputBuffer::kernel(&sent, 1)), 1L));
    for (const long fd : native_fds) {
      if (!visible) {
        require(ut::eq(vfs::syscall::do_dup(native_table, anchor), fd));
      }
      require(ut::eq(vfs::syscall::do_write(native_table, fd, vfs::InputBuffer::kernel(&sent, 1)), 1L));
    }
    require(!visible);
    for (const long fd : native_fds) {
      require(ut::eq(native_table->close_fd(fd), 0L));
    }
    require(ut::eq(native_table->close_fd(anchor), 0L));
    require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    __atomic_store_n(&phase, 2 * cycles + 17, __ATOMIC_RELEASE);
    for (u32 kind = 0; kind < 4; ++kind) {
      const u32 start = 2 * cycles + 18 + 3 * kind;
      require(ut::eq(vfs::syscall::do_open(&reader, "/fixture.bin", vfs::O_RDONLY, 0), 0L));
      require(ut::eq(vfs::syscall::do_open(&reader, "/dev/null", vfs::O_WRONLY, 0), 1L));
      __atomic_store_n(&dup_ready, 0U, __ATOMIC_RELEASE);
      __atomic_store_n(&phase, start, __ATOMIC_RELEASE);
      while (!__atomic_load_n(&dup_ready, __ATOMIC_ACQUIRE) &&
             __atomic_load_n(&arrived, __ATOMIC_ACQUIRE) != start + 2) {
        arch::cpu_yield();
      }
      require(__atomic_load_n(&dup_ready, __ATOMIC_ACQUIRE) != 0);
      require(ut::eq(reader.close_fd(0), 0L));
      require(reader.get_file(0) == nullptr);
      require(ut::eq(vfs::syscall::do_dup2(&reader, 1, 2), 2L));
      require(ut::eq(vfs::syscall::do_write(&reader, 2, vfs::InputBuffer::kernel(&sent, 1)), 1L));
      __atomic_store_n(&phase, start + 1, __ATOMIC_RELEASE);
      ContainerInterleaving::wait_for(arrived, start + 2);
      // Atomic duplication either precedes source close, or sees EBADF. It
      // cannot resurrect source FD 0 or overwrite the later target replacement.
      require(dup_result == 2 || dup_result == -static_cast<long>(vfs::VfsError::BadFd));
      require(reader.get_file(0) == nullptr);
      require(ut::eq(vfs::syscall::do_write(&reader, 1, vfs::InputBuffer::kernel(&sent, 1)), 1L));
      require(ut::eq(vfs::syscall::do_write(&reader, 2, vfs::InputBuffer::kernel(&sent, 1)), 1L));
      require(ut::eq(reader.descriptor_flags(2), 0L));
      reader.close_all();
      require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
      __atomic_store_n(&phase, start + 2, __ATOMIC_RELEASE);
    }
  }
};
FileReferences *file_references = nullptr;

struct TimerCancellation {
  timer::HrTimer pending;
  u32 peers_ready = 0, callback_cpu = ~0U;
  u32 entered = 0, release = 0, callback_returned = 0, cancel_returned = 0, observer_returned = 0;
  bool cancel_ok = false, observer_ok = false;

  static void callback(void *data) noexcept {
    auto &self = *static_cast<TimerCancellation *>(data);
    __atomic_store_n(&self.callback_cpu, arch::get_current_cpu_id(), __ATOMIC_RELAXED);
    __atomic_store_n(&self.entered, 1U, __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(self.release, 1);
    __atomic_store_n(&self.callback_returned, 1U, __ATOMIC_RELEASE);
  }

  bool peer(long actor) {
    if (actor < 1 || actor > 2 || arch::get_current_cpu_id() != static_cast<u32>(actor)) {
      return false;
    }
    // The timer queue is global: a timer IRQ must not take over either actor
    // whose progress the deliberately held callback needs. Publish readiness
    // only after masking local IRQs, before the owner arms the real timer.
    const bool restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();
    __atomic_fetch_or(&peers_ready, 1U << static_cast<u32>(actor), __ATOMIC_RELEASE);
    ContainerInterleaving::wait_for(entered, 1);
    if (actor == 1) {
      pending.cancel_sync();
      cancel_ok = __atomic_load_n(&callback_returned, __ATOMIC_ACQUIRE) == 1;
      __atomic_store_n(&cancel_returned, 1U, __ATOMIC_RELEASE);
      if (restore_irqs) {
        arch::enable_interrupts();
      }
      return cancel_ok;
    }
    // A periodic timer remains queued while its callback runs. Inactive thus
    // proves CPU1 has reached cancellation, not merely its pre-call barrier.
    while (pending.is_active()) {
      arch::cpu_yield();
    }
    const auto restarted = pending.start_relative(1000000000ULL);
    observer_ok = !restarted && restarted.error() == ErrorCode::InvalidState;
    // Hold the callback across a bounded observation window. Cancellation may
    // not return during it; the host deadline still bounds every peer barrier.
    auto &clock = timer::TimerSubsystem::instance();
    const u64 until = clock.now_ns() + 2000000ULL;
    while (!__atomic_load_n(&cancel_returned, __ATOMIC_ACQUIRE) && clock.now_ns() < until) {
      arch::cpu_yield();
    }
    observer_ok = observer_ok && !__atomic_load_n(&cancel_returned, __ATOMIC_ACQUIRE);
    __atomic_store_n(&release, 1U, __ATOMIC_RELEASE);
    __atomic_store_n(&observer_returned, 1U, __ATOMIC_RELEASE);
    if (restore_irqs) {
      arch::enable_interrupts();
    }
    return observer_ok;
  }

  bool owner() {
    ContainerInterleaving::wait_for(peers_ready, 6);
    pending.init(timer::TimerMode::Periodic, callback, this);
    if (!pending.start_relative(1000000ULL)) {
      return false;
    }
    ContainerInterleaving::wait_for(cancel_returned, 1);
    ContainerInterleaving::wait_for(observer_returned, 1);
    pending.cancel_sync();
    const u32 cpu = __atomic_load_n(&callback_cpu, __ATOMIC_ACQUIRE);
    return cancel_ok && observer_ok && callback_returned == 1 && !pending.is_active() && cpu != 1 && cpu != 2;
  }
};
TimerCancellation *timer_cancellation = nullptr;
process::Thread *early_sleep_thread = nullptr;
u32 early_sleep_visits = 0;
bool early_sleep_woken = true;
bool dispatch_boundary_checked = false;

void failing_case() { ut::expect(false); }
[[noreturn]] void panic_case() {
  Event("fatal").str("case", active_case).str("kind", "panic").send();
  logging::klog::panic("validation intentional panic");
  // Exercise the emergency path with TX already locked by this CPU. It must
  // still reach the real panic/halt; peers cannot interleave its diagnostic.
  hal::uart::TransmitGuard guard;
  arch::kernel_panic("validation intentional panic");
}
[[noreturn]] void timeout_case() {
  Event("fatal").str("case", active_case).str("kind", "timeout").send();
  for (;;) {
    arch::cpu_yield();
  }
}

void kernel_stack_initialization() {
  process::Process owner(0);
  auto *thread = new process::Thread(0, 0);
  if (!ut::expect(thread != nullptr)) {
    return;
  }
  if (!ut::expect(owner.register_thread(thread).has_value())) {
    delete thread;
    return;
  }
  constexpr usize order = 2;
  // Thread::allocate_kernel_stack uses order 2: four 4 KiB pages (16 KiB).
  // Poison that exact block before reuse to verify the entire stack is cleared.
  constexpr usize size = page_size << order;
  auto dirty = mm::allocate_pages(order);
  if (!ut::expect(dirty.has_value())) {
    return;
  }
  auto *words = reinterpret_cast<u64 *>(phys_to_virt(*dirty));
  for (usize i = 0; i < size / sizeof(u64); ++i) {
    words[i] = 0xfeedfacecafebeefULL;
  }

  // Exhaust the real allocator, then offer only the poisoned block. This
  // checks rollback and recycled-page initialization without allocation hooks.
  PagePressure pressure;
  ut::expect(pressure.acquire(0));
  auto exhausted = thread->allocate_kernel_stack();
  ut::expect(!exhausted && exhausted.error() == ErrorCode::OutOfMemory);
  ut::expect(thread->kernel_stack_base == 0 && thread->kernel_stack_size == 0);
  ut::expect(mm::free_pages(*dirty, order).has_value());
  auto allocated = thread->allocate_kernel_stack();
  pressure.release();
  if (!ut::expect(allocated.has_value())) {
    return;
  }
  const auto base = thread->kernel_stack_base;
  if (!ut::expect(base != 0 && base % size == 0 && thread->kernel_stack_size == size)) {
    return;
  }
  words = reinterpret_cast<u64 *>(base);
  bool zeroed = true;
  for (usize i = 0; i < size / sizeof(u64); ++i) {
    zeroed = zeroed && words[i] == 0;
  }
  ut::expect(zeroed);
  ut::expect(thread->kernel_stack_top() == base + size);
  auto repeated = thread->allocate_kernel_stack();
  ut::expect(!repeated && repeated.error() == ErrorCode::InvalidState);
  ut::expect(thread->kernel_stack_base == base && thread->kernel_stack_size == size);
}

void scheduler_self_selection(bool realtime) {
  using namespace process;
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  if (!ut::expect(scheduler.get() != nullptr)) {
    return;
  }
  Thread current(0, 0), peer(1, 0);
  const auto cpu = arch::get_current_cpu_id();
  current.cpu = peer.cpu = cpu;
  current.cpu_affinity_mask.set(cpu);
  peer.cpu_affinity_mask.set(cpu);
  current.state = ProcessState::Running;
  current.se.vruntime = 1;
  current.se.sum_exec_runtime = 2 * cfs_params::SCHED_LATENCY_NS;
  peer.se.vruntime = 1000000000ULL;
  if (realtime) {
    current.sched_class = SchedClass::RealTime;
    current.sched_policy = SchedPolicy::RR;
    current.rt.priority = priority::DEFAULT_RT_PRIORITY;
    current.rt.time_slice_remaining = 0;
  }

  // Exercise the production tick with local queues, without dispatching a
  // synthetic context or exposing it to this CPU's real timer interrupt.
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  auto *original = CfsScheduler::get_current_task();
  CfsScheduler::set_current_task(&current);
  if (!realtime) {
    scheduler->enqueue_task(&peer, cpu);
  }
  scheduler->scheduler_tick();
  const bool running = current.state == ProcessState::Running;
  const bool unqueued = !current.se.rb_on_rq && scheduler->get_cpu_nr_running(cpu) == (realtime ? 0U : 1U);
  const bool reselected = scheduler->total_preemptions() == 1;
  // Also clean up the unfixed self-selection state, so a failed assertion
  // remains a reportable failure rather than a second insertion hanging.
  scheduler->dequeue_task(&current);
  if (!realtime) {
    scheduler->dequeue_task(&peer);
  }
  CfsScheduler::set_current_task(original);
  if (restore_irqs) {
    arch::enable_interrupts();
  }
  ut::expect(reselected);
  ut::expect(running);
  ut::expect(unqueued);
  ut::expect(scheduler->get_cpu_nr_running(cpu) == 0);
}

void migration_current_owner() {
  using namespace process;
  if (!ut::expect(g_num_cpus >= 2)) {
    return;
  }
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  unique_ptr<LoadBalancer> balancer(new LoadBalancer());
  if (!ut::expect(scheduler && balancer)) {
    return;
  }
  Thread current(0, 0), peer(1, 0);
  const auto source = arch::get_current_cpu_id();
  const auto target = (source + 1) % g_num_cpus;
  current.cpu_affinity_mask.set(source);
  current.cpu_affinity_mask.set(target);
  peer.cpu_affinity_mask.set(source);
  peer.cpu_affinity_mask.set(target);
  current.se.vruntime = cfs_params::SCHED_LATENCY_NS;
  peer.se.vruntime = 1;

  // Reproduce yield/preemption's published-Ready, unsaved-continuation window
  // through real queues and migration, without dispatching synthetic contexts.
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  auto *original = CfsScheduler::get_current_task();
  CfsScheduler::set_current_task(&current);
  scheduler->enqueue_task(&peer, source);
  scheduler->enqueue_task(&current, source);
  (void)balancer->migrate_task(source, target, *scheduler);
  const bool retained = current.cpu == source && scheduler->pick_next_task(target) != &current;
  scheduler->dequeue_task(&current);
  scheduler->dequeue_task(&peer);
  CfsScheduler::set_current_task(original);

  // A pinned highest-vruntime task must not hide another eligible task.
  // These local queues never dispatch the synthetic contexts on the target.
  current.cpu_affinity_mask = CpuBitmap::single(source);
  scheduler->enqueue_task(&peer, source);
  scheduler->enqueue_task(&current, source);
  const bool eligible_peer = balancer->migrate_task(source, target, *scheduler) && current.cpu == source &&
                             peer.cpu == target && scheduler->get_cpu_nr_running(source) == 1 &&
                             scheduler->get_cpu_nr_running(target) == 1;
  scheduler->dequeue_task(&current);
  scheduler->dequeue_task(&peer);

  current.cpu_affinity_mask.set(target);
  scheduler->enqueue_task(&peer, source);
  scheduler->enqueue_task(&current, source);
  const bool moved = balancer->migrate_task(source, target, *scheduler) && current.cpu == target &&
                     peer.cpu == source && scheduler->get_cpu_nr_running(source) == 1 &&
                     scheduler->get_cpu_nr_running(target) == 1;
  scheduler->dequeue_task(&peer);
  scheduler->enqueue_task(&peer, target);
  // Exercise a remote source queue from this CPU, retaining one task there.
  const bool moved_back = balancer->migrate_task(target, source, *scheduler) && current.cpu == source &&
                          peer.cpu == target && scheduler->get_cpu_nr_running(source) == 1 &&
                          scheduler->get_cpu_nr_running(target) == 1;
  scheduler->dequeue_task(&current);
  scheduler->dequeue_task(&peer);

  current.cpu_affinity_mask = peer.cpu_affinity_mask = CpuBitmap::single(source);
  scheduler->enqueue_task(&peer, source);
  scheduler->enqueue_task(&current, source);
  const bool pinned = !balancer->migrate_task(source, target, *scheduler) && current.cpu == source &&
                      peer.cpu == source && scheduler->get_cpu_nr_running(source) == 2 &&
                      scheduler->get_cpu_nr_running(target) == 0;
  scheduler->dequeue_task(&current);
  scheduler->dequeue_task(&peer);
  if (restore_irqs) {
    arch::enable_interrupts();
  }
  ut::expect(retained);
  ut::expect(eligible_peer);
  ut::expect(moved);
  ut::expect(moved_back);
  ut::expect(pinned);
  ut::expect(scheduler->get_cpu_nr_running(source) == 0 && scheduler->get_cpu_nr_running(target) == 0);
}

void declare_cases() {
  ut::register_suite("resources", [] { ut::register_test("cpu_memory", resources); });
  ut::register_suite("mm", [] {
    ut::register_test("pageblock_units", moss::test::scheduler_regression::pageblock_units);
    ut::register_test("mmu_granule", [] { ut::expect(moss::test::hardware::arm64_mmu_granule_regression()); });
    ut::register_test("orders_alignment", pages);
    ut::register_test("reuse", reuse_pages);
  });
  ut::register_suite("pfa", [] {
    ut::register_test("release_contract", page_release_contract);
    ut::register_test("exhaustion", page_exhaustion);
  });
  ut::register_suite("heap", [] {
    ut::register_test("alignment", heap_alignment);
    ut::register_test("invalid_requests", heap_invalid_requests);
    ut::register_test("release_contract", heap_release_contract);
    ut::register_test("reuse", heap_reuse);
    ut::register_test("exhaustion", heap_exhaustion);
  });
  ut::register_suite("containers", [] {
    ut::register_test("queue_reuse", moss::test::queue_regression::run);
    ut::register_test("ipc_heap_rollback", ipc_heap_rollback);
    ut::register_test("ipc_shared_backing", moss::test::ipc_regression::shared_backing);
    ut::register_test("ipc_shared_lifecycle", moss::test::ipc_regression::shared_lifecycle);
    ut::register_test("ipc_ring_wrap", moss::test::ipc_regression::ring_wrap);
    ut::register_test("ipc_ring_geometry", moss::test::ipc_regression::ring_geometry);
    ut::register_test("ownership", container_ownership);
    ut::register_test("release_reuse", container_release_reuse);
    ut::register_test("map_ownership", container_map_ownership);
    ut::register_test("held_reader", container_held_reader);
    ut::register_test("reentry", container_reentry);
  });
  ut::register_suite("containers.smp", [] { ut::register_test("interleaving", empty_case); });
  ut::register_suite("vfs.smp", [] { ut::register_test("shared_references", empty_case); });
  ut::register_suite("vfs", [] {
    ut::register_test("read_position_eof", file_read);
    ut::register_test("errors_readonly", file_errors);
    ut::register_test("fd_boundaries", fd_boundaries);
    ut::register_test("pipe_reuse", pipe_reuse);
    ut::register_test("pipe_fd_rollback", pipe_fd_rollback);
    ut::register_test("directory_capacity", directory_capacity);
    ut::register_test("writable_lifecycle", writable_lifecycle);
    ut::register_test("rename_lifecycle", rename_lifecycle);
    ut::register_test("rename_boundaries", rename_boundaries);
    ut::register_test("access_permissions", access_permissions);
    ut::register_test("working_directory_lifecycle", working_directory_lifecycle);
  });
  ut::register_suite("timers", [] {
    ut::register_test("clocksource_high_frequency",
                      [] { ut::expect(moss::test::hardware::clocksource_high_frequency_regression()); });
    ut::register_test("contracts", timer_contracts);
    ut::register_test("dispatch", timer_dispatch);
    ut::register_test("capacity", timer_capacity);
  });
  ut::register_suite("scheduler", [] {
    ut::register_test("pelt_large_runtime", moss::test::scheduler_regression::pelt_large_runtime);
    ut::register_test("pelt_partitioned_runtime", moss::test::scheduler_regression::pelt_partitioned_runtime);
    ut::register_test("pelt_half_life", moss::test::scheduler_regression::pelt_half_life);
    ut::register_test("pelt_continuous_normalization", moss::test::scheduler_regression::pelt_continuous_normalization);
    ut::register_test("kernel_stack_initialization", kernel_stack_initialization);
    ut::register_test("cfs_self_selection", [] { scheduler_self_selection(false); });
    ut::register_test("rr_self_selection", [] { scheduler_self_selection(true); });
    ut::register_test("migration_current_owner", migration_current_owner);
  });
  ut::register_suite("process", [] { ut::register_test("heap_rollback", process_heap_rollback); });
  ut::register_suite("users", [] {
    ut::register_test("syscall_values", empty_case);
    ut::register_test("user_ranges", empty_case);
    ut::register_test("fork_exec_exit_reap", empty_case);
    ut::register_test("pipe_output_rollback", empty_case);
    ut::register_test("yield_reuse", empty_case);
    ut::register_test("wait_status_rollback", empty_case);
    ut::register_test("pipe_waits_for_writer", empty_case);
    ut::register_test("pipe_cross_cpu_roundtrip", empty_case);
    ut::register_test("pipe_waits_for_reader", empty_case);
    ut::register_test("cross_cpu_exit_reap", empty_case);
    ut::register_test("fork_fd_allocation_rollback", empty_case);
    ut::register_test("mmap_heap_rollback", empty_case);
    ut::register_test("fork_process_allocation_rollback", empty_case);
    ut::register_test("fork_metadata_allocation_rollback", empty_case);
  });
  ut::register_suite("users.frame", [] {
    ut::register_test("native_frame", empty_case);
    ut::register_test("fork_registers", empty_case);
    ut::register_test("signal_return", empty_case);
  });
  ut::register_suite("users.uaccess", [] {
    for (const auto *name : uaccess_cases) {
      ut::register_test(name, empty_case);
    }
  });
  ut::register_suite("users.vm", [] {
    ut::register_test("private_cow", empty_case);
    ut::register_test("readonly_cow", empty_case);
    ut::register_test("access_permissions", empty_case);
  });
  ut::register_suite("users.lifecycle", [] { ut::register_test("core_paths_recovery", empty_case); });
  ut::register_suite("users.applications", [] { ut::register_test("core_application_recovery", empty_case); });
  ut::register_suite("users.timers", [] {
    ut::register_test("relative_sleep", empty_case);
    ut::register_test("absolute_sleep", empty_case);
    ut::register_test("invalid_arguments", empty_case);
    ut::register_test("short_reuse", empty_case);
    ut::register_test("cancel_in_flight", empty_case);
    ut::register_test("early_wakeup", empty_case);
    ut::register_test("arm_failure_recovery", empty_case);
  });
  ut::register_suite("users.libc", [] {
    ut::register_test("static_runtime", empty_case);
    ut::register_test("filesystem_permissions", empty_case);
  });
  ut::register_suite("users.exec", [] {
    constexpr const char *names[] = {"rejects_invalid_entry", "rejects_phentsize", "rejects_load_size",
                                     "bad_env_vector",        "bad_env_string",    "argument_count_limit",
                                     "combined_count_limit",  "string_byte_limit", "exact_combined_count",
                                     "exact_string_bytes",    "empty_vectors",     "allocation_rollback"};
    for (const auto *name : names) {
      ut::register_test(name, empty_case);
    }
  });
  ut::register_suite("users.busybox", [] {
    ut::register_test("ash_exit", empty_case);
    ut::register_test("ash_substitution", empty_case);
    ut::register_test("ash_exec_environment", empty_case);
    ut::register_test("text_pipeline", empty_case);
    ut::register_test("directory_lifecycle", empty_case);
    ut::register_test("file_redirection", empty_case);
    ut::register_test("file_copy", empty_case);
    ut::register_test("file_rename", empty_case);
    ut::register_test("application_workflow", empty_case);
    ut::register_test("head", empty_case);
    ut::register_test("cut", empty_case);
    ut::register_test("sort", empty_case);
    ut::register_test("uniq", empty_case);
    ut::register_test("tr", empty_case);
    ut::register_test("tee", empty_case);
    ut::register_test("cmp", empty_case);
    ut::register_test("basename", empty_case);
    ut::register_test("dirname", empty_case);
    ut::register_test("rmdir", empty_case);
    ut::register_test("uname", empty_case);
    ut::register_test("kill", empty_case);
    ut::register_test("find", empty_case);
    ut::register_test("find_rejects_unsupported", empty_case);
  });
  ut::register_suite("users.signals", [] {
    ut::register_test("basic_handler", empty_case);
    ut::register_test("nested_signals", empty_case);
    ut::register_test("sigchld", empty_case);
    ut::register_test("sigprocmask", empty_case);
    ut::register_test("sigaltstack", empty_case);
    ut::register_test("sig_ign", empty_case);
    ut::register_test("invalid_arguments", empty_case);
    ut::register_test("frame_validation", empty_case);
    ut::register_test("inheritance", empty_case);
    ut::register_test("pid_lifecycle", empty_case);
    ut::register_test("pipe_sigpipe", empty_case);
    ut::register_test("pipe_interrupted", empty_case);
    ut::register_test("pipe_noninterrupting_signals", empty_case);
    ut::register_test("pipe_partial_interrupt", empty_case);
    ut::register_test("signal_wakeup_affinity", empty_case);
  });
#if defined(MOSS_ARCH_X64)
  ut::register_suite("users.simd_fault", [] { ut::register_test("isolation", empty_case); });
#endif
  ut::register_suite("mm.permissions", [] {
    ut::register_test("table_defaults", table_permission_defaults);
    ut::register_test("kernel_mappings", kernel_mapping_permissions);
    ut::register_test("active_user_mappings", active_user_mapping_permissions);
    ut::register_test("kernel_wx", kernel_wx_permissions);
    ut::register_test("address_space_ownership", address_space_ownership);
    ut::register_test("cow_clone_permissions", cow_clone_permissions);
    ut::register_test("vma_boundaries", vma_boundaries);
  });
  ut::register_suite("mm.transactions", [] {
    ut::register_test("map_preserves_existing", map_preserves_existing);
    ut::register_test("map_allocation_rollback", map_allocation_rollback);
    ut::register_test("map_rejects_blocks", map_rejects_blocks);
    ut::register_test("clone_preserves_destination", clone_preserves_destination);
    ut::register_test("clone_allocation_rollback", clone_allocation_rollback);
    ut::register_test("address_space_heap_rollback", address_space_heap_rollback);
    ut::register_test("vma_heap_rollback", vma_heap_rollback);
    ut::register_test("asid_leases", asid_leases);
    ut::register_test("unmap_reclaims_tables", unmap_reclaims_tables);
  });
  ut::register_suite("self", [] {
    ut::register_test("accounting_registration", accounting);
    ut::register_test("registry_limits", registry_limits);
    ut::register_test("cleanup_guards", cleanup_guards);
    ut::register_test("heap_bounds", heap_bounds);
  });
  ut::register_suite("self.fail", [] {
    ut::register_test("intentional_assertion", failing_case);
    ut::register_test("not_run", empty_case);
  });
  ut::register_suite("self.panic", [] { ut::register_test("intentional_panic", panic_case); });
  ut::register_suite("self.timeout", [] { ut::register_test("intentional_timeout", timeout_case); });
}

void start_case(const char *name) {
  active_case = name;
  ut::test_result::assertions_passed = 0;
  ut::test_result::assertions_failed = 0;
  Event("case_start").str("case", name).send();
}
void end_case() {
  failed = failed || ut::test_result::assertions_failed != 0;
  Event("case_end")
      .str("case", active_case)
      .number("passed", static_cast<u64>(ut::test_result::assertions_passed))
      .number("failed", static_cast<u64>(ut::test_result::assertions_failed))
      .send();
  ++completed;
  active_case = nullptr;
}

bench::Clock discover_clock() {
  bench::Clock result;
#if defined(MOSS_ARCH_ARM64)
  asm volatile("mrs %0, cntfrq_el0" : "=r"(result.frequency));
  result.source = "cntfrq_el0";
#elif defined(MOSS_ARCH_RISCV64)
  result.frequency = moss::fdt::get_platform_info().timebase_frequency;
  result.source = "dtb.timebase-frequency";
#else
  u32 eax, ebx, ecx, edx;
  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0), "c"(0));
  // CPUID leaf 0x15 supplies crystal Hz (ECX) and the TSC/crystal ratio
  // EBX/EAX. A missing leaf or zero field requires independent calibration.
  if (eax >= 0x15) {
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x15), "c"(0));
    if (eax && ebx && ecx) {
      result.frequency = static_cast<u64>(ecx) * ebx / eax;
      result.source = "cpuid.15";
    }
  }
  if (!result.frequency) {
    auto out = [](u16 port, u8 value) { asm volatile("outb %0, %1" ::"a"(value), "Nd"(port)); };
    auto in = [](u16 port) {
      u8 value;
      asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
      return value;
    };
    auto count = [&] {
      // PC PIT command port 0x43 / channel-0 port 0x40. Command zero latches
      // channel 0, then low/high reads reconstruct one stable 16-bit sample.
      out(0x43, 0);
      u16 low = in(0x40);
      return static_cast<u16>(low | (static_cast<u16>(in(0x40)) << 8));
    };
    u64 frequencies[3]{};
    // Three independent samples let the median reject one outlier. The 5%
    // maximum spread below is a calibration acceptance policy, not precision proof.
    for (unsigned sample = 0; sample < 3; ++sample) {
      // 0x30 selects channel 0, low/high access, binary mode 0. Reload 0xffff
      // provides a finite countdown; an observed wrap invalidates the sample.
      out(0x43, 0x30);
      out(0x40, 0xFF);
      out(0x40, 0xFF);
      u16 first = count();
      u64 start = bench::read_counter();
      u16 last = first;
      // One million port-read attempts bounds a stalled PIT. Require at least
      // 16384 reference ticks (~13.7 ms); the 60000-tick ceiling stays below a
      // full 16-bit countdown. Exact retry/window tuning evidence is not recorded.
      for (u32 retry = 0; retry < 1000000 && first - last < 16384; ++retry) {
        last = count();
      }
      u64 end = bench::read_counter();
      if (last > first || first - last < 16384 || first - last > 60000 || end <= start) {
        return {};
      }
      result.calibration_ticks[sample] = end - start;
      result.reference_ticks[sample] = static_cast<u64>(first - last);
      // Convert using the PC PIT's 1,193,182 Hz reference, also QEMU's PIT_FREQ:
      // https://github.com/qemu/qemu/blob/master/include/hw/timer/i8254.h
      frequencies[sample] = (end - start) * 1193182ULL / result.reference_ticks[sample];
    }
    u64 low = frequencies[0], high = frequencies[0];
    for (u64 frequency : frequencies) {
      if (frequency < low) {
        low = frequency;
      }
      if (frequency > high) {
        high = frequency;
      }
    }
    if (!low || (high - low) * 100 > low * 5) {
      return {};
    }
    result.frequency = frequencies[0] + frequencies[1] + frequencies[2] - low - high;
    // Scale relative spread by 10^6 for ppm and add a policy margin of 1000 ppm
    // (0.1%); the margin has no recorded measurement-based derivation.
    result.uncertainty_ppm = static_cast<unsigned>((high - low) * 1000000 / low) + 1000;
    result.source = "pit.channel0";
  }
#endif
  u64 begin = bench::read_counter();
  u64 previous = begin;
  // Probe monotonicity 4096 times, then accept 1 kHz..100 GHz as a broad policy
  // sanity range. These finite checks do not establish counter accuracy or stability.
  for (unsigned i = 0; i < 4096; ++i) {
    u64 current = bench::read_counter();
    if (current < previous) {
      return {};
    }
    previous = current;
  }
  if (previous <= begin || result.frequency < 1000 || result.frequency > 100000000000ULL) {
    return {};
  }
  return result;
}

void record_batch(u64 ticks, usize operations, bool warmup, u64 overhead) {
  Event("batch")
      .number("index", sample_index++)
      .number("ticks", ticks)
      .number("operations", operations)
      .number("warmup", warmup ? 1 : 0)
      .number("overhead_ticks", overhead)
      .number("cpu", arch::get_current_cpu_id())
      .str("measurement_kind", ut::same_id(selection, "bench.signal") || ut::same_id(selection, "bench.timer") ||
                                       ut::same_id(selection, "bench.wakeup")
                                   ? "event_sum"
                                   : "elapsed_batch")
      .send();
}

struct TimerBenchmark {
  timer::HrTimer pending;
  process::Thread *sleeper = nullptr;
  u64 observed = 0;

  static void callback(void *data) noexcept {
    const auto now = bench::read_counter();
    auto &self = *static_cast<TimerBenchmark *>(data);
    __atomic_store_n(&self.observed, now, __ATOMIC_RELEASE);
    if (self.sleeper) {
      process::g_scheduler->task_wakeup(self.sleeper, 0);
    }
  }

  u64 measure(bool wakeup) {
    auto &subsystem = timer::TimerSubsystem::instance();
    // Arm 1 ms ahead and measure lateness after the expected counter value;
    // the programmed delay itself is not included in the returned latency.
    const auto deadline = subsystem.now_ns() + 1000000ULL;
    const auto expected = subsystem.clocksource().deadline_counter(deadline);
    observed = 0;
    sleeper = wakeup ? process::CfsScheduler::get_current_task() : nullptr;
    const bool restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();
    if (sleeper) {
      sleeper->state = process::ProcessState::Sleeping;
    }
    pending.init(timer::TimerMode::OneShot, callback, this);
    if (!pending.start(deadline)) {
      if (sleeper) {
        sleeper->state = process::ProcessState::Running;
      }
      if (restore_irqs) {
        arch::enable_interrupts();
      }
      return 0;
    }
    u64 resumed = 0;
    if (sleeper) {
      process::g_scheduler->dequeue_task(sleeper);
      process::CfsScheduler::switch_to_bootstrap(sleeper->context);
      resumed = bench::read_counter();
    }
    if (restore_irqs) {
      arch::enable_interrupts();
    }
    while (!__atomic_load_n(&observed, __ATOMIC_ACQUIRE)) {
      arch::cpu_yield();
    }
    pending.cancel_sync();
    const auto fired = __atomic_load_n(&observed, __ATOMIC_ACQUIRE);
    if (!ut::expect(fired >= expected && affinity_valid() && (!sleeper || resumed > fired))) {
      return 0;
    }
    return sleeper ? resumed - fired : fired - expected;
  }
};

void timer_benchmark(bench::Context &context, bool wakeup) {
  TimerBenchmark fixture;
  const auto before = LifecycleResources::capture();
  auto batch = [&](usize count) {
    u64 ticks = 0;
    for (usize i = 0; i < count; ++i) {
      const auto elapsed = fixture.measure(wakeup);
      if (!elapsed) {
        context.valid = false;
      }
      ticks += elapsed;
    }
    return ticks;
  };
  // Cap event batches at 64 to bound repeated timer/scheduler setup. The pilot
  // uses a 1 ms sum-of-lateness target, matching the batch harness time scale.
  constexpr usize capacity = 64;
  if (context.iterations > capacity) {
    context.valid = false;
    return;
  }
  if (!context.iterations) {
    context.iterations = 1;
    while (context.valid && batch(context.iterations) < context.clock.frequency / 1000 &&
           context.iterations < capacity) {
      context.iterations *= 2;
    }
  }
  for (unsigned i = 0; context.valid && i < context.warmup + context.samples; ++i) {
    const auto ticks = batch(context.iterations);
    const auto begin = bench::read_counter();
    const auto overhead = (bench::read_counter() - begin) * context.iterations;
    context.valid = context.valid && LifecycleResources::capture() == before;
    if (context.valid) {
      record_batch(ticks, context.iterations, i < context.warmup, overhead);
    }
  }
}

void prepare_clock() {
  clock_info = discover_clock();
  Event("clock")
      .number("frequency", clock_info.frequency)
      .str("source", clock_info.source)
      .number("uncertainty_ppm", clock_info.uncertainty_ppm)
      .send();
  for (unsigned i = 0; i < 3; ++i) {
    if (clock_info.reference_ticks[i]) {
      Event("calibration")
          .number("index", i)
          .number("ticks", clock_info.calibration_ticks[i])
          .number("reference_ticks", clock_info.reference_ticks[i])
          .number("reference_frequency", 1193182)
          .send();
    }
  }
  if (!clock_info.frequency) {
    failed = true;
    finish("invalid_clock");
  }
}

void allocation_benchmark(bench::Context &context, unsigned mode) {
  // Track the Context's default 256 operations without heap bookkeeping.
  // Orders 0..4 bound each block to 1..16 pages, keeping the fixture workload
  // separate from the allocator's larger production MAX_ORDER limit.
  PhysAddr pages_owned[256]{};
  bool valid = true;
  usize order = static_cast<usize>(numeric_option("moss.order", 0));
  if (order > 4) {
    failed = true;
    finish("invalid_order");
  }
  auto before = mm::PageFrameAllocator::get_memory_stats().free_pages;
  context.measure_batches(
      [&](usize count) {
        valid = true;
        for (usize i = 0; i < count; ++i) {
          pages_owned[i] = 0;
          if (mode == 1) {
            auto page = mm::PageFrameAllocator::allocate_pages(order);
            if (!page) {
              valid = false;
              break;
            }
            pages_owned[i] = *page;
          }
        }
        return valid;
      },
      [&](usize i) {
        if (mode == 1) {
          if (!mm::PageFrameAllocator::free_pages(pages_owned[i], order)) {
            valid = false;
          } else {
            pages_owned[i] = 0;
          }
        } else {
          auto page = mm::PageFrameAllocator::allocate_pages(order);
          if (!page) {
            valid = false;
            return;
          }
          pages_owned[i] = *page;
          asm volatile("" : "+m"(pages_owned[i]) : : "memory");
          if (mode == 2) {
            if (!mm::PageFrameAllocator::free_pages(pages_owned[i], order)) {
              valid = false;
            } else {
              pages_owned[i] = 0;
            }
          }
        }
      },
      [&](usize count) {
        for (usize i = 0; i < count; ++i) {
          if (pages_owned[i]) {
            valid = static_cast<bool>(mm::PageFrameAllocator::free_pages(pages_owned[i], order)) && valid;
            pages_owned[i] = 0;
          }
        }
        return valid && mm::PageFrameAllocator::get_memory_stats().free_pages == before && affinity_valid();
      });
  failed = !context.valid;
}

void read_benchmark(bench::Context &context) {
  void *table = fd_table();
  long fd = vfs::syscall::do_open(table, "/fixture.bin", 0, 0);
  if (fd < 0) {
    failed = true;
    return;
  }
  // 256 operations each read 256 bytes: 65536 bytes cover the largest batch.
  // Static storage keeps this buffer off the 16 KiB kernel stack.
  static u8 bytes[65536];
  bool valid = true;
  // Each timed operation reads a separate 256-byte slice of the fixture.
  context.measure_batches(
      [&](usize) {
        valid = true;
        return vfs::syscall::do_lseek(table, fd, 0, 0) == 0;
      },
      [&](usize i) {
        valid = (vfs::syscall::do_read(table, fd, vfs::OutputBuffer::kernel(bytes + i * 256, 256)) == 256) && valid;
      },
      [&](usize count) {
        for (usize i = 0; i < count * 256; ++i) {
          valid = valid && bytes[i] == i % 256;
        }
        return valid && affinity_valid();
      });
  failed = !context.valid;
  failed = vfs::syscall::do_close(table, fd) != 0 || failed;
}
} // namespace

extern "C" void moss_validation_boot() noexcept {
  if (!option("moss.validation", selection, sizeof(selection)) || !ut::valid_id(selection)) {
    failed = true;
    finish("invalid_selection");
  }
  warmup_count = static_cast<unsigned>(numeric_option("moss.warmup", 5));
  sample_count = static_cast<unsigned>(numeric_option("moss.samples", 30));
  fixed_iterations = static_cast<usize>(numeric_option("moss.iterations", 0));
  const u64 stability_option = numeric_option("moss.stability", 0);
  if (stability_option > 1 || (stability_option && !is_lifecycle())) {
    failed = true;
    finish("invalid_stability_parameters");
  }
  stability = stability_option == 1;
  // Policy ceilings bound serial output and run time; 65536 also matches the
  // largest userspace syscall fixture capacity. They are not measured accuracy limits.
  if (!sample_count || sample_count > 1000 || warmup_count > 100 || fixed_iterations > 65536) {
    failed = true;
    finish("invalid_parameters");
  }
  declare_cases();
  bench::register_benchmark("bench.allocate", [](bench::Context &context) { allocation_benchmark(context, 0); });
  bench::register_benchmark("bench.release", [](bench::Context &context) { allocation_benchmark(context, 1); });
  bench::register_benchmark("bench.combined", [](bench::Context &context) { allocation_benchmark(context, 2); });
  bench::register_benchmark("bench.read", read_benchmark);
  bench::register_benchmark("bench.getpid", [](bench::Context &) {});
  for (const char *name : user_benchmark_names) {
    bench::register_benchmark(name, [](bench::Context &) {});
  }
  bench::register_benchmark("bench.wakeup", [](bench::Context &context) { timer_benchmark(context, true); });
  bench::register_benchmark("bench.timer", [](bench::Context &context) { timer_benchmark(context, false); });
  if (bench::registry.error) {
    failed = true;
    finish(bench::registry.error);
  }
  if (ut::registry.error) {
    failed = true;
    finish(ut::registry.error);
  }
  const auto &info = moss::fdt::get_platform_info();
  Event("ready")
      .number("detected_cpus", info.cpu_count)
      .number("online_mask", __atomic_load_n(&moss::boot::online_cpu_mask, __ATOMIC_ACQUIRE))
      .number("work_mask", __atomic_load_n(&moss::boot::cpu_work_mask, __ATOMIC_ACQUIRE))
      .number("ram_bytes", info.total_memory_size)
      .number("managed_pages", mm::PageFrameAllocator::get_memory_stats().total_pages)
      .send();
  for (unsigned i = 0; i < ut::registry.case_count; ++i) {
    const auto &item = ut::registry.cases[i];
    if (ut::same_id(item.suite_name, selection)) {
      Event("catalog").str("case", item.name).send();
      ++selected_count;
    }
  }
  for (unsigned i = 0; i < bench::registry.count; ++i) {
    if (ut::same_id(selection, bench::registry.scenarios[i].name)) {
      selected_benchmark = &bench::registry.scenarios[i];
      selected_count = 1;
      Event("catalog").str("case", selection).send();
    }
  }
  if (!selected_count) {
    failed = true;
    finish("empty_selection");
  }
}

extern "C" void moss_validation_dispatch_selected() noexcept {
  if (is_lifecycle() && active_case) {
    dispatch_boundary_checked = true;
    // A timer IRQ must not consume this cached selection before it is dispatched.
    ut::expect(!arch::interrupts_enabled());
  }
}

extern "C" void moss_validation_pipe_published(void *table) noexcept {
  if (!file_references || table != &file_references->reader) {
    return;
  }
  const u32 phase = __atomic_load_n(&file_references->phase, __ATOMIC_ACQUIRE);
  if (phase != 2 * FileReferences::cycles + 9 && phase != 2 * FileReferences::cycles + 12) {
    return;
  }
  __atomic_store_n(&file_references->pipe_published, 1U, __ATOMIC_RELEASE);
  ContainerInterleaving::wait_for(file_references->phase, phase + 1);
}

extern "C" void moss_validation_pipe_copyout(void *table, long first, long second) noexcept {
  if (!file_references || table != file_references->native_table ||
      __atomic_load_n(&file_references->phase, __ATOMIC_ACQUIRE) != 2 * FileReferences::cycles + 15) {
    return;
  }
  file_references->native_fds[0] = first;
  file_references->native_fds[1] = second;
  __atomic_store_n(&file_references->copyout_ready, 1U, __ATOMIC_RELEASE);
  ContainerInterleaving::wait_for(file_references->phase, 2 * FileReferences::cycles + 16);
}

extern "C" void moss_validation_dup_selected(void *table) noexcept {
  if (!file_references || table != &file_references->reader || arch::get_current_cpu_id() != 1) {
    return;
  }
  const u32 phase = __atomic_load_n(&file_references->phase, __ATOMIC_ACQUIRE);
  const u32 offset = phase - (2 * FileReferences::cycles + 18);
  if (offset >= 12 || offset % 3 != 0) {
    return;
  }
  __atomic_store_n(&file_references->dup_ready, 1U, __ATOMIC_RELEASE);
  ContainerInterleaving::wait_for(file_references->phase, phase + 1);
}

extern "C" void moss_validation_sleep_armed(void *pending) noexcept {
  auto *thread = process::CfsScheduler::get_current_task();
  if (!thread || thread != early_sleep_thread) {
    return;
  }
  ++early_sleep_visits;
  auto &armed = *static_cast<timer::HrTimer *>(pending);
  // Local IRQs are masked by the real sleep syscall. Another CPU must expire
  // its timer before this caller saves its context. The host bounds the wait.
  while (armed.is_active()) {
    arch::cpu_yield();
  }
  armed.cancel_sync();
  early_sleep_woken = early_sleep_woken && !arch::interrupts_enabled() &&
                      (thread->state == process::ProcessState::Ready || thread->sleep_handoff.load() == 2);
}

extern "C" void moss_validation_fd_clone(bool entering) noexcept {
  if (!fork_clone_pressure || fork_clone_exhausted) {
    return;
  }
  if (entering) {
    ut::expect(fork_clone_pressure->acquire(sizeof(vfs::FdTable)));
  } else {
    fork_clone_pressure->release();
    fork_clone_exhausted = true;
  }
}

extern "C" void moss_validation_fork_metadata(unsigned stage, bool entering) noexcept {
  auto *probe = fork_metadata_pressure;
  if (!probe || probe->stage != stage || probe->exhausted) {
    return;
  }
  if (entering) {
    // Fail after one successful VMA copy, not only before any work is owned.
    if (stage == 0 && probe->vmas++ == 0) {
      return;
    }
    // Keep the fork hook's stage order: VMA=0, Thread=1, ThreadEntry=2.
    usize size = sizeof(void *);
    if (stage == 0) {
      size = sizeof(process::VmaRegion);
    } else if (stage == 1) {
      size = sizeof(process::Thread);
    } else if (stage == 2) {
      size = sizeof(process::ThreadEntry);
    }
    probe->holding = true;
    ut::expect(probe->heap.acquire(size));
  } else if (probe->holding) {
    probe->heap.release();
    probe->holding = false;
    probe->exhausted = true;
  }
}

extern "C" long moss_validation_call(long op, long arg1, [[maybe_unused]] long arg2) noexcept {
  // These opcodes and returned mode IDs form the private validation protocol
  // shared with src/userspace validation programs, not Linux syscall numbers.
  // Keep both endpoints in sync: opcode zero performs the startup handshake;
  // later operations drive suite-specific ownership checks and benchmarks.
  // Invoked only after the production scheduler and a real userspace exec.
  if (op == 0) {
    if (arg1 != 0 || !affinity_valid()) {
      failed = true;
      finish("affinity");
    }
    Event("worker").number("cpu", arch::get_current_cpu_id()).number("affinity", 1).send();
    if (ut::same_id(selection, "users")) {
      return 1;
    }
    if (ut::same_id(selection, "users.vm")) {
      return 5;
    }
    if (ut::same_id(selection, "users.frame")) {
      return 6;
    }
    if (ut::same_id(selection, "users.uaccess")) {
      return 7;
    }
    if (ut::same_id(selection, "users.signals")) {
      return 8;
    }
    if (ut::same_id(selection, "users.lifecycle")) {
      return 9;
    }
    if (ut::same_id(selection, "users.applications")) {
      return 23;
    }
    if (ut::same_id(selection, "users.timers")) {
      return 10;
    }
    if (ut::same_id(selection, "users.libc")) {
      return 20;
    }
    if (ut::same_id(selection, "users.exec")) {
      return 22;
    }
    if (ut::same_id(selection, "users.busybox")) {
      return 21;
    }
    if (ut::same_id(selection, "users.simd_fault")) {
      start_case("isolation");
      return 4;
    }
    if (ut::same_id(selection, "containers.smp")) {
      start_case("interleaving");
      if (!ut::expect(g_num_cpus >= 2)) {
        end_case();
        finish("requires_smp");
      }
      container_interleaving = new ContainerInterleaving();
      container_interleaving->map.insert_or_update(u32{1},
                                                   make_shared<ConcurrentValue>(&container_interleaving->destroyed));
      return 3;
    }
    if (ut::same_id(selection, "vfs.smp")) {
      start_case("shared_references");
      FileReferences::require(g_num_cpus >= 2);
      file_references = new FileReferences();
      return 3; // Reuse the existing CPU0/CPU1 fork, control and reap protocol.
    }
    if (selected_benchmark) {
      start_case(selection);
      prepare_clock();
      if (const long mode = user_benchmark_mode()) {
#if defined(MOSS_ARCH_ARM64)
        u64 control;
        asm volatile("mrs %0, cntkctl_el1" : "=r"(control));
        control |= 2; // EL0VCTEN: userspace brackets the real syscall round trip.
        asm volatile("msr cntkctl_el1, %0; isb" ::"r"(control) : "memory");
#endif
        syscall_pilot = fixed_iterations == 0;
        syscall_capacity = mode == 2 || mode == 13 || mode == 15 || mode == 16 ? 65536 : 64;
        if (fixed_iterations > syscall_capacity) {
          failed = true;
          finish("invalid_iterations");
        }
        syscall_iterations = fixed_iterations ? fixed_iterations : 1;
        return mode;
      }
      // Restore ordinary interrupts during the in-kernel workload. The pinned
      // thread still uses the real scheduler, locks, and syscall return path.
      arch::enable_interrupts();
      bench::Context context{.clock = clock_info,
                             .iterations = fixed_iterations,
                             .capacity = 256,
                             .warmup = warmup_count,
                             .samples = sample_count,
                             .cpu = 0,
                             .valid = true,
                             .record = record_batch};
      selected_benchmark->run(context);
      failed = failed || !context.valid;
      if (failed) {
        ++ut::test_result::assertions_failed;
      }
      end_case();
      finish();
    }
    arch::enable_interrupts();
    for (unsigned i = 0; i < ut::registry.case_count && !failed; ++i) {
      auto &item = ut::registry.cases[i];
      if (ut::same_id(item.suite_name, selection)) {
        start_case(item.name);
        item.test_function();
        end_case();
      }
    }
    finish();
  }
  const bool user_suite = ut::same_id(selection, "users") || ut::same_id(selection, "users.vm") ||
                          ut::same_id(selection, "users.frame") || ut::same_id(selection, "users.uaccess") ||
                          ut::same_id(selection, "users.signals") || is_lifecycle() ||
                          ut::same_id(selection, "users.timers") || ut::same_id(selection, "users.libc") ||
                          ut::same_id(selection, "users.busybox") || ut::same_id(selection, "users.exec");
  if (op == 1 && user_suite && !failed && !active_case && arg1 == static_cast<long>(completed) && arg1 >= 0) {
    long index = 0;
    for (unsigned i = 0; i < ut::registry.case_count; ++i) {
      const auto &item = ut::registry.cases[i];
      if (ut::same_id(item.suite_name, selection) && index++ == arg1) {
        start_case(item.name);
        return 0;
      }
    }
  }
  if (op == 2 && active_case && (user_suite || ut::same_id(selection, "users.simd_fault"))) {
    if (arg2 != 0) {
      logging::klog::error("users checks failed: mask={:#x}", static_cast<u64>(arg2));
    }
    ut::expect(arg1 != 0 && affinity_valid());
    if (is_lifecycle()) {
      ut::expect(lifecycle_complete && dispatch_boundary_checked);
    }
    end_case();
    return failed ? 0 : 1;
  }
  if (op == 3) {
    finish();
  }
  if (ut::same_id(selection, "users") && ut::same_id(active_case, "fork_fd_allocation_rollback") && affinity_valid()) {
    if (op == 40 && !fork_clone_pressure) {
      fork_clone_baseline = LifecycleResources::capture();
      fork_clone_exhausted = false;
      fork_clone_pressure = new HeapPressure{};
      return 1;
    }
    if (op == 41 && fork_clone_pressure) {
      const bool exercised = ut::expect(fork_clone_exhausted);
      delete fork_clone_pressure;
      fork_clone_pressure = nullptr;
      return ut::expect(LifecycleResources::capture() == fork_clone_baseline) && exercised;
    }
  }
  if (ut::same_id(selection, "users") && ut::same_id(active_case, "fork_metadata_allocation_rollback") &&
      affinity_valid()) {
    if (op == 46 && arg1 >= 0 && arg1 < 4 && !fork_metadata_pressure) {
      const auto baseline = LifecycleResources::capture();
      fork_metadata_pressure = new ForkMetadataPressure{};
      fork_metadata_pressure->baseline = baseline;
      fork_metadata_pressure->stage = static_cast<unsigned>(arg1);
      return 1;
    }
    if (op == 47 && fork_metadata_pressure) {
      const auto baseline = fork_metadata_pressure->baseline;
      const bool exercised = ut::expect(fork_metadata_pressure->exhausted && !fork_metadata_pressure->holding);
      delete fork_metadata_pressure;
      fork_metadata_pressure = nullptr;
      return ut::expect(LifecycleResources::capture() == baseline) && exercised;
    }
  }
  const bool mmap_pressure_case = ut::same_id(active_case, "mmap_heap_rollback");
  if (ut::same_id(selection, "users") &&
      (mmap_pressure_case || ut::same_id(active_case, "fork_process_allocation_rollback")) && affinity_valid()) {
    if (op == (mmap_pressure_case ? 42 : 44) && !user_heap_pressure) {
      user_heap_baseline = LifecycleResources::capture();
      user_heap_pressure = new HeapPressure{};
      if (user_heap_pressure->acquire(mmap_pressure_case ? sizeof(process::VmaRegion) : sizeof(process::Process))) {
        return 1;
      }
      delete user_heap_pressure;
      user_heap_pressure = nullptr;
      return 0;
    }
    if (op == (mmap_pressure_case ? 43 : 45) && user_heap_pressure) {
      delete user_heap_pressure;
      user_heap_pressure = nullptr;
      return ut::expect(LifecycleResources::capture() == user_heap_baseline);
    }
  }
  if (op == 39 && ut::same_id(selection, "users.signals") &&
      (ut::same_id(active_case, "pipe_interrupted") || ut::same_id(active_case, "pipe_noninterrupting_signals"))) {
    if (arg1 == 0 && arg2 == 0) {
      return 1; // The production image's weak hook still returns ENOSYS.
    }
    auto caller = process::current_process();
    if (!caller || arg1 != static_cast<long>(caller->parent_pid()) || arg2 < 0 || arg2 >= vfs::MAX_FDS) {
      return -1;
    }
    auto parent = process::g_process_manager->find_process(caller->parent_pid());
    auto *thread = parent ? parent->get_main_thread() : nullptr;
    if (!thread) {
      return -1;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(thread->sleep_lock);
    if (thread->state != process::ProcessState::Sleeping || thread->sleep_handoff.load() != 0) {
      return 0;
    }
    // Do not mistake the parent's preparation nanosleep for the intended I/O.
    auto *frame = thread->trap_frame;
    return frame && (frame->syscall_number() == 32 || frame->syscall_number() == 33) &&
           frame->argument(0) == static_cast<u64>(arg2);
  }
  if (op == 38 && arg1 == 0 && arg2 == 0 && ut::same_id(selection, "users.libc") &&
      ut::same_id(active_case, "filesystem_permissions")) {
    auto owner = process::current_process();
    if (!owner || owner->euid() != 0) {
      return -1;
    }
    // Test precondition, not a production identity-management interface.
    owner->set_uid(99);
    owner->set_gid(99);
    return 0;
  }
  if (ut::same_id(selection, "users.exec") && ut::same_id(active_case, "allocation_rollback") && affinity_valid()) {
    const long stages = mm::PageTableManager::is_user_range(1ULL << 39, page_size) ? 5 : 4;
    if (op == 35) {
      return stages; // Root, stack leaf, then every missing intermediate table.
    }
    auto *thread = process::CfsScheduler::get_current_task();
    auto owner = process::g_process_manager->find_process(thread->owner_pid);
    auto *as = owner ? owner->address_space() : nullptr;
    if (op == 36 && !exec_pressure && as && arg1 >= 0 && arg1 < stages) {
      exec_baseline = LifecycleResources::capture();
      exec_original = as;
      exec_root = as->pgd_phys;
      exec_root_hash = page_table_hash(exec_root);
      __builtin_memcpy(exec_name, owner->name(), sizeof(exec_name));
      exec_pressure = new PagePressure{};
      bool ready = ut::expect(exec_pressure && exec_pressure->acquire(static_cast<usize>(arg1)));
      while (ready && exec_pressure->count) {
        exec_pressure->give_one();
      }
      if (ready) {
        return 1;
      }
      delete exec_pressure;
      exec_pressure = nullptr;
      return 0;
    }
    if (op == 37 && exec_pressure) {
      bool valid = ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == static_cast<usize>(arg1));
      valid = ut::expect(as && as == exec_original && as->pgd_phys == exec_root &&
                         page_table_hash(exec_root) == exec_root_hash &&
                         __builtin_memcmp(exec_name, owner->name(), sizeof(exec_name)) == 0) &&
              valid;
      delete exec_pressure;
      exec_pressure = nullptr;
      valid = ut::expect(LifecycleResources::capture() == exec_baseline) && valid;
      return valid ? 1 : 0;
    }
  }
  if (ut::same_id(selection, "users.timers") && ut::same_id(active_case, "arm_failure_recovery") && affinity_valid()) {
    if (op == 28 && !sleep_capacity) {
      sleep_capacity_heap_before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
      sleep_capacity = new TimerCapacity();
      return sleep_capacity && sleep_capacity->full;
    }
    if (op == 29 && sleep_capacity) {
      delete sleep_capacity;
      sleep_capacity = nullptr;
      return mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == sleep_capacity_heap_before;
    }
  }
  if (ut::same_id(selection, "users.timers") && ut::same_id(active_case, "early_wakeup")) {
    if (op == 24 && !early_sleep_thread && arch::get_current_cpu_id() == 1) {
      early_sleep_visits = 0;
      early_sleep_woken = true;
      early_sleep_thread = process::CfsScheduler::get_current_task();
      return g_num_cpus >= 2;
    }
    if (op == 25 && early_sleep_thread == process::CfsScheduler::get_current_task()) {
      early_sleep_thread = nullptr;
      return early_sleep_woken && early_sleep_visits == 2;
    }
  }
  if (ut::same_id(selection, "users.timers") && ut::same_id(active_case, "cancel_in_flight")) {
    if (op == 20 && !timer_cancellation && affinity_valid()) {
      timer_cancellation = new TimerCancellation();
      return timer_cancellation && g_num_cpus >= 3;
    }
    if (op == 21 && timer_cancellation) {
      arch::enable_interrupts();
      return timer_cancellation->peer(arg1);
    }
    if (op == 22 && timer_cancellation && affinity_valid()) {
      arch::enable_interrupts();
      return timer_cancellation->owner();
    }
    if (op == 23 && timer_cancellation && affinity_valid()) {
      delete timer_cancellation;
      timer_cancellation = nullptr;
      return 1;
    }
  }
  if ((op == 12 || op == 13) && ut::same_id(selection, "users.uaccess") && active_case) {
    auto *thread = process::CfsScheduler::get_current_task();
    auto owner = thread && process::g_process_manager ? process::g_process_manager->find_process(thread->owner_pid)
                                                      : shared_ptr<process::Process>{};
    auto *as = owner ? owner->address_space() : nullptr;
    const u64 address = static_cast<u64>(arg1);
    if (!ut::expect(as && as->allows_user_access(address, 8, process::vma_flags::WRITE))) {
      return 0;
    }
    auto *leaf = mm::PageTableManager::get_user_pte(as->pgd_phys, address);
    ut::expect(!leaf || !leaf->is_valid());
    usize released_pages = 0;
    if (op == 12) {
      if (!ut::expect(uaccess_pressure == nullptr)) {
        return 0;
      }
      uaccess_pressure = new PagePressure{};
      uaccess_free_before = mm::PageFrameAllocator::get_memory_stats().free_pages;
      if (ut::expect(uaccess_pressure && uaccess_pressure->acquire(0))) {
        return 1;
      }
    } else {
      ut::expect(uaccess_pressure != nullptr);
      released_pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
      // The sigframe test reaps its child before releasing the pressure.
      // Account for those newly freed, non-fixture pages without relaxing the
      // zero-free-page invariant of the other fault cases.
      if (arg2 != 1 || !ut::same_id(active_case, "sigframe_fault")) {
        ut::expect(released_pages == 0);
      }
    }
    delete uaccess_pressure;
    uaccess_pressure = nullptr;
    ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == uaccess_free_before + released_pages);
    return 0;
  }
  if (op == 14 && ut::same_id(selection, "users.uaccess") && ut::same_id(active_case, "partial_read") &&
      uaccess_pressure) {
    u8 bytes[8];
    for (auto &byte : bytes) {
      byte = 0xa5;
    }
    bool valid = ut::expect(process::copy_from_user(bytes, static_cast<u64>(arg1), sizeof(bytes)) == 4);
    for (usize i = 0; i < sizeof(bytes); ++i) {
      valid = ut::expect(bytes[i] == (i < 4 ? i : 0)) && valid;
    }
    return valid ? 1 : 0;
  }
  if (op == 10 && ut::same_id(selection, "users.frame") && active_case) {
    auto *thread = process::CfsScheduler::get_current_task();
    const u64 address = thread ? reinterpret_cast<u64>(thread->trap_frame) : 0;
    // Validate ownership before dereferencing: the old RV entry supplies 511,
    // and the old x86 entry supplies null, neither is a kernel-stack frame.
    const bool owned = thread && address >= thread->kernel_stack_base &&
                       address <= thread->kernel_stack_top() - sizeof(moss::abi::TrapFrame) && (address & 15) == 0;
    if (!owned && thread) {
      logging::klog::error("frame validation: frame={:#x}, stack=[{:#x}, {:#x})", address, thread->kernel_stack_base,
                           thread->kernel_stack_top());
    }
    ut::expect(owned);
    if (owned) {
      auto &frame = *thread->trap_frame;
      ut::expect(frame.from_user() && frame.syscall_number() == 511);
      for (u32 i = 0; i < 6; ++i) {
        ut::expect(frame.argument(i) == (i == 0 ? 10 : i * 11));
      }
      ut::expect(mm::PageTableManager::is_user_range(frame.pc, 1));
      ut::expect(mm::PageTableManager::is_user_range(frame.sp, 1));
    }
    return owned ? 12345 : -1;
  }
  if (op == 4 && user_benchmark_mode()) {
    if (sample_index >= warmup_count + sample_count) {
      end_case();
      return 0;
    }
    return static_cast<long>(syscall_iterations);
  }
  if (op == 5 && user_benchmark_mode()) {
    if (arg1 <= 0 || !arg2 || !affinity_valid()) {
      failed = true;
      finish("invalid_sample");
    }
    if (syscall_pilot) {
      if (static_cast<u64>(arg1) >= clock_info.frequency / 1000 || syscall_iterations == syscall_capacity) {
        syscall_pilot = false;
      } else {
        syscall_iterations *= 2;
      }
    } else {
      record_batch(static_cast<u64>(arg1), syscall_iterations, sample_index < warmup_count, syscall_overhead);
    }
    return 0;
  }
  if (op == 6 && user_benchmark_mode() && arg1 >= 0) {
    syscall_overhead = static_cast<u64>(arg1);
    return 0;
  }
  if (op == 30 && user_benchmark_mode() >= 11 && active_case && affinity_valid()) {
    const auto now = LifecycleResources::capture();
    if (arg1 == 0) {
      benchmark_resources = now;
      benchmark_switches = process::g_scheduler->total_context_switches();
      return 1;
    }
    bool valid = !benchmark_warmed || now == benchmark_resources;
    benchmark_warmed = true;
    if (ut::same_id(selection, "bench.switch")) {
      valid = valid && process::g_scheduler->total_context_switches() >= benchmark_switches + syscall_iterations;
    }
    return ut::expect(valid) ? 1 : 0;
  }
  if (op == 31 && (ut::same_id(selection, "bench.fault") || ut::same_id(selection, "bench.cow")) && active_case) {
    auto *thread = process::CfsScheduler::get_current_task();
    auto owner = process::g_process_manager->find_process(thread->owner_pid);
    auto *as = owner ? owner->address_space() : nullptr;
    bool valid =
        as && arg2 > 0 && arg2 <= 64 &&
        as->allows_user_access(static_cast<u64>(arg1), static_cast<usize>(arg2) * page_size, process::vma_flags::WRITE);
    for (long i = 0; valid && i < arg2; ++i) {
      const auto *pte =
          mm::PageTableManager::get_user_pte(as->pgd_phys, static_cast<u64>(arg1) + static_cast<u64>(i) * page_size);
      valid = ut::same_id(selection, "bench.fault")
                  ? !pte || !pte->is_valid()
                  : pte && pte->is_valid() && pte->is_cow() &&
                        mm::PageFrameAllocator::page_ref_get(pte->get_phys_addr()) >= 2;
    }
    return ut::expect(valid) ? 1 : 0;
  }
  if (op == 10 && is_lifecycle() && active_case) {
    const bool applications = ut::same_id(selection, "users.applications");
    // The userspace fixture reports every 10 application cycles or 100 core
    // lifecycle cycles; changing these intervals must keep that protocol in sync.
    const u64 interval = applications ? 10 : 100;
    if (!ut::expect(arg1 >= 0 && arg2 == (applications ? arg1 : 0))) {
      return 0; // Each declared core cycle must also complete its BusyBox child.
    }
    const auto now = LifecycleResources::capture();
    const u64 time_ns = timer::TimerSubsystem::instance().now_ns();
    if (arg1 == 0 && !lifecycle_started) {
      lifecycle_started_ns = time_ns;
    }
    const u64 elapsed_ns = time_ns - lifecycle_started_ns;
    Event("checkpoint")
        .str("case", active_case)
        .number("cycles", static_cast<u64>(arg1))
        .number("application_cycles", static_cast<u64>(arg2))
        .number("elapsed_ns", elapsed_ns)
        .number("heap_bytes", now.heap_bytes)
        .number("free_pages", now.free_pages)
        .number("processes", now.processes)
        .number("threads", now.threads)
        .number("user_pages", now.user_pages)
        .number("stack_pages", now.stack_pages)
        .number("descriptors", now.descriptors)
        .number("file_refs", now.file_refs)
        .number("vfs_inodes", now.vfs_pools.inodes)
        .number("vfs_dentries", now.vfs_pools.dentries)
        .number("vfs_files", now.vfs_pools.files)
        .send();
    if (arg1 == 0 && !lifecycle_started) {
      lifecycle_baseline = now;
      lifecycle_started = true;
      return 1;
    }
    if (!ut::expect(lifecycle_started && !lifecycle_complete &&
                    arg1 == static_cast<long>(lifecycle_checkpoint + interval) && now == lifecycle_baseline)) {
      return 0;
    }
    lifecycle_checkpoint = static_cast<u64>(arg1);
    if (stability && !lifecycle_host_released) {
      lifecycle_host_released = moss::abi::bridge::console_try_getc() == 'S';
    }
    // Guest clocks may drift relative to the host (e.g. calibrated x86 TSC).
    // Keep doing complete cycles until both clocks and the host agree to stop.
    // Routine callers may lower the cycle count through moss.iterations only
    // when it lands on this workload's checkpoint cadence. Stability remains
    // fixed at 10,000 cycles and 30 minutes (1.8e12 ns), plus a host release.
    const u64 target_cycles = stability ? 10000U : (fixed_iterations ? fixed_iterations : 1000U);
    if (!ut::expect(target_cycles % interval == 0)) {
      return 0;
    }
    lifecycle_complete = lifecycle_checkpoint >= target_cycles && elapsed_ns >= (stability ? 1800000000000ULL : 0) &&
                         (!stability || lifecycle_host_released);
    return lifecycle_complete ? 2 : 1;
  }
  if (ut::same_id(selection, "vfs.smp") && active_case && file_references) {
    if (op == 7 && arg1 == 0) {
      arch::enable_interrupts();
      return file_references->peer();
    }
    if (op == 7 && arg1 == 1) {
      file_references->native_result = arg2;
      __atomic_store_n(&file_references->arrived, 2 * FileReferences::cycles + 17, __ATOMIC_RELEASE);
      arch::enable_interrupts();
      ContainerInterleaving::wait_for(file_references->phase, 2 * FileReferences::cycles + 17);
      return file_references->dup_peer();
    }
    if (op == 8) {
      FileReferences::require(arg1 && affinity_valid());
      arch::enable_interrupts();
      file_references->owner();
      return 0;
    }
    if (op == 9) {
      ut::expect(arg1 && affinity_valid());
      delete file_references;
      file_references = nullptr;
      end_case();
      finish();
    }
  }
  if (ut::same_id(selection, "containers.smp") && active_case && container_interleaving) {
    if (op == 7 && arg1 == 0) {
      arch::enable_interrupts();
      return container_interleaving->peer() ? 1 : 0;
    }
    if (op == 8) {
      if (!ut::expect(arg1 && affinity_valid())) {
        end_case();
        finish("worker_setup");
      }
      arch::enable_interrupts();
      container_interleaving->owner();
      return 0;
    }
    if (op == 9) {
      ut::expect(arg1 && affinity_valid());
      delete container_interleaving;
      container_interleaving = nullptr;
      end_case();
      finish();
    }
  }
  failed = true;
  finish("invalid_control");
}
