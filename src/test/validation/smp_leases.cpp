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
// The target roots are real but never installed in hardware. This isolates
// software page ownership from the still-separate remote-TLB contract.
struct UserPageLeases {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  static constexpr VirtAddr address = process::user_layout::CODE_BASE;
  // Read versus unmap, then write versus fork. Each round publishes three
  // milestones: ready/borrowed, contending, and mutation/cleanup completed.
  static constexpr u32 scenarios = 2, milestones = 3;
  // Distinct arbitrary patterns identify original bytes and copied output.
  static constexpr u8 original_byte = 0x5a, payload_byte = 0x3c;
  shared_ptr<process::AddressSpace> target, clone;
  u8 buffer[page_size]{};
  PhysAddr original = 0;
  u64 expected = 0, cloned_contents = 0;
  u32 phase = 0, arrived = 0, round_base = 0, borrows = 0, contentions = 0;
  bool write = false, armed = false, peer_ok = false;

  void borrowed(PhysAddr root, VirtAddr cursor, PhysAddr page, bool writing) {
    if (!armed || !target || root != target->pgd_phys || cursor != address) {
      return;
    }
    armed = false;
    ++borrows;
    __atomic_store_n(&phase, round_base + 1, __ATOMIC_RELEASE);
    wait_for_phase(arrived, round_base + 2);
    const bool owned = page == original && Pfa::page_ref_get(page) == 1;
    if (!ut::expect(owned && writing == write && arch::get_current_cpu_id() == 0)) {
      // A missing lease can already have freed or shared this frame. Report
      // the red result before copying through a dangling or COW-bypassing alias.
      end_case();
      finish("user_page_ownership");
    }
  }

  void contended(PhysAddr root) {
    if (target && root == target->pgd_phys && arch::get_current_cpu_id() == 1) {
      ++contentions;
      // The real failed try_lock proves the competing mutation cannot enter
      // while copying owns the frame. Allow the owner to finish its copy.
      __atomic_store_n(&arrived, round_base + 2, __ATOMIC_RELEASE);
    }
  }

