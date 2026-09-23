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
#include "validation/core_cases.hpp"
#include "validation/isolation.hpp"
#include "validation/memory_cases.hpp"
#include "validation/memory_internal.hpp"
#include "validation/resources.hpp"
#include "validation/runtime.hpp"
#include "validation/runtime_state.hpp"
#include "validation/smp_cases.hpp"
#include "validation_internal.hpp"

#include "validation/uaccess.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;

namespace moss::test::validation {
namespace {
constexpr usize page_size = moss::kernel::PAGE_SIZE;
PagePressure *uaccess_pressure = nullptr;
usize uaccess_free_before = 0;
// Private controls following the isolation pair 50/51. Keep these operations
// synchronized with the real fork/pressure protocol in userspace/validation.c.
constexpr long COW_BEGIN = 52;
constexpr long COW_ARM = 53;
constexpr long COW_CHECK = 54;
constexpr long COW_RELEASE = 55;
constexpr long COW_SPLIT = 56;
constexpr long COW_FINISH = 57;

struct CowAllocationPressure {
  using Tables = mm::PageTableManager;
  using Pfa = mm::PageFrameAllocator;
  PagePressure pressure;
  LifecycleResources baseline;
  ProcessId parent = INVALID_PROCESS_ID;
  ProcessId child = INVALID_PROCESS_ID;
  VirtAddr address = 0;
  PhysAddr physical = 0;
  u64 contents = 0, parent_entry = 0, child_entry = 0;
  usize free_before = 0;
  bool armed = false, checked = false, released = false, split = false;

  static u64 stable_entry(u64 raw) {
    // Hardware may update access/dirty state independently of COW ownership.
    // Every software permission, physical-address and COW bit remains checked.
    u64 hardware_bits = mm::page_attr::AF;
#if !defined(MOSS_ARCH_ARM64)
    hardware_bits |= mm::page_attr::DIRTY;
#endif
    return raw & ~hardware_bits;
  }

  mm::PageTableEntry *leaf(process::Process &owner) const {
    // This private protocol keeps both processes in the same image until all
    // control calls finish. The returned PTE is borrowed only within that
    // quiescent interval; it is not a general concurrent-VM lookup API.
    auto as = owner.address_space();
    if (!ut::expect(as && as->allows_user_access(address, page_size, process::vma_flags::WRITE))) {
      return nullptr;
    }
    auto *entry = Tables::get_user_pte(as->pgd_phys, address);
    return ut::expect(entry && entry->is_valid() && (entry->raw & mm::page_attr::USER)) ? entry : nullptr;
  }

  bool begin(process::Process &owner, VirtAddr target) {
    address = target;
    parent = owner.pid();
    auto *entry = leaf(owner);
    if (!entry || !ut::expect((address & (page_size - 1)) == 0 && entry->is_writable() && !entry->is_cow())) {
      return false;
    }
    physical = entry->get_phys_addr();
    if (!ut::expect(Pfa::page_ref_get(physical) == 1)) {
      return false;
    }
    contents = memory_hash(phys_to_virt(physical), page_size);
    baseline = LifecycleResources::capture();
    return true;
  }

  bool arm(process::Process &owner) {
    // No retained Process owner may hide destruction after waitpid. The parent
    // remains alive by protocol; lookup owners only within each control call.
    auto peer = process::g_process_manager->find_process(parent);
    if (!ut::expect(!armed && peer && owner.parent_pid() == parent && owner.pid() != parent)) {
      return false;
    }
    auto *source = leaf(*peer);
    auto *target = leaf(owner);
    if (!source || !target ||
        !ut::expect(source->get_phys_addr() == physical && target->get_phys_addr() == physical && source->is_cow() &&
                    target->is_cow() && !source->is_writable() && !target->is_writable() &&
                    Pfa::page_ref_get(physical) == 2)) { // Exactly the real parent and child mappings.
      return false;
    }
    // The preceding page is the writable prefix of the partial-copy case.
    auto as = owner.address_space();
    auto *prefix = Tables::get_user_pte(as->pgd_phys, address - page_size);
    if (!ut::expect(prefix && prefix->is_valid() && prefix->is_writable() && !prefix->is_cow() &&
                    Pfa::page_ref_get(prefix->get_phys_addr()) == 1)) {
      return false;
    }
    child = owner.pid();
    parent_entry = stable_entry(source->raw);
    child_entry = stable_entry(target->raw);
    free_before = Pfa::get_memory_stats().free_pages;
    armed = ut::expect(pressure.acquire(0));
    return armed;
  }

