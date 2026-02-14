// MOSS MMU Hardware Abstraction Layer
//
// Provides architecture-specific page table entry format, address space
// configuration, and MMU control operations.
//
// What lives here (architecture-specific):
//   - PTE bit-field definitions (PageAttr, PagePerms)
//   - Address space configuration registers (TCR, MAIR / CR3 / satp)
//   - Virtual address breakdown (index widths, shift amounts)
//   - MMU enable/disable and register read-back
//
// What stays in mm.cppm (architecture-independent):
//   - PageTableManager (allocation, walk, map_page/map_region)
//   - PageTable/PageTableEntry structs (512 entries × 8B = 4KB on all archs)
//   - Higher-level MM subsystems (buddy, slab, vmalloc, etc.)

module;

#include "arch_detect.h"

export module moss.hal.mmu;

import moss.std;
import moss.types;
import moss.result;
import moss.arch;
import moss.platform;

export namespace moss::kernel::hal::mmu {

using moss::u8;
using moss::u16;
using moss::u32;
using moss::u64;
using moss::kernel::PhysAddr;
using moss::kernel::VirtAddr;
using moss::kernel::usize;
using moss::kernel::VoidResult;
using moss::kernel::ErrorCode;

// ============================================================================
// Page Table Entry attribute bits — architecture-specific PTE format
// ============================================================================
namespace PageAttr {

#if defined(MOSS_ARCH_ARM64)
// ARM64 Stage-1 descriptor format (ARMv8-A, 4KB granule)
inline constexpr u64 VALID          = (1ULL << 0);
inline constexpr u64 TABLE          = (1ULL << 1);
inline constexpr u64 USER           = (1ULL << 6);   // AP[1]
inline constexpr u64 READONLY       = (1ULL << 7);   // AP[2]
inline constexpr u64 SHARED         = (1ULL << 8);   // SH[1:0] low bit
inline constexpr u64 AF             = (1ULL << 10);   // Access Flag
inline constexpr u64 NG             = (1ULL << 11);   // not Global
inline constexpr u64 PXN            = (1ULL << 53);   // Privileged eXecute Never
inline constexpr u64 XN             = (1ULL << 54);   // eXecute Never

inline constexpr u64 ATTR_IDX_SHIFT = 2;
inline constexpr u64 ATTR_DEVICE    = (0ULL << ATTR_IDX_SHIFT); // MAIR index 0
inline constexpr u64 ATTR_NORMAL    = (1ULL << ATTR_IDX_SHIFT); // MAIR index 1
inline constexpr u64 ATTR_NORMAL_NC = (2ULL << ATTR_IDX_SHIFT); // MAIR index 2

#elif defined(MOSS_ARCH_X86_64)
// x86_64 4-level paging PTE format (Intel SDM Vol.3, Ch.4)
inline constexpr u64 VALID          = (1ULL << 0);   // Present
inline constexpr u64 TABLE          = (1ULL << 0);   // Present (same bit for tables)
inline constexpr u64 USER           = (1ULL << 2);   // U/S (User/Supervisor)
inline constexpr u64 READONLY       = 0;             // x86 uses WRITABLE, absence = RO
inline constexpr u64 SHARED         = 0;             // No direct equivalent
inline constexpr u64 AF             = (1ULL << 5);   // Accessed
inline constexpr u64 NG             = 0;             // No direct equivalent (use PCID)
inline constexpr u64 PXN            = 0;             // No PXN on x86 (use NX)
inline constexpr u64 XN             = (1ULL << 63);  // NX (No Execute)

inline constexpr u64 ATTR_IDX_SHIFT = 0;
inline constexpr u64 ATTR_DEVICE    = (1ULL << 4);   // PCD (Page Cache Disable)
inline constexpr u64 ATTR_NORMAL    = 0;             // Default: cacheable
inline constexpr u64 ATTR_NORMAL_NC = (1ULL << 4);   // PCD

// x86-specific: WRITABLE bit (ARM64 uses READONLY inversion)
inline constexpr u64 WRITABLE       = (1ULL << 1);   // R/W
inline constexpr u64 DIRTY          = (1ULL << 6);   // Dirty
inline constexpr u64 HUGE_PAGE      = (1ULL << 7);   // PS (Page Size, for 2MB/1GB)
inline constexpr u64 GLOBAL         = (1ULL << 8);   // Global

#elif defined(MOSS_ARCH_RISCV)
// RISC-V Sv48 PTE format (RISC-V Privileged Spec, Ch. 4.4)
inline constexpr u64 VALID          = (1ULL << 0);   // V (Valid)
inline constexpr u64 TABLE          = (1ULL << 0);   // V (leaf vs non-leaf determined by RWX)
inline constexpr u64 USER           = (1ULL << 4);   // U (User)
inline constexpr u64 READONLY       = 0;             // RISC-V uses explicit R/W/X
inline constexpr u64 SHARED         = 0;             // No direct equivalent
inline constexpr u64 AF             = (1ULL << 6);   // A (Accessed)
inline constexpr u64 NG             = 0;             // No direct equivalent
inline constexpr u64 PXN            = 0;             // No PXN concept
inline constexpr u64 XN             = 0;             // RISC-V uses explicit X bit

inline constexpr u64 ATTR_IDX_SHIFT = 0;
inline constexpr u64 ATTR_DEVICE    = 0;             // RISC-V uses PMA, not PTE attrs
inline constexpr u64 ATTR_NORMAL    = 0;
inline constexpr u64 ATTR_NORMAL_NC = 0;

// RISC-V-specific: explicit permission bits
inline constexpr u64 READ           = (1ULL << 1);   // R
inline constexpr u64 WRITE          = (1ULL << 2);   // W
inline constexpr u64 EXECUTE        = (1ULL << 3);   // X
inline constexpr u64 DIRTY          = (1ULL << 7);   // D (Dirty)
inline constexpr u64 GLOBAL         = (1ULL << 5);   // G (Global)
#endif

} // namespace PageAttr

// ============================================================================
// Common page permission presets
// ============================================================================
namespace PagePerms {

#if defined(MOSS_ARCH_ARM64)
inline constexpr u64 KERNEL_RO = PageAttr::VALID | PageAttr::AF |
                                 PageAttr::ATTR_NORMAL | PageAttr::READONLY |
                                 PageAttr::PXN | PageAttr::XN;
inline constexpr u64 KERNEL_RW = PageAttr::VALID | PageAttr::AF |
                                 PageAttr::ATTR_NORMAL | PageAttr::PXN |
                                 PageAttr::XN;
inline constexpr u64 KERNEL_RX = PageAttr::VALID | PageAttr::AF |
                                 PageAttr::ATTR_NORMAL | PageAttr::READONLY;
inline constexpr u64 USER_RO = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                               PageAttr::READONLY | PageAttr::ATTR_NORMAL;
inline constexpr u64 USER_RW = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                               PageAttr::ATTR_NORMAL;
inline constexpr u64 USER_RX = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                               PageAttr::READONLY | PageAttr::ATTR_NORMAL;
inline constexpr u64 DEVICE = PageAttr::VALID | PageAttr::AF |
                              PageAttr::ATTR_DEVICE | PageAttr::XN |
                              PageAttr::PXN;

#elif defined(MOSS_ARCH_X86_64)
inline constexpr u64 KERNEL_RO = PageAttr::VALID | PageAttr::AF |
                                 PageAttr::ATTR_NORMAL | PageAttr::XN;
inline constexpr u64 KERNEL_RW = PageAttr::VALID | PageAttr::AF |
                                 PageAttr::ATTR_NORMAL | PageAttr::WRITABLE |
                                 PageAttr::XN;
inline constexpr u64 KERNEL_RX = PageAttr::VALID | PageAttr::AF |
                                 PageAttr::ATTR_NORMAL;
inline constexpr u64 USER_RO = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                               PageAttr::ATTR_NORMAL | PageAttr::XN;
inline constexpr u64 USER_RW = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                               PageAttr::WRITABLE | PageAttr::ATTR_NORMAL |
                               PageAttr::XN;
inline constexpr u64 USER_RX = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                               PageAttr::ATTR_NORMAL;
inline constexpr u64 DEVICE = PageAttr::VALID | PageAttr::AF |
                              PageAttr::ATTR_DEVICE | PageAttr::XN |
                              PageAttr::WRITABLE;

#elif defined(MOSS_ARCH_RISCV)
inline constexpr u64 KERNEL_RO = PageAttr::VALID | PageAttr::AF |
                                 PageAttr::READ | PageAttr::GLOBAL;
inline constexpr u64 KERNEL_RW = PageAttr::VALID | PageAttr::AF |
                                 PageAttr::READ | PageAttr::WRITE |
                                 PageAttr::GLOBAL;
inline constexpr u64 KERNEL_RX = PageAttr::VALID | PageAttr::AF |
                                 PageAttr::READ | PageAttr::EXECUTE |
                                 PageAttr::GLOBAL;
inline constexpr u64 USER_RO = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                               PageAttr::READ;
inline constexpr u64 USER_RW = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                               PageAttr::READ | PageAttr::WRITE;
inline constexpr u64 USER_RX = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                               PageAttr::READ | PageAttr::EXECUTE;
inline constexpr u64 DEVICE = PageAttr::VALID | PageAttr::AF |
                              PageAttr::READ | PageAttr::WRITE | PageAttr::GLOBAL;
#endif

} // namespace PagePerms

// ============================================================================
// Virtual address breakdown — index bit positions for 4KB granule 4-level paging
// ============================================================================
//
// All three architectures use the same 4-level structure with 9-bit indices
// for 4KB pages (512 entries per table):
//   ARM64:  PGD[47:39] PUD[38:30] PMD[29:21] PTE[20:12] Offset[11:0]
//   x86_64: PML4[47:39] PDPT[38:30] PD[29:21] PT[20:12] Offset[11:0]
//   RISC-V Sv48: L3[47:39] L2[38:30] L1[29:21] L0[20:12] Offset[11:0]

struct VirtualAddressBreakdown {
  u16 pgd_index;     // Level 4 (ARM64: PGD, x86: PML4, RV: L3)
  u16 pud_index;     // Level 3 (ARM64: PUD, x86: PDPT, RV: L2)
  u16 pmd_index;     // Level 2 (ARM64: PMD, x86: PD,   RV: L1)
  u16 pte_index;     // Level 1 (ARM64: PTE, x86: PT,   RV: L0)
  u16 page_offset;   // Offset within page [11:0]
};

[[nodiscard]] inline constexpr VirtualAddressBreakdown
break_virtual_address(VirtAddr vaddr) noexcept {
  return {.pgd_index   = static_cast<u16>((vaddr >> 39) & 0x1FF),
          .pud_index   = static_cast<u16>((vaddr >> 30) & 0x1FF),
          .pmd_index   = static_cast<u16>((vaddr >> 21) & 0x1FF),
          .pte_index   = static_cast<u16>((vaddr >> 12) & 0x1FF),
          .page_offset = static_cast<u16>(vaddr & 0xFFF)};
}

// ============================================================================
// Address space configuration — architecture-specific MMU register values
// ============================================================================
struct AddressSpaceConfig {
#if defined(MOSS_ARCH_ARM64)
  // TCR_EL1 register value for 48-bit VA, 4KB granule, Inner Shareable
  static constexpr u64 TCR_VALUE =
      (16ULL << 0)  |  // T0SZ: 48-bit TTBR0 region
      (16ULL << 16) |  // T1SZ: 48-bit TTBR1 region
      (0ULL << 6)   |  // not used
      (0ULL << 23)  |  // not used
      (0ULL << 14)  |  // TG0: 4KB granule
      (0ULL << 30)  |  // TG1: 4KB granule
      (1ULL << 8)   |  // IRGN0: WB-WA
      (1ULL << 10)  |  // ORGN0: WB-WA
      (3ULL << 12)  |  // SH0: Inner Shareable
      (1ULL << 24)  |  // IRGN1: WB-WA
      (1ULL << 26)  |  // ORGN1: WB-WA
      (3ULL << 28)  |  // SH1: Inner Shareable
      (5ULL << 32);    // IPS: 48-bit PA

