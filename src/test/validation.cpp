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
#include "validation/benchmark.hpp"
#include "validation/core_cases.hpp"
#include "validation/exec_control.hpp"
#include "validation/isolation.hpp"
#include "validation/lifecycle.hpp"
#include "validation/memory_cases.hpp"
#include "validation/memory_internal.hpp"
#include "validation/process_control.hpp"
#include "validation/registry.hpp"
#include "validation/resources.hpp"
#include "validation/runtime.hpp"
#include "validation/runtime_state.hpp"
#include "validation/signal_control.hpp"
#include "validation/smp_cases.hpp"
#include "validation/timer_control.hpp"
#include "validation/uaccess.hpp"
#include "validation_internal.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;
using moss::test::validation::active_case;
using moss::test::validation::address_space_control_exhausted;
using moss::test::validation::address_space_control_pressure;
using moss::test::validation::completed;
using moss::test::validation::Event;
using moss::test::validation::exec_registration_refused;
using moss::test::validation::exec_source_swaps;
using moss::test::validation::failed;
using moss::test::validation::fixed_iterations;
using moss::test::validation::is_lifecycle;
using moss::test::validation::sample_count;
using moss::test::validation::sample_index;
using moss::test::validation::selected_count;
using moss::test::validation::selection;
using moss::test::validation::stability;
using moss::test::validation::warmup_count;

namespace moss::test::validation {
char selection[81]{};
const char *active_case = nullptr;
bool failed = false;
unsigned completed = 0;
unsigned selected_count = 0;
unsigned sample_index = 0;
unsigned warmup_count = 5;
unsigned sample_count = 30;
usize fixed_iterations = 0;
bool stability = false;
bool is_lifecycle() {
  return ut::same_id(selection, "users.lifecycle") || ut::same_id(selection, "users.applications");
}
} // namespace moss::test::validation

namespace moss::kernel {
void kernel_uart_puts(const char *str) noexcept { hal::uart::puts(str); }
[[noreturn]] void kernel_test_exit([[maybe_unused]] int code) noexcept {
  // The host observes the completed serial protocol and owns process termination.
  arch::disable_interrupts();
  for (;;) {
    arch::cpu_halt();
  }
}
} // namespace moss::kernel

namespace {

// Match valid_id's 80-character limit and reserve one byte for the terminator.
[[noreturn]] void finish(const char *reason = "complete") {
  Event("end")
      .number("completed", completed)
      .number("selected", selected_count)
      .number("failed", failed ? 1 : 0)
      .str("reason", reason)
      .send();
  kernel_test_exit(failed ? 1 : 0);
}

bool option(const char *key, char *out, usize capacity) {
  const char *args = moss::fdt::get_platform_info().bootargs;
  if (!args) {
    return false;
  }
  while (*args) {
    while (*args == ' ') {
      ++args;
    }
    const char *token = args;
    while (*args && *args != ' ') {
      ++args;
    }
    const char *p = token;
    const char *k = key;
    while (*k && p < args && *p == *k) {
      ++k;
      ++p;
    }
    if (*k == 0 && p < args && *p == '=') {
      ++p;
      usize n = static_cast<usize>(args - p);
      if (!n || n >= capacity) {
        return false;
      }
      for (usize i = 0; i < n; ++i) {
        out[i] = p[i];
      }
      out[n] = 0;
      return true;
    }
  }
  return false;
}

u64 numeric_option(const char *key, u64 fallback) {
  // Twenty-four bytes fit a u64's 20 decimal digits and terminator. The parser
  // additionally rejects a pre-digit accumulator above 100,000,000, bounding
  // subsequent multiply/add; selection-specific limits are checked at boot.
  char value[24];
  if (!option(key, value, sizeof(value))) {
    return fallback;
  }
  u64 parsed = 0;
  for (const char *p = value; *p; ++p) {
    if (*p < '0' || *p > '9' || parsed > 100000000) {
      failed = true;
      return 0;
    }
    parsed = parsed * 10 + static_cast<unsigned>(*p - '0');
  }
  return parsed;
}

bool affinity_valid() {
  auto *thread = process::CfsScheduler::get_current_task();
  return thread && thread->cpu_affinity_mask.low_word() == 1 && arch::get_current_cpu_id() == 0;
}

void start_case(const char *name) {
  active_case = name;
  ut::test_result::assertions_passed = 0;
  ut::test_result::assertions_failed = 0;
  Event("case_start").str("case", name).send();
}
void end_case() {
  failed = failed || ut::test_result::assertions_failed != 0;
  Event("case_end")
      .str("case", active_case)
      .number("passed", static_cast<u64>(ut::test_result::assertions_passed))
      .number("failed", static_cast<u64>(ut::test_result::assertions_failed))
      .send();
  ++completed;
  active_case = nullptr;
}

} // namespace

