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
#include "validation/process_control.hpp"
#include "validation/resources.hpp"
#include "validation/runtime.hpp"
#include "validation/runtime_state.hpp"
#include "validation/smp_cases.hpp"
#include "validation/timer_control.hpp"
#include "validation/uaccess.hpp"
#include "validation_internal.hpp"

#include "validation/exec_control.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;

namespace moss::test::validation {
namespace {
struct ExecAllocationPressure {
  LifecycleResources baseline;
  HeapPressure heap;
  process::AddressSpace *original = nullptr;
  PhysAddr root = 0;
  u64 root_hash = 0;
  char name[16]{}; // Matches Process's 15-byte comm plus terminating NUL.
  unsigned stage = 0, vmas = 0;
  bool holding = false, exhausted = false;
};
ExecAllocationPressure *exec_allocation_pressure = nullptr;
// Validation controls 48/49 are the next unused pair in this private protocol.
// Six stages cover arguments, both SharedPtr allocations, image bytes, the
// address space and a partially populated VMA list; keep userspace in sync.
inline constexpr long EXEC_HEAP_PRESSURE_ARM = 48;
inline constexpr long EXEC_HEAP_PRESSURE_RELEASE = 49;
inline constexpr long EXEC_REGISTER_DORMANT_PEER = 52;
inline constexpr unsigned EXEC_HEAP_ALLOCATION_STAGES = 6;
inline constexpr unsigned EXEC_VMA_ALLOCATION_STAGE = 5;
PagePressure *exec_pressure = nullptr;
LifecycleResources exec_baseline;
process::AddressSpace *exec_original = nullptr;
PhysAddr exec_root = 0;
u64 exec_root_hash = 0;
char exec_name[16]{};
} // namespace

extern "C" void moss_validation_exec_allocation(unsigned stage, bool entering, usize allocation_size) noexcept {
  auto *probe = exec_allocation_pressure;
  if (!probe || probe->stage != stage) {
    return;
  }
  if (entering) {
    if (probe->holding || probe->exhausted) {
      return;
    }
    // The final stage is the VMA-node boundary. Let one node become owned first so the
    // failure proves cleanup of a partially prepared address space.
    if (stage == EXEC_VMA_ALLOCATION_STAGE && probe->vmas++ == 0) {
      return;
    }
    probe->holding = true;
    probe->exhausted = ut::expect(probe->heap.acquire(allocation_size));
  } else if (probe->holding) {
    probe->heap.release();
    probe->holding = false;
  }
}

unsigned exec_source_swaps = 0;
bool exec_registration_refused = false;

extern "C" void moss_validation_exec_source_snapshot(PhysAddr root, VirtAddr argv) noexcept {
  if (!ut::same_id(selection, "users.exec")) {
    return;
  }
  if (ut::same_id(active_case, "registration_gate")) {
    auto owner = process::current_process();
    auto *peer = owner ? process::Thread::try_create(process::Process::allocate_thread_id(), owner->pid()) : nullptr;
    if (!ut::expect(owner && peer)) {
      return;
    }
    auto registered = owner->register_thread(peer);
    exec_registration_refused =
        !registered && registered.error() == ErrorCode::InvalidState && owner->thread_count() == 1;
    if (!registered) {
      delete peer;
    }
    return;
  }
  if (!ut::same_id(active_case, "source_version")) {
    return;
  }
  auto owner = process::current_process();
  auto source = owner ? owner->address_space() : shared_ptr<process::AddressSpace>{};
  const VirtAddr page = argv & ~(VirtAddr{page_size} - 1);
  if (!ut::expect(source && source->pgd_phys == root && argv - page <= page_size - sizeof(VirtAddr))) {
    return;
  }
  auto replacement = process::user_space::create_user_address_space();
  auto frame = mm::allocate_pages(0);
  if (!ut::expect(replacement && frame)) {
    if (frame) {
      (void)mm::free_pages(*frame, 0);
    }
    return;
  }
  auto *bytes = reinterpret_cast<u8 *>(phys_to_virt(*frame));
  if (!ut::expect(source->copy_from_user(bytes, page, page_size) == 0)) {
    (void)mm::free_pages(*frame, 0);
    return;
  }
  VirtAddr name = 0;
  __builtin_memcpy(&name, bytes + argv - page, sizeof(name));
  if (!ut::expect(name >= page && name - page <= page_size - sizeof("fail"))) {
    (void)mm::free_pages(*frame, 0);
    return;
  }
  __builtin_memcpy(bytes + name - page, "fail", sizeof("fail"));
  if (!ut::expect(
          (*replacement)
              ->add_vma(page, page + page_size, process::vma_flags::READ | process::vma_flags::WRITE,
                        process::VmaType::MMAP) &&
          mm::PageTableManager::map_user_page((*replacement)->pgd_phys, page, *frame, mm::page_perms::USER_RW))) {
    (void)mm::free_pages(*frame, 0);
    return;
  }
  // The syscall remains on its retained old hardware root until exec commits;
  // no user instruction runs from this deliberately incomplete replacement.
  (void)owner->set_address_space(moss::move(*replacement));
  ++exec_source_swaps;
}

long exec_control(long op, long arg1, [[maybe_unused]] long arg2) {
  if (op == EXEC_REGISTER_DORMANT_PEER && ut::same_id(selection, "users.exec") &&
      ut::same_id(active_case, "shared_thread_gate") && affinity_valid()) {
    auto owner = process::current_process();
    auto *peer = owner ? process::Thread::try_create(process::Process::allocate_thread_id(), owner->pid()) : nullptr;
    if (!peer) {
      return 0;
    }
    if (!owner->register_thread(peer)) {
      delete peer;
      return 0;
    }
    // The peer is registered but never enqueued, so this probes exec admission
    // without running an unsupported shared user-root or second user stack.
    return owner->thread_count() == 2 ? 1 : 0;
  }
  if (ut::same_id(selection, "users.exec") && ut::same_id(active_case, "mutable_snapshot_rollback") &&
      affinity_valid()) {
    auto owner = process::current_process();
    auto as = owner ? owner->address_space() : shared_ptr<process::AddressSpace>{};
    if (op == EXEC_HEAP_PRESSURE_ARM && !exec_allocation_pressure && as && arg1 >= 0 &&
        arg1 < EXEC_HEAP_ALLOCATION_STAGES) {
      // Capture before allocating the probe so release can require an exact
      // return to the caller's pre-injection ownership state.
      const auto baseline = LifecycleResources::capture();
      auto *probe = new ExecAllocationPressure{};
      if (!probe) {
        return 0;
      }
      probe->baseline = baseline;
      probe->original = as.get();
      probe->root = as->pgd_phys;
      probe->root_hash = page_table_hash(probe->root);
      __builtin_memcpy(probe->name, owner->name(), sizeof(probe->name));
      probe->stage = static_cast<unsigned>(arg1);
      exec_allocation_pressure = probe;
      return 1;
    }
    if (op == EXEC_HEAP_PRESSURE_RELEASE && exec_allocation_pressure) {
      auto *probe = exec_allocation_pressure;
      bool valid = ut::expect(arg1 >= 0 && static_cast<unsigned>(arg1) == probe->stage);
      valid = ut::expect(probe->exhausted && !probe->holding) && valid;
      if (probe->stage == EXEC_VMA_ALLOCATION_STAGE) {
        valid = ut::expect(probe->vmas >= 2) && valid;
      }
      valid = ut::expect(as && as.get() == probe->original && as->pgd_phys == probe->root &&
                         page_table_hash(probe->root) == probe->root_hash &&
                         __builtin_memcmp(probe->name, owner->name(), sizeof(probe->name)) == 0) &&
              valid;
      const auto baseline = probe->baseline;
      delete probe;
      exec_allocation_pressure = nullptr;
      valid = ut::expect(LifecycleResources::capture() == baseline) && valid;
      return valid ? 1 : 0;
    }
  }
  if (ut::same_id(selection, "users.exec") && ut::same_id(active_case, "allocation_rollback") && affinity_valid()) {
    const long stages = mm::PageTableManager::is_user_range(1ULL << 39, page_size) ? 5 : 4;
    if (op == 35) {
      return stages; // Root, stack leaf, then every missing intermediate table.
    }
    auto *thread = process::CfsScheduler::get_current_task();
    auto owner = process::g_process_manager->find_process(thread->owner_pid);
    auto as = owner ? owner->address_space() : shared_ptr<process::AddressSpace>{};
    if (op == 36 && as && arg1 >= 0 && arg1 < stages) {
      if (!exec_pressure) {
        if (arg1 != 0) {
          return 0;
        }
        exec_baseline = LifecycleResources::capture();
        exec_original = as.get();
        exec_root = as->pgd_phys;
        exec_root_hash = page_table_hash(exec_root);
        __builtin_memcpy(exec_name, owner->name(), sizeof(exec_name));
        exec_pressure = new PagePressure{};
        // Exhaust the PFA once while retaining one allowance page per native
        // page-table stage. Each completed failure releases one more allowance,
        // producing the same 0..N-page budgets without rescanning all RAM.
        const bool ready = ut::expect(exec_pressure && exec_pressure->acquire(static_cast<usize>(stages)));
        if (!ready) {
          delete exec_pressure;
          exec_pressure = nullptr;
          return 0;
        }
      }
      const auto exposed_pages = static_cast<long>(static_cast<usize>(stages) - exec_pressure->count);
      if (!ut::expect(arg1 == exposed_pages)) {
        delete exec_pressure;
        exec_pressure = nullptr;
        return 0;
      }
      return 1;
    }
    if (op == 37 && exec_pressure) {
      bool valid = ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == static_cast<usize>(arg1));
      valid =
          ut::expect(arg1 >= 0 && arg1 < stages && exec_pressure->count == static_cast<usize>(stages - arg1)) && valid;
      valid = ut::expect(as && as.get() == exec_original && as->pgd_phys == exec_root &&
                         page_table_hash(exec_root) == exec_root_hash &&
                         __builtin_memcmp(exec_name, owner->name(), sizeof(exec_name)) == 0) &&
              valid;
      if (valid && arg1 + 1 < stages) {
        exec_pressure->give_one();
        valid = ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == static_cast<usize>(arg1 + 1));
        if (valid) {
          return 1;
        }
      }
      delete exec_pressure;
      exec_pressure = nullptr;
      valid = ut::expect(LifecycleResources::capture() == exec_baseline) && valid;
      return valid && arg1 + 1 == stages ? 1 : 0;
    }
  }
  invalid_control();
}
} // namespace moss::test::validation
