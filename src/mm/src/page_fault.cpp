// MOSS Page Fault Handler
//
// Resolves user demand/COW faults, including accesses from kernel copies, and
// diagnoses unrecoverable faults. ARM64, x64 and RISC-V entry paths pass their
// native fault state; only registered copy instructions can use uaccess fixups.

module;

// extern "C" handler symbols — defined in this file, called from assembly.
// Forward declarations required in GMF so the extern "C" linkage is
// established before the module purview begins.
extern "C" void kernel_page_fault_handler(unsigned long long esr, unsigned long long far_addr,
                                          unsigned long long elr) noexcept;

extern "C" void unhandled_exception_handler(unsigned long long esr, unsigned long long far_addr, unsigned long long elr,
                                            unsigned long long saved_x30, unsigned long long frame_sp) noexcept;

extern "C" void user_page_fault_handler(unsigned long long esr, unsigned long long far_addr,
                                        unsigned long long elr) noexcept;

extern "C" [[noreturn]] void unhandled_user_exception_handler(unsigned long long esr, unsigned long long far_addr,
                                                              unsigned long long elr) noexcept;

extern "C" void riscv64_page_fault_handler(unsigned long long scause, unsigned long long stval,
                                           unsigned long long sepc) noexcept;

extern "C" void x64_page_fault_handler(unsigned long long error_code, unsigned long long cr2,
                                       unsigned long long rip) noexcept;

module moss.mm;

import moss.abi;

// POSIX SIGSEGV. moss.mm cannot import moss.process's signal constants without
// introducing a module cycle through the process address-space dependency.
constexpr int kSegmentationFaultSignal = 11;

// No-ops in production. Validation orders real faults at the old-frame
// snapshot and records a committed frame before a contending unmap can retire it.
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_cow_snapshot(moss::kernel::PhysAddr /*root*/,
                                                                          moss::kernel::VirtAddr /*address*/) noexcept {
}

extern "C" [[gnu::weak, gnu::noinline]] void
moss_validation_demand_snapshot(moss::kernel::PhysAddr /*root*/, moss::kernel::VirtAddr /*address*/) noexcept {}
extern "C" [[gnu::weak, gnu::noinline]] void
moss_validation_demand_committed(moss::kernel::PhysAddr /*root*/, moss::kernel::VirtAddr /*address*/,
                                 moss::kernel::PhysAddr /*page*/) noexcept {}

namespace moss::kernel::mm {

// ============================================================================
// EC (Exception Class) to human-readable string
// ============================================================================
// ARM64 ESR_EL1 encodings: EC is [31:26] (six bits), ISS is [24:0],
// abort FSC is [5:0] and WnR is bit 6. These are architectural fields, not
// local thresholds; the switches below decode their assigned values.
static auto ec_to_string(u64 ec) noexcept -> const char * {
  switch (ec) {
  case 0x00:
    return "Unknown reason";
  case 0x01:
    return "WFI/WFE trapped";
  case 0x03:
    return "MCR/MRC (cp15)";
  case 0x04:
    return "MCRR/MRRC (cp15)";
  case 0x05:
    return "MCR/MRC (cp14)";
  case 0x06:
    return "LDC/STC access (cp14)";
  case 0x07:
    return "SVE/SIMD/FP trapped";
  case 0x0C:
    return "MRRC (cp14)";
  case 0x0E:
    return "Illegal execution state";
  case 0x11:
    return "SVC from AArch32";
  case 0x15:
    return "SVC from AArch64";
  case 0x18:
    return "MSR/MRS/system instr trapped";
  case 0x19:
    return "SVE access trapped";
  case 0x20:
    return "Instruction Abort (lower EL)";
  case 0x21:
    return "Instruction Abort (same EL)";
  case 0x22:
    return "PC alignment fault";
  case 0x24:
    return "Data Abort (lower EL)";
  case 0x25:
    return "Data Abort (same EL)";
  case 0x26:
    return "SP alignment fault";
  case 0x28:
    return "FP exception (AArch32)";
  case 0x2C:
    return "FP exception (AArch64)";
  case 0x2F:
    return "SError interrupt";
  case 0x30:
    return "Breakpoint (lower EL)";
  case 0x31:
    return "Breakpoint (same EL)";
  case 0x32:
    return "Software Step (lower EL)";
  case 0x33:
    return "Software Step (same EL)";
  case 0x34:
    return "Watchpoint (lower EL)";
  case 0x35:
    return "Watchpoint (same EL)";
  case 0x38:
    return "BKPT (AArch32)";
  case 0x3C:
    return "BRK (AArch64)";
  default:
    return "Other/reserved";
  }
}

// ============================================================================
// DFSC/IFSC (Data/Instruction Fault Status Code) to string
// ============================================================================
static auto dfsc_to_string(u64 dfsc) noexcept -> const char * {
  switch (dfsc & 0x3F) {
  case 0x00:
    return "Address size fault (L0)";
  case 0x01:
    return "Address size fault (L1)";
  case 0x02:
    return "Address size fault (L2)";
  case 0x03:
    return "Address size fault (L3)";
  case 0x04:
    return "Translation fault (L0)";
  case 0x05:
    return "Translation fault (L1)";
  case 0x06:
    return "Translation fault (L2)";
  case 0x07:
    return "Translation fault (L3)";
  case 0x08:
    return "Access flag fault (L0)";
  case 0x09:
    return "Access flag fault (L1)";
  case 0x0A:
    return "Access flag fault (L2)";
  case 0x0B:
    return "Access flag fault (L3)";
  case 0x0C:
    return "Permission fault (L0)";
  case 0x0D:
    return "Permission fault (L1)";
  case 0x0E:
    return "Permission fault (L2)";
  case 0x0F:
    return "Permission fault (L3)";
  case 0x10:
    return "Synchronous external abort";
  case 0x14:
    return "Synchronous external abort (L0)";
  case 0x15:
    return "Synchronous external abort (L1)";
  case 0x16:
    return "Synchronous external abort (L2)";
  case 0x17:
    return "Synchronous external abort (L3)";
  case 0x21:
    return "Alignment fault";
  default:
    return "Other/reserved DFSC";
  }
}

} // namespace moss::kernel::mm

