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
import moss.ipc;
import moss.drivers;
import moss.result;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "ipc_regression.hpp"
#include "queue_regression.hpp"
#include "scheduler_regression.hpp"
#include "validation/memory_internal.hpp"
#include "validation/smp_cases.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;

namespace moss::test::validation {
#if !defined(MOSS_ARCH_ARM64)
// These probes run during the first secondary's actual boot registration, not
// by editing the live membership mask or re-registering an already online CPU.
// The BSP pauses after its firmware start request, before starting other CPUs.
struct TlbJoinObservation {
  // Release/acquire milestones coordinate boot CPUs without a scheduler.
  u32 prepared = 0, publishing = 0, contended = 0, joined = 0, writer_done = 0, finished = 0;
  u64 targets = 0;
  usize pages = 0, heap = 0;
  bool primed = false, refreshed = false, old_owned = false, released = false, restored = false;
};
TlbJoinObservation tlb_join_observed;

struct TlbJoin {
  using Pfa = mm::PageFrameAllocator;
  using Tables = mm::PageTableManager;
  using Scheduler = process::CfsScheduler;
  enum class Order { None, RequestFirst, CpuFirst };
  // BSP is the publisher; logical CPU1 is the first secondary on both boot paths.
  static constexpr u32 publisher_cpu = 0, joining_cpu = 1;
  static constexpr u32 joining_bit = 1U << joining_cpu;
  static constexpr VirtAddr address = process::user_layout::CODE_BASE;
  // Distinct nonzero bytes expose a retained translation to the old frame.
  static constexpr u8 old_byte = 0x39, new_byte = 0xc6;
  shared_ptr<process::AddressSpace> space;
  mm::PageTableEntry *leaf = nullptr;
  PhysAddr original = 0, replacement = 0;
  Order order;
  inline static TlbJoin *probe = nullptr;
#if defined(MOSS_ARCH_RISCV64)
  // sstatus.SUM (bit 18) permits S-mode loads from U pages. This boot callback
  // has not entered the exception path that normally enables it for raw copy.
  static constexpr u64 supervisor_user_access = u64{1} << 18;
  u64 saved_user_access = 0;
#endif

  static Order selected() {
    // Size derives from the longest accepted workload ID, including its NUL.
    char value[sizeof("mm.tlb_join.request_first")];
    if (!boot_option("moss.validation", value, sizeof(value))) {
      return Order::None;
    }
    if (ut::same_id(value, "mm.tlb_join.request_first")) {
      return Order::RequestFirst;
    }
    return ut::same_id(value, "mm.tlb_join.cpu_first") ? Order::CpuFirst : Order::None;
  }

  static void require(bool valid) {
    if (!valid) {
      // A setup failure must stop this guest, not publish fabricated evidence.
      // The host startup deadline and native failure capture still apply.
      arch::kernel_panic("TLB join fixture setup failed");
    }
  }

  explicit TlbJoin(Order selected_order) : order(selected_order) {
    auto created = process::user_space::create_user_address_space();
    require(static_cast<bool>(created));
    space = moss::move(*created);
    constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE | process::vma_flags::DEMAND_ZERO;
    require(space->add_vma(address, address + page_size, flags));
    require(space->copy_to_user(address, &old_byte, sizeof(old_byte)) == 0);
    leaf = Tables::get_user_pte(space->pgd_phys, address);
    require(leaf && leaf->is_valid());
    original = leaf->get_phys_addr();
    // Retain the old frame independently so a deliberately missed flush reads
    // owned stale data, never a freed/reallocated physical page.
    Pfa::page_ref_inc(original);
    auto page = mm::allocate_pages(0);
    require(static_cast<bool>(page));
    replacement = *page;
    *reinterpret_cast<u8 *>(phys_to_virt(replacement)) = new_byte;
    Scheduler::use_address_space(space);
#if defined(MOSS_ARCH_RISCV64)
    // Keep SUM stable from priming through the final load: changing access
    // state between reads may discard cached translations and hide a missing
    // TLB invalidation. IRQs stay masked for this entire boot-only fixture.
    asm volatile("csrrs %0, sstatus, %1" : "=r"(saved_user_access) : "r"(supervisor_user_access) : "memory");
#endif
    prime();
  }

  ~TlbJoin() {
#if defined(MOSS_ARCH_RISCV64)
    // Restore only the access bit we changed, not unrelated status fields.
    if (!(saved_user_access & supervisor_user_access)) {
      asm volatile("csrc sstatus, %0" ::"r"(supervisor_user_access) : "memory");
    }
    u64 restored;
    asm volatile("csrr %0, sstatus" : "=r"(restored));
    require(((restored ^ saved_user_access) & supervisor_user_access) == 0);
#endif
  }

  void prime() const {
    u8 byte = 0;
    tlb_join_observed.primed = tlb_active(*space) && tlb_read(byte, address) && byte == old_byte;
    require(tlb_join_observed.primed);
  }

