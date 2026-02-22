// MOSS Memory Management Module - Page Table partition
// Contains: PageTableEntry, PageTable, PageTableManager

export module moss.mm:page_table;

import :core;

import moss.std;
import moss.types;
import moss.result;
import moss.containers;
import moss.arch;
import moss.hal.mmu;
import moss.abi;

// ========================================================================
// page_table.hpp - ARM64 MMU types
// ========================================================================
export namespace moss::kernel::mm {

// Re-export basic types used throughout
using moss::kernel::ErrorCode;
using moss::kernel::KernelResult;
using moss::kernel::PhysAddr;
using moss::kernel::VirtAddr;
using moss::kernel::VoidResult;

enum class MemoryAttributes : u8 {
  NORMAL_CACHEABLE = 0,
  NORMAL_NON_CACHEABLE = 1,
  DEVICE_nGnRnE = 2,
  DEVICE_nGnRE = 3,
  DEVICE_GRE = 4
};

enum class PageLevel : u32 { PGD = 0, PUD = 1, PMD = 2, PTE = 3 };

enum class PageSize : u64 { Size4KB = PAGE_SIZE, Size2MB = 2ULL * 1024 * 1024, Size1GB = 1024ULL * 1024 * 1024 };

// page_attr and page_perms are re-exported from the MMU HAL.
// This provides architecture-specific PTE bit-field definitions
// (ARM64 descriptors, x86_64 PTE bits, RISC-V Sv48 PTE bits)
// from a single source of truth in moss.hal.mmu.
namespace page_attr = ::moss::kernel::hal::mmu::page_attr;
namespace page_perms = ::moss::kernel::hal::mmu::page_perms;

struct [[gnu::packed]] PageTableEntry {
  u64 raw;
  constexpr PageTableEntry() : raw(0) {}
  constexpr explicit PageTableEntry(u64 value) : raw(value) {}
  [[nodiscard]] constexpr bool is_valid() const { return raw & page_attr::VALID; }
  [[nodiscard]] constexpr bool is_table() const { return raw & page_attr::TABLE; }
  [[nodiscard]] constexpr bool is_block() const { return is_valid() && !is_table(); }
  [[nodiscard]] constexpr PhysAddr get_phys_addr() const {
    // RISC-V: PPN in bits[53:10], physical addr = PPN << 12 = (pte & mask) << 2
#if defined(MOSS_ARCH_RISCV)
    return (raw & hal::mmu::PTE_ADDR_MASK) << 2;
#else
    return raw & hal::mmu::PTE_ADDR_MASK;
#endif
  }
  constexpr void set_table(PhysAddr next_table_pa) {
#if defined(MOSS_ARCH_RISCV)
    raw = ((next_table_pa >> 2) & hal::mmu::PTE_ADDR_MASK) | page_attr::VALID | page_attr::TABLE;
#else
    raw = (next_table_pa & hal::mmu::PTE_ADDR_MASK) | page_attr::VALID | page_attr::TABLE;
#endif
  }
  constexpr void set_block(PhysAddr block_pa, u64 attributes) {
#if defined(MOSS_ARCH_RISCV)
    raw = ((block_pa >> 2) & hal::mmu::PTE_ADDR_MASK) | attributes | page_attr::VALID;
#else
    raw = (block_pa & hal::mmu::PTE_ADDR_MASK) | attributes | page_attr::VALID;
#endif
  }
  // L3 page descriptor: bits[1:0]=0b11 (same encoding as table descriptor)
  constexpr void set_page(PhysAddr page_pa, u64 attributes) {
#if defined(MOSS_ARCH_RISCV)
    raw = ((page_pa >> 2) & hal::mmu::PTE_ADDR_MASK) | attributes | page_attr::VALID | page_attr::TABLE;
#else
    raw = (page_pa & hal::mmu::PTE_ADDR_MASK) | attributes | page_attr::VALID | page_attr::TABLE;
#endif
  }
  constexpr void clear() { raw = 0; }

  // ---- COW (Copy-on-Write) helpers ----
  [[nodiscard]] constexpr bool is_cow() const { return (raw & page_attr::SW_COW) != 0; }
  constexpr void set_cow() { raw |= page_attr::SW_COW; }
  constexpr void clear_cow() { raw &= ~page_attr::SW_COW; }