  // MAIR_EL1: Memory Attribute Indirection Register
  static constexpr u64 MAIR_DEVICE_nGnRnE = 0x00ULL;
  static constexpr u64 MAIR_NORMAL_WBWA   = 0xFFULL;
  static constexpr u64 MAIR_NORMAL_NC     = 0x44ULL;
  static constexpr u64 MAIR_VALUE =
      (MAIR_DEVICE_nGnRnE << 0) | (MAIR_NORMAL_WBWA << 8) | (MAIR_NORMAL_NC << 16);

#elif defined(MOSS_ARCH_X86_64)
  // x86_64 does not use MAIR/TCR; page attributes are directly in PTE bits
  static constexpr u64 TCR_VALUE  = 0; // not applicable
  static constexpr u64 MAIR_VALUE = 0; // not applicable

#elif defined(MOSS_ARCH_RISCV)
  // RISC-V uses satp register; mode=8 for Sv48, mode=9 for Sv57
  static constexpr u64 SATP_MODE_SV48 = 8ULL << 60;
  static constexpr u64 TCR_VALUE  = 0; // not applicable (satp used instead)
  static constexpr u64 MAIR_VALUE = 0; // RISC-V uses PMA, not MAIR
#endif
};

// ============================================================================
// PTE address mask — extract physical address from a PTE entry
// ============================================================================
#if defined(MOSS_ARCH_ARM64)
inline constexpr u64 PTE_ADDR_MASK = 0x0000FFFFFFFFF000ULL; // bits [47:12]
#elif defined(MOSS_ARCH_X86_64)
inline constexpr u64 PTE_ADDR_MASK = 0x000FFFFFFFFFF000ULL; // bits [51:12]
#elif defined(MOSS_ARCH_RISCV)
inline constexpr u64 PTE_ADDR_MASK = 0x003FFFFFFFFFFC00ULL; // PPN in bits [53:10], shift <<2 for addr
#endif

// ============================================================================
// MMU control operations — enable/disable/query
// ============================================================================

/// Write address space configuration registers (MAIR, TCR or equivalent).
/// Must be called before enable_mmu().
inline void configure_address_space(PhysAddr pgd_phys) noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("msr mair_el1, %0" ::"r"(AddressSpaceConfig::MAIR_VALUE));
  asm volatile("msr tcr_el1, %0"  ::"r"(AddressSpaceConfig::TCR_VALUE));
  asm volatile("msr ttbr0_el1, %0" ::"r"(pgd_phys));
  asm volatile("msr ttbr1_el1, %0" ::"r"(pgd_phys));
  // Ensure writes are visible
  asm volatile("isb" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  // x86_64: load CR3 with page table base
  asm volatile("mov %0, %%cr3" ::"r"(pgd_phys) : "memory");
#elif defined(MOSS_ARCH_RISCV)
  // RISC-V: set satp register (Sv48 mode + PPN)
  u64 satp_val = AddressSpaceConfig::SATP_MODE_SV48 |
                 ((pgd_phys >> 12) & 0x00000FFFFFFFFFFFULL);
  asm volatile("csrw satp, %0" ::"r"(satp_val) : "memory");
  asm volatile("sfence.vma" ::: "memory");
#endif
}

/// Enable the MMU (and caches where applicable).
/// The page table must already be configured via configure_address_space().
inline VoidResult enable_mmu(PhysAddr pgd_phys) noexcept {
#if defined(MOSS_ARCH_ARM64)
  // Read current SCTLR
  u64 sctlr;
  asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));

  // Configure address space registers
  configure_address_space(pgd_phys);

  // Invalidate TLB and I-cache before enabling
  moss::kernel::arch::flush_tlb();
  moss::kernel::arch::invalidate_icache();

  // Enable MMU + D-cache + I-cache
  sctlr |= (1ULL << 0);   // M:  MMU enable
  sctlr |= (1ULL << 2);   // C:  Data cache enable
  sctlr |= (1ULL << 12);  // I:  Instruction cache enable
  asm volatile("msr sctlr_el1, %0" ::"r"(sctlr));
  asm volatile("dsb sy");
  asm volatile("isb");