  static void registration(u32 stage) {
    if (arch::get_current_cpu_id() != joining_cpu || selected() == Order::None) {
      return;
    }
    auto &seen = tlb_join_observed;
    if (stage == 0) { // Before acquiring the real publisher lock.
      seen.pages = Pfa::get_memory_stats().free_pages;
      seen.heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
      probe = new TlbJoin(selected());
      __atomic_store_n(&seen.prepared, 1U, __ATOMIC_RELEASE);
      if (probe->order == Order::RequestFirst) {
        wait_for_phase(seen.publishing, 1);
      }
    } else if (stage == 1) { // Full local flush and membership publication done; lock still held.
      if (probe->order == Order::CpuFirst) {
        // Refill after the registration flush, before the BSP changes the PTE.
        // Only the later request can now remove this cached old translation.
        probe->prime();
        __atomic_store_n(&seen.joined, 1U, __ATOMIC_RELEASE);
        wait_for_phase(seen.contended, 1U << publisher_cpu);
      }
    } else { // Lock released; remain IRQ-masked until the final hardware load.
      wait_for_phase(seen.writer_done, 1);
      u8 byte = 0;
      seen.refreshed = tlb_active(*probe->space) && tlb_read(byte, address) && byte == new_byte;
      seen.old_owned = Pfa::page_ref_get(probe->original) == 1 &&
                       *reinterpret_cast<const u8 *>(phys_to_virt(probe->original)) == old_byte &&
                       probe->leaf->get_phys_addr() == probe->replacement;
      const auto original = probe->original, replacement = probe->replacement;
      Scheduler::use_kernel_address_space();
      delete probe;
      probe = nullptr;
      require(Pfa::page_ref_dec(original) == 0 && mm::free_pages(original, 0));
      seen.released = Pfa::page_ref_get(original) == 0 && Pfa::page_ref_get(replacement) == 0;
      seen.restored = !Scheduler::active_address_space() && Pfa::get_memory_stats().free_pages == seen.pages &&
                      mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == seen.heap;
      __atomic_store_n(&seen.finished, 1U, __ATOMIC_RELEASE);
    }
  }

  static void cpu_started(u32 cpu) {
    if (cpu != joining_cpu || selected() == Order::None) {
      return;
    }
    require(arch::get_current_cpu_id() == publisher_cpu);
    const bool interrupts = arch::interrupts_enabled();
    arch::disable_interrupts();
    auto &seen = tlb_join_observed;
    wait_for_phase(seen.prepared, 1);
    if (probe->order == Order::CpuFirst) {
      wait_for_phase(seen.joined, 1);
    }
    {
      auto transaction = probe->space->lock_vm();
      mm::PageTableEntry updated;
      updated.set_page(probe->replacement, probe->leaf->raw & ~hal::mmu::PTE_ADDR_MASK);
      // One atomic PTE replacement isolates the single production invalidation
      // request under test. An unmap/map pair could hide a missing join flush
      // with its second invalidation. Transfer the old mapping's frame reference
      // to the fixture's retained copy; the new allocation becomes the mapping.
      __atomic_store_n(&probe->leaf->raw, updated.raw, __ATOMIC_RELEASE);
      require(Pfa::page_ref_dec(probe->original) == 1);
      arch::flush_tlb_addr(address);
    }
    __atomic_store_n(&seen.writer_done, 1U, __ATOMIC_RELEASE);
    // CPU1 may reclaim tables while we wait. Cooperatively ACK that independent
    // cleanup, and do not touch its fixture again after publishing writer_done.
    wait_for_phase(seen.finished, 1);
    if (interrupts) {
      arch::enable_interrupts();
    }
  }

  static bool recording() {
    return __atomic_load_n(&tlb_join_observed.prepared, __ATOMIC_ACQUIRE) != 0 &&
           __atomic_load_n(&tlb_join_observed.writer_done, __ATOMIC_ACQUIRE) == 0;
  }

  static void publishing() {
    if (recording() && arch::get_current_cpu_id() == publisher_cpu && probe->order == Order::RequestFirst) {
      __atomic_store_n(&tlb_join_observed.publishing, 1U, __ATOMIC_RELEASE);
      wait_for_phase(tlb_join_observed.contended, joining_bit);
    }
  }

  static void contended() {
    if (recording()) {
      __atomic_fetch_or(&tlb_join_observed.contended, 1U << arch::get_current_cpu_id(), __ATOMIC_RELEASE);
    }
  }

  static void targets(u64 mask) {
    if (recording() && arch::get_current_cpu_id() == publisher_cpu) {
      tlb_join_observed.targets = mask;
    }
  }

  static void verify() {
    const auto &seen = tlb_join_observed;
    ut::expect(__atomic_load_n(&seen.finished, __ATOMIC_ACQUIRE) == 1);
    ut::expect(seen.primed && seen.refreshed);
    ut::expect(seen.old_owned && seen.released && seen.restored);
    const bool request_first = selected() == Order::RequestFirst;
    ut::expect(seen.targets == (request_first ? 0 : joining_bit));
    ut::expect(seen.contended == (request_first ? joining_bit : 1U << publisher_cpu));
    ut::expect(probe == nullptr);
  }
};
#endif

void register_tlb_join_smp() {
#if !defined(MOSS_ARCH_ARM64)
  ut::register_suite("mm.tlb_join.request_first", [] { ut::register_test("registration", TlbJoin::verify); });
  ut::register_suite("mm.tlb_join.cpu_first", [] { ut::register_test("registration", TlbJoin::verify); });
#endif
}
void tlb_join_contended() {
#if !defined(MOSS_ARCH_ARM64)
  TlbJoin::contended();
#endif
}
void tlb_join_publishing() {
#if !defined(MOSS_ARCH_ARM64)
  TlbJoin::publishing();
#endif
}
extern "C" void moss_validation_tlb_registration([[maybe_unused]] unsigned stage) noexcept {
#if !defined(MOSS_ARCH_ARM64)
  TlbJoin::registration(stage);
#endif
}

extern "C" void moss_validation_tlb_targets([[maybe_unused]] moss::u64 targets) noexcept {
#if !defined(MOSS_ARCH_ARM64)
  TlbJoin::targets(targets);
#endif
}

extern "C" void moss_validation_cpu_started([[maybe_unused]] unsigned cpu) noexcept {
#if !defined(MOSS_ARCH_ARM64)
  TlbJoin::cpu_started(cpu);
#endif
}

} // namespace moss::test::validation