// Import bridge functions from moss.abi (previously declared as extern "C" in this file)
using moss::abi::bridge::resolve_current_user_fault;
using moss::abi::bridge::terminate_current_user_process;

// Forward declarations for static helpers used by both kernel and user handlers
[[noreturn]] static void kill_user_process(const char *reason, unsigned long long far_addr,
                                           unsigned long long elr) noexcept;
using FaultAccess = moss::kernel::mm::UserFaultAccess;
static bool try_cow_fault(moss::kernel::u64 far_addr) noexcept {
  return resolve_current_user_fault(far_addr, static_cast<unsigned int>(FaultAccess::Write), true) != 0;
}
static bool try_demand_page(moss::kernel::u64 far_addr, FaultAccess access) noexcept {
  return resolve_current_user_fault(far_addr, static_cast<unsigned int>(access), false) != 0;
}

static bool fixup_user_access(void *raw_frame, moss::kernel::u64 address) noexcept {
  // Restrict recovery to a registered copy PC and a user fault address. A
  // kernel-buffer fault must remain visible rather than becoming a short copy.
  return raw_frame && moss::kernel::mm::PageTableManager::is_user_range(address, 1) &&
         moss::abi::uaccess::fixup(*static_cast<moss::abi::TrapFrame *>(raw_frame));
}

