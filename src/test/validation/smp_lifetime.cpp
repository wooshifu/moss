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
// Exercise the production Process getter/replacement boundary on distinct CPUs.
// The target is deliberately not scheduled: software readers must retain its
// tables independently of both Process lifetime and any active hardware root.
struct AddressSpaceReaders {
  unique_ptr<process::Process> target;
  PhysAddr old_root = 0, new_root = 0, data = 0;
  u16 old_asid = 0, new_asid = 0;
  u64 contents = 0;
  u32 phase = 0, arrived = 0;
  bool peer_ok = false;
  using Pfa = mm::PageFrameAllocator;
  using Tables = mm::PageTableManager;

  template <typename Condition> static void require(Condition valid) {
    if (!ut::expect(static_cast<Condition &&>(valid))) {
      end_case();
      finish("address_space_setup");
    }
  }

  bool old_alive() const { return Pfa::page_ref_get(old_root) == 1 && Pfa::page_ref_get(data) == 1; }

  bool peer() {
    auto *thread = process::CfsScheduler::get_current_task();
    // CPU1 is selected by bit 1 in the affinity mask, not by mask value 1.
    peer_ok = arch::get_current_cpu_id() == 1 && thread && thread->cpu_affinity_mask.low_word() == 2;
    // arrived acknowledges reader milestones, phase releases writer milestones.
    // Both are monotonic; the host's normal case deadline bounds a stuck peer.
    __atomic_store_n(&arrived, 1U, __ATOMIC_RELEASE);
    wait_for_phase(phase, 1);
    {
      auto held = target->address_space();
      peer_ok = peer_ok && held && held->pgd_phys == old_root;
      __atomic_store_n(&arrived, 2U, __ATOMIC_RELEASE);
      wait_for_phase(phase, 2);
      // A broken implementation may have freed both pages. Detect that through
      // PFA metadata before dereferencing either the old object or its tables.
      const bool alive = old_alive();
      peer_ok = peer_ok && alive;
      if (alive && held) {
        auto *pte = Tables::get_user_pte(held->pgd_phys, process::user_layout::CODE_BASE);
        peer_ok = peer_ok && held->pgd_phys == old_root && held->asid == old_asid && pte &&
                  pte->get_phys_addr() == data && memory_hash(phys_to_virt(data), page_size) == contents &&
                  held->allows_user_access(process::user_layout::CODE_BASE, page_size, process::vma_flags::WRITE);
      }
      auto latest = target->address_space();
      peer_ok = peer_ok && latest && latest->pgd_phys == new_root && latest->asid == new_asid;
      __atomic_store_n(&arrived, 3U, __ATOMIC_RELEASE);
      wait_for_phase(phase, 3);
      const bool latest_alive = Pfa::page_ref_get(new_root) == 1;
      peer_ok = peer_ok && old_alive() && latest_alive;
      if (latest_alive && latest) {
        peer_ok = peer_ok && latest->pgd_phys == new_root;
      }
    }
    __atomic_store_n(&arrived, 4U, __ATOMIC_RELEASE);
    // Do not exit/reap the real worker while the owner checks fixture cleanup.
    wait_for_phase(phase, 4);
    return peer_ok;
  }

  void owner() {
    wait_for_phase(arrived, 1);
    const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    const auto pages = Pfa::get_memory_stats().free_pages;
    target = make_unique<process::Process>(INVALID_PROCESS_ID);
    auto original = process::user_space::create_user_address_space();
    auto replacement = process::user_space::create_user_address_space();
    require(original && replacement);
    old_root = (*original)->pgd_phys;
    new_root = (*replacement)->pgd_phys;
    old_asid = (*original)->asid;
    new_asid = (*replacement)->asid;
    require((*original)->add_vma(process::user_layout::CODE_BASE, process::user_layout::CODE_BASE + page_size,
                                 process::vma_flags::READ | process::vma_flags::WRITE));
    auto allocated = mm::allocate_pages(0);
    require(allocated.has_value());
    data = *allocated;
    // An arbitrary nonzero pattern detects clobbering or recycled/demand-zero
    // storage, independently of the table and allocator-reference assertions.
    auto *bytes = reinterpret_cast<u8 *>(phys_to_virt(data));
    for (usize index = 0; index < page_size; ++index) {
      bytes[index] = 0x5a;
    }
    contents = memory_hash(phys_to_virt(data), page_size);
    require(Tables::map_user_page(old_root, process::user_layout::CODE_BASE, data, mm::page_perms::USER_RW));
    require(target->set_address_space(moss::move(*original)));
    __atomic_store_n(&phase, 1U, __ATOMIC_RELEASE);
    wait_for_phase(arrived, 2);
    require(target->set_address_space(moss::move(*replacement)));
    ut::expect(old_alive());
    logging::klog::info("Address-space replacement: old root refs {}, data refs {}", Pfa::page_ref_get(old_root),
                        Pfa::page_ref_get(data));
    __atomic_store_n(&phase, 2U, __ATOMIC_RELEASE);
    wait_for_phase(arrived, 3);
    target->clear_address_space();
    ut::expect(!target->address_space());
    target->clear_address_space(); // Exit teardown is idempotent, even with held readers.
    ut::expect(old_alive() && Pfa::page_ref_get(new_root) == 1);
    {
      auto other = process::user_space::create_user_address_space();
      require(other.has_value());
      // Neither detached version's translation tag may be leased to a new
      // image while the peer still owns it, even after Process detachment.
      ut::expect((*other)->asid != old_asid && (*other)->asid != new_asid);
    }
    target.reset();
    ut::expect(old_alive() && Pfa::page_ref_get(new_root) == 1);
    __atomic_store_n(&phase, 3U, __ATOMIC_RELEASE);
    wait_for_phase(arrived, 4);
    ut::expect(peer_ok && affinity_valid());
    ut::expect(Pfa::page_ref_get(old_root) == 0 && Pfa::page_ref_get(new_root) == 0 && Pfa::page_ref_get(data) == 0);
    ut::expect(Pfa::get_memory_stats().free_pages == pages);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    __atomic_store_n(&phase, 4U, __ATOMIC_RELEASE);
  }
};
AddressSpaceReaders *address_space_readers = nullptr;

