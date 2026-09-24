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
import moss.drivers;
import moss.result;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "queue_regression.hpp"
#include "scheduler_regression.hpp"
#include "validation/memory_internal.hpp"
#include "validation/smp_cases.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;

namespace moss::test::validation {
// Exercise synchronous cross-CPU invalidation with IRQs disabled. The
// peer really installs the owned root and primes its translation, so software
// PTE inspection alone cannot make the remote-remap assertion pass.
struct TlbBroadcast {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  static constexpr VirtAddr address = process::user_layout::CODE_BASE;
  // Local, IRQ-masked remote, VM-lock contention, interrupt-only delivery and
  // full-tree pruning, then concurrent remaps. Software publishers exercise
  // each CPU taking the lock first; ARM64 uses its native broadcasts. Every
  // round has prime/remap/root-retired milestones in the outer handshake.
  static constexpr u32 rounds = 7, milestones = 3;
  static constexpr u32 lock_round = 2, irq_round = 3, prune_round = 4;
  static constexpr u32 publisher_round = prune_round + 1;
  static constexpr const char *names[rounds] = {"local_remap",  "remote_remap",       "locked_remap",      "ipi_remap",
                                                "pruned_remap", "concurrent_remap_0", "concurrent_remap_1"};
  // Arbitrary, distinct values reveal whether a cached old frame was used.
  static constexpr u8 before = 0x5a, after = 0xa5;
  shared_ptr<process::AddressSpace> target;
  u32 phase = 0, arrived = 0, scenario = 0, contentions = 0;
  bool remote = false, peer_ok = false;
  u8 peer_before = 0, peer_after = 0;

  static bool active(PhysAddr physical, [[maybe_unused]] u16 asid) {
    u64 root;
#if defined(MOSS_ARCH_ARM64)
    asm volatile("mrs %0, ttbr0_el1" : "=r"(root));
    // TTBR0's ASID field starts at bit 48; require the actual nonzero tag,
    // not just a software AddressSpace whose root was never installed.
    return !arch::interrupts_enabled() && root == (physical | (static_cast<u64>(asid) << 48));
#elif defined(MOSS_ARCH_X64)
    asm volatile("mov %%cr3, %0" : "=r"(root));
    return !arch::interrupts_enabled() && root == physical;
#elif defined(MOSS_ARCH_RISCV64)
    asm volatile("csrr %0, satp" : "=r"(root));
    return !arch::interrupts_enabled() && root == hal::mmu::make_satp_value(physical, asid);
#endif
  }

  static bool active(const process::AddressSpace &space) { return active(space.pgd_phys, space.asid); }

  static bool read(u8 &value, VirtAddr user_address = address) {
    return moss::abi::uaccess::moss_raw_copy_from_user(&value, reinterpret_cast<const void *>(user_address),
                                                       sizeof(value)) == 0;
  }

  struct Publishers {
    // One publisher on each of the two pinned workers. The masks let either
    // worker finish first without overwriting the other's completion signal.
    static constexpr u32 count = 2, both = (1U << count) - 1;
    // Different bytes and VAs distinguish both old frames and both descriptors.
    static constexpr u8 old_bytes[count] = {0x35, 0x6a}, new_bytes[count] = {0xca, 0x95};
    shared_ptr<process::AddressSpace> spaces[count];
    PhysAddr original[count]{}, replacement[count]{};
    mm::PageTableEntry *leaves[count]{};
    u64 permissions[count]{};
    u32 ready = 0, completed = 0, publication = 0, contention = 0, first = 0;
    bool armed = false, ok[count]{};
    u8 primed[count]{}, observed[count]{};

    static VirtAddr location(u32 cpu) { return address + cpu * page_size; }

    void prepare(u32 first_cpu) {
      first = first_cpu;
      ready = completed = publication = contention = 0;
      for (u32 cpu = 0; cpu < count; ++cpu) {
        ok[cpu] = false;
        primed[cpu] = observed[cpu] = 0;
        auto created = process::user_space::create_user_address_space();
        smp_require(created);
        spaces[cpu] = moss::move(*created);
        constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE | process::vma_flags::DEMAND_ZERO;
        smp_require(spaces[cpu]->add_vma(address, address + count * page_size, flags));
        // A resident sibling prevents pruning from masking a broken VA request
        // with a full-tree flush. The two writers hold different real VM locks.
        for (u32 page = 0; page < count; ++page) {
          smp_require(spaces[cpu]->copy_to_user(location(page), &old_bytes[cpu], sizeof(u8)) == 0);
        }
        leaves[cpu] = Tables::get_user_pte(spaces[cpu]->pgd_phys, location(cpu));
        smp_require(leaves[cpu] && spaces[cpu]->asid != 0);
        original[cpu] = leaves[cpu]->get_phys_addr();
        permissions[cpu] = leaves[cpu]->raw & ~hal::mmu::PTE_ADDR_MASK;
        Pfa::page_ref_inc(original[cpu]);
        auto frame = mm::allocate_pages(0);
        smp_require(frame && *frame != original[cpu] && Pfa::page_ref_get(original[cpu]) == 2);
        replacement[cpu] = *frame;
        *reinterpret_cast<u8 *>(phys_to_virt(*frame)) = new_bytes[cpu];
      }
      smp_require(spaces[0]->asid != spaces[1]->asid);
    }