// ============================================================================
// Unhandled exception handler — print diagnostics, then return to asm (halt)
// ============================================================================
extern "C" void unhandled_exception_handler(unsigned long long esr, unsigned long long far_addr, unsigned long long elr,
                                            unsigned long long saved_x30, unsigned long long frame_sp) noexcept {
  namespace log = moss::kernel::logging;
  using moss::u64;

  u64 ec = (esr >> 26) & 0x3F;
  u64 iss = esr & 0x1FFFFFF;

  log::klog::panic("=== UNHANDLED EXCEPTION ===");
  log::klog::panic("EC:  {:#x} ({})", ec, moss::kernel::mm::ec_to_string(ec));
  log::klog::panic("ISS: {:#x}", iss);
  log::klog::panic("ESR: {:#x}", esr);
  log::klog::panic("FAR: {:#x}", far_addr);
  log::klog::panic("ELR: {:#x}", elr);
  log::klog::panic("saved x30 (LR at fault): {:#x}", saved_x30);

#if defined(MOSS_ARCH_ARM64)
  // Diagnostic: dump critical system registers
  u64 sp_val = 0, ttbr0 = 0, ttbr1 = 0, sctlr = 0, spsr = 0;
  asm volatile("mov %0, sp" : "=r"(sp_val));
  asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
  asm volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
  asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
  asm volatile("mrs %0, spsr_el1" : "=r"(spsr));
  log::klog::panic("SP:  {:#x}", sp_val);
  log::klog::panic("TTBR0: {:#x}  TTBR1: {:#x}", ttbr0, ttbr1);
  log::klog::panic("SCTLR: {:#x}  SPSR: {:#x}", sctlr, spsr);

  // Dump selected registers from the exception frame.
  // Frame layout: x0..x30 at [frame_sp + 0..30*8], ELR/SPSR at [31*8], SP_EL0 at [33*8]
  if (frame_sp != 0) {
    const auto *frame = reinterpret_cast<const u64 *>(frame_sp);
    log::klog::panic("--- Exception Frame Dump ---");
    log::klog::panic("frame x0={:#x}  x1={:#x}", frame[0], frame[1]);
    log::klog::panic("frame x8={:#x}  x9={:#x}", frame[8], frame[9]);
    log::klog::panic("frame x19={:#x} x20={:#x} x21={:#x}", frame[19], frame[20], frame[21]);
    log::klog::panic("frame x22={:#x} x23={:#x} x24={:#x}", frame[22], frame[23], frame[24]);
    log::klog::panic("frame x28={:#x} x29={:#x} x30={:#x}", frame[28], frame[29], frame[30]);
    // ELR/SPSR saved at slots 31/32
    log::klog::panic("frame saved_ELR={:#x} saved_SPSR={:#x}", frame[31], frame[32]);
  }
#else
  (void)saved_x30;
  (void)frame_sp;
#endif
  // Returns to asm which executes `b halt`
}

// ============================================================================
// Kernel page fault handler — same-EL faults (EC=0x25 Data, EC=0x21 Instr)
//
// Handles two scenarios:
// 1. Kernel code accessing user pointer (e.g. syscall writing to user buffer):
//    the fault address is in user VA range — resolve via demand paging / COW.
// 2. Genuine kernel fault (bug): address is in kernel VA range — panic.
// ============================================================================
extern "C" void kernel_page_fault_handler(unsigned long long esr, unsigned long long far_addr, unsigned long long elr,
                                          void *raw_frame) noexcept {
  namespace log = moss::kernel::logging;
  namespace mm = moss::kernel::mm;
  using moss::u64;

  u64 ec = (esr >> 26) & 0x3F;
  u64 dfsc = esr & 0x3F;
  [[maybe_unused]] bool is_write = ((esr >> 6) & 1) != 0;
  auto access = FaultAccess::Read;
  if (ec == 0x21) { // ARM64 ESR EC: instruction abort from the current EL.
    access = FaultAccess::Execute;
  } else if (is_write) {
    access = FaultAccess::Write;
  }

  // Translation faults: DFSC 0x04-0x07 (L0-L3 translation miss)
  bool is_translation_fault = (dfsc >= 0x04 && dfsc <= 0x07);

  // Permission faults: DFSC 0x0C-0x0F (page exists, permission denied)
  bool is_permission_fault = (dfsc >= 0x0C && dfsc <= 0x0F);

  // Detect user-space address: kernel code (e.g. syscall handler) accessing
  // user pointer triggers a same-EL fault, but the address belongs to the
  // current process's user address space and should be handled like a user fault.
  bool is_user_address = mm::PageTableManager::is_user_range(far_addr, 1);

  if (is_translation_fault) {
    // Low-level raw user accesses can fault in kernel mode. Shared process
    // uaccess resolves pages before copying through physical aliases instead;
    // raw accesses still use the native demand/COW and exact-PC fixup path.
    if (is_user_address) {
      if (try_demand_page(far_addr, access)) {
        return; // Demand page resolved — eret retries instruction
      }
      if (ec == 0x25 && fixup_user_access(raw_frame, far_addr)) {
        return;
      }
      // No VMA found — this is a bad user pointer passed to syscall.
      // Terminate the faulting user process instead of panicking the kernel.
      kill_user_process("kernel access to unmapped user addr", far_addr, elr);
    }

    // Kernel address fault — always log for diagnostics.
    log::klog::warn("kernel page fault: addr={:#x} pc={:#x} write={} dfsc={:#x} ({})", far_addr, elr, is_write, dfsc,
                    mm::dfsc_to_string(dfsc));

    // Kernel address: check if mapping exists (stale TLB)
    auto info = mm::PageTableManager::query_page(far_addr);
    if (info.mapped) {
      mm::PageTableManager::invalidate_tlb_addr(far_addr);
      log::klog::warn("page fault: addr={:#x} was mapped (level={}), TLB invalidated — retrying", far_addr,
                      static_cast<moss::u32>(info.level));
      return; // eret will retry the faulting instruction
    }

    // Genuine kernel fault — no mapping in kernel page tables.
    log::klog::panic("KERNEL PAGE FAULT: no mapping for addr={:#x}", far_addr);
    log::klog::panic("  EC:   {:#x} ({})", ec, mm::ec_to_string(ec));
    log::klog::panic("  DFSC: {:#x} ({})", dfsc, mm::dfsc_to_string(dfsc));
    log::klog::panic("  ELR:  {:#x}", elr);
    while (true) {
#if defined(MOSS_ARCH_ARM64)
      asm volatile("wfi");
#endif
    }
  }

  if (is_permission_fault) {
    // User address with permission fault: try COW resolution
    if (is_user_address && access == FaultAccess::Write) {
      if (try_cow_fault(far_addr)) {
        return; // COW resolved — eret retries instruction
      }
    }
    if (ec == 0x25 && fixup_user_access(raw_frame, far_addr)) {
      return;
    }
    log::klog::panic("KERNEL PERMISSION FAULT: addr={:#x} pc={:#x} write={:#b}", far_addr, elr, is_write);
    log::klog::panic("  DFSC: {:#x} ({})", dfsc, mm::dfsc_to_string(dfsc));
    while (true) {
#if defined(MOSS_ARCH_ARM64)
      asm volatile("wfi");
#endif
    }
  }

  // Other DFSC values (alignment, external abort, etc.)
  if (ec == 0x25 && fixup_user_access(raw_frame, far_addr)) {
    return;
  }
  log::klog::panic("KERNEL FAULT: unhandled dfsc={:#x} ({}) addr={:#x} pc={:#x}", dfsc, mm::dfsc_to_string(dfsc),
                   far_addr, elr);
  while (true) {
#if defined(MOSS_ARCH_ARM64)
    asm volatile("wfi");
#endif
  }
}

