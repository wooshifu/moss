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

#include "validation/benchmark.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;

namespace moss::bench {
Tick read_counter() noexcept {
#if defined(MOSS_ARCH_ARM64)
  Tick value;
  asm volatile("dsb ish; isb; mrs %0, cntvct_el0; isb" : "=r"(value) : : "memory");
  return value;
#elif defined(MOSS_ARCH_X64)
  unsigned lo, hi;
  asm volatile("mfence; lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) : : "memory");
  return (static_cast<Tick>(hi) << 32) | lo;
#else
  Tick value;
  asm volatile("fence iorw,iorw; rdtime %0; fence iorw,iorw" : "=r"(value) : : "memory");
  return value;
#endif
}
unsigned current_cpu() noexcept { return arch::get_current_cpu_id(); }
} // namespace moss::bench

namespace moss::test::validation {
bench::Clock clock_info;
namespace {

bench::Scenario *selected_benchmark = nullptr;
usize syscall_iterations = 1;
bool syscall_pilot = true;
u64 syscall_overhead = 0;
usize syscall_capacity = 65536;
const char *const user_benchmark_names[] = {"bench.fault",     "bench.cow",    "bench.switch",
                                            "bench.lifecycle", "bench.signal", "bench.pipe"};

// Private userspace dispatch modes: 2 is getpid and 11..16 follow this six-name
// catalog's order. Keep these numbers synchronized with userspace/validation.c.
long user_benchmark_mode() {
  if (ut::same_id(selection, "bench.getpid")) {
    return 2;
  }
  for (long i = 0; i < 6; ++i) {
    if (ut::same_id(selection, user_benchmark_names[i])) {
      return 11 + i;
    }
  }
  return 0;
}

LifecycleResources benchmark_resources;
bool benchmark_warmed = false;
u64 benchmark_switches = 0;
} // namespace

void register_benchmarks() {
  bench::register_benchmark("bench.allocate", [](bench::Context &context) { allocation_benchmark(context, 0); });
  bench::register_benchmark("bench.release", [](bench::Context &context) { allocation_benchmark(context, 1); });
  bench::register_benchmark("bench.combined", [](bench::Context &context) { allocation_benchmark(context, 2); });
  bench::register_benchmark("bench.read", read_benchmark);
  bench::register_benchmark("bench.getpid", [](bench::Context &) {});
  for (const char *name : user_benchmark_names) {
    bench::register_benchmark(name, [](bench::Context &) {});
  }
  bench::register_benchmark("bench.wakeup", [](bench::Context &context) { timer_benchmark(context, true); });
  bench::register_benchmark("bench.timer", [](bench::Context &context) { timer_benchmark(context, false); });
}

void catalog_benchmark() {
  for (unsigned i = 0; i < bench::registry.count; ++i) {
    if (ut::same_id(selection, bench::registry.scenarios[i].name)) {
      selected_benchmark = &bench::registry.scenarios[i];
      selected_count = 1;
      Event("catalog").str("case", selection).send();
    }
  }
}

bool has_selected_benchmark() { return selected_benchmark != nullptr; }

long start_benchmark() {
  if (selected_benchmark) {
    start_case(selection);
    prepare_clock();
    if (const long mode = user_benchmark_mode()) {
#if defined(MOSS_ARCH_ARM64)
      u64 control;
      asm volatile("mrs %0, cntkctl_el1" : "=r"(control));
      control |= 2; // EL0VCTEN: userspace brackets the real syscall round trip.
      asm volatile("msr cntkctl_el1, %0; isb" ::"r"(control) : "memory");
#endif
      syscall_pilot = fixed_iterations == 0;
      syscall_capacity = mode == 2 || mode == 13 || mode == 15 || mode == 16 ? 65536 : 64;
      if (fixed_iterations > syscall_capacity) {
        failed = true;
        finish("invalid_iterations");
      }
      syscall_iterations = fixed_iterations ? fixed_iterations : 1;
      return mode;
    }
    // Restore ordinary interrupts during the in-kernel workload. The pinned
    // thread still uses the real scheduler, locks, and syscall return path.
    arch::enable_interrupts();
    bench::Context context{.clock = clock_info,
                           .iterations = fixed_iterations,
                           .capacity = 256,
                           .warmup = warmup_count,
                           .samples = sample_count,
                           .cpu = 0,
                           .valid = true,
                           .record = record_batch};
    selected_benchmark->run(context);
    failed = failed || !context.valid;
    if (failed) {
      ++ut::test_result::assertions_failed;
    }
    end_case();
    finish();
  }
  invalid_control();
}

long benchmark_control(long op, long arg1, long arg2) {
  if (op == 4 && user_benchmark_mode()) {
    if (sample_index >= warmup_count + sample_count) {
      end_case();
      return 0;
    }
    return static_cast<long>(syscall_iterations);
  }
  if (op == 5 && user_benchmark_mode()) {
    if (arg1 <= 0 || !arg2 || !affinity_valid()) {
      failed = true;
      finish("invalid_sample");
    }
    if (syscall_pilot) {
      if (static_cast<u64>(arg1) >= clock_info.frequency / 1000 || syscall_iterations == syscall_capacity) {
        syscall_pilot = false;
      } else {
        syscall_iterations *= 2;
      }
    } else {
      record_batch(static_cast<u64>(arg1), syscall_iterations, sample_index < warmup_count, syscall_overhead);
    }
    return 0;
  }
  if (op == 6 && user_benchmark_mode() && arg1 >= 0) {
    syscall_overhead = static_cast<u64>(arg1);
    return 0;
  }
  if (op == 30 && user_benchmark_mode() >= 11 && active_case && affinity_valid()) {
    const auto now = LifecycleResources::capture();
    if (arg1 == 0) {
      benchmark_resources = now;
      benchmark_switches = process::g_scheduler->total_context_switches();
      return 1;
    }
    bool valid = !benchmark_warmed || now == benchmark_resources;
    benchmark_warmed = true;
    if (ut::same_id(selection, "bench.switch")) {
      valid = valid && process::g_scheduler->total_context_switches() >= benchmark_switches + syscall_iterations;
    }
    return ut::expect(valid) ? 1 : 0;
  }
  if (op == 31 && (ut::same_id(selection, "bench.fault") || ut::same_id(selection, "bench.cow")) && active_case) {
    auto *thread = process::CfsScheduler::get_current_task();
    auto owner = process::g_process_manager->find_process(thread->owner_pid);
    auto as = owner ? owner->address_space() : shared_ptr<process::AddressSpace>{};
    bool valid =
        as && arg2 > 0 && arg2 <= 64 &&
        as->allows_user_access(static_cast<u64>(arg1), static_cast<usize>(arg2) * page_size, process::vma_flags::WRITE);
    for (long i = 0; valid && i < arg2; ++i) {
      const auto *pte =
          mm::PageTableManager::get_user_pte(as->pgd_phys, static_cast<u64>(arg1) + static_cast<u64>(i) * page_size);
      valid = ut::same_id(selection, "bench.fault")
                  ? !pte || !pte->is_valid()
                  : pte && pte->is_valid() && pte->is_cow() &&
                        mm::PageFrameAllocator::page_ref_get(pte->get_phys_addr()) >= 2;
    }
    return ut::expect(valid) ? 1 : 0;
  }
  invalid_control();
}
} // namespace moss::test::validation