    void publishing() {
      if (__atomic_load_n(&armed, __ATOMIC_ACQUIRE) && arch::get_current_cpu_id() == first &&
          __atomic_exchange_n(&publication, 1U, __ATOMIC_RELEASE) == 0) {
        // Hold the acquired production publisher lock before publishing any
        // request. The peer must actually fail its acquisition before we
        // proceed, so an early cooperative ACK cannot hide a broken lock wait.
        while ((__atomic_load_n(&contention, __ATOMIC_ACQUIRE) & (1U << (first ^ 1U))) == 0) {
          arch::cpu_yield();
        }
      }
    }

    void contended() {
      const auto cpu = arch::get_current_cpu_id();
      if (cpu < count && __atomic_load_n(&armed, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_or(&contention, 1U << cpu, __ATOMIC_RELEASE);
      }
    }

    void perform(u32 cpu) {
      auto process = process::current_process();
      auto saved = process ? process->address_space() : shared_ptr<process::AddressSpace>{};
      smp_require(saved && arch::get_current_cpu_id() == cpu);
      const bool interrupts = arch::interrupts_enabled();
      arch::disable_interrupts();
      const auto other = cpu ^ 1U;
      process::CfsScheduler::use_address_space(spaces[other]);
      ok[cpu] = active(*spaces[other]) && read(primed[cpu], location(other));
      __atomic_fetch_or(&ready, 1U << cpu, __ATOMIC_RELEASE);
      wait_for_phase(ready, both);
#if !defined(MOSS_ARCH_ARM64)
      if (cpu == first) {
        // Arm only after both workers have masked IRQs and primed their real
        // roots. A preempting task during preparation must not hit the barrier.
        __atomic_store_n(&armed, true, __ATOMIC_RELEASE);
      } else {
        wait_for_phase(publication, 1);
      }
#endif
      {
        auto transaction = spaces[cpu]->lock_vm();
        Tables::unmap_user_page(spaces[cpu]->pgd_phys, location(cpu));
        smp_require(Tables::map_user_page(spaces[cpu]->pgd_phys, location(cpu), replacement[cpu], permissions[cpu]));
      }
      __atomic_fetch_or(&completed, 1U << cpu, __ATOMIC_RELEASE);
      // Service the peer's requests even after our own remap finished. IRQs
      // stay masked, so neither timer rescheduling nor a root reload can hide
      // a missed invalidation before the actual load from the peer's new page.
      wait_for_phase(completed, both);
      // Both production remaps returned. Disarm before either worker restores
      // IRQs, so an unrelated task on these CPUs cannot enter our observers.
      __atomic_store_n(&armed, false, __ATOMIC_RELEASE);
      ok[cpu] = active(*spaces[other]) && read(observed[cpu], location(other)) && ok[cpu];
      process::CfsScheduler::use_address_space(saved);
      if (interrupts) {
        arch::enable_interrupts();
      }
    }

    void finish() {
#if !defined(MOSS_ARCH_ARM64)
      ut::expect(__atomic_load_n(&publication, __ATOMIC_ACQUIRE) == 1 &&
                 (__atomic_load_n(&contention, __ATOMIC_ACQUIRE) & (1U << (first ^ 1U))) != 0);
#endif
      for (u32 cpu = 0; cpu < count; ++cpu) {
        ut::expect(ok[cpu] && primed[cpu] == old_bytes[cpu ^ 1U] && observed[cpu] == new_bytes[cpu ^ 1U]);
        auto *leaf = Tables::get_user_pte(spaces[cpu]->pgd_phys, location(cpu));
        ut::expect(leaf == leaves[cpu] && leaf->get_phys_addr() == replacement[cpu]);
        ut::expect(Pfa::page_ref_get(original[cpu]) == 1 &&
                   *reinterpret_cast<const u8 *>(phys_to_virt(original[cpu])) == old_bytes[cpu]);
        spaces[cpu].reset();
        smp_require(Pfa::page_ref_dec(original[cpu]) == 0 && mm::free_pages(original[cpu], 0));
      }
    }
  } publishers;

