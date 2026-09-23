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
#include "validation/memory_internal.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;
using moss::test::validation::HeapPressure;

namespace moss::test::validation {
void empty_case() {}
void ipc_heap_rollback() {
  const auto heap_baseline = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto page_baseline = mm::PageFrameAllocator::get_memory_stats().free_pages;
  ipc::SharedMemoryManager manager;
  bool recovered = false;
  unsigned failures = 0;
  {
    HeapPressure pressure;
    if (!ut::expect(pressure.acquire(sizeof(void *)))) {
      return;
    }
    // Restore heap capacity one real allocation at a time. Descriptor,
    // control-block and tracking-node failures must all return the backing.
    for (;;) {
      const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
      auto created = manager.create_region(0, page_size);
      if (created) {
        recovered = true;
        ut::expect(manager.destroy_region(*created).has_value());
      } else {
        ++failures;
        ut::expect(created.error() == KernelError::OutOfMemory);
      }
      ut::expect(manager.get_statistics().total_regions == 0);
      ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
      ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == page_baseline);
      if (recovered || !ut::expect(pressure.release_one())) {
        break;
      }
    }
  }
  ut::expect(failures > 0 && recovered);
  {
    HeapPressure pressure;
    usize exhausted_pages = 0;
    {
      ipc::SharedMemoryManager teardown;
      auto created = teardown.create_region(0, page_size);
      if (!ut::expect(created.has_value()) || !ut::expect(pressure.acquire(sizeof(void *)))) {
        return;
      }
      exhausted_pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
      // The manager leaves scope while every heap allocation still fails.
      // Teardown must not allocate a snapshot just to release existing regions.
    }
    ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == exhausted_pages + 1);
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap_baseline);
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == page_baseline);
}

void timer_contracts() {
  ut::expect(timer::Clocksource{}.deadline_counter(0) == 0);
  const auto &clock = timer::TimerSubsystem::instance().clocksource();
  for (unsigned i = 0; i < 16; ++i) {
    const auto before = bench::read_counter();
    const auto at_deadline = clock.deadline_counter(clock.now_ns());
    const auto after = bench::read_counter();
    ut::expect(at_deadline >= before && at_deadline <= after);
  }
  timer::HrTimer test;
  test.init(timer::TimerMode::OneShot, nullptr);
  auto result = test.start_relative(1);
  ut::expect(!result && result.error() == ErrorCode::InvalidParameter && !test.is_active());
  test.init(timer::TimerMode::OneShot, [](void *) noexcept {});
  result = test.start_relative(~u64{0});
  ut::expect(!result && result.error() == ErrorCode::InvalidParameter && !test.is_active());
  // One second in nanoseconds keeps these state/cancel checks armed in the
  // future; it is a fixture delay rather than a timer precision requirement.
  const auto expiry = timer::TimerSubsystem::instance().now_ns() + 1000000000ULL;
  ut::expect(static_cast<bool>(test.start(expiry)));
  result = test.start_relative(1);
  ut::expect(!result && result.error() == ErrorCode::InvalidState && test.is_active() && test.expires_ns() == expiry);
  test.cancel();
  test.cancel();
  ut::expect(!test.is_active());
  test.init(timer::TimerMode::Periodic, [](void *) noexcept {});
  result = test.start_relative(0);
  ut::expect(!result && result.error() == ErrorCode::InvalidParameter && !test.is_active());
  result = test.start(expiry);
  ut::expect(!result && result.error() == ErrorCode::InvalidParameter && !test.is_active());
  ut::expect(static_cast<bool>(test.start_relative(1000000000ULL)));
  test.cancel();
}

struct TimerObservation {
  unsigned count = 0;
  u64 first_ns = 0;
  static void fired(void *data) noexcept {
    auto *self = static_cast<TimerObservation *>(data);
    if (__atomic_load_n(&self->count, __ATOMIC_RELAXED) == 0) {
      self->first_ns = timer::TimerSubsystem::instance().now_ns();
    }
    __atomic_add_fetch(&self->count, 1U, __ATOMIC_RELEASE);
  }
  unsigned calls() const { return __atomic_load_n(&count, __ATOMIC_ACQUIRE); }
};