  // Make page read-only (architecture-specific bit manipulation)
  constexpr void make_readonly() {
#if defined(MOSS_ARCH_ARM64)
    raw |= page_attr::READONLY; // AP[2]=1 -> read-only
#elif defined(MOSS_ARCH_X86_64)
    raw &= ~page_attr::WRITABLE; // Clear R/W bit -> read-only
#elif defined(MOSS_ARCH_RISCV)
    raw &= ~page_attr::WRITE; // Clear W bit -> read-only
#endif
  }

  // Make page writable (architecture-specific bit manipulation)
  constexpr void make_writable() {
#if defined(MOSS_ARCH_ARM64)
    raw &= ~page_attr::READONLY; // Clear AP[2] -> read-write
#elif defined(MOSS_ARCH_X86_64)
    raw |= page_attr::WRITABLE; // Set R/W bit -> read-write
#elif defined(MOSS_ARCH_RISCV)
    raw |= page_attr::WRITE; // Set W bit -> read-write
#endif
  }
};

static_assert(sizeof(PageTableEntry) == 8, "PageTableEntry must be 8 bytes");

struct alignas(PAGE_SIZE) PageTable {
  static constexpr usize ENTRIES_PER_TABLE = PAGE_SIZE / sizeof(PageTableEntry);
  PageTableEntry entries[ENTRIES_PER_TABLE];
  constexpr PageTable() : entries{} {}
  [[nodiscard]] constexpr PageTableEntry &operator[](usize index) { return entries[index]; }
  [[nodiscard]] constexpr const PageTableEntry &operator[](usize index) const { return entries[index]; }
};

static_assert(sizeof(PageTable) == PAGE_SIZE, "PageTable must be one page");

// VirtualAddressBreakdown, break_virtual_address, and AddressSpaceConfig
// are re-exported from the MMU HAL -- architecture-specific definitions
// live in moss.hal.mmu (hal/mmu/src/mmu_hal.cppm).
using hal::mmu::AddressSpaceConfig;
using hal::mmu::break_virtual_address;
using hal::mmu::VirtualAddressBreakdown;

class PageTableManager {
private:
  static constexpr usize MAX_EARLY_TABLES = 64;
  alignas(PAGE_SIZE) static inline PageTable early_tables[MAX_EARLY_TABLES];
  static inline usize next_table_index = 0;
  static inline PageTable *kernel_pgd = nullptr;
  static inline PageTable *kernel_high_pgd = nullptr; // TTBR1 high-half PGD
  static inline bool use_dynamic_alloc = false;       // Switch to buddy-backed alloc

public:
  [[nodiscard]] static KernelResult<PageTable *> allocate_page_table();

  // Dynamically allocate a page table from buddy allocator (post-boot only)
  [[nodiscard]] static KernelResult<PageTable *> allocate_page_table_dynamic();

  [[nodiscard]] static PhysAddr get_physical_address(const PageTable *table) {
    auto va = reinterpret_cast<VirtAddr>(table);
    if (is_kernel_addr(va)) {
      return virt_to_phys(va);
    }
    return static_cast<PhysAddr>(va); // identity-mapped (early boot)
  }
  [[nodiscard]] static PageTable *get_table_from_physical(PhysAddr pa) {
    if (use_dynamic_alloc) {
      return reinterpret_cast<PageTable *>(phys_to_virt(pa));
    }
    return reinterpret_cast<PageTable *>(pa); // identity-mapped (early boot)
  }
  [[nodiscard]] static usize get_table_index(const PageTable *table) {
    if (!table || table < early_tables || table >= early_tables + MAX_EARLY_TABLES) {
      return MAX_EARLY_TABLES;
    }
    return static_cast<usize>(table - early_tables);
  }
  [[nodiscard]] static PageTable *get_table_by_index(usize index) {
    if (index >= MAX_EARLY_TABLES) {
      return nullptr;
    }
    return &early_tables[index];
  }
  [[nodiscard]] static VoidResult setup_kernel_page_tables();
  [[nodiscard]] static VoidResult map_region(VirtAddr virt_addr, PhysAddr phys_addr, usize size, u64 permissions);
  [[nodiscard]] static VoidResult map_page(VirtAddr virt_addr, PhysAddr phys_addr, u64 permissions);
  [[nodiscard]] static VoidResult enable_mmu();
  [[nodiscard]] static PageTable *get_kernel_pgd() { return kernel_pgd; }
  [[nodiscard]] static PageTable *get_kernel_high_pgd() { return kernel_high_pgd; }
  static void enable_dynamic_alloc() { use_dynamic_alloc = true; }
  [[nodiscard]] static bool is_dynamic_alloc() { return use_dynamic_alloc; }

