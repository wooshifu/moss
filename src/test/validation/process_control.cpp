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
#include "validation/timer_control.hpp"
#include "validation/uaccess.hpp"
#include "validation_internal.hpp"

#include "validation/process_control.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;

namespace moss::test::validation {
namespace {
struct ForkMetadataPressure {
  LifecycleResources baseline;
  HeapPressure heap;
  unsigned stage = 0, vmas = 0;
  bool holding = false, exhausted = false;
};
ForkMetadataPressure *fork_metadata_pressure = nullptr;
HeapPressure *fork_clone_pressure = nullptr;
LifecycleResources fork_clone_baseline;
bool fork_clone_exhausted = false;
HeapPressure *user_heap_pressure = nullptr;
LifecycleResources user_heap_baseline;
} // namespace

extern "C" void moss_validation_fd_clone(bool entering) noexcept {
  if (!fork_clone_pressure || fork_clone_exhausted) {
    return;
  }
  if (entering) {
    ut::expect(fork_clone_pressure->acquire(sizeof(vfs::FdTable)));
  } else {
    fork_clone_pressure->release();
    fork_clone_exhausted = true;
  }
}

extern "C" void moss_validation_fork_metadata(unsigned stage, bool entering) noexcept {
  auto *probe = fork_metadata_pressure;
  if (!probe || probe->stage != stage || probe->exhausted) {
    return;
  }
  if (entering) {
    // Fail after one successful VMA copy, not only before any work is owned.
    if (stage == 0 && probe->vmas++ == 0) {
      return;
    }
    // Keep the fork hook's stage order: VMA=0, Thread=1, ThreadEntry=2.
    usize size = sizeof(void *);
    if (stage == 0) {
      size = sizeof(process::VmaRegion);
    } else if (stage == 1) {
      size = sizeof(process::Thread);
    } else if (stage == 2) {
      size = sizeof(process::ThreadEntry);
    }
    probe->holding = true;
    ut::expect(probe->heap.acquire(size));
  } else if (probe->holding) {
    probe->heap.release();
    probe->holding = false;
    probe->exhausted = true;
  }
}

long process_control(long op, long arg1, [[maybe_unused]] long arg2) {
  if (ut::same_id(selection, "users") && ut::same_id(active_case, "fork_fd_allocation_rollback") && affinity_valid()) {
    if (op == 40 && !fork_clone_pressure) {
      fork_clone_baseline = LifecycleResources::capture();
      fork_clone_exhausted = false;
      fork_clone_pressure = new HeapPressure{};
      return 1;
    }
    if (op == 41 && fork_clone_pressure) {
      const bool exercised = ut::expect(fork_clone_exhausted);
      delete fork_clone_pressure;
      fork_clone_pressure = nullptr;
      return ut::expect(LifecycleResources::capture() == fork_clone_baseline) && exercised;
    }
  }
  if (ut::same_id(selection, "users") && ut::same_id(active_case, "fork_metadata_allocation_rollback") &&
      affinity_valid()) {
    if (op == 46 && arg1 >= 0 && arg1 < 4 && !fork_metadata_pressure) {
      const auto baseline = LifecycleResources::capture();
      fork_metadata_pressure = new ForkMetadataPressure{};
      fork_metadata_pressure->baseline = baseline;
      fork_metadata_pressure->stage = static_cast<unsigned>(arg1);
      return 1;
    }
    if (op == 47 && fork_metadata_pressure) {
      const auto baseline = fork_metadata_pressure->baseline;
      const bool exercised = ut::expect(fork_metadata_pressure->exhausted && !fork_metadata_pressure->holding);
      delete fork_metadata_pressure;
      fork_metadata_pressure = nullptr;
      return ut::expect(LifecycleResources::capture() == baseline) && exercised;
    }
  }
  const bool mmap_pressure_case = ut::same_id(active_case, "mmap_heap_rollback");
  if (ut::same_id(selection, "users") &&
      (mmap_pressure_case || ut::same_id(active_case, "fork_process_allocation_rollback")) && affinity_valid()) {
    if (op == (mmap_pressure_case ? 42 : 44) && !user_heap_pressure) {
      user_heap_baseline = LifecycleResources::capture();
      user_heap_pressure = new HeapPressure{};
      if (user_heap_pressure->acquire(mmap_pressure_case ? sizeof(process::VmaRegion) : sizeof(process::Process))) {
        return 1;
      }
      delete user_heap_pressure;
      user_heap_pressure = nullptr;
      return 0;
    }
    if (op == (mmap_pressure_case ? 43 : 45) && user_heap_pressure) {
      delete user_heap_pressure;
      user_heap_pressure = nullptr;
      return ut::expect(LifecycleResources::capture() == user_heap_baseline);
    }
  }
  // 52 arms the case, 53 gates the migrated child, and 54 verifies the wake
  // ordering before clearing the fixture for later cases.
  invalid_control();
}
} // namespace moss::test::validation