// ============================================================================
// User-mode page fault handler
//
// Called from lower_el_sync_dispatch when EC=0x24 (Data Abort, lower EL) or
// EC=0x20 (Instruction Abort, lower EL).
//
// Resolve demand paging, COW and eligible stack growth first. An unresolved
// user fault terminates that process and returns control to the scheduler.
// ============================================================================
// Helper: kill the current user process and hand control to the scheduler
[[noreturn]] static void kill_user_process(const char *reason, unsigned long long far_addr,
                                           unsigned long long elr) noexcept {
  namespace log = moss::kernel::logging;
  log::klog::error("USER FAULT: {} addr={:#x} pc={:#x}", reason, far_addr, elr);
  log::klog::error("  Terminating user process (SIGSEGV equivalent)");

  // Bridge to kernel module: terminates process + restores kernel TTBR0 +
  // calls schedule_after_exit().  Never returns.
  terminate_current_user_process(kSegmentationFaultSignal);
}

// Attempt COW (Copy-on-Write) resolution for a write permission fault.
// Returns true if the fault was a COW page and has been resolved.
bool moss::kernel::mm::resolve_user_cow_fault(const UserFaultContext &context, VirtAddr far_addr) noexcept {
  // The caller's address-space protocol must exclude concurrent clone/unmap;
  // an atomic refcount read does not stabilize the mutable PTE or its frame.
  namespace mm = moss::kernel::mm;
  namespace log = moss::kernel::logging;
  using moss::kernel::phys_to_virt;
  using moss::kernel::PhysAddr;
  using moss::kernel::u32;
  using moss::kernel::u64;
  using moss::kernel::u8;
  using moss::kernel::usize;
  using moss::kernel::VirtAddr;

  constexpr usize PG_SIZE = 4096; // Same 4 KiB granule as mm::PAGE_SIZE and order-0 frames.
  if (!mm::PageTableManager::is_user_range(far_addr, 1)) {
    return false;
  }
  // A COW marker does not grant write authority. The caller's current VMA
  // snapshot must authorize it as well; shared mmap remains unsupported.
  if ((context.flags & static_cast<u32>(UserFaultAccess::Write)) == 0) {
    return false;
  }
  VirtAddr fault_page = far_addr & ~(static_cast<u64>(PG_SIZE) - 1);
  PhysAddr pgd_phys = context.root;
  if (pgd_phys == 0) {
    return false;
  }

  // Walk page tables to get a mutable pointer to the PTE
  auto *pte = mm::PageTableManager::get_user_pte(pgd_phys, fault_page);
  if (!pte || !pte->is_valid() || (pte->raw & mm::page_attr::USER) == 0) {
    return false;
  }
  if (pte->is_writable() && !pte->is_cow()) {
    // Another fault may have committed before this transaction acquired the
    // lock. Do not allocate/copy/drop its frame again or kill the waiting task.
    // No permissions are added here; the VMA and existing leaf already allow
    // the write. Retry after invalidating the stale faulting translation.
    mm::PageTableManager::invalidate_tlb_addr(fault_page);
    return true;
  }

  // Must be a COW-marked page
  if (!pte->is_cow() || pte->is_writable()) {
    return false;
  }

  PhysAddr old_pa = pte->get_phys_addr();
  u32 refcount = mm::PageFrameAllocator::page_ref_get(old_pa);
  if (refcount == 0) {
    return false; // Never manufacture ownership for an unreferenced frame.
  }
  moss_validation_cow_snapshot(pgd_phys, fault_page);

  if (refcount > 1) {
    // Shared page: allocate new page, copy content, remap writable
    auto new_page = mm::page_alloc::alloc_kernel_pages(0);
    if (!new_page) {
      // Preserve the shared frame, its reference and the read-only COW PTE on
      // OOM. In particular, do not promote permissions before allocation: a
      // copy fixup must leave the same mapping safe to retry after recovery.
      return false; // The entry decides between user termination and copy fixup.
    }
    PhysAddr new_pa = *new_page;

    // Copy 4KB from old page to new page
    const auto *src = reinterpret_cast<const u8 *>(phys_to_virt(old_pa));
    auto *dst = reinterpret_cast<u8 *>(phys_to_virt(new_pa));
    for (usize i = 0; i < PG_SIZE; i++) {
      dst[i] = src[i];
    }

    // Reuse the architecture's PTE encoder (RISC-V 64 stores PPN, not PA).
    // Keep permissions/cache attributes unchanged except COW and writability.
    auto replacement = *pte;
    replacement.clear_cow();
    replacement.make_writable();
    pte->set_page(new_pa, replacement.raw & ~::moss::kernel::hal::mmu::PTE_ADDR_MASK);
    mm::PageTableManager::invalidate_tlb_addr(fault_page);
    // The mapping owns the new frame now; release the old frame only after
    // replacing this mapping and invalidating its translation.
    if (mm::PageFrameAllocator::page_ref_dec(old_pa) == 0) {
      (void)mm::free_pages(old_pa, 0);
    }
    return true;
  }
  // Last reference: just clear COW flag and make writable
  pte->clear_cow();
  pte->make_writable();

  mm::PageTableManager::invalidate_tlb_addr(fault_page);

  return true;
}