  bool unchanged(process::Process &owner) {
    auto peer = process::g_process_manager->find_process(parent);
    if (!ut::expect(armed && !released && owner.pid() == child && peer)) {
      return false;
    }
    auto *source = leaf(*peer);
    auto *target = leaf(owner);
    bool valid = ut::expect(source && target && stable_entry(source->raw) == parent_entry &&
                            stable_entry(target->raw) == child_entry);
    valid =
        ut::expect(Pfa::page_ref_get(physical) == 2 && memory_hash(phys_to_virt(physical), page_size) == contents) &&
        valid;
    // Reading the same resident COW page must need no allocation. This exercises
    // the production input-copy path while output-copy allocation cannot succeed.
    u64 input = 0;
    valid = ut::expect(process::copy_from_user(&input, address, sizeof(input)) == 0 &&
                       input == *reinterpret_cast<const u64 *>(phys_to_virt(physical))) &&
            valid;
    valid = ut::expect(Pfa::get_memory_stats().free_pages == 0) && valid;
    checked = valid;
    return valid;
  }

  bool release(process::Process &owner) {
    const bool valid = unchanged(owner);
    pressure.release();
    released = true;
    return ut::expect(Pfa::get_memory_stats().free_pages == free_before) && valid;
  }

  bool verify_split(process::Process &owner) {
    auto peer = process::g_process_manager->find_process(parent);
    if (!ut::expect(checked && released && owner.pid() == child && peer)) {
      return false;
    }
    auto *source = leaf(*peer);
    auto *target = leaf(owner);
    if (!source || !target) {
      return false;
    }
    auto expected = *target;
    expected.raw = child_entry;
    expected.clear_cow();
    expected.make_writable();
    expected.set_page(target->get_phys_addr(), expected.raw & ~hal::mmu::PTE_ADDR_MASK);
    split = ut::expect(stable_entry(source->raw) == parent_entry && target->get_phys_addr() != physical &&
                       stable_entry(target->raw) == stable_entry(expected.raw) &&
                       Pfa::page_ref_get(target->get_phys_addr()) == 1 && Pfa::page_ref_get(physical) == 1 &&
                       memory_hash(phys_to_virt(physical), page_size) == contents);
    // Only the formerly shared data page may allocate during the retry.
    split = ut::expect(Pfa::get_memory_stats().free_pages + 1 == free_before) && split;
    return split;
  }