void timer_dispatch() {
  // Use 2 ms one-shot / 1 ms periodic delays and a 100 ms observation budget.
  // At least three callbacks distinguishes repetition from a one-shot; the
  // 5 ms post-cancel window spans multiple periods. These are test budgets,
  // not measured dispatch-latency guarantees.
  auto &subsystem = timer::TimerSubsystem::instance();
  for (unsigned phase = 0; phase != 2; ++phase) {
    TimerObservation once, periodic;
    timer::HrTimer one_shot, repeating;
    one_shot.init(timer::TimerMode::OneShot, TimerObservation::fired, &once);
    repeating.init(timer::TimerMode::Periodic, TimerObservation::fired, &periodic);
    const auto deadline = subsystem.now_ns() + 2000000ULL;
    ut::expect(static_cast<bool>(one_shot.start(deadline)));
    if (phase != 0) {
      // Model preemption between arming the two independent timers.
      while (subsystem.now_ns() < deadline + 100000000ULL) {
        arch::cpu_yield();
      }
    }
    const auto first_period = subsystem.now_ns() + 1000000ULL;
    ut::expect(static_cast<bool>(repeating.start_relative(1000000ULL)));
    // Each timer gets the same observation window, even after preemption
    // between registrations. Setup time is not periodic dispatch time.
    const auto observation_end = subsystem.now_ns() + 100000000ULL;
    while ((once.calls() == 0 || periodic.calls() < 3) && subsystem.now_ns() < observation_end) {
      arch::cpu_yield();
    }
    one_shot.cancel();
    repeating.cancel();
    ut::expect(ut::eq(once.calls(), 1U));
    ut::expect(ut::ge(once.first_ns, deadline));
    const auto stopped_count = periodic.calls();
    ut::expect(ut::ge(stopped_count, 3U));
    ut::expect(ut::ge(periodic.first_ns, first_period));
    const auto after_cancel = subsystem.now_ns() + 5000000ULL;
    while (subsystem.now_ns() < after_cancel) {
      arch::cpu_yield();
    }
    ut::expect(once.calls() == 1 && periodic.calls() == stopped_count);
  }
}