// Attempt demand paging for a user translation fault.
// Returns true if the fault was resolved (caller should return to eret).
bool moss::kernel::mm::resolve_user_demand_fault(const UserFaultContext &context, VirtAddr far_addr,
                                                 UserFaultAccess access) noexcept {
  namespace mm = moss::kernel::mm;
  using moss::kernel::phys_to_virt;
  using moss::kernel::PhysAddr;
  using moss::kernel::u32;
  using moss::kernel::u64;
  using moss::kernel::u8;
  using moss::kernel::usize;
  using moss::kernel::VirtAddr;

  namespace log = moss::kernel::logging;

  const PhysAddr pgd_phys = context.root;
  if (!pgd_phys || !mm::PageTableManager::is_user_range(far_addr, 1)) {
    return false;
  }
  const u32 vma_flags = context.flags;
  const u8 *backing_data = context.backing;
  const usize backing_offset = context.backing_offset;
  const usize backing_size = context.backing_size;
  const VirtAddr vma_start = context.start;

  // Bridge permission encoding matches process::vma_flags (READ/WRITE/EXEC
  // in bits 0/1/2); local copies avoid a process -> mm -> process import cycle.
  constexpr u32 VMA_READ = 1U << 0;
  constexpr u32 VMA_WRITE = 1U << 1;
  constexpr u32 VMA_EXEC = 1U << 2;

  u32 required = VMA_READ;
  if (access == FaultAccess::Execute) {
    required = VMA_EXEC;
  } else if (access == FaultAccess::Write) {
    required = VMA_WRITE;
  }
  if ((vma_flags & required) == 0) {
    return false; // Deny READ/WRITE/EXEC before allocating, including PROT_NONE.
  }

  const auto *existing = mm::PageTableManager::get_user_pte(pgd_phys, far_addr);
  if (existing && existing->is_valid()) {
    // A concurrent demand fault may already have supplied the page. Reuse it
    // only if its actual permissions admit this access; never refill resident
    // contents or turn a protection fault (including COW) into a fresh page.
    bool allowed = (existing->raw & mm::page_attr::USER) != 0;
    if (access == UserFaultAccess::Write) {
      allowed = allowed && existing->is_writable() && !existing->is_cow();
    } else if (access == UserFaultAccess::Execute) {
#if defined(MOSS_ARCH_RISCV64)
      allowed = allowed && (existing->raw & mm::page_attr::EXECUTE) != 0;
#else
      allowed = allowed && (existing->raw & mm::page_attr::XN) == 0;
#endif
    }
#if defined(MOSS_ARCH_RISCV64)
    if (access == UserFaultAccess::Read) {
      allowed = allowed && (existing->raw & mm::page_attr::READ) != 0;
    }
#endif
    if (allowed) {
      mm::PageTableManager::invalidate_tlb_addr(far_addr);
    }
    return allowed;
  }

  moss_validation_demand_snapshot(pgd_phys, far_addr & ~(static_cast<VirtAddr>(PAGE_SIZE) - 1));
  // Allocate a physical page
  constexpr usize PG_SIZE = 4096; // Matches mm::PAGE_SIZE; fills exactly one order-0 frame.
  auto page_result = mm::page_alloc::alloc_kernel_pages(0);
  if (!page_result) {
    return false;
  }
  PhysAddr page_pa = *page_result;
  auto *page_va = reinterpret_cast<u8 *>(phys_to_virt(page_pa));

  // Fill page from backing data or zero
  VirtAddr fault_page = far_addr & ~(static_cast<u64>(PG_SIZE) - 1);
  u64 page_offset = fault_page - vma_start;

  if (backing_data != nullptr && page_offset < backing_size) {
    u64 copy_size = backing_size - page_offset;
    if (copy_size > PG_SIZE) {
      copy_size = PG_SIZE;
    }
    for (u64 i = 0; i < copy_size; i++) {
      page_va[i] = backing_data[backing_offset + page_offset + i];
    }
    for (u64 i = copy_size; i < PG_SIZE; i++) {
      page_va[i] = 0;
    }
  } else {
    // Fresh physical pages are page-aligned. Clear whole words so the Debug
    // demand-zero path does not execute a byte loop for every slab page.
    auto *words = reinterpret_cast<u64 *>(page_va);
    for (usize i = 0; i < PG_SIZE / sizeof(u64); i++) {
      words[i] = 0;
    }
  }

  // Build user PTE permissions from VMA flags using portable page_attr constants.
  // Base: valid, accessed, user-accessible, normal memory.
  namespace pa = ::moss::kernel::hal::mmu::page_attr;
  u64 perms = pa::VALID | pa::AF | pa::USER | pa::ATTR_NORMAL;

#if defined(MOSS_ARCH_ARM64)
  // ARM64-specific: non-global (per-process ASID), inner-shareable, PXN
  perms |= pa::NG | pa::PXN | (3ULL << 8); // SH=Inner Shareable (bits [9:8]=0b11)
  if (!(vma_flags & VMA_WRITE)) {
    perms |= pa::READONLY; // AP[2]=1 → read-only
  }
  if (!(vma_flags & VMA_EXEC)) {
    perms |= pa::XN; // UXN → no user execute
  }
#elif defined(MOSS_ARCH_X64)
  if (vma_flags & VMA_WRITE) {
    perms |= pa::WRITABLE;
  }
  if (!(vma_flags & VMA_EXEC)) {
    perms |= pa::XN; // NX bit
  }
#elif defined(MOSS_ARCH_RISCV64)
  perms |= pa::READ; // Always readable
  if (vma_flags & VMA_WRITE) {
    // RISC-V 64: Dirty (D) bit must be pre-set for writable pages.
    // Without D, the first store triggers a Store Page Fault (scause=15)
    // even though the page is mapped, creating an infinite fault loop.
    perms |= pa::WRITE | pa::DIRTY;
  }
  if (vma_flags & VMA_EXEC) {
    perms |= pa::EXECUTE;
  }
#endif

  auto map_result = mm::PageTableManager::map_user_page(pgd_phys, fault_page, page_pa, perms);
  if (!map_result) {
    (void)mm::free_pages(page_pa, 0); // The failed mapping did not acquire this frame.
    return false;
  }

  // map_user_page publishes the entry and invalidates this address itself.
  moss_validation_demand_committed(pgd_phys, fault_page, page_pa);
  return true;
}