struct HardwareRootLifetime {
  using Scheduler = process::CfsScheduler;
  using Pfa = mm::PageFrameAllocator;
  using Tables = mm::PageTableManager;
  static constexpr VirtAddr address = process::user_layout::CODE_BASE;
  // Cover both user->user replacement and detached user->kernel retirement.
  static constexpr u32 scenario_count = 2;
  static constexpr const char *names[scenario_count] = {"hardware_root", "kernel_root"};
  // A distinct nonzero byte distinguishes the resident old page from the
  // demand-zero neighbour and any replacement image's contents.
  static constexpr u8 payload = 0x69;
  unique_ptr<process::Process> target;
  PhysAddr root = 0, data = 0;
  u16 asid = 0;
  // CPU0 owns publication, CPU1 owns the temporary hardware root. Milestones
  // are 1=install prepared tree, 2=publication detached, 3=hardware retired.
  // Release/acquire handshakes make the final allocator checks quiescent.
  u32 phase = 0, arrived = 0, early = 0, retirements = 0;
  bool peer_ok = false;

  void retiring(PhysAddr retiring_root) {
    if (retiring_root != root) {
      return;
    }
    __atomic_fetch_add(&retirements, 1U, __ATOMIC_RELEASE);
    if (arch::get_current_cpu_id() == 1) {
      // Destruction must happen after the hardware write, not merely after
      // swapping the software owner. Stop before freeing an active tree.
      if (!ut::expect(!tlb_active(root, asid))) {
        end_case();
        finish("active_root_retired_early");
      }
    } else if (__atomic_load_n(&arrived, __ATOMIC_ACQUIRE) < 3) {
      // Make a missing CPU lease fail safely: the destructor has started but
      // has not freed anything. Ask the peer to switch away before continuing,
      // rather than probing a freed table through the MMU in the negative run.
      __atomic_store_n(&early, 1U, __ATOMIC_RELEASE);
      __atomic_store_n(&phase, 2U, __ATOMIC_RELEASE);
      wait_for_phase(arrived, 3);
    }
  }

  bool peer(bool kernel_retirement) {
    auto current = process::current_process();
    auto saved = current ? current->address_space() : shared_ptr<process::AddressSpace>{};
    smp_require(saved && arch::get_current_cpu_id() == 1);
    __atomic_store_n(&arrived, 1U, __ATOMIC_RELEASE);
    wait_for_phase(phase, 1);
    const bool interrupts = arch::interrupts_enabled();
    arch::disable_interrupts();
    {
      auto incoming = target->address_space();
      smp_require(incoming && incoming->pgd_phys == root);
      Scheduler::use_address_space(moss::move(incoming));
    }
    // No task-stack or fixture shared_ptr owns the old tree now. Only the
    // Process publication and the CPU's installed-root lease may retain it.
    u8 byte = 0;
    peer_ok = tlb_active(root, asid) && tlb_read(byte, address) && byte == payload;
    __atomic_store_n(&arrived, 2U, __ATOMIC_RELEASE);
    wait_for_phase(phase, 2);
    if (__atomic_load_n(&early, __ATOMIC_ACQUIRE) == 0) {
      peer_ok = tlb_active(root, asid) && tlb_read(byte, address) && byte == payload && peer_ok;
      // This page belongs only to the detached old image and is not resident.
      // A real raw-copy fault must resolve against the CPU's owned root, not
      // the replacement Process version or this worker's ordinary image.
      peer_ok = tlb_read(byte, address + page_size) && byte == 0 && peer_ok;
    } else {
      peer_ok = false;
    }
    if (kernel_retirement) {
      Scheduler::use_kernel_address_space();
      peer_ok = !Scheduler::active_address_space() && peer_ok;
    }
    Scheduler::use_address_space(saved);
    __atomic_store_n(&arrived, 3U, __ATOMIC_RELEASE);
    wait_for_phase(phase, 3);
    if (interrupts) {
      arch::enable_interrupts();
    }
    return peer_ok;
  }