// Keep validation protocol state here while split suites share its case lifecycle.
namespace moss::test::validation {
const char *selected_suite() { return selection; }
const char *running_case() { return active_case; }
bool boot_option(const char *key, char *out, usize capacity) { return ::option(key, out, capacity); }
u64 numeric_boot_option(const char *key, u64 fallback) { return ::numeric_option(key, fallback); }
bool affinity_valid() { return ::affinity_valid(); }
void start_case(const char *name) { ::start_case(name); }
void end_case() { ::end_case(); }
[[noreturn]] void finish(const char *reason) { ::finish(reason); }
[[noreturn]] void invalid_control() {
  failed = true;
  ::finish("invalid_control");
}
} // namespace moss::test::validation

extern "C" void moss_validation_boot() noexcept {
  if (!option("moss.validation", selection, sizeof(selection)) || !ut::valid_id(selection)) {
    failed = true;
    finish("invalid_selection");
  }
  warmup_count = static_cast<unsigned>(numeric_option("moss.warmup", 5));
  sample_count = static_cast<unsigned>(numeric_option("moss.samples", 30));
  fixed_iterations = static_cast<usize>(numeric_option("moss.iterations", 0));
  const u64 stability_option = numeric_option("moss.stability", 0);
  if (stability_option > 1 || (stability_option && !is_lifecycle())) {
    failed = true;
    finish("invalid_stability_parameters");
  }
  stability = stability_option == 1;
  // Policy ceilings bound serial output and run time; 65536 also matches the
  // largest userspace syscall fixture capacity. They are not measured accuracy limits.
  if (!sample_count || sample_count > 1000 || warmup_count > 100 || fixed_iterations > 65536) {
    failed = true;
    finish("invalid_parameters");
  }
  moss::test::validation::declare_cases();
  moss::test::validation::register_benchmarks();
  if (bench::registry.error) {
    failed = true;
    finish(bench::registry.error);
  }
  if (ut::registry.error) {
    failed = true;
    finish(ut::registry.error);
  }
  const auto &info = moss::fdt::get_platform_info();
  Event("ready")
      .number("detected_cpus", info.cpu_count)
      .number("online_mask", __atomic_load_n(&moss::boot::online_cpu_mask, __ATOMIC_ACQUIRE))
      .number("work_mask", __atomic_load_n(&moss::boot::cpu_work_mask, __ATOMIC_ACQUIRE))
      .number("ram_bytes", info.total_memory_size)
      .number("managed_pages", mm::PageFrameAllocator::get_memory_stats().total_pages)
      .send();
  for (unsigned i = 0; i < ut::registry.case_count; ++i) {
    const auto &item = ut::registry.cases[i];
    if (ut::same_id(item.suite_name, selection)) {
      Event("catalog").str("case", item.name).send();
      ++selected_count;
    }
  }
  moss::test::validation::catalog_benchmark();
  if (!selected_count) {
    failed = true;
    finish("empty_selection");
  }
}

extern "C" void moss_validation_address_space_allocation(bool entering, bool control_block, usize size) noexcept {
  if (!address_space_control_pressure || !control_block) {
    return;
  }
  if (entering) {
    // The AddressSpace object already exists. Exhaust the real allocator for
    // the exact control-block request, not a synthetic failure return value.
    address_space_control_exhausted = ut::expect(address_space_control_pressure->acquire(size));
  } else {
    address_space_control_pressure->release();
  }
}