extern "C" void user_page_fault_handler(unsigned long long esr, unsigned long long far_addr,
                                        unsigned long long elr) noexcept {
  namespace log = moss::kernel::logging;
  namespace mm = moss::kernel::mm;
  using namespace moss::kernel;

  u64 dfsc = esr & 0x3F;
  u64 ec = (esr >> 26) & 0x3F;
  bool is_write = ((esr >> 6) & 1) != 0;
  auto access = FaultAccess::Read;
  if (ec == 0x20) { // ARM64 ESR EC: instruction abort from a lower EL.
    access = FaultAccess::Execute;
  } else if (is_write) {
    access = FaultAccess::Write;
  }

  // Permission faults (DFSC 0x0C-0x0F): try COW resolution first
  bool is_permission_fault = (dfsc >= 0x0C && dfsc <= 0x0F);
  if (is_permission_fault && access == FaultAccess::Write) {
    if (try_cow_fault(far_addr)) {
      return; // COW resolved — eret retries instruction
    }
  }

  // Translation faults (DFSC 0x04-0x07): attempt demand paging
  bool is_translation_fault = (dfsc >= 0x04 && dfsc <= 0x07);

  if (is_translation_fault) {
    if (try_demand_page(far_addr, access)) {
      return; // Fault resolved — eret retries instruction
    }
  }

  // No VMA, not a translation fault, or bridge not registered — fatal
  log::klog::error("USER PAGE FAULT: addr={:#x} pc={:#x} write={} ec={:#x} ({})", far_addr, elr, is_write, ec,
                   mm::ec_to_string(ec));
  log::klog::error("  DFSC: {:#x} ({})", dfsc, mm::dfsc_to_string(dfsc));
  kill_user_process("no VMA for address", far_addr, elr);
}