  bool finish(process::Process &owner, bool direct_fault) {
    bool valid = ut::expect(armed && owner.pid() == parent && !process::g_process_manager->find_process(child));
    // A direct user fault exits without releasing pressure. Recover it only
    // after the parent reaps the real child, then compare the full pre-fork
    // resource baseline. This also safely cleans a failed child-side oracle.
    pressure.release();
    auto *entry = leaf(owner);
    valid = ut::expect(entry && stable_entry(entry->raw) == parent_entry && Pfa::page_ref_get(physical) == 1 &&
                       memory_hash(phys_to_virt(physical), page_size) == contents) &&
            valid;
    valid = ut::expect(direct_fault || (checked && released && split)) && valid;
    return ut::expect(LifecycleResources::capture() == baseline) && valid;
  }
};

CowAllocationPressure *cow_pressure = nullptr;

} // namespace

long uaccess_control(long op, long arg1, long arg2) {
  if (op >= COW_BEGIN && op <= COW_FINISH && ut::same_id(selection, "users.uaccess") && affinity_valid() &&
      (ut::same_id(active_case, "cow_copy_fault") || ut::same_id(active_case, "cow_partial_read") ||
       ut::same_id(active_case, "cow_user_fault"))) {
    auto *thread = process::CfsScheduler::get_current_task();
    auto owner = thread ? process::g_process_manager->find_process(thread->owner_pid) : shared_ptr<process::Process>{};
    if (!ut::expect(static_cast<bool>(owner))) {
      return 0;
    }
    if (op == COW_BEGIN) {
      if (!ut::expect(!cow_pressure)) {
        return 0;
      }
      cow_pressure = new CowAllocationPressure{};
      if (ut::expect(cow_pressure != nullptr) && cow_pressure->begin(*owner, static_cast<VirtAddr>(arg1))) {
        return 1;
      }
      delete cow_pressure;
      cow_pressure = nullptr;
      return 0;
    }
    if (!ut::expect(cow_pressure && cow_pressure->address == static_cast<VirtAddr>(arg1))) {
      return 0;
    }
    if (op == COW_ARM) {
      return cow_pressure->arm(*owner);
    }
    if (op == COW_CHECK) {
      return cow_pressure->unchanged(*owner);
    }
    if (op == COW_RELEASE) {
      return cow_pressure->release(*owner);
    }
    if (op == COW_SPLIT) {
      return cow_pressure->verify_split(*owner);
    }
    const bool valid = cow_pressure->finish(*owner, ut::same_id(active_case, "cow_user_fault"));
    delete cow_pressure;
    cow_pressure = nullptr;
    return valid;
  }
  if ((op == 12 || op == 13) && ut::same_id(selection, "users.uaccess") && active_case) {
    auto *thread = process::CfsScheduler::get_current_task();
    auto owner = thread && process::g_process_manager ? process::g_process_manager->find_process(thread->owner_pid)
                                                      : shared_ptr<process::Process>{};
    auto as = owner ? owner->address_space() : shared_ptr<process::AddressSpace>{};
    const u64 address = static_cast<u64>(arg1);
    if (!ut::expect(as && as->allows_user_access(address, 8, process::vma_flags::WRITE))) {
      return 0;
    }
    auto *leaf = mm::PageTableManager::get_user_pte(as->pgd_phys, address);
    ut::expect(!leaf || !leaf->is_valid());
    usize released_pages = 0;
    if (op == 12) {
      if (!ut::expect(uaccess_pressure == nullptr)) {
        return 0;
      }
      uaccess_pressure = new PagePressure{};
      uaccess_free_before = mm::PageFrameAllocator::get_memory_stats().free_pages;
      if (ut::expect(uaccess_pressure && uaccess_pressure->acquire(0))) {
        return 1;
      }
    } else {
      ut::expect(uaccess_pressure != nullptr);
      released_pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
      // The sigframe test reaps its child before releasing the pressure.
      // Account for those newly freed, non-fixture pages without relaxing the
      // zero-free-page invariant of the other fault cases.
      if (arg2 != 1 || !ut::same_id(active_case, "sigframe_fault")) {
        ut::expect(released_pages == 0);
      }
    }
    delete uaccess_pressure;
    uaccess_pressure = nullptr;
    ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == uaccess_free_before + released_pages);
    return 0;
  }
  if (op == 14 && ut::same_id(selection, "users.uaccess") && ut::same_id(active_case, "partial_read") &&
      uaccess_pressure) {
    u8 bytes[8];
    for (auto &byte : bytes) {
      byte = 0xa5;
    }
    bool valid = ut::expect(process::copy_from_user(bytes, static_cast<u64>(arg1), sizeof(bytes)) == 4);
    for (usize i = 0; i < sizeof(bytes); ++i) {
      valid = ut::expect(bytes[i] == (i < 4 ? i : 0)) && valid;
    }
    return valid ? 1 : 0;
  }
  invalid_control();
}
} // namespace moss::test::validation
