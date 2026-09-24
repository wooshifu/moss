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
// Exercise the same fault transaction as native exception entry, on a real
// but inactive root. This checks software PTE/frame ownership, not remote TLBs.
struct FaultTransactions {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  static constexpr VirtAddr address = process::user_layout::CODE_BASE;
  static constexpr u8 resident_byte = 0x5a; // Nonzero so demand-zero cannot masquerade as resident data.
  // COW, demand-zero, fault/unmap, fault/fork, then fork/unmap. Each uses the
  // same milestones so the peer contends before the owner commits.
  static constexpr u32 scenarios = 5, milestones = 3;
  shared_ptr<process::AddressSpace> source, target, clone;
  PhysAddr original = 0, owner_page = 0, peer_page = 0;
  u64 contents = 0;
  // Independent monotonic clocks: phase = snapshot/commit/cleanup; arrived =
  // ready/contending/finished. The host's normal deadline bounds a stuck peer.
  u32 phase = 0, arrived = 0, snapshots = 0, contentions = 0;
  u32 round_base = 0;
  bool demand = false, unmap = false, fork = false, fork_unmap = false;
  bool peer_ok = false;

  void snapshot(PhysAddr root, VirtAddr fault_address) {
    if (!target || root != target->pgd_phys || fault_address != address) {
      return;
    }
    (void)__atomic_fetch_add(&snapshots, 1U, __ATOMIC_RELAXED);
    if (arch::get_current_cpu_id() == 0) {
      __atomic_store_n(&phase, round_base + 1, __ATOMIC_RELEASE);
      wait_for_phase(arrived, round_base + 2);
    } else {
      // Without serialization the peer reaches the same old-frame snapshot.
      // Force it to publish only after the owner has committed its replacement.
      __atomic_store_n(&arrived, round_base + 2, __ATOMIC_RELEASE);
      wait_for_phase(phase, round_base + 2);
    }
  }

  void contended(PhysAddr root) {
    if (target && root == target->pgd_phys && arch::get_current_cpu_id() == 1) {
      (void)__atomic_fetch_add(&contentions, 1U, __ATOMIC_RELAXED);
      // With serialization this is the actual failed try_lock boundary. The
      // peer cannot snapshot the old PTE; let the owner complete before retry.
      __atomic_store_n(&arrived, round_base + 2, __ATOMIC_RELEASE);
    }
  }

  void committed(PhysAddr root, VirtAddr fault_address, PhysAddr page) {
    if (target && root == target->pgd_phys && fault_address == address && arch::get_current_cpu_id() == 0 && unmap) {
      // The peer may unmap immediately after resolve_fault releases vm_lock_.
      owner_page = page;
    }
  }

  bool peer() {
    peer_ok = arch::get_current_cpu_id() == 1;
    for (u32 round = 0; round < scenarios; ++round) {
      const auto base = round * milestones;
      __atomic_store_n(&arrived, base + 1, __ATOMIC_RELEASE);
      wait_for_phase(phase, base + 1);
      if (fork) {
        auto transaction = target->lock_vm();
        peer_ok = static_cast<bool>(Tables::clone_user_page_tables(target->pgd_phys, clone->pgd_phys)) && peer_ok;
        const auto *pte = Tables::get_user_pte(clone->pgd_phys, address);
        peer_page = pte && pte->is_valid() ? pte->get_phys_addr() : 0;
        peer_ok = peer_page != 0 && peer_ok;
      } else if (unmap || fork_unmap) {
        auto transaction = target->lock_vm();
        const auto *pte = Tables::get_user_pte(target->pgd_phys, address);
        peer_page = pte && pte->is_valid() ? pte->get_phys_addr() : 0;
        Tables::unmap_user_page(target->pgd_phys, address);
        peer_ok = target->remove_vma(address, address + page_size) && peer_page != 0 && peer_ok;
      } else {
        peer_ok = target->resolve_fault(address, mm::UserFaultAccess::Write, !demand) && peer_ok;
        const auto *pte = Tables::get_user_pte(target->pgd_phys, address);
        peer_page = pte ? pte->get_phys_addr() : 0;
      }
      __atomic_store_n(&arrived, base + 3, __ATOMIC_RELEASE);
      wait_for_phase(phase, base + 3);
    }
    return peer_ok;
  }