// ============================================================================
// Unhandled user-mode exception handler
//
// Called for any Lower EL synchronous exception that is NOT an SVC, Data
// Abort, or Instruction Abort (e.g., SP alignment fault, illegal execution
// state, FP trap, etc.).
// ============================================================================
extern "C" [[noreturn]] void unhandled_user_exception_handler(unsigned long long esr, unsigned long long far_addr,
                                                              unsigned long long elr) noexcept {
  namespace log = moss::kernel::logging;
  using moss::u64;

  u64 ec = (esr >> 26) & 0x3F;
  u64 iss = esr & 0x1FFFFFF;

  log::klog::error("=== UNHANDLED USER EXCEPTION ===");
  log::klog::error("EC:  {:#x} ({})", ec, moss::kernel::mm::ec_to_string(ec));
  log::klog::error("ISS: {:#x}", iss);
  log::klog::error("ESR: {:#x}", esr);
  log::klog::error("FAR: {:#x}", far_addr);
  log::klog::error("ELR: {:#x}", elr);
  log::klog::error("Terminating user process");

  // Terminate the faulting process and let the scheduler pick the next task.
  terminate_current_user_process(kSegmentationFaultSignal);
}

// ============================================================================
// RISC-V 64 page fault handler
//
// Called from riscv64_syscall.S for scause 12 (Instruction Page Fault),
// 13 (Load Page Fault), and 15 (Store/AMO Page Fault).
//
// RISC-V 64 encodes the fault type directly in scause (unlike ARM64 which
// uses ESR bit-fields).  The faulting address is in stval (≡ FAR_EL1).
// ============================================================================
#if defined(MOSS_ARCH_RISCV64) || defined(__riscv) || defined(__riscv__)
extern "C" void riscv64_page_fault_handler(unsigned long long scause, unsigned long long stval, unsigned long long sepc,
                                           void *raw_frame) noexcept {
  namespace log = moss::kernel::logging;
  using namespace moss::kernel;

  bool is_write = scause == 15 || scause == 7; // Store/AMO page or access fault
  const auto access = scause == 12 ? FaultAccess::Execute : is_write ? FaultAccess::Write : FaultAccess::Read;
  // scause 12 = Instruction page fault, 13 = Load page fault

  // 1. Resolve a resident, COW-marked write fault without refilling the page.
  if (scause == 15 && try_cow_fault(stval)) {
    return;
  }

  // 2. Try demand paging only for absent pages (U-mode and S-mode accesses).
  //    S-mode faults remain possible for low-level raw user accesses. Shared
  //    process uaccess resolves pages before its physical-alias copy instead.
  if (scause >= 12 && try_demand_page(stval, access)) {
    return; // Fault resolved — sret retries instruction
  }

  if (access != FaultAccess::Execute && fixup_user_access(raw_frame, stval))
    return;

  // 3. Determine if this is a kernel-mode or user-mode fault.
  //    S-mode faults that reach here are unrecoverable kernel bugs.
  //    U-mode faults terminate the user process (SIGSEGV equivalent).
  bool from_smode = !static_cast<moss::abi::TrapFrame *>(raw_frame)->from_user();

  if (from_smode) {
    log::klog::error("KERNEL PAGE FAULT: scause={:#x} addr={:#x} pc={:#x}", scause, stval, sepc);
    while (true) {
      asm volatile("wfi");
    }
  }

  log::klog::error("RISC-V 64 PAGE FAULT: scause={:#x} addr={:#x} pc={:#x}", scause, stval, sepc);
  kill_user_process("RISC-V 64 page fault", stval, sepc);
}
#endif // MOSS_ARCH_RISCV64