extern "C" long moss_validation_call(long op, long arg1, [[maybe_unused]] long arg2) noexcept {
  // These opcodes and returned mode IDs form the private validation protocol
  // shared with src/userspace validation programs, not Linux syscall numbers.
  // Keep both endpoints in sync: opcode zero performs the startup handshake;
  // later operations drive suite-specific ownership checks and benchmarks.
  // Invoked only after the production scheduler and a real userspace exec.
  if (op == 0) {
    if (arg1 != 0 || !affinity_valid()) {
      failed = true;
      finish("affinity");
    }
    Event("worker").number("cpu", arch::get_current_cpu_id()).number("affinity", 1).send();
    if (ut::same_id(selection, "users")) {
      return 1;
    }
    if (ut::same_id(selection, "users.vm")) {
      return 5;
    }
    if (ut::same_id(selection, "users.frame")) {
      return 6;
    }
    if (ut::same_id(selection, "users.uaccess")) {
      return 7;
    }
    if (ut::same_id(selection, "users.signals")) {
      return 8;
    }
#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X64)
    if (ut::same_id(selection, "users.console_irq")) {
      return 24;
    }
#endif
    if (ut::same_id(selection, "users.lifecycle")) {
      return 9;
    }
    if (ut::same_id(selection, "users.applications")) {
      return 23;
    }
    if (ut::same_id(selection, "users.timers")) {
      return 10;
    }
    if (ut::same_id(selection, "users.libc")) {
      return 20;
    }
    if (ut::same_id(selection, "users.exec")) {
      return 22;
    }
    if (ut::same_id(selection, "users.busybox")) {
      return 21;
    }
    if (ut::same_id(selection, "users.simd_fault")) {
      start_case("isolation");
      return 4;
    }
    if (const long mode = moss::test::validation::start_smp_suite()) {
      return mode;
    }
    if (moss::test::validation::has_selected_benchmark()) {
      return moss::test::validation::start_benchmark();
    }
    arch::enable_interrupts();
    for (unsigned i = 0; i < ut::registry.case_count && !failed; ++i) {
      auto &item = ut::registry.cases[i];
      if (ut::same_id(item.suite_name, selection)) {
        start_case(item.name);
        item.test_function();
        end_case();
      }
    }
    finish();
  }
  const bool user_suite = ut::same_id(selection, "users") || ut::same_id(selection, "users.vm") ||
                          ut::same_id(selection, "users.frame") || ut::same_id(selection, "users.uaccess") ||
                          ut::same_id(selection, "users.signals") || ut::same_id(selection, "users.console_irq") ||
                          is_lifecycle() || ut::same_id(selection, "users.timers") ||
                          ut::same_id(selection, "users.libc") || ut::same_id(selection, "users.busybox") ||
                          ut::same_id(selection, "users.exec");
  if (op == 1 && user_suite && !failed && !active_case && arg1 == static_cast<long>(completed) && arg1 >= 0) {
    long index = 0;
    for (unsigned i = 0; i < ut::registry.case_count; ++i) {
      const auto &item = ut::registry.cases[i];
      if (ut::same_id(item.suite_name, selection) && index++ == arg1) {
        start_case(item.name);
        return 0;
      }
    }
  }
  if (op == 2 && active_case && (user_suite || ut::same_id(selection, "users.simd_fault"))) {
    if (arg2 != 0) {
      logging::klog::error("users checks failed: mask={:#x}", static_cast<u64>(arg2));
    }
    ut::expect(arg1 != 0 && affinity_valid());
    if (is_lifecycle()) {
      ut::expect(moss::test::validation::lifecycle_case_complete());
    }
    if (ut::same_id(selection, "users.exec") && ut::same_id(active_case, "source_version")) {
      ut::expect(exec_source_swaps == 1);
    }
    if (ut::same_id(selection, "users.exec") && ut::same_id(active_case, "registration_gate")) {
      ut::expect(exec_registration_refused);
    }
    end_case();
    return failed ? 0 : 1;
  }
  if (op == 3) {
    finish();
  }
  if ((op == moss::test::validation::ISOLATION_PREPARE || op == moss::test::validation::ISOLATION_VERIFY) &&
      (ut::same_id(selection, "users.vm") || ut::same_id(selection, "users.signals"))) {
    return moss::test::validation::isolation_control(op, arg1, arg2);
  }
  if (ut::same_id(selection, "users")) {
    return moss::test::validation::process_control(op, arg1, arg2);
  }
  if (ut::same_id(selection, "users.timers")) {
    return moss::test::validation::timer_control(op, arg1, arg2);
  }
  if (ut::same_id(selection, "users.signals") || ut::same_id(selection, "users.console_irq")) {
    return moss::test::validation::signal_control(op, arg1, arg2);
  }
  if (op == 38 && arg1 == 0 && arg2 == 0 && ut::same_id(selection, "users.libc") &&
      ut::same_id(active_case, "filesystem_permissions")) {
    auto owner = process::current_process();
    if (!owner || owner->euid() != 0) {
      return -1;
    }
    // Test precondition, not a production identity-management interface.
    owner->set_uid(99);
    owner->set_gid(99);
    return 0;
  }
  if (ut::same_id(selection, "users.exec")) {
    return moss::test::validation::exec_control(op, arg1, arg2);
  }
  if (ut::same_id(selection, "users.uaccess")) {
    return moss::test::validation::uaccess_control(op, arg1, arg2);
  }
  if (op == 10 && ut::same_id(selection, "users.frame") && active_case) {
    auto *thread = process::CfsScheduler::get_current_task();
    const u64 address = thread ? reinterpret_cast<u64>(thread->trap_frame) : 0;
    // Validate ownership before dereferencing: the old RV entry supplies 511,
    // and the old x86 entry supplies null, neither is a kernel-stack frame.
    const bool owned = thread && address >= thread->kernel_stack_base &&
                       address <= thread->kernel_stack_top() - sizeof(moss::abi::TrapFrame) && (address & 15) == 0;
    if (!owned && thread) {
      logging::klog::error("frame validation: frame={:#x}, stack=[{:#x}, {:#x})", address, thread->kernel_stack_base,
                           thread->kernel_stack_top());
    }
    ut::expect(owned);
    if (owned) {
      auto &frame = *thread->trap_frame;
      ut::expect(frame.from_user() && frame.syscall_number() == 511);
      for (u32 i = 0; i < 6; ++i) {
        ut::expect(frame.argument(i) == (i == 0 ? 10 : i * 11));
      }
      ut::expect(mm::PageTableManager::is_user_range(frame.pc, 1));
      ut::expect(mm::PageTableManager::is_user_range(frame.sp, 1));
    }
    return owned ? 12345 : -1;
  }
  if (moss::test::validation::has_selected_benchmark()) {
    return moss::test::validation::benchmark_control(op, arg1, arg2);
  }
  if (is_lifecycle()) {
    return moss::test::validation::lifecycle_control(op, arg1, arg2);
  }
  return moss::test::validation::smp_control(op, arg1, arg2);
}