  void owner() {
    for (u32 round = 0; round < scenarios; ++round) {
      wait_for_phase(arrived, round * milestones + 1);
      round_base = round * milestones;
      demand = round != 0;
      unmap = round == 2;
      fork = round == 3;
      fork_unmap = round == 4;
      snapshots = 0;
      contentions = 0;
      if (fork_unmap) {
        end_case();
        start_case("fork_unmap");
      } else if (fork) {
        end_case();
        start_case("fault_fork");
      } else if (unmap) {
        end_case();
        start_case("fault_unmap");
      } else if (demand) {
        end_case();
        start_case("demand_fault");
      }
      owner_round();
      __atomic_store_n(&phase, round_base + 3, __ATOMIC_RELEASE);
    }
  }

  void owner_round() {
    if (fork_unmap) {
      fork_unmap_round();
      return;
    }
    const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    const auto pages = Pfa::get_memory_stats().free_pages;
    auto original_space = process::user_space::create_user_address_space();
    auto copied_space = process::user_space::create_user_address_space();
    smp_require(original_space && copied_space);
    source = moss::move(*original_space);
    target = moss::move(*copied_space);
    constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE;
    smp_require(source->add_vma(address, address + page_size, flags));
    smp_require(target->add_vma(address, address + page_size, flags));
    if (fork) {
      auto cloned_space = process::user_space::create_user_address_space();
      smp_require(cloned_space.has_value());
      clone = moss::move(*cloned_space);
      smp_require(clone->add_vma(address, address + page_size, flags));
    }
    auto allocated = mm::allocate_pages(0);
    smp_require(allocated.has_value());
    original = *allocated;
    auto *bytes = reinterpret_cast<u8 *>(phys_to_virt(original));
    // A nonzero pattern distinguishes the copied contents from demand-zero.
    for (usize index = 0; index < page_size; ++index) {
      bytes[index] = demand ? 0 : resident_byte;
    }
    contents = memory_hash(phys_to_virt(original), page_size);
    smp_require(Tables::map_user_page(source->pgd_phys, address, original, mm::page_perms::USER_RW));
    if (!demand) {
      smp_require(Tables::clone_user_page_tables(source->pgd_phys, target->pgd_phys));
    }
    // COW starts with one shared mapping in each root. Demand starts with an
    // empty target and a separate resident zero-page content oracle in source.
    smp_require(Pfa::page_ref_get(original) == (demand ? 1U : 2U));
    owner_page = 0;
    const bool owner_ok = target->resolve_fault(address, mm::UserFaultAccess::Write, !demand);
    if (!unmap) {
      const auto *owner_pte = Tables::get_user_pte(target->pgd_phys, address);
      owner_page = owner_pte ? owner_pte->get_phys_addr() : 0;
    }
    __atomic_store_n(&phase, round_base + 2, __ATOMIC_RELEASE);
    wait_for_phase(arrived, round_base + 3);
    ut::expect(owner_ok && peer_ok && affinity_valid());
    if (unmap) {
      const auto *pte = Tables::get_user_pte(target->pgd_phys, address);
      ut::expect(owner_page != 0 && peer_page == owner_page && (!pte || !pte->is_valid()) &&
                 !target->find_vma(address) && Pfa::page_ref_get(owner_page) == 0);
      ut::expect(Pfa::page_ref_get(original) == 1 && memory_hash(phys_to_virt(original), page_size) == contents);
      ut::expect(snapshots == 1 && contentions == 1);
      target.reset();
      source.reset();
      ut::expect(Pfa::get_memory_stats().free_pages == pages);
      ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
      return;
    }
    if (fork) {
      const auto *parent_pte = Tables::get_user_pte(target->pgd_phys, address);
      const auto *child_pte = Tables::get_user_pte(clone->pgd_phys, address);
      ut::expect(owner_page != 0 && owner_page != original && peer_page == owner_page && parent_pte && child_pte &&
                 parent_pte->get_phys_addr() == owner_page && child_pte->get_phys_addr() == owner_page &&
                 parent_pte->is_cow() && child_pte->is_cow() && !parent_pte->is_writable() &&
                 !child_pte->is_writable() && Pfa::page_ref_get(owner_page) == 2);
      ut::expect(Pfa::page_ref_get(original) == 1 && memory_hash(phys_to_virt(original), page_size) == contents &&
                 memory_hash(phys_to_virt(owner_page), page_size) == contents);
      ut::expect(snapshots == 1 && contentions == 1);
      clone.reset();
      target.reset();
      source.reset();
      ut::expect(Pfa::get_memory_stats().free_pages == pages);
      ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
      return;
    }
    ut::expect(owner_page != 0 && owner_page == peer_page && owner_page != original);
    const auto old_refs = Pfa::page_ref_get(original);
    const bool ownership = old_refs == 1 && peer_page != 0 && Pfa::page_ref_get(peer_page) == 1;
    if (!ut::expect(ownership)) {
      // A red run may already have freed a frame still mapped by source. Do
      // not dereference it or run destructors that would release it twice.
      end_case();
      finish("fault_ownership");
    }
    const auto *source_pte = Tables::get_user_pte(source->pgd_phys, address);
    const auto *target_pte = Tables::get_user_pte(target->pgd_phys, address);
    ut::expect(source_pte && source_pte->get_phys_addr() == original && source_pte->is_cow() == !demand &&
               source_pte->is_writable() == demand);
    ut::expect(target_pte && target_pte->get_phys_addr() == peer_page && !target_pte->is_cow() &&
               target_pte->is_writable());
    ut::expect(memory_hash(phys_to_virt(original), page_size) == contents);
    ut::expect(memory_hash(phys_to_virt(peer_page), page_size) == contents);
    ut::expect(snapshots == 1 && contentions == 1);
    // A resident retry must neither refill modified bytes nor relax protection.
    auto *private_bytes = reinterpret_cast<u8 *>(phys_to_virt(peer_page));
    private_bytes[0] ^= 0xff; // Flip every bit of one byte to distinguish a refill.
    const auto modified = memory_hash(phys_to_virt(peer_page), page_size);
    ut::expect(target->resolve_fault(address, mm::UserFaultAccess::Read, false));
    ut::expect(memory_hash(phys_to_virt(peer_page), page_size) == modified);
    ut::expect(!target->resolve_fault(address, mm::UserFaultAccess::Execute, false));
    if (!demand) {
      ut::expect(!source->resolve_fault(address, mm::UserFaultAccess::Write, false));
    }
    target.reset();
    source.reset();
    ut::expect(Pfa::get_memory_stats().free_pages == pages);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  }