#elif defined(MOSS_ARCH_X86_64)
  // x86_64: MMU is always on in long mode; loading CR3 activates new tables
  configure_address_space(pgd_phys);

#elif defined(MOSS_ARCH_RISCV)
  // RISC-V: writing satp with non-zero mode enables virtual memory
  configure_address_space(pgd_phys);
  asm volatile("sfence.vma" ::: "memory");
#endif

  return VoidResult{};
}

/// Check if the MMU is currently enabled.
[[nodiscard]] inline bool mmu_enabled() noexcept {
#if defined(MOSS_ARCH_ARM64)
  u64 sctlr;
  asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
  return (sctlr & (1ULL << 0)) != 0;
#elif defined(MOSS_ARCH_X86_64)
  // In long mode, paging is always enabled
  return true;
#elif defined(MOSS_ARCH_RISCV)
  u64 satp;
  asm volatile("csrr %0, satp" : "=r"(satp));
  return ((satp >> 60) & 0xF) != 0; // non-zero mode = paging on
#else
  return false;
#endif
}

// ============================================================================
// Early block mapping helpers — for initial 1GB identity mapping
// ============================================================================

/// Build a 1GB block descriptor for device memory.
[[nodiscard]] inline constexpr u64 make_device_block(PhysAddr block_addr) noexcept {
#if defined(MOSS_ARCH_ARM64)
  return (block_addr & PTE_ADDR_MASK) |
         PageAttr::VALID | PageAttr::AF | PageAttr::ATTR_DEVICE;
#elif defined(MOSS_ARCH_X86_64)
  return (block_addr & PTE_ADDR_MASK) |
         PageAttr::VALID | PageAttr::WRITABLE | PageAttr::AF |
         PageAttr::ATTR_DEVICE | PageAttr::HUGE_PAGE;
#elif defined(MOSS_ARCH_RISCV)
  // RISC-V: leaf PTE with R+W, no execute
  return (block_addr & PTE_ADDR_MASK) |
         PageAttr::VALID | PageAttr::AF | PageAttr::READ | PageAttr::WRITE |
         PageAttr::GLOBAL;
#endif
}

/// Build a 1GB block descriptor for normal (cacheable) memory.
[[nodiscard]] inline constexpr u64 make_normal_block(PhysAddr block_addr) noexcept {
#if defined(MOSS_ARCH_ARM64)
  return (block_addr & PTE_ADDR_MASK) |
         PageAttr::VALID | PageAttr::AF | PageAttr::ATTR_NORMAL |
         (3ULL << 8); // Inner Shareable
#elif defined(MOSS_ARCH_X86_64)
  return (block_addr & PTE_ADDR_MASK) |
         PageAttr::VALID | PageAttr::WRITABLE | PageAttr::AF |
         PageAttr::ATTR_NORMAL | PageAttr::HUGE_PAGE;
#elif defined(MOSS_ARCH_RISCV)
  // RISC-V: leaf PTE with R+W+X
  return (block_addr & PTE_ADDR_MASK) |
         PageAttr::VALID | PageAttr::AF | PageAttr::READ | PageAttr::WRITE |
         PageAttr::EXECUTE | PageAttr::GLOBAL;
#endif
}

} // namespace moss::kernel::hal::mmu
