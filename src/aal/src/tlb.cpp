module moss.arch;

// Observe the real publisher lock in validation; production keeps the same
// acquisition and request path, with inert observers and no alternate mailbox.
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_tlb_contended() noexcept {}
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_tlb_publishing() noexcept {}
// Registration stages: before lock acquisition, membership published under
// that lock, then lock released with IRQs still masked. Observers cannot change
// membership or replace the actual invalidate/request path.
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_tlb_registration(unsigned /*stage*/) noexcept {}
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_tlb_targets(moss::u64 /*targets*/) noexcept {}

namespace moss::kernel::arch {
#if !defined(MOSS_ARCH_ARM64)
namespace {
// One mailbox serializes publishers without allocating in VM/IRQ context.
// Request fields stay immutable until every pending bit has been acknowledged.
// These constant-initialized objects are also valid during early MM bootstrap.
u32 publisher = 0;
u64 online = 0, pending = 0;
VirtAddr address = 0;
bool entire = false;
TlbNotifier notifier = nullptr;

void invalidate_local(VirtAddr addr, bool full) noexcept {
#if defined(MOSS_ARCH_X64)
  if (!full) {
    asm volatile("invlpg (%0)" ::"r"(addr) : "memory");
    return;
  }
  u64 cr3, cr4;
  asm volatile("mov %%cr3, %0; mov %%cr4, %1" : "=r"(cr3), "=r"(cr4));
  // CR4.PGE (bit 7) preserves global translations across CR3 reloads. Toggle
  // it off when enabled so full-tree reclamation also removes those entries.
  constexpr u64 pge = u64{1} << 7;
  if (cr4 & pge) {
    asm volatile("mov %0, %%cr4; mov %1, %%cr4" ::"r"(cr4 & ~pge), "r"(cr4) : "memory");
  } else {
    // Moss currently switches roots with PCID disabled, without no-flush CR3.
    asm volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
  }
#elif defined(MOSS_ARCH_RISCV64)
  if (full) {
    // A non-leaf PTE removal requires all walk levels, not one leaf VA.
    asm volatile("sfence.vma zero, zero" ::: "memory");
  } else {
    // rs2=x0 invalidates all ASIDs, including global mappings.
    asm volatile("sfence.vma %0, zero" ::"r"(addr) : "memory");
  }
#endif
}

void acquire_publisher() noexcept {
  if (__atomic_exchange_n(&publisher, 1U, __ATOMIC_ACQUIRE) != 0) {
    moss_validation_tlb_contended();
    // Another publisher may be waiting for this CPU. Never wait with IRQs
    // masked without servicing its request, including while holding a VM lock.
    do {
      cpu_yield();
    } while (__atomic_exchange_n(&publisher, 1U, __ATOMIC_ACQUIRE) != 0);
  }
}
} // namespace
#endif

void service_tlb_shootdown() noexcept {
#if !defined(MOSS_ARCH_ARM64)
  if (__atomic_load_n(&pending, __ATOMIC_ACQUIRE) == 0) {
    return;
  }
  const bool interrupts = interrupts_enabled();
  disable_interrupts();
  const auto cpu = get_current_cpu_id();
  // The mailbox uses one bit per logical CPU. Current platform capacity fits
  // in u64; an unregistered early-boot CPU must not shift by an invalid ID.
  if (cpu < sizeof(pending) * 8) {
    const u64 bit = u64{1} << cpu;
    if (__atomic_load_n(&pending, __ATOMIC_ACQUIRE) & bit) {
      invalidate_local(address, entire);
      // Complete accesses that used the old translation before acknowledging.
      // IRQ masking prevents reentrant consumption and a late clear of the
      // same bit after the publisher has moved on to a subsequent request.
      memory_barrier();
      __atomic_fetch_and(&pending, ~bit, __ATOMIC_RELEASE);
    }
  }
  if (interrupts) {
    enable_interrupts();
  }
#endif
}

void register_tlb_cpu([[maybe_unused]] TlbNotifier notify) noexcept {
#if !defined(MOSS_ARCH_ARM64)
  const bool interrupts = interrupts_enabled();
  disable_interrupts();
  const auto cpu = get_current_cpu_id();
  if (!notify || cpu >= sizeof(online) * 8) {
    kernel_panic("Invalid TLB participant");
  }
#if defined(MOSS_ARCH_X64)
  u64 cr4;
  asm volatile("mov %%cr4, %0" : "=r"(cr4));
  // CR4.PCIDE is bit 17. Current root switches rely on CR3 flushing inactive
  // address spaces; PCID/no-flush switching needs a different tracking model.
  if (cr4 & (u64{1} << 17)) {
    kernel_panic("TLB protocol requires PCID disabled");
  }
#elif defined(MOSS_ARCH_RISCV64)
  // SSIE is sie bit 1. Every registered hart, including the BSP, must accept
  // software IPIs when SIE is enabled; STIE alone cannot deliver shootdowns.
  asm volatile("csrsi sie, 2" ::: "memory");
#endif
  moss_validation_tlb_registration(0);
  acquire_publisher();
  if (notifier && notifier != notify) {
    kernel_panic("Inconsistent TLB notifier");
  }
  notifier = notify;
  // A request ordered before this acquisition did not target this CPU. Flush
  // everything it may have cached before publishing membership; requests
  // ordered afterward must include it. One lock closes the gap between them.
  invalidate_local(0, true);
  online |= u64{1} << cpu;
  moss_validation_tlb_registration(1);
  __atomic_store_n(&publisher, 0U, __ATOMIC_RELEASE);
  moss_validation_tlb_registration(2);
  if (interrupts) {
    enable_interrupts();
  }
#endif
}

void synchronize_tlb(VirtAddr addr, bool full) noexcept {
#if defined(MOSS_ARCH_ARM64)
  if (full) {
    flush_tlb();
  } else {
    flush_tlb_addr(addr);
  }
#else
  const bool interrupts = interrupts_enabled();
  disable_interrupts();
  acquire_publisher();
  moss_validation_tlb_publishing();
  const auto cpu = get_current_cpu_id();
  const u64 self = cpu < sizeof(online) * 8 ? u64{1} << cpu : 0;
  const u64 targets = online & ~self;
  moss_validation_tlb_targets(targets);
  // Explicit data fence precedes notification, as required by the RISC-V
  // remote-SFENCE protocol. Release/acquire also publishes the descriptor.
  memory_barrier();
  address = addr;
  entire = full;
  __atomic_store_n(&pending, targets, __ATOMIC_RELEASE);
  for (u32 target = 0; target < sizeof(targets) * 8; ++target) {
    if ((targets & (u64{1} << target)) && (!notifier || !notifier(target))) {
      // Returning here would allow frame reuse while a remote TLB is stale.
      kernel_panic("TLB notification failed");
    }
  }
  invalidate_local(addr, full);
  memory_barrier();
  while (__atomic_load_n(&pending, __ATOMIC_ACQUIRE) != 0) {
    cpu_yield();
  }
  __atomic_store_n(&publisher, 0U, __ATOMIC_RELEASE);
  if (interrupts) {
    enable_interrupts();
  }
#endif
}
} // namespace moss::kernel::arch