  void fork_unmap_round() {
    const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    const auto pages = Pfa::get_memory_stats().free_pages;
    auto parent = process::user_space::create_user_address_space();
    auto child = process::user_space::create_user_address_space();
    smp_require(parent && child);
    target = moss::move(*parent);
    clone = moss::move(*child);
    constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE;
    smp_require(target->add_vma(address, address + page_size, flags));
    auto allocated = mm::allocate_pages(0);
    smp_require(allocated.has_value());
    original = *allocated;
    auto *bytes = reinterpret_cast<u8 *>(phys_to_virt(original));
    for (usize index = 0; index < page_size; ++index) {
      bytes[index] = resident_byte;
    }
    contents = memory_hash(phys_to_virt(original), page_size);
    smp_require(Tables::map_user_page(target->pgd_phys, address, original, mm::page_perms::USER_RW));

    bool cloned = false;
    bool metadata_copied = false;
    {
      auto transaction = target->lock_vm();
      __atomic_store_n(&phase, round_base + 1, __ATOMIC_RELEASE);
      wait_for_phase(arrived, round_base + 2);
      cloned = static_cast<bool>(Tables::clone_user_page_tables(target->pgd_phys, clone->pgd_phys));
      auto vma = target->find_vma(address);
      metadata_copied = vma && clone->add_vma(vma->start_addr, vma->end_addr, vma->flags);
      __atomic_store_n(&phase, round_base + 2, __ATOMIC_RELEASE);
    }
    wait_for_phase(arrived, round_base + 3);
    const auto *parent_pte = Tables::get_user_pte(target->pgd_phys, address);
    const auto *child_pte = Tables::get_user_pte(clone->pgd_phys, address);
    const bool child_owned = child_pte && child_pte->is_valid() && child_pte->get_phys_addr() == original;
    ut::expect(cloned && metadata_copied && peer_ok && affinity_valid());
    ut::expect((!parent_pte || !parent_pte->is_valid()) && !target->find_vma(address) && peer_page == original);
    ut::expect(child_owned && child_pte->is_cow() && !child_pte->is_writable() && clone->find_vma(address) &&
               Pfa::page_ref_get(original) == 1);
    if (child_owned) {
      ut::expect(memory_hash(phys_to_virt(original), page_size) == contents);
    }
    ut::expect(snapshots == 0 && contentions == 1);
    clone.reset();
    target.reset();
    ut::expect(Pfa::get_memory_stats().free_pages == pages);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  }
};
FaultTransactions *fault_transactions = nullptr;

