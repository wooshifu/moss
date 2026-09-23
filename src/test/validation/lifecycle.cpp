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
#include "validation/benchmark.hpp"
#include "validation/core_cases.hpp"
#include "validation/exec_control.hpp"
#include "validation/isolation.hpp"
#include "validation/memory_cases.hpp"
#include "validation/memory_internal.hpp"
#include "validation/process_control.hpp"
#include "validation/resources.hpp"
#include "validation/runtime.hpp"
#include "validation/runtime_state.hpp"
#include "validation/signal_control.hpp"
#include "validation/smp_cases.hpp"
#include "validation/timer_control.hpp"
#include "validation/uaccess.hpp"
#include "validation_internal.hpp"

#include "validation/lifecycle.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;

namespace moss::test::validation {
namespace {
LifecycleResources lifecycle_baseline;
u64 lifecycle_checkpoint = 0;
u64 lifecycle_started_ns = 0;
bool lifecycle_started = false;
bool lifecycle_complete = false;
bool lifecycle_host_released = false;

bool dispatch_boundary_checked = false;

} // namespace

bool lifecycle_case_complete() { return lifecycle_complete && dispatch_boundary_checked; }

extern "C" void moss_validation_dispatch_selected() noexcept {
  if (is_lifecycle() && active_case) {
    dispatch_boundary_checked = true;
    // A timer IRQ must not consume this cached selection before it is dispatched.
    ut::expect(!arch::interrupts_enabled());
  }
}

long lifecycle_control(long op, long arg1, long arg2) {
  if (op == 10 && is_lifecycle() && active_case) {
    const bool applications = ut::same_id(selection, "users.applications");
    // Five is the midpoint of CTest's ten-cycle application profile, keeping
    // the 30-second no-progress deadline sensitive to a real stall instead of
    // aggregate host throttling. Core runs stay at 100 cycles to avoid making
    // their 1,000/10,000-cycle profiles protocol-bound. Userspace and the host
    // parser must use the same cadence.
    const u64 interval = applications ? 5 : 100;
    if (!ut::expect(arg1 >= 0 && arg2 == (applications ? arg1 : 0))) {
      return 0; // Each declared core cycle must also complete its BusyBox child.
    }
    const auto now = LifecycleResources::capture();
    const u64 time_ns = timer::TimerSubsystem::instance().now_ns();
    if (arg1 == 0 && !lifecycle_started) {
      lifecycle_started_ns = time_ns;
    }
    const u64 elapsed_ns = time_ns - lifecycle_started_ns;
    Event("checkpoint")
        .str("case", active_case)
        .number("cycles", static_cast<u64>(arg1))
        .number("application_cycles", static_cast<u64>(arg2))
        .number("elapsed_ns", elapsed_ns)
        .number("heap_bytes", now.heap_bytes)
        .number("free_pages", now.free_pages)
        .number("processes", now.processes)
        .number("threads", now.threads)
        .number("user_pages", now.user_pages)
        .number("stack_pages", now.stack_pages)
        .number("descriptors", now.descriptors)
        .number("file_refs", now.file_refs)
        .number("vfs_inodes", now.vfs_pools.inodes)
        .number("vfs_dentries", now.vfs_pools.dentries)
        .number("vfs_files", now.vfs_pools.files)
        .send();
    if (arg1 == 0 && !lifecycle_started) {
      lifecycle_baseline = now;
      lifecycle_started = true;
      return 1;
    }
    if (!ut::expect(lifecycle_started && !lifecycle_complete &&
                    arg1 == static_cast<long>(lifecycle_checkpoint + interval) && now == lifecycle_baseline)) {
      return 0;
    }
    lifecycle_checkpoint = static_cast<u64>(arg1);
    if (stability && !lifecycle_host_released) {
      lifecycle_host_released = moss::abi::bridge::console_try_getc() == 'S';
    }
    // Guest clocks may drift relative to the host (e.g. calibrated x86 TSC).
    // Keep doing complete cycles until both clocks and the host agree to stop.
    // Routine callers may lower the cycle count through moss.iterations only
    // when it lands on this workload's checkpoint cadence. Stability remains
    // fixed at 10,000 cycles and 30 minutes (1.8e12 ns), plus a host release.
    const u64 routine_cycles = fixed_iterations ? fixed_iterations : 1000U;
    const u64 target_cycles = stability ? 10000U : routine_cycles;
    if (!ut::expect(target_cycles % interval == 0)) {
      return 0;
    }
    lifecycle_complete = lifecycle_checkpoint >= target_cycles && elapsed_ns >= (stability ? 1800000000000ULL : 0) &&
                         (!stability || lifecycle_host_released);
    return lifecycle_complete ? 2 : 1;
  }
  invalid_control();
}
} // namespace moss::test::validation