  // Build TTBR1 kernel page table: map physical RAM at KERNEL_DIRECT_MAP_BASE
  [[nodiscard]] static VoidResult setup_kernel_high_half_tables();

  // Map a 4KB page into a user process page table (operates on user PGD, not kernel PGD)
  [[nodiscard]] static VoidResult map_user_page(PhysAddr pgd_phys, VirtAddr va, PhysAddr pa, u64 perms);

  static void invalidate_tlb() { moss::kernel::arch::flush_tlb(); }
  [[nodiscard]] static VoidResult initialize_from_current() { return VoidResult{}; }
  static void print_page_table_details();
  static void print_pgd_entries();
  static void print_mmu_registers();

  // ---- Page fault support: unmap, query, TLB invalidation ----

  // Page mapping query result
  struct PageInfo {
    PhysAddr phys_addr; // Physical address (0 if unmapped)
    u64 attributes;     // Raw PTE attribute bits
    bool mapped;        // Whether a valid mapping exists
    u8 level;           // Mapping granularity: 1=1GB block, 2=2MB block, 3=4KB page
  };

  // Remove a 4KB page mapping. Returns NotFound if no valid PTE exists,
  // NotSupported if the address falls within a block mapping (1GB/2MB).
  [[nodiscard]] static VoidResult unmap_page(VirtAddr virt_addr);

  // Query the mapping state of a virtual address without modifying anything.
  [[nodiscard]] static PageInfo query_page(VirtAddr virt_addr);

  // Free all user page tables and demand-paged physical pages for a process.
  // Walks PGD->PUD->PMD->PTE, frees leaf pages and intermediate tables.
  // Skips PGD[0] (shared kernel identity map).
  // The PGD page itself is also freed.
  // COW-aware: only frees physical pages when refcount drops to 0.
  static void free_user_page_tables(PhysAddr pgd_phys);

  // Walk user page tables and return a mutable pointer to the L3 PTE.
  // Returns nullptr if any intermediate table is missing (does not allocate).
  // Used by the COW fault handler to modify PTE in-place.
  [[nodiscard]] static PageTableEntry *get_user_pte(PhysAddr pgd_phys, VirtAddr va);

  // Unmap a single user page: clear PTE, invalidate TLB, free physical page
  // when refcount drops to 0 (COW-aware).  No-op if the PTE is not mapped.
  static void unmap_user_page(PhysAddr pgd_phys, VirtAddr va) noexcept;

  // Clone a user page table tree for fork().
  // Allocates fresh intermediate tables (PUD/PMD/PTE) for dst_pgd_phys.
  // Leaf pages are shared: both src and dst PTEs are marked READONLY + SW_COW,
  // and physical page refcounts are incremented.
  // PGD[0] (kernel identity map) is skipped (already copied by create_user_address_space).
  static void clone_user_page_tables(PhysAddr src_pgd_phys, PhysAddr dst_pgd_phys);

  // Invalidate TLB entry for a single virtual address
  static void invalidate_tlb_addr(VirtAddr virt_addr) {
#if defined(MOSS_ARCH_ARM64)
    asm volatile("dsb ishst" ::: "memory");
    asm volatile("tlbi vale1is, %0" ::"r"(virt_addr >> 12) : "memory");
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
    asm volatile("invlpg (%0)" ::"r"(virt_addr) : "memory");
#elif defined(MOSS_ARCH_RISCV)
    asm volatile("sfence.vma %0, zero" ::"r"(virt_addr) : "memory");
#endif
  }
};

[[nodiscard]] VoidResult setup_mmu();
void invalidate_all_tlb();

} // namespace moss::kernel::mm