static void empty_case() {}
void register_fault_smp() {
  ut::register_suite("mm.concurrent", [] {
    ut::register_test("cow_fault", empty_case);
    ut::register_test("demand_fault", empty_case);
    ut::register_test("fault_unmap", empty_case);
    ut::register_test("fault_fork", empty_case);
    ut::register_test("fork_unmap", empty_case);
  });
}

long start_fault_smp() {
  const char *selection = selected_suite();
  if (ut::same_id(selection, "mm.concurrent")) {
    start_case("cow_fault");
    smp_require(g_num_cpus >= 2);
    fault_transactions = new FaultTransactions();
    return 3; // The same real fork/affinity/reap protocol as mm.lifetime.
  }
  return 0;
}

long control_fault_smp(long op, long arg1, [[maybe_unused]] long arg2) {
  const char *selection = selected_suite();
  const char *active_case = running_case();
  if (ut::same_id(selection, "mm.concurrent") && active_case && fault_transactions) {
    if (op == 7 && arg1 == 0) {
      arch::enable_interrupts();
      return fault_transactions->peer() ? 1 : 0;
    }
    if (op == 8) {
      smp_require(arg1 && affinity_valid());
      arch::enable_interrupts();
      fault_transactions->owner();
      return 0;
    }
    if (op == 9) {
      ut::expect(arg1 && affinity_valid());
      delete fault_transactions;
      fault_transactions = nullptr;
      end_case();
      finish();
    }
  }
  invalid_control();
}
extern "C" void moss_validation_cow_snapshot(PhysAddr root, VirtAddr address) noexcept {
  if (fault_transactions) {
    fault_transactions->snapshot(root, address);
  }
}

extern "C" void moss_validation_demand_snapshot(PhysAddr root, VirtAddr address) noexcept {
  if (fault_transactions) {
    fault_transactions->snapshot(root, address);
  }
}

extern "C" void moss_validation_demand_committed(PhysAddr root, VirtAddr address, PhysAddr page) noexcept {
  if (fault_transactions) {
    fault_transactions->committed(root, address, page);
  }
}

void fault_contended(PhysAddr root) {
  if (fault_transactions) {
    fault_transactions->contended(root);
  }
}
} // namespace moss::test::validation