  void owner(bool detach) {
    wait_for_phase(arrived, 1);
    const auto pages = Pfa::get_memory_stats().free_pages;
    const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    target = make_unique<process::Process>(INVALID_PROCESS_ID);
    auto original = process::user_space::create_user_address_space();
    smp_require(original);
    root = (*original)->pgd_phys;
    asid = (*original)->asid;
    // One resident page plus one demand-only neighbour exercises both stable
    // hardware translations and a fault after Process ownership was replaced.
    constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE | process::vma_flags::DEMAND_ZERO;
    smp_require((*original)->add_vma(address, address + 2 * page_size, flags));
    smp_require((*original)->copy_to_user(address, &payload, sizeof(payload)) == 0);
    auto *leaf = Tables::get_user_pte(root, address);
    smp_require(leaf);
    data = leaf->get_phys_addr();
    smp_require(target->set_address_space(moss::move(*original)));
    __atomic_store_n(&phase, 1U, __ATOMIC_RELEASE);
    wait_for_phase(arrived, 2);
    if (detach) {
      target->clear_address_space();
      target.reset(); // Process destruction must not remove a remote CPU's pin.
    } else {
      auto replacement = process::user_space::create_user_address_space();
      smp_require(replacement);
      smp_require(target->set_address_space(moss::move(*replacement)));
    }
    ut::expect(__atomic_load_n(&early, __ATOMIC_ACQUIRE) == 0);
    ut::expect(__atomic_load_n(&retirements, __ATOMIC_ACQUIRE) == 0);
    ut::expect(Pfa::page_ref_get(root) == 1 && Pfa::page_ref_get(data) == 1);
    {
      auto other = process::user_space::create_user_address_space();
      smp_require(other);
      ut::expect((*other)->asid != asid);
    }
    __atomic_store_n(&phase, 2U, __ATOMIC_RELEASE);
    wait_for_phase(arrived, 3);
    ut::expect(peer_ok && affinity_valid());
    ut::expect(__atomic_load_n(&retirements, __ATOMIC_ACQUIRE) == 1);
    ut::expect(Pfa::page_ref_get(root) == 0 && Pfa::page_ref_get(data) == 0);
    // Stop observing this physical number before allocator reuse could make
    // a later, unrelated address space look like another retirement of it.
    root = 0;
    target.reset();
    ut::expect(Pfa::get_memory_stats().free_pages == pages);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    __atomic_store_n(&phase, 3U, __ATOMIC_RELEASE);
  }
};
HardwareRootLifetime *hardware_root_lifetime = nullptr;

static void empty_case() {}
void register_lifetime_smp() {
  ut::register_suite("mm.lifetime", [] {
    ut::register_test("held_readers", empty_case);
    for (const auto *name : HardwareRootLifetime::names) {
      ut::register_test(name, empty_case);
    }
  });
}

long start_lifetime_smp() {
  const char *selection = selected_suite();
  if (ut::same_id(selection, "mm.lifetime")) {
    start_case("held_readers");
    smp_require(g_num_cpus >= 2);
    address_space_readers = new AddressSpaceReaders();
    hardware_root_lifetime = new HardwareRootLifetime[HardwareRootLifetime::scenario_count];
    return 3; // Existing mode pins a forked reader to CPU1 and its owner to CPU0.
  }
  return 0;
}

long control_lifetime_smp(long op, long arg1, [[maybe_unused]] long arg2) {
  const char *selection = selected_suite();
  const char *active_case = running_case();
  if (ut::same_id(selection, "mm.lifetime") && active_case && address_space_readers) {
    if (op == 7 && arg1 == 0) {
      arch::enable_interrupts();
      bool readers_ok = address_space_readers->peer();
      for (u32 index = 0; index < HardwareRootLifetime::scenario_count; ++index) {
        readers_ok = hardware_root_lifetime[index].peer(index != 0) && readers_ok;
      }
      return readers_ok ? 1 : 0;
    }
    if (op == 8) {
      smp_require(arg1 && affinity_valid());
      arch::enable_interrupts();
      address_space_readers->owner();
      for (u32 index = 0; index < HardwareRootLifetime::scenario_count; ++index) {
        end_case();
        start_case(HardwareRootLifetime::names[index]);
        hardware_root_lifetime[index].owner(index != 0);
      }
      return 0;
    }
    if (op == 9) {
      ut::expect(arg1 && affinity_valid());
      delete address_space_readers;
      address_space_readers = nullptr;
      delete[] hardware_root_lifetime;
      hardware_root_lifetime = nullptr;
      end_case();
      finish();
    }
  }
  invalid_control();
}
extern "C" void moss_validation_address_space_retiring(PhysAddr root) noexcept {
  if (hardware_root_lifetime) {
    for (u32 index = 0; index < HardwareRootLifetime::scenario_count; ++index) {
      hardware_root_lifetime[index].retiring(root);
    }
  }
}

} // namespace moss::test::validation
