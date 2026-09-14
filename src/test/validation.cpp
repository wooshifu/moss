import moss.std;
import moss.types;
import moss.arch;
import moss.abi;
import moss.boot;
import moss.fdt;
import moss.mm;
import moss.vfs;
import moss.process;
import moss.containers;
import moss.smart_ptr;
import moss.hal.uart;
import moss.hal.mmu;
import moss.logging;

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"

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
#elif defined(MOSS_ARCH_X86_64)
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
  unsigned root_shift = 39;
  u64 kernel_bytes = 0;
  usize user_leaves = 0;
  bool check_wx = false;

  KernelPermissions() {
#if defined(MOSS_ARCH_RISCV)
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
      const bool high_half = (base & (1ULL << (root_shift + 8))) != 0;
      bool descendant_user_access = user_access;
#if defined(MOSS_ARCH_X86_64)
      descendant_user_access = user_access && (entry.raw & mm::page_attr::USER) != 0;
#endif
      if (shift > 12 && entry.is_table()) {
#if defined(MOSS_ARCH_X86_64)
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
#elif defined(MOSS_ARCH_X86_64)
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
#if defined(MOSS_ARCH_X86_64)
  ut::expect((entry.raw & mm::page_attr::USER) != 0);
#else
  // ARM64/RISC-V user permission belongs to the leaf, not this table descriptor.
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
  // This verifies the current 0-4 GiB identity/direct-map contract, not a board map.
#if defined(MOSS_ARCH_ARM64)
  constexpr u64 kernel_bytes = 0x100000000ULL;
#else
  constexpr u64 kernel_bytes = 0x200000000ULL;
#endif
  ut::expect(kernel.kernel_bytes == kernel_bytes && kernel.user_leaves == 0);
  KernelPermissions high;
  high.walk(Tables::get_physical_address(Tables::get_kernel_high_pgd()), high.root_shift, 0, false);
#if defined(MOSS_ARCH_RISCV)
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
#elif defined(MOSS_ARCH_X86_64)
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
#if defined(MOSS_ARCH_X86_64)
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
    if (!ut::expect(Tables::clone_user_page_tables(parent_root, child_root).has_value()))
      return;
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
    if (!ut::expect(Tables::clone_user_page_tables(roots[0], roots[1]).has_value()))
      return;
    if (!ut::expect(Tables::clone_user_page_tables(roots[1], roots[2]).has_value()))
      return;
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
      if (original)
        (void)mm::free_pages(*original, 0);
      if (replacement)
        (void)mm::free_pages(*replacement, 0);
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

// Own real PFA blocks while leaving a precisely controlled number of pages
// available to the production allocator. No allocation failure hook or mock.
struct PagePressure {
  PhysAddr head = 0;
  PhysAddr allowance[16]{};
  usize count = 0;

  bool acquire(usize budget) {
    if (budget > 16)
      return false;
    while (count < budget) {
      auto page = mm::allocate_pages(0);
      if (!page)
        return false;
      allowance[count++] = *page;
    }
    for (usize next_order = MAX_ORDER + 1; next_order; --next_order) {
      const usize order = next_order - 1;
      for (;;) {
        auto block = mm::allocate_pages(order);
        if (!block)
          break;
        auto *record = reinterpret_cast<u64 *>(phys_to_virt(*block));
        record[0] = head;
        record[1] = order;
        head = *block;
      }
    }
    return mm::PageFrameAllocator::get_memory_stats().free_pages == 0;
  }

  void give_one() { ut::expect(count && mm::free_pages(allowance[--count], 0).has_value()); }

  void release() {
    while (head) {
      const auto page = head;
      const auto *record = reinterpret_cast<const u64 *>(phys_to_virt(page));
      head = record[0];
      const auto order = static_cast<usize>(record[1]);
      ut::expect(mm::free_pages(page, order).has_value());
    }
    while (count)
      give_one();
  }

  ~PagePressure() { release(); }
};

PagePressure *uaccess_pressure = nullptr;
usize uaccess_free_before = 0;
constexpr const char *uaccess_cases[] = {"allocation_fault", "write_fault",     "read_fault",
                                         "partial_read",     "partial_write",   "partial_pipe_read",
                                         "sigframe_fault",   "sigreturn_fault", "devices"};
constexpr long uaccess_case_count = sizeof(uaccess_cases) / sizeof(uaccess_cases[0]);

void map_allocation_rollback() {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  const auto free_before = Pfa::get_memory_stats().free_pages;
  {
    auto space = process::user_space::create_user_address_space();
    if (!ut::expect(space.has_value()))
      return;
    auto page = mm::allocate_pages(0);
    if (!ut::expect(page.has_value()))
      return;
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
      if (leaf && leaf->is_valid())
        leaf->clear();
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
    if (!ut::expect(mapped.has_value()))
      (void)mm::free_pages(*page, 0);
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
    if (!ut::expect(space && destination))
      return;
    auto page = mm::allocate_pages(0);
    if (!ut::expect(page.has_value()))
      return;
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
    if (!ut::expect(source && destination))
      return;
    const PhysAddr roots[] = {(*source)->pgd_phys, (*destination)->pgd_phys};
    constexpr VirtAddr address = process::user_layout::CODE_BASE;
    mm::PageTableEntry original[2];
    for (usize i = 0; i < 2; ++i) {
      auto page = mm::allocate_pages(0);
      if (!ut::expect(page.has_value()))
        return;
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
    if (!ut::expect(parent && child))
      return;
    const PhysAddr parent_root = (*parent)->pgd_phys;
    const bool four_levels = Tables::is_user_range(1ULL << 39, page_size);
    constexpr VirtAddr base = process::user_layout::CODE_BASE;
    const VirtAddr addresses[] = {base, base + (1ULL << 21), base + (1ULL << 30),
                                  four_levels ? 1ULL << 39 : base + (1ULL << 37)};
    mm::PageTableEntry originals[4];
    for (usize i = 0; i < 4; ++i) {
      auto page = mm::allocate_pages(0);
      if (!ut::expect(page.has_value()))
        return;
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
      if (!ut::expect(fresh.has_value()))
        return false;
      (*child)->pgd_phys = *fresh;
      return true;
    };
    // Measure the real table-page requirement with ample memory, then undo this
    // inactive calibration clone. No parent user instruction can run here.
    const auto before_clone = Pfa::get_memory_stats().free_pages;
    if (!ut::expect(Tables::clone_user_page_tables(parent_root, (*child)->pgd_phys).has_value()))
      return;
    const usize needed = before_clone - Pfa::get_memory_stats().free_pages;
    if (!ut::expect(needed > 0 && needed <= 16) || !recreate_child())
      return;
    for (usize i = 0; i < 4; ++i) {
      *Tables::get_user_pte(parent_root, addresses[i]) = originals[i];
    }
    PagePressure pressure;
    if (!ut::expect(pressure.acquire(needed)))
      return;
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
      if (!recreate_child())
        return;
      for (usize i = 0; i < 4; ++i) {
        *Tables::get_user_pte(parent_root, addresses[i]) = originals[i];
      }
      pressure.give_one();
    }
    // Exactly the required number of pages must suffice, including after all
    // preceding failed attempts; there is no extra reserve hidden in a mock.
    if (!ut::expect(Tables::clone_user_page_tables(parent_root, (*child)->pgd_phys).has_value()))
      return;
    ut::expect(Pfa::get_memory_stats().free_pages == 0);
    for (usize i = 0; i < 4; ++i) {
      const auto *pte = Tables::get_user_pte((*child)->pgd_phys, addresses[i]);
      ut::expect(pte && pte->is_cow() && pte->get_phys_addr() == originals[i].get_phys_addr());
      ut::expect(Pfa::page_ref_get(originals[i].get_phys_addr()) == 2);
    }
  }
  ut::expect(Pfa::get_memory_stats().free_pages == free_before);
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

char selection[81]{};
const char *active_case = nullptr;
bool failed = false;
unsigned completed = 0;
unsigned selected_count = 0;
unsigned sample_index = 0;
unsigned warmup_count = 5;
unsigned sample_count = 30;
usize fixed_iterations = 0;
bench::Clock clock_info;
bench::Scenario *selected_benchmark = nullptr;
usize syscall_iterations = 1;
bool syscall_pilot = true;
u64 syscall_overhead = 0;

// All strings come from the bounded catalog or fixed diagnostic identifiers.
// One complete record per UART write avoids allocating a formatting buffer.
class Event {
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
  u64 cpus = numeric_option("moss.cpus", 4);
  u64 memory = numeric_option("moss.memory", 2048) * 1024 * 1024;
  u64 mask = (1ULL << cpus) - 1;
  ut::expect(info.cpu_count == cpus);
  ut::expect(__atomic_load_n(&moss::boot::online_cpu_mask, __ATOMIC_ACQUIRE) == mask);
  ut::expect(__atomic_load_n(&moss::boot::cpu_work_mask, __ATOMIC_ACQUIRE) == mask);
  ut::expect(info.memory_map_valid);
  ut::expect(info.total_memory_size <= memory && info.total_memory_size + 0x200000 >= memory);
  auto stats = mm::PageFrameAllocator::get_memory_stats();
  ut::expect(stats.total_pages * page_size > 256UL * 1024 * 1024);
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
    high = *page >= info.total_memory_start + 256UL * 1024 * 1024;
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
  // Store only block descriptors, not one pointer per page. This covers the
  // current below-4-GiB allocator without consuming the test worker's stack.
  static Owned owned[2048]{};
  const auto &info = moss::fdt::get_platform_info();
  auto initrd_hash = [&] {
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
  static char ids[129][5];
  for (unsigned i = 0; i < 129; ++i) {
    ids[i][0] = 'c';
    ids[i][1] = static_cast<char>('0' + i / 100);
    ids[i][2] = static_cast<char>('0' + i / 10 % 10);
    ids[i][3] = static_cast<char>('0' + i % 10);
  }
  local.begin_suite("limit");
  for (unsigned i = 0; i < 129; ++i) {
    local.add_test(ids[i], empty_case);
  }
  ut::expect(local.case_count == 128 && ut::same_id(local.error, "case_capacity"));
  local = {};
  for (unsigned i = 0; i <= ut::Registry::suite_capacity; ++i) {
    local.begin_suite(ids[i]);
    local.active_suite = nullptr;
  }
  ut::expect(local.suite_count == ut::Registry::suite_capacity && ut::same_id(local.error, "suite_capacity"));
  local = {};
  local.begin_suite("duplicate");
  local.active_suite = nullptr;
  local.begin_suite("duplicate");
  ut::expect(ut::same_id(local.error, "duplicate_suite"));
  local = {};
  local.begin_suite("invalid id");
  ut::expect(ut::same_id(local.error, "invalid_suite"));
  bench::Registry benchmarks;
  for (unsigned i = 0; i < 17; ++i) {
    benchmarks.add(ids[i], [](bench::Context &) {});
  }
  ut::expect(benchmarks.count == 16 && ut::same_id(benchmarks.error, "benchmark_capacity"));
}
void cleanup_guards() {
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
#if defined(MOSS_ARCH_RISCV)
  entry.raw = original ^ mm::page_attr::EXECUTE;
#else
  entry.raw = original ^ mm::page_attr::XN;
#endif
  ut::expect(page_table_hash(address) != hash);
  entry.set_page(2 * page_size, mm::page_perms::USER_RW);
  ut::expect(page_table_hash(address) != hash);
}
void heap_bounds() {
  auto before = mm::RuntimeHeapAllocator::get_heap_end();
  ut::expect(before <= moss::abi::linker::heap_end());
  ut::expect(!mm::RuntimeHeapAllocator::expand_heap(1ULL << 40));
  ut::expect(before == mm::RuntimeHeapAllocator::get_heap_end());
}
void heap_alignment() {
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
  const auto before = Heap::get_heap_stats().allocated_bytes;
  u32 seed = 0x5eed;
  bool valid = true;
  for (usize step = 0; step < 4096 && valid; ++step) {
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
  using Heap = mm::RuntimeHeapAllocator;
  constexpr usize chunk_size = 64 * 1024;
  void *owned[128]{};
  auto sentinel = mm::PageFrameAllocator::allocate_pages(0);
  if (!ut::expect(static_cast<bool>(sentinel))) {
    return;
  }
  auto *page = reinterpret_cast<volatile u64 *>(*sentinel);
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
  constexpr usize probes[] = {4 * 1024, 64 * 1024, 256 * 1024};
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
  auto reused = Heap::allocate(1024 * 1024);
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
    // The host's case deadline bounds a stuck peer; no guest-clock dependency.
    while (__atomic_load_n(&value, __ATOMIC_ACQUIRE) != expected) {
    }
  }

  shared_ptr<ConcurrentValue> race_insert(bool &list_inserted, u32 actor) {
    list_inserted = list.push_front_unless([](u32 value) { return value == 42; }, u32{42});
    auto result = map.get_or_insert(u32{2}, [&] {
      auto candidate = make_shared<ConcurrentValue>(&destroyed);
      __atomic_fetch_add(&factories, 1U, __ATOMIC_RELEASE);
      wait_for(factories, 2); // Force both creators past the initial lookup.
      return candidate;
    });
    if (actor == 2)
      peer_value = reinterpret_cast<VirtAddr>(result.get());
    __atomic_fetch_or(&inserted, actor, __ATOMIC_RELEASE);
    wait_for(inserted, 3);
    return result;
  }

  bool peer() {
    peer_cpu = arch::get_current_cpu_id();
    auto *thread = process::CfsScheduler::get_current_task();
    peer_ok = peer_cpu == 1 && thread && thread->cpu_affinity_mask.low_word() == 2;
    wait_for(phase, 1);
    auto held = map.find(u32{1});
    peer_ok = peer_ok && held && *held;
    __atomic_store_n(&phase, 2U, __ATOMIC_RELEASE);
    wait_for(phase, 3);
    const bool alive = __atomic_load_n(&destroyed, __ATOMIC_ACQUIRE) == 0;
    peer_ok = peer_ok && alive;
    if (held && alive)
      peer_ok = peer_ok && (*held)->destroyed == &destroyed;
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

void failing_case() { ut::expect(false); }
[[noreturn]] void panic_case() {
  Event("fatal").str("case", active_case).str("kind", "panic").send();
  logging::klog::panic("validation intentional panic");
  arch::kernel_panic("validation intentional panic");
}
[[noreturn]] void timeout_case() {
  Event("fatal").str("case", active_case).str("kind", "timeout").send();
  for (;;) {
    arch::cpu_yield();
  }
}

void declare_cases() {
  ut::register_suite("resources", [] { ut::register_test("cpu_memory", resources); });
  ut::register_suite("mm", [] {
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
    ut::register_test("ownership", container_ownership);
    ut::register_test("release_reuse", container_release_reuse);
    ut::register_test("map_ownership", container_map_ownership);
    ut::register_test("held_reader", container_held_reader);
    ut::register_test("reentry", container_reentry);
  });
  ut::register_suite("containers.smp", [] { ut::register_test("interleaving", empty_case); });
  ut::register_suite("vfs", [] {
    ut::register_test("read_position_eof", file_read);
    ut::register_test("errors_readonly", file_errors);
  });
  ut::register_suite("users", [] {
    ut::register_test("syscall_values", empty_case);
    ut::register_test("user_ranges", empty_case);
    ut::register_test("fork_exec_exit_reap", empty_case);
  });
  ut::register_suite("users.frame", [] {
    ut::register_test("native_frame", empty_case);
    ut::register_test("fork_registers", empty_case);
    ut::register_test("signal_return", empty_case);
  });
  ut::register_suite("users.uaccess", [] {
    for (auto *name : uaccess_cases)
      ut::register_test(name, empty_case);
  });
  ut::register_suite("users.vm", [] {
    ut::register_test("private_cow", empty_case);
    ut::register_test("readonly_cow", empty_case);
    ut::register_test("access_permissions", empty_case);
  });
#if defined(MOSS_ARCH_X86_64)
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
#elif defined(MOSS_ARCH_RISCV)
  result.frequency = moss::fdt::get_platform_info().timebase_frequency;
  result.source = "dtb.timebase-frequency";
#else
  u32 eax, ebx, ecx, edx;
  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0), "c"(0));
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
      out(0x43, 0);
      u16 low = in(0x40);
      return static_cast<u16>(low | (static_cast<u16>(in(0x40)) << 8));
    };
    u64 frequencies[3]{};
    for (unsigned sample = 0; sample < 3; ++sample) {
      out(0x43, 0x30);
      out(0x40, 0xFF);
      out(0x40, 0xFF);
      u16 first = count();
      u64 start = bench::read_counter();
      u16 last = first;
      for (u32 retry = 0; retry < 1000000 && first - last < 16384; ++retry) {
        last = count();
      }
      u64 end = bench::read_counter();
      if (last > first || first - last < 16384 || first - last > 60000 || end <= start) {
        return {};
      }
      result.calibration_ticks[sample] = end - start;
      result.reference_ticks[sample] = static_cast<u64>(first - last);
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
    result.uncertainty_ppm = static_cast<unsigned>((high - low) * 1000000 / low) + 1000;
    result.source = "pit.channel0";
  }
#endif
  u64 begin = bench::read_counter();
  u64 previous = begin;
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
      .send();
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

extern "C" long moss_validation_call(long op, long arg1, [[maybe_unused]] long arg2) noexcept {
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
    if (selected_benchmark) {
      start_case(selection);
      prepare_clock();
      if (ut::same_id(selection, "bench.getpid")) {
#if defined(MOSS_ARCH_ARM64)
        u64 control;
        asm volatile("mrs %0, cntkctl_el1" : "=r"(control));
        control |= 2; // EL0VCTEN: userspace brackets the real syscall round trip.
        asm volatile("msr cntkctl_el1, %0; isb" ::"r"(control) : "memory");
#endif
        syscall_pilot = fixed_iterations == 0;
        syscall_iterations = fixed_iterations ? fixed_iterations : 1;
        return 2;
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
  if (op == 1 &&
      (ut::same_id(selection, "users") || ut::same_id(selection, "users.vm") || ut::same_id(selection, "users.frame") ||
       ut::same_id(selection, "users.uaccess")) &&
      !failed && !active_case && arg1 == static_cast<long>(completed) && arg1 >= 0 &&
      arg1 < (ut::same_id(selection, "users.uaccess") ? uaccess_case_count : 3)) {
    if (ut::same_id(selection, "users.uaccess")) {
      start_case(uaccess_cases[arg1]);
    } else if (ut::same_id(selection, "users.frame")) {
      start_case(arg1 == 0 ? "native_frame" : (arg1 == 1 ? "fork_registers" : "signal_return"));
    } else if (ut::same_id(selection, "users.vm")) {
      start_case(arg1 == 0 ? "private_cow" : (arg1 == 1 ? "readonly_cow" : "access_permissions"));
    } else {
      start_case(arg1 == 0 ? "syscall_values" : (arg1 == 1 ? "user_ranges" : "fork_exec_exit_reap"));
    }
    return 0;
  }
  if (op == 2 && active_case &&
      (ut::same_id(selection, "users") || ut::same_id(selection, "users.vm") ||
       ut::same_id(selection, "users.simd_fault") || ut::same_id(selection, "users.frame") ||
       ut::same_id(selection, "users.uaccess"))) {
    if (arg2 != 0) {
      logging::klog::error("users checks failed: mask={:#x}", static_cast<u64>(arg2));
    }
    ut::expect(arg1 != 0 && affinity_valid());
    end_case();
    return failed ? 0 : 1;
  }
  if (op == 3) {
    finish();
  }
  if ((op == 12 || op == 13) && ut::same_id(selection, "users.uaccess") && active_case) {
    auto *thread = process::CfsScheduler::get_current_task();
    auto owner = thread && process::g_process_manager ? process::g_process_manager->find_process(thread->owner_pid)
                                                      : shared_ptr<process::Process>{};
    auto *as = owner ? owner->address_space() : nullptr;
    const u64 address = static_cast<u64>(arg1);
    if (!ut::expect(as && as->allows_user_access(address, 8, process::vma_flags::WRITE)))
      return 0;
    auto *leaf = mm::PageTableManager::get_user_pte(as->pgd_phys, address);
    ut::expect(!leaf || !leaf->is_valid());
    usize released_pages = 0;
    if (op == 12) {
      if (!ut::expect(uaccess_pressure == nullptr))
        return 0;
      uaccess_pressure = new PagePressure{};
      uaccess_free_before = mm::PageFrameAllocator::get_memory_stats().free_pages;
      if (ut::expect(uaccess_pressure && uaccess_pressure->acquire(0)))
        return 1;
    } else {
      ut::expect(uaccess_pressure != nullptr);
      released_pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
      // The sigframe test reaps its child before releasing the pressure.
      // Account for those newly freed, non-fixture pages without relaxing the
      // zero-free-page invariant of the other fault cases.
      if (!(arg2 == 1 && ut::same_id(active_case, "sigframe_fault")))
        ut::expect(released_pages == 0);
    }
    delete uaccess_pressure;
    uaccess_pressure = nullptr;
    ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == uaccess_free_before + released_pages);
    return 0;
  }
  if (op == 14 && ut::same_id(selection, "users.uaccess") && ut::same_id(active_case, "partial_read") &&
      uaccess_pressure) {
    u8 bytes[8];
    for (auto &byte : bytes)
      byte = 0xa5;
    bool valid = ut::expect(process::copy_from_user(bytes, static_cast<u64>(arg1), sizeof(bytes)) == 4);
    for (usize i = 0; i < sizeof(bytes); ++i)
      valid = ut::expect(bytes[i] == (i < 4 ? i : 0)) && valid;
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
  if (op == 4 && ut::same_id(selection, "bench.getpid")) {
    if (sample_index >= warmup_count + sample_count) {
      end_case();
      return 0;
    }
    return static_cast<long>(syscall_iterations);
  }
  if (op == 5 && ut::same_id(selection, "bench.getpid")) {
    if (arg1 <= 0 || !arg2 || !affinity_valid()) {
      failed = true;
      finish("invalid_sample");
    }
    if (syscall_pilot) {
      if (static_cast<u64>(arg1) >= clock_info.frequency / 1000 || syscall_iterations == 65536) {
        syscall_pilot = false;
      } else {
        syscall_iterations *= 2;
      }
    } else {
      record_batch(static_cast<u64>(arg1), syscall_iterations, sample_index < warmup_count, syscall_overhead);
    }
    return 0;
  }
  if (op == 6 && ut::same_id(selection, "bench.getpid") && arg1 >= 0) {
    syscall_overhead = static_cast<u64>(arg1);
    return 0;
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