// ============================================================================
// x64 Page Fault Handler (#PF, vector 14)
//
// Called from x64_interrupt_handler() in boot_impl.cpp.
// Error code bits:  0=Present  1=Write  2=User  3=ReservedBit  4=InstructionFetch
// CR2 holds the faulting virtual address.
// ============================================================================
#if defined(MOSS_ARCH_X64) || defined(__x86_64__) || defined(__x86_64)
extern "C" void x64_page_fault_handler(unsigned long long error_code, unsigned long long cr2, unsigned long long rip,
                                       void *raw_frame) noexcept {
  namespace log = moss::kernel::logging;
  using namespace moss::kernel;

  bool is_present = (error_code & (1ULL << 0)) != 0;
  bool is_write = (error_code & (1ULL << 1)) != 0;
  const bool is_execute = (error_code & (1ULL << 4)) != 0;
  const bool reserved_bit_fault = (error_code & (1ULL << 3)) != 0;
  const auto access = is_execute ? FaultAccess::Execute : is_write ? FaultAccess::Write : FaultAccess::Read;

  // 1. Not-present fault (translation fault equivalent) — demand paging
  if (!is_present && !reserved_bit_fault) {
    if (try_demand_page(cr2, access)) {
      return; // Demand page resolved — iretq retries instruction
    }
  }

  // 2. Protection violation on write — COW resolution
  if (is_present && access == FaultAccess::Write && !reserved_bit_fault) {
    if (try_cow_fault(cr2)) {
      return; // COW resolved — iretq retries instruction
    }
  }

  if (!is_execute && !reserved_bit_fault && fixup_user_access(raw_frame, cr2))
    return;

  // 3. Unresolvable fault
  bool is_user_mode = (error_code & (1ULL << 2)) != 0;

  if (is_user_mode) {
    log::klog::error("x64 USER PAGE FAULT: addr={:#x} pc={:#x} err={:#x}", cr2, rip, error_code);
    kill_user_process("page fault", cr2, rip);
  }

  log::klog::panic("KERNEL PAGE FAULT: addr={:#x} pc={:#x} err={:#x}", cr2, rip, error_code);
  while (true) {
    asm volatile("hlt");
  }
}
#endif // MOSS_ARCH_X64