  bool peer() {
    peer_ok = arch::get_current_cpu_id() == 1;
    for (u32 round = 0; round < scenarios; ++round) {
      const auto base = round * milestones;
      __atomic_store_n(&arrived, base + 1, __ATOMIC_RELEASE);
      wait_for_phase(phase, base + 1);
      {
        auto transaction = target->lock_vm();
        if (write) {
          peer_ok = static_cast<bool>(Tables::clone_user_page_tables(target->pgd_phys, clone->pgd_phys)) && peer_ok;
          const auto *pte = Tables::get_user_pte(clone->pgd_phys, address);
          peer_ok = pte && pte->is_valid() && peer_ok;
          if (peer_ok) {
            // Observe at clone completion, before any later owner write, so
            // the test distinguishes serialization from an eventual match.
            cloned_contents = memory_hash(phys_to_virt(pte->get_phys_addr()), page_size);
          }
        } else {
          Tables::unmap_user_page(target->pgd_phys, address);
          peer_ok = target->remove_vma(address, address + page_size) && peer_ok;
        }
        // Also release a lock-disabled red run that never saw contention.
        __atomic_store_n(&arrived, base + 2, __ATOMIC_RELEASE);
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
      write = round != 0;
      borrows = 0;
      contentions = 0;
      if (write) {
        end_case();
        start_case("copy_fork");
      }
      owner_round();
      __atomic_store_n(&phase, round_base + 3, __ATOMIC_RELEASE);
    }
  }

  void owner_round() {
    const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    const auto pages = Pfa::get_memory_stats().free_pages;
    auto created = process::user_space::create_user_address_space();
    smp_require(created.has_value());
    target = moss::move(*created);
    constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE;
    smp_require(target->add_vma(address, address + page_size, flags));
    if (write) {
      auto child = process::user_space::create_user_address_space();
      smp_require(child.has_value());
      clone = moss::move(*child);
      smp_require(clone->add_vma(address, address + page_size, flags));
    }
    auto allocated = mm::allocate_pages(0);
    smp_require(allocated.has_value());
    original = *allocated;
    auto *bytes = reinterpret_cast<u8 *>(phys_to_virt(original));
    for (usize index = 0; index < page_size; ++index) {
      bytes[index] = original_byte;
      buffer[index] = payload_byte;
    }
    expected = memory_hash(write ? reinterpret_cast<VirtAddr>(buffer) : phys_to_virt(original), page_size);
    smp_require(Tables::map_user_page(target->pgd_phys, address, original, mm::page_perms::USER_RW));
    armed = true;
    const auto remaining =
        write ? target->copy_to_user(address, buffer, page_size) : target->copy_from_user(buffer, address, page_size);
    wait_for_phase(arrived, round_base + 3);
    ut::expect(remaining == 0 && peer_ok && affinity_valid());
    ut::expect(borrows == 1 && contentions == 1);
    if (write) {
      const auto *parent_pte = Tables::get_user_pte(target->pgd_phys, address);
      const auto *child_pte = Tables::get_user_pte(clone->pgd_phys, address);
      const bool shared = parent_pte && child_pte && parent_pte->get_phys_addr() == original &&
                          child_pte->get_phys_addr() == original && Pfa::page_ref_get(original) == 2;
      smp_require(shared);
      ut::expect(parent_pte->is_cow() && child_pte->is_cow() && !parent_pte->is_writable() &&
                 !child_pte->is_writable());
      ut::expect(cloned_contents == expected && memory_hash(phys_to_virt(original), page_size) == expected);
      // A subsequent write must split the shared frame, not modify the child.
      constexpr u8 private_byte = 0xc3; // Different from both initial patterns.
      ut::expect(target->copy_to_user(address, &private_byte, sizeof(private_byte)) == 0);
      parent_pte = Tables::get_user_pte(target->pgd_phys, address);
      const PhysAddr private_page = parent_pte ? parent_pte->get_phys_addr() : 0;
      smp_require(private_page && private_page != original && Pfa::page_ref_get(private_page) == 1 &&
                  Pfa::page_ref_get(original) == 1);
      ut::expect(parent_pte->is_writable() && !parent_pte->is_cow());
      ut::expect(*reinterpret_cast<const u8 *>(phys_to_virt(private_page)) == private_byte);
      ut::expect(memory_hash(phys_to_virt(original), page_size) == expected);
    } else {
      const auto *pte = Tables::get_user_pte(target->pgd_phys, address);
      ut::expect((!pte || !pte->is_valid()) && !target->find_vma(address) && Pfa::page_ref_get(original) == 0);
      ut::expect(memory_hash(reinterpret_cast<VirtAddr>(buffer), page_size) == expected);
      ut::expect(target->copy_from_user(buffer, address, page_size) == page_size);
      bool zeroed = true;
      for (const u8 byte : buffer) {
        zeroed = byte == 0 && zeroed;
      }
      ut::expect(zeroed);
    }
    clone.reset();
    target.reset();
    ut::expect(Pfa::get_memory_stats().free_pages == pages);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  }
};
UserPageLeases *user_page_leases = nullptr;

static void empty_case() {}
void register_leases_smp() {
  ut::register_suite("mm.uaccess", [] {
    ut::register_test("copy_unmap", empty_case);
    ut::register_test("copy_fork", empty_case);
  });
}

long start_leases_smp() {
  const char *selection = selected_suite();
  if (ut::same_id(selection, "mm.uaccess")) {
    start_case("copy_unmap");
    smp_require(g_num_cpus >= 2);
    user_page_leases = new UserPageLeases();
    return 3; // Share the CPU0/CPU1 fork, affinity, control and reap protocol.
  }
  return 0;
}

long control_leases_smp(long op, long arg1, [[maybe_unused]] long arg2) {
  const char *selection = selected_suite();
  const char *active_case = running_case();
  if (ut::same_id(selection, "mm.uaccess") && active_case && user_page_leases) {
    if (op == 7 && arg1 == 0) {
      arch::enable_interrupts();
      return user_page_leases->peer() ? 1 : 0;
    }
    if (op == 8) {
      smp_require(arg1 && affinity_valid());
      arch::enable_interrupts();
      user_page_leases->owner();
      return 0;
    }
    if (op == 9) {
      ut::expect(arg1 && affinity_valid());
      delete user_page_leases;
      user_page_leases = nullptr;
      end_case();
      finish();
    }
  }
  invalid_control();
}
extern "C" void moss_validation_user_page(PhysAddr root, VirtAddr address, PhysAddr page, bool write) noexcept {
  if (user_page_leases) {
    user_page_leases->borrowed(root, address, page, write);
  }
}

void leases_contended(PhysAddr root) {
  if (user_page_leases) {
    user_page_leases->contended(root);
  }
}
} // namespace moss::test::validation
