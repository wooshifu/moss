// MOSS Page Fault Handler
//
// Handles kernel-mode page faults (Data Abort EC=0x25, Instruction Abort EC=0x21)
// and prints diagnostic information for all other unhandled exceptions.
//
// Called from the ARM64 exception vector table in start_arm64.S.
// x86_64 and RISC-V stubs are provided for link compatibility.

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

extern "C" void riscv_page_fault_handler(unsigned long long scause, unsigned long long stval,
                                         unsigned long long sepc) noexcept;

module moss.mm;

import moss.abi;

namespace moss::kernel::mm {

// ============================================================================
// EC (Exception Class) to human-readable string
// ============================================================================
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
using moss::abi::bridge::demand_page_lookup;
using moss::abi::bridge::get_current_pgd_phys;
using moss::abi::bridge::terminate_current_user_process;

// Forward declarations for static helpers used by both kernel and user handlers
[[noreturn]] static void kill_user_process(const char *reason, unsigned long long far_addr,
                                           unsigned long long elr) noexcept;
static bool try_cow_fault(moss::kernel::u64 far_addr, unsigned long long elr) noexcept;
static bool try_demand_page(moss::kernel::u64 far_addr, bool is_write, unsigned long long elr) noexcept;

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
extern "C" void kernel_page_fault_handler(unsigned long long esr, unsigned long long far_addr,
                                          unsigned long long elr) noexcept {
  namespace log = moss::kernel::logging;
  namespace mm = moss::kernel::mm;
  using moss::u64;

  u64 ec = (esr >> 26) & 0x3F;
  u64 dfsc = esr & 0x3F;
  [[maybe_unused]] bool is_write = ((esr >> 6) & 1) != 0;

  // Translation faults: DFSC 0x04-0x07 (L0-L3 translation miss)
  bool is_translation_fault = (dfsc >= 0x04 && dfsc <= 0x07);

  // Permission faults: DFSC 0x0C-0x0F (page exists, permission denied)
  bool is_permission_fault = (dfsc >= 0x0C && dfsc <= 0x0F);

  // Detect user-space address: kernel code (e.g. syscall handler) accessing
  // user pointer triggers a same-EL fault, but the address belongs to the
  // current process's user address space and should be handled like a user fault.
  bool is_user_address = !moss::kernel::is_kernel_addr(far_addr);

  if (is_translation_fault) {
    // For user addresses accessed from kernel mode (e.g. sys_topinfo writing
    // to a user-provided TopInfo pointer on the demand-zero stack), attempt
    // demand paging exactly like user_page_fault_handler would.
    if (is_user_address) {
      if (try_demand_page(far_addr, is_write, elr)) {
        return; // Demand page resolved — eret retries instruction
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
    if (is_user_address && is_write) {
      if (try_cow_fault(far_addr, elr)) {
        return; // COW resolved — eret retries instruction
      }
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
// MVP behaviour: log diagnostics and terminate the faulting user process.
// The kernel does NOT panic — it simply kills the offending process and
// lets the scheduler pick the next runnable task.
//
// Future: demand paging, COW, stack growth, mmap fault-in.
// ============================================================================
// Helper: kill the current user process and hand control to the scheduler
[[noreturn]] static void kill_user_process(const char *reason, unsigned long long far_addr,
                                           unsigned long long elr) noexcept {
  namespace log = moss::kernel::logging;
  log::klog::error("USER FAULT: {} addr={:#x} pc={:#x}", reason, far_addr, elr);
  log::klog::error("  Terminating user process (SIGSEGV equivalent)");

  // Bridge to kernel module: terminates process + restores kernel TTBR0 +
  // calls schedule_after_exit().  Never returns.
  terminate_current_user_process(-11); // -11 ≈ SIGSEGV
}

// Attempt COW (Copy-on-Write) resolution for a write permission fault.
// Returns true if the fault was a COW page and has been resolved.
static bool try_cow_fault(moss::kernel::u64 far_addr, unsigned long long elr) noexcept {
  namespace mm = moss::kernel::mm;
  namespace log = moss::kernel::logging;
  using moss::kernel::phys_to_virt;
  using moss::kernel::PhysAddr;
  using moss::kernel::u32;
  using moss::kernel::u64;
  using moss::kernel::u8;
  using moss::kernel::usize;
  using moss::kernel::VirtAddr;

  constexpr usize PG_SIZE = 4096;
  VirtAddr fault_page = far_addr & ~(static_cast<u64>(PG_SIZE) - 1);
  PhysAddr pgd_phys = get_current_pgd_phys();
  if (pgd_phys == 0) {
    return false;
  }

  // Walk page tables to get a mutable pointer to the PTE
  auto *pte = mm::PageTableManager::get_user_pte(pgd_phys, fault_page);
  if (!pte || !pte->is_valid()) {
    return false;
  }

  // Must be a COW-marked page
  if (!pte->is_cow()) {
    return false;
  }

  PhysAddr old_pa = pte->get_phys_addr();
  u32 refcount = mm::PageFrameAllocator::page_ref_get(old_pa);

  if (refcount > 1) {
    // Shared page: allocate new page, copy content, remap writable
    auto new_page = mm::page_alloc::alloc_kernel_pages(0);
    if (!new_page) {
      kill_user_process("COW: out of memory", far_addr, elr);
    }
    PhysAddr new_pa = *new_page;

    // Copy 4KB from old page to new page
    const auto *src = reinterpret_cast<const u8 *>(phys_to_virt(old_pa));
    auto *dst = reinterpret_cast<u8 *>(phys_to_virt(new_pa));
    for (usize i = 0; i < PG_SIZE; i++) {
      dst[i] = src[i];
    }

    // Decrement old page refcount
    mm::PageFrameAllocator::page_ref_dec(old_pa);
    // New page has refcount=1 (set by allocator)

    // Update PTE: new physical page, clear COW, make writable
    u64 attrs = pte->raw & ~::moss::kernel::hal::mmu::PTE_ADDR_MASK;
    attrs &= ~mm::page_attr::SW_COW;
#if defined(MOSS_ARCH_ARM64)
    attrs &= ~mm::page_attr::READONLY;
#elif defined(MOSS_ARCH_X86_64)
    attrs |= mm::page_attr::WRITABLE;
#elif defined(MOSS_ARCH_RISCV)
    attrs |= mm::page_attr::WRITE;
#endif
    pte->raw = (new_pa & ::moss::kernel::hal::mmu::PTE_ADDR_MASK) | attrs;
  } else {
    // Last reference: just clear COW flag and make writable
    pte->clear_cow();
    pte->make_writable();
  }

  mm::PageTableManager::invalidate_tlb_addr(fault_page);

  return true;
}

// Attempt demand paging for a user translation fault.
// Returns true if the fault was resolved (caller should return to eret).
static bool try_demand_page(moss::kernel::u64 far_addr, bool is_write, unsigned long long elr) noexcept {
  namespace mm = moss::kernel::mm;
  using moss::kernel::phys_to_virt;
  using moss::kernel::PhysAddr;
  using moss::kernel::u32;
  using moss::kernel::u64;
  using moss::kernel::u8;
  using moss::kernel::usize;
  using moss::kernel::VirtAddr;

  namespace log = moss::kernel::logging;

  u32 vma_flags = 0;
  const u8 *backing_data = nullptr;
  u64 backing_offset = 0;
  u64 backing_size = 0;
  u64 vma_start = 0;

  int found = demand_page_lookup(far_addr, &vma_flags, &backing_data, &backing_offset, &backing_size, &vma_start);
  if (!found) {
    return false;
  }

  // vma_flags bit definitions (must match process::vma_flags)
  constexpr u32 VMA_WRITE = 1U << 1;
  constexpr u32 VMA_EXEC = 1U << 2;

  // Permission check: write to read-only VMA
  if (is_write && !(vma_flags & VMA_WRITE)) {
    kill_user_process("write to read-only VMA", far_addr, elr);
  }

  // Allocate a physical page
  constexpr usize PG_SIZE = 4096;
  auto page_result = mm::page_alloc::alloc_kernel_pages(0);
  if (!page_result) {
    kill_user_process("out of memory", far_addr, elr);
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
    for (usize i = 0; i < PG_SIZE; i++) {
      page_va[i] = 0;
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
#elif defined(MOSS_ARCH_X86_64)
  if (vma_flags & VMA_WRITE) {
    perms |= pa::WRITABLE;
  }
  if (!(vma_flags & VMA_EXEC)) {
    perms |= pa::XN; // NX bit
  }
#elif defined(MOSS_ARCH_RISCV)
  perms |= pa::READ; // Always readable
  if (vma_flags & VMA_WRITE) {
    // RISC-V: Dirty (D) bit must be pre-set for writable pages.
    // Without D, the first store triggers a Store Page Fault (scause=15)
    // even though the page is mapped, creating an infinite fault loop.
    perms |= pa::WRITE | pa::DIRTY;
  }
  if (vma_flags & VMA_EXEC) {
    perms |= pa::EXECUTE;
  }
#endif

  PhysAddr pgd_phys = get_current_pgd_phys();
  auto map_result = mm::PageTableManager::map_user_page(pgd_phys, fault_page, page_pa, perms);
  if (!map_result) {
    kill_user_process("map_user_page failed", far_addr, elr);
  }

  mm::PageTableManager::invalidate_tlb_addr(fault_page);
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

  // Permission faults (DFSC 0x0C-0x0F): try COW resolution first
  bool is_permission_fault = (dfsc >= 0x0C && dfsc <= 0x0F);
  if (is_permission_fault && is_write) {
    if (try_cow_fault(far_addr, elr)) {
      return; // COW resolved — eret retries instruction
    }
  }

  // Translation faults (DFSC 0x04-0x07): attempt demand paging
  bool is_translation_fault = (dfsc >= 0x04 && dfsc <= 0x07);

  if (is_translation_fault) {
    if (try_demand_page(far_addr, is_write, elr)) {
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
  terminate_current_user_process(-11); // -11 ≈ SIGSEGV
}

// ============================================================================
// RISC-V page fault handler
//
// Called from riscv_syscall.S for scause 12 (Instruction Page Fault),
// 13 (Load Page Fault), and 15 (Store/AMO Page Fault).
//
// RISC-V encodes the fault type directly in scause (unlike ARM64 which
// uses ESR bit-fields).  The faulting address is in stval (≡ FAR_EL1).
// ============================================================================
#if defined(MOSS_ARCH_RISCV) || defined(__riscv) || defined(__riscv__)
extern "C" void riscv_page_fault_handler(unsigned long long scause, unsigned long long stval,
                                         unsigned long long sepc) noexcept {
  namespace log = moss::kernel::logging;
  using namespace moss::kernel;

  bool is_write = (scause == 15); // Store/AMO page fault
  // scause 12 = Instruction page fault, 13 = Load page fault

  // 1. Try demand paging (handles both U-mode and S-mode faults).
  //    S-mode faults occur when kernel code (e.g. console_write in sys_write)
  //    accesses a user buffer whose page hasn't been demand-faulted yet.
  if (try_demand_page(stval, is_write, sepc)) {
    return; // Fault resolved — sret retries instruction
  }

  // 2. Try COW resolution for write faults to read-only mapped pages
  if (is_write && try_cow_fault(stval, sepc)) {
    return; // COW resolved — sret retries instruction
  }

  // 3. Determine if this is a kernel-mode or user-mode fault.
  //    S-mode faults that reach here are unrecoverable kernel bugs.
  //    U-mode faults terminate the user process (SIGSEGV equivalent).
  unsigned long long sstatus_val;
  asm volatile("csrr %0, sstatus" : "=r"(sstatus_val));
  bool from_smode = (sstatus_val & (1ULL << 8)) != 0; // SPP bit

  if (from_smode) {
    log::klog::error("KERNEL PAGE FAULT: scause={:#x} addr={:#x} pc={:#x}", scause, stval, sepc);
    while (true) {
      asm volatile("wfi");
    }
  }

  log::klog::error("RISC-V PAGE FAULT: scause={:#x} addr={:#x} pc={:#x}", scause, stval, sepc);
  kill_user_process("RISC-V page fault", stval, sepc);
}
#endif // MOSS_ARCH_RISCV