unsigned deferred_calls = 0;
void deferred_case() { ++deferred_calls; }
void accounting() {
  unsigned evaluations = 0;
  int before = ut::test_result::assertions_passed;
  if (!ut::expect(++evaluations == 1)) {
    return;
  }
  ut::expect(evaluations == 1 && ut::test_result::assertions_passed == before + 1);
  ut::Registry local;
  ut::expect(local.begin_suite("sample"));
  local.add_test("deferred", deferred_case);
  ut::expect(local.case_count == 1 && !local.error && deferred_calls == 0);
  local.cases[0].test_function();
  ut::expect(deferred_calls == 1);
  local.add_test("deferred", empty_case);
  ut::expect(ut::same_id(local.error, "duplicate_case"));
}
void registry_limits() {
  static ut::Registry local;
  static const ut::Registry empty; // Avoid reset temporaries on the 16 KiB kernel stack.
  static_assert(ut::Registry::suite_capacity <= ut::Registry::case_capacity);
  // One extra entry triggers overflow; each ID is 'c', three decimal digits,
  // and the zero-initialized terminator. Capacities here must stay below 1000.
  static char ids[ut::Registry::case_capacity + 1][5];
  for (unsigned i = 0; i <= ut::Registry::case_capacity; ++i) {
    ids[i][0] = 'c';
    ids[i][1] = static_cast<char>('0' + i / 100);
    ids[i][2] = static_cast<char>('0' + i / 10 % 10);
    ids[i][3] = static_cast<char>('0' + i % 10);
  }
  local.begin_suite("limit");
  for (unsigned i = 0; i <= ut::Registry::case_capacity; ++i) {
    local.add_test(ids[i], empty_case);
  }
  ut::expect(local.case_count == ut::Registry::case_capacity && ut::same_id(local.error, "case_capacity"));
  local = empty;
  for (unsigned i = 0; i <= ut::Registry::suite_capacity; ++i) {
    local.begin_suite(ids[i]);
    local.active_suite = nullptr;
  }
  ut::expect(local.suite_count == ut::Registry::suite_capacity && ut::same_id(local.error, "suite_capacity"));
  local = empty;
  local.begin_suite("duplicate");
  local.active_suite = nullptr;
  local.begin_suite("duplicate");
  ut::expect(ut::same_id(local.error, "duplicate_suite"));
  local = empty;
  local.begin_suite("invalid id");
  ut::expect(ut::same_id(local.error, "invalid_suite"));
  bench::Registry benchmarks;
  // The benchmark registry holds 16 entries; the seventeenth must report
  // exhaustion rather than silently dropping a registered workload.
  for (unsigned i = 0; i < 17; ++i) {
    benchmarks.add(ids[i], [](bench::Context &) {});
  }
  ut::expect(benchmarks.count == 16 && ut::same_id(benchmarks.error, "benchmark_capacity"));
}
void cleanup_guards() {
  // An arbitrary valid 1 MHz fixture clock: only cleanup/error accounting is
  // under test, so one iteration/sample and no warmup avoid unrelated work.
  bench::Clock clock{.frequency = 1000000};
  bench::Context context{.clock = clock, .iterations = 1, .capacity = 1, .warmup = 0, .samples = 1};
  unsigned cleaned = 0, called = 0;
  context.measure_batches([](usize) { return false; }, [&](usize) { ++called; },
                          [&](usize) {
                            ++cleaned;
                            return true;
                          });
  ut::expect(!context.valid && called == 0 && cleaned == 1);
  context.valid = true;
  context.measure_batches([](usize) { return true; }, [&](usize) { ++called; },
                          [&](usize) {
                            ++cleaned;
                            return false;
                          });
  ut::expect(!context.valid && called == 1 && cleaned == 2);

  // An uninstalled descriptor checks the snapshot filter, not MMU behavior.
  mm::PageTable table;
  auto &entry = table.entries[0];
  entry.set_page(page_size, mm::page_perms::USER_RW);
  const auto address = reinterpret_cast<PhysAddr>(&table);
  const auto original = entry.raw;
  const auto hash = page_table_hash(address);
  entry.raw ^= mm::page_attr::AF;
#if !defined(MOSS_ARCH_ARM64)
  entry.raw ^= mm::page_attr::DIRTY;
#endif
  ut::expect(page_table_hash(address) == hash);
  entry.raw = original ^ mm::page_attr::USER;
  ut::expect(page_table_hash(address) != hash);
  entry.raw = original ^ mm::page_attr::SW_COW;
  ut::expect(page_table_hash(address) != hash);
  entry.raw = original;
  entry.make_readonly();
  ut::expect(page_table_hash(address) != hash);
#if defined(MOSS_ARCH_RISCV64)
  entry.raw = original ^ mm::page_attr::EXECUTE;
#else
  entry.raw = original ^ mm::page_attr::XN;
#endif
  ut::expect(page_table_hash(address) != hash);
  entry.set_page(2 * page_size, mm::page_perms::USER_RW);
  ut::expect(page_table_hash(address) != hash);
}
void heap_bounds() {
  // One TiB is intentionally beyond the linker-reserved heap, testing rejection
  // without requiring allocation or iteration proportional to the request.
  auto before = mm::RuntimeHeapAllocator::get_heap_end();
  ut::expect(before <= moss::abi::linker::heap_end());
  ut::expect(!mm::RuntimeHeapAllocator::expand_heap(1ULL << 40));
  ut::expect(before == mm::RuntimeHeapAllocator::get_heap_end());
}
void register_self_cases() {
  ut::register_suite("self", [] {
    ut::register_test("accounting_registration", accounting);
    ut::register_test("registry_limits", registry_limits);
    ut::register_test("cleanup_guards", cleanup_guards);
    ut::register_test("heap_bounds", heap_bounds);
  });
}

} // namespace moss::test::validation