  void contended(PhysAddr root) {
    if (scenario == lock_round && target && root == target->pgd_phys && arch::get_current_cpu_id() == 1) {
      ++contentions;
      __atomic_store_n(&arrived, scenario * milestones + 2, __ATOMIC_RELEASE);
    }
  }

  bool peer() {
    auto process = process::current_process();
    auto saved = process ? process->address_space() : shared_ptr<process::AddressSpace>{};
    peer_ok = saved && arch::get_current_cpu_id() == 1;
    for (u32 round = 0; round < rounds; ++round) {
      const auto base = round * milestones;
      __atomic_store_n(&arrived, base + 1, __ATOMIC_RELEASE);
      wait_for_phase(phase, base + 1);
      if (round >= publisher_round) {
        publishers.perform(1);
        __atomic_store_n(&arrived, base + 3, __ATOMIC_RELEASE);
        wait_for_phase(phase, base + 3);
        continue;
      }
      const bool interrupts = arch::interrupts_enabled();
      if (remote && peer_ok) {
        arch::disable_interrupts();
        process::CfsScheduler::use_address_space(target);
        peer_ok = active(*target) && read(peer_before) && peer_ok;
      }
      if (round == lock_round) {
        // Owner already holds this real VM lock. Its shootdown must complete
        // while we wait in the production IRQ-masked ticket-spin path.
        auto transaction = target->lock_vm();
      } else {
        if (round == irq_round) {
          // Prevent a timer context switch / CR3 or SATP reload from hiding a
          // missing TLB IPI. This wait deliberately does not service by polling.
          hal::timer::disable();
          arch::enable_interrupts();
        }
        __atomic_store_n(&arrived, base + 2, __ATOMIC_RELEASE);
      }
      if (round == irq_round) {
        while (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) < base + 2) {
          asm volatile("" ::: "memory");
        }
        arch::disable_interrupts();
      } else {
        wait_for_phase(phase, base + 2);
      }
      if (remote && saved) {
        peer_ok = active(*target) && read(peer_after) && peer_ok;
        process::CfsScheduler::use_address_space(saved);
        if (round == irq_round) {
          hal::timer::enable();
        }
        if (interrupts) {
          arch::enable_interrupts();
        }
      }
      // No CPU may retain the target root when the owner releases its tables.
      __atomic_store_n(&arrived, base + 3, __ATOMIC_RELEASE);
      wait_for_phase(phase, base + 3);
    }
    return peer_ok;
  }

  void owner() {
    for (u32 round = 0; round < rounds; ++round) {
      const auto base = round * milestones;
      wait_for_phase(arrived, base + 1);
      scenario = round;
      contentions = 0;
      remote = round != 0;
      if (remote) {
        end_case();
        start_case(names[round]);
      }
      owner_round(base);
      __atomic_store_n(&phase, base + 3, __ATOMIC_RELEASE);
    }
  }

  void owner_round(u32 base) {
    const auto pages = Pfa::get_memory_stats().free_pages;
    const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    if (scenario >= publisher_round) {
      publishers.prepare(scenario - publisher_round);
      __atomic_store_n(&phase, base + 1, __ATOMIC_RELEASE);
      publishers.perform(0);
      wait_for_phase(arrived, base + 3);
      publishers.finish();
      ut::expect(affinity_valid());
      ut::expect(Pfa::get_memory_stats().free_pages == pages);
      ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
      return;
    }
    auto created = process::user_space::create_user_address_space();
    smp_require(created.has_value());
    target = moss::move(*created);
    // Keep a resident sibling in the same leaf table: otherwise unmap's
    // separate full-tree pruning flush would mask a broken per-address TLBI.
    constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE | process::vma_flags::DEMAND_ZERO;
    smp_require(target->add_vma(address, address + 2 * page_size, flags));
    smp_require(target->copy_to_user(address, &before, sizeof(before)) == 0);
    if (scenario != prune_round) {
      smp_require(target->copy_to_user(address + page_size, &before, sizeof(before)) == 0);
    }
    auto *leaf = Tables::get_user_pte(target->pgd_phys, address);
    smp_require(leaf && target->asid != 0);
#if defined(MOSS_ARCH_ARM64)
    smp_require((leaf->raw & mm::page_attr::NG) != 0);
#endif
    const auto original = leaf->get_phys_addr();
    const auto permissions = leaf->raw & ~hal::mmu::PTE_ADDR_MASK;
    // Keep the old frame owned even in a failing run: a stale translation
    // reports the old byte without accessing storage recycled for another use.
    Pfa::page_ref_inc(original);
    auto replacement = mm::allocate_pages(0);
    smp_require(replacement && *replacement != original && Pfa::page_ref_get(original) == 2);
    *reinterpret_cast<u8 *>(phys_to_virt(*replacement)) = after;
    auto process = process::current_process();
    auto saved = process ? process->address_space() : shared_ptr<process::AddressSpace>{};
    smp_require(saved && saved->asid != target->asid && affinity_valid());
    const bool interrupts = arch::interrupts_enabled();
    arch::disable_interrupts();
    u8 local_before = 0, local_after = 0;
    bool local_ok = true;
    if (!remote) {
      process::CfsScheduler::use_address_space(target);
      local_ok = active(*target) && read(local_before);
    }
    {
      auto transaction = target->lock_vm();
      __atomic_store_n(&phase, base + 1, __ATOMIC_RELEASE);
      wait_for_phase(arrived, base + 2);
      Tables::unmap_user_page(target->pgd_phys, address);
      smp_require(Tables::map_user_page(target->pgd_phys, address, *replacement, permissions));
    }
    if (!remote) {
      local_ok = read(local_after) && local_ok;
      process::CfsScheduler::use_address_space(saved);
    }
    __atomic_store_n(&phase, base + 2, __ATOMIC_RELEASE);
    wait_for_phase(arrived, base + 3);
    if (interrupts) {
      arch::enable_interrupts();
    }
    ut::expect(local_ok && peer_ok && affinity_valid());
    ut::expect((remote ? peer_before : local_before) == before);
    ut::expect((remote ? peer_after : local_after) == after);
    auto *observed = Tables::get_user_pte(target->pgd_phys, address);
    ut::expect(observed && observed->get_phys_addr() == *replacement && (scenario == prune_round || observed == leaf));
    if (scenario == lock_round) {
      ut::expect(contentions == 1);
    }
    ut::expect(*reinterpret_cast<const u8 *>(phys_to_virt(original)) == before && Pfa::page_ref_get(original) == 1);
    target.reset();
    smp_require(Pfa::page_ref_dec(original) == 0 && mm::free_pages(original, 0));
    ut::expect(Pfa::get_memory_stats().free_pages == pages);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  }
};
TlbBroadcast *tlb_broadcast = nullptr;

bool tlb_active(PhysAddr root, u16 asid) { return TlbBroadcast::active(root, asid); }
bool tlb_active(const process::AddressSpace &space) { return TlbBroadcast::active(space); }
bool tlb_read(u8 &value, VirtAddr address) { return TlbBroadcast::read(value, address); }
static void empty_case() {}
void register_tlb_smp() {
  ut::register_suite("mm.tlb_broadcast", [] {
    for (const auto *name : TlbBroadcast::names) {
      ut::register_test(name, empty_case);
    }
  });
}

long start_tlb_smp() {
  const char *selection = selected_suite();
  if (ut::same_id(selection, "mm.tlb_broadcast")) {
    start_case("local_remap");
    smp_require(g_num_cpus >= 2);
    tlb_broadcast = new TlbBroadcast();
    return 3; // Real CPU0/CPU1 workers, with temporary owned hardware roots.
  }
  return 0;
}

long control_tlb_smp(long op, long arg1, [[maybe_unused]] long arg2) {
  const char *selection = selected_suite();
  const char *active_case = running_case();
  if (ut::same_id(selection, "mm.tlb_broadcast") && active_case && tlb_broadcast) {
    if (op == 7 && arg1 == 0) {
      arch::enable_interrupts();
      return tlb_broadcast->peer() ? 1 : 0;
    }
    if (op == 8) {
      smp_require(arg1 && affinity_valid());
      arch::enable_interrupts();
      tlb_broadcast->owner();
      return 0;
    }
    if (op == 9) {
      ut::expect(arg1 && affinity_valid());
      delete tlb_broadcast;
      tlb_broadcast = nullptr;
      end_case();
      finish();
    }
  }
  invalid_control();
}
void tlb_broadcast_contended(PhysAddr root) {
  if (tlb_broadcast) {
    tlb_broadcast->contended(root);
  }
}
void tlb_broadcast_tlb_contended() {
  if (tlb_broadcast) {
    tlb_broadcast->publishers.contended();
  }
}
void tlb_broadcast_tlb_publishing() {
  if (tlb_broadcast) {
    tlb_broadcast->publishers.publishing();
  }
}
} // namespace moss::test::validation
