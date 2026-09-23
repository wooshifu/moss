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
#include "validation/memory_internal.hpp"
#include "validation/core_cases.hpp"

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
struct ContainerValue {
  u32 value;
  u32 *destroyed;
  ContainerValue(u32 v, u32 *counter) : value(v), destroyed(counter) {}
  ~ContainerValue() { ++*destroyed; }
};

void container_ownership() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  u32 destroyed = 0;
  // If reachable nodes have already been reclaimed, do not walk them again
  // during failed-case cleanup. The host discards this suite's kernel.
  auto *list = new containers::LockedList<ContainerValue>();
  for (u32 value = 1; value <= 3; ++value) {
    list->push_front(value, &destroyed);
  }
  logging::klog::info("Container ownership: {} reachable values destroyed after insertion", destroyed);
  if (!ut::expect(destroyed == 0)) {
    return;
  }
  u32 count = 0;
  u32 sum = 0;
  list->for_each([&](const ContainerValue &value) {
    ++count;
    sum += value.value;
  });
  ut::expect(count == 3 && sum == 6);
  delete list;
  ut::expect(destroyed == 3);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}

void container_release_reuse() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  u32 destroyed = 0;
  auto *list = new containers::LockedList<ContainerValue>();
  constexpr u32 length = 1024;
  for (u32 i = 0; i < length; ++i) {
    list->push_front(i, &destroyed);
    if (!ut::expect(destroyed == 0)) {
      return;
    }
  }
  if (!ut::expect(destroyed == 0 && list->size() == length)) {
    return;
  }
  constexpr u32 removed[] = {0, 511, length - 1}; // Tail, interior, head.
  u32 deleted = 0;
  for (u32 id : removed) {
    if (!ut::expect(list->remove_if([&](const ContainerValue &item) { return item.value == id; }))) {
      return;
    }
    if (!ut::expect(destroyed == ++deleted)) {
      return;
    }
    list->for_each([&](const ContainerValue &item) { ut::expect(item.value != id); });
  }
  u32 count = 0;
  u32 sum = 0;
  list->for_each([&](const ContainerValue &value) {
    ++count;
    sum += value.value;
  });
  ut::expect(count == length - 3 && sum == length * (length - 1) / 2 - 511 - (length - 1));
  list->clear(); // No bounded retirement queue remains.
  if (!ut::expect(destroyed == length && list->empty() && list->size() == 0)) {
    return;
  }
  list->push_front(length, &destroyed);
  ut::expect(list->size() == 1 && destroyed == length);
  delete list;
  ut::expect(destroyed == length + 1);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}

void container_map_ownership() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  u32 destroyed = 0;
  // Force collisions and use real owned values, as the IPC channel map does.
  auto *map = new containers::LockedHashMap<u32, shared_ptr<ContainerValue>, 1>();
  for (u32 key = 1; key <= 3; ++key) {
    map->insert_or_update(key, make_shared<ContainerValue>(key, &destroyed));
  }
  if (!ut::expect(destroyed == 0 && map->size() == 3)) {
    return;
  }
  map->insert_or_update(u32{2}, make_shared<ContainerValue>(u32{20}, &destroyed));
  if (!ut::expect(destroyed == 1 && map->size() == 3)) {
    return;
  }
  for (u32 key = 1; key <= 3; ++key) {
    auto value = map->find(key);
    ut::expect(value && *value && (*value)->value == (key == 2 ? 20 : key));
  }
  u32 deleted = 1;
  constexpr u32 keys[] = {1, 3, 2};
  for (u32 key : keys) {
    ut::expect(map->remove(key));
    ut::expect(!map->remove(key));
    if (!ut::expect(destroyed == ++deleted && !map->find(key))) {
      return;
    }
  }
  ut::expect(map->empty());
  delete map;
  ut::expect(destroyed == 4);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}
void container_held_reader() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  u32 destroyed = 0;
  auto *map = new containers::LockedHashMap<u32, shared_ptr<ContainerValue>, 1>();
  map->insert_or_update(u32{1}, make_shared<ContainerValue>(u32{1}, &destroyed));
  {
    auto borrowed = map->find(u32{1});
    if (!ut::expect(borrowed && *borrowed)) {
      return;
    }
    ut::expect(map->remove(u32{1}));
    logging::klog::info("Container held reader: {} values destroyed before reader release", destroyed);
    if (!ut::expect(destroyed == 0)) {
      return; // Do not dereference reclaimed storage; discard this failed suite.
    }
    ut::expect((*borrowed)->value == 1);
  }
  delete map;
  ut::expect(destroyed == 1);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}

struct ReentrantValue {
  using Map = containers::LockedHashMap<u32, shared_ptr<ReentrantValue>, 1>;
  Map *owner;
  u32 *destroyed;
  ReentrantValue(Map *map, u32 *counter) : owner(map), destroyed(counter) {}
  ~ReentrantValue() {
    // This would deadlock if remove/replacement invoked destructors under the map lock.
    ut::expect(owner->size() <= 1);
    ++*destroyed;
  }
};

void container_reentry() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  {
    containers::LockedList<u32> list;
    list.push_front(u32{1});
    auto copy = list.find(u32{1});
    ut::expect(list.update_if([](u32 value) { return value == 1; }, [](u32 &value) { value = 2; }));
    ut::expect(copy && *copy == 1 && !list.find(u32{1}));
    ut::expect(!list.push_front_unless([](u32 value) { return value == 2; }, u32{3}));
    list.push_front(u32{3});
    u32 count = 0;
    list.for_each_snapshot([&](u32 value) {
      ut::expect(list.remove(value));
      ++count;
    });
    ut::expect(count == 2 && list.empty());

    containers::LockedHashMap<u32, u32, 1> values;
    ut::expect(values.get_or_insert(u32{1}, [] { return u32{10}; }) == 10);
    ut::expect(values.get_or_insert(u32{1}, [] { return u32{20}; }) == 10);
    values.insert_or_update(u32{2}, u32{20});
    auto wider_key = values.find(u64{1});
    ut::expect(wider_key && *wider_key == 10);
    count = 0;
    values.for_each_snapshot([&](const auto &entry) {
      ut::expect(values.remove(entry.key));
      ++count;
    });
    ut::expect(count == 2 && values.empty());

    ReentrantValue::Map map;
    u32 destroyed = 0;
    map.insert_or_update(u32{1}, make_shared<ReentrantValue>(&map, &destroyed));
    map.insert_or_update(u32{1}, make_shared<ReentrantValue>(&map, &destroyed));
    ut::expect(destroyed == 1);
    auto held = map.find(u32{1});
    map.clear();
    ut::expect(held && destroyed == 1);
    held.reset();
    ut::expect(destroyed == 2);
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}

void kernel_stack_initialization() {
  process::Process owner(0);
  auto *thread = new process::Thread(0, 0);
  if (!ut::expect(thread != nullptr)) {
    return;
  }
  if (!ut::expect(owner.register_thread(thread).has_value())) {
    delete thread;
    return;
  }
  constexpr usize order = 2;
  // Thread::allocate_kernel_stack uses order 2: four 4 KiB pages (16 KiB).
  // Poison that exact block before reuse to verify the entire stack is cleared.
  constexpr usize size = page_size << order;
  auto dirty = mm::allocate_pages(order);
  if (!ut::expect(dirty.has_value())) {
    return;
  }
  auto *words = reinterpret_cast<u64 *>(phys_to_virt(*dirty));
  for (usize i = 0; i < size / sizeof(u64); ++i) {
    words[i] = 0xfeedfacecafebeefULL;
  }

  // Exhaust the real allocator, then offer only the poisoned block. This
  // checks rollback and recycled-page initialization without allocation hooks.
  PagePressure pressure;
  ut::expect(pressure.acquire(0));
  auto exhausted = thread->allocate_kernel_stack();
  ut::expect(!exhausted && exhausted.error() == ErrorCode::OutOfMemory);
  ut::expect(thread->kernel_stack_base == 0 && thread->kernel_stack_size == 0);
  ut::expect(mm::free_pages(*dirty, order).has_value());
  auto allocated = thread->allocate_kernel_stack();
  pressure.release();
  if (!ut::expect(allocated.has_value())) {
    return;
  }
  const auto base = thread->kernel_stack_base;
  if (!ut::expect(base != 0 && base % size == 0 && thread->kernel_stack_size == size)) {
    return;
  }
  words = reinterpret_cast<u64 *>(base);
  bool zeroed = true;
  for (usize i = 0; i < size / sizeof(u64); ++i) {
    zeroed = zeroed && words[i] == 0;
  }
  ut::expect(zeroed);
  ut::expect(thread->kernel_stack_top() == base + size);
  auto repeated = thread->allocate_kernel_stack();
  ut::expect(!repeated && repeated.error() == ErrorCode::InvalidState);
  ut::expect(thread->kernel_stack_base == base && thread->kernel_stack_size == size);
}

void scheduler_self_selection(bool realtime) {
  using namespace process;
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  if (!ut::expect(scheduler.get() != nullptr)) {
    return;
  }
  Thread current(0, 0), peer(1, 0);
  const auto cpu = arch::get_current_cpu_id();
  current.cpu = peer.cpu = cpu;
  current.cpu_affinity_mask.set(cpu);
  peer.cpu_affinity_mask.set(cpu);
  current.state = ProcessState::Running;
  current.se.vruntime = 1;
  current.se.sum_exec_runtime = 2 * cfs_params::SCHED_LATENCY_NS;
  peer.se.vruntime = 1000000000ULL;
  if (realtime) {
    current.sched_class = SchedClass::RealTime;
    current.sched_policy = SchedPolicy::RR;
    current.rt.priority = priority::DEFAULT_RT_PRIORITY;
    current.rt.time_slice_remaining = 0;
  }

  // Exercise the production tick with local queues, without dispatching a
  // synthetic context or exposing it to this CPU's real timer interrupt.
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  auto *original = CfsScheduler::get_current_task();
  CfsScheduler::set_current_task(&current);
  if (!realtime) {
    scheduler->enqueue_task(&peer, cpu);
  }
  scheduler->scheduler_tick();
  const bool running = current.state == ProcessState::Running;
  const bool unqueued = !current.se.rb_on_rq && scheduler->get_cpu_nr_running(cpu) == (realtime ? 0U : 1U);
  const bool reselected = scheduler->total_preemptions() == 1;
  // Also clean up the unfixed self-selection state, so a failed assertion
  // remains a reportable failure rather than a second insertion hanging.
  scheduler->dequeue_task(&current);
  if (!realtime) {
    scheduler->dequeue_task(&peer);
  }
  CfsScheduler::set_current_task(original);
  if (restore_irqs) {
    arch::enable_interrupts();
  }
  ut::expect(reselected);
  ut::expect(running);
  ut::expect(unqueued);
  ut::expect(scheduler->get_cpu_nr_running(cpu) == 0);
}

void migration_current_owner() {
  using namespace process;
  if (!ut::expect(g_num_cpus >= 2)) {
    return;
  }
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  unique_ptr<LoadBalancer> balancer(new LoadBalancer());
  if (!ut::expect(scheduler && balancer)) {
    return;
  }
  Thread current(0, 0), peer(1, 0);
  const auto source = arch::get_current_cpu_id();
  const auto target = (source + 1) % g_num_cpus;
  current.cpu_affinity_mask.set(source);
  current.cpu_affinity_mask.set(target);
  peer.cpu_affinity_mask.set(source);
  peer.cpu_affinity_mask.set(target);
  current.se.vruntime = cfs_params::SCHED_LATENCY_NS;
  peer.se.vruntime = 1;

  // Reproduce yield/preemption's published-Ready, unsaved-continuation window
  // through real queues and migration, without dispatching synthetic contexts.
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  auto *original = CfsScheduler::get_current_task();
  CfsScheduler::set_current_task(&current);
  scheduler->enqueue_task(&peer, source);
  scheduler->enqueue_task(&current, source);
  (void)balancer->migrate_task(source, target, *scheduler);
  const bool retained = current.cpu == source && scheduler->pick_next_task(target) != &current;
  scheduler->dequeue_task(&current);
  scheduler->dequeue_task(&peer);
  CfsScheduler::set_current_task(original);

  // A pinned highest-vruntime task must not hide another eligible task.
  // These local queues never dispatch the synthetic contexts on the target.
  current.cpu_affinity_mask = CpuBitmap::single(source);
  scheduler->enqueue_task(&peer, source);
  scheduler->enqueue_task(&current, source);
  const bool eligible_peer = balancer->migrate_task(source, target, *scheduler) && current.cpu == source &&
                             peer.cpu == target && scheduler->get_cpu_nr_running(source) == 1 &&
                             scheduler->get_cpu_nr_running(target) == 1;
  scheduler->dequeue_task(&current);
  scheduler->dequeue_task(&peer);

  current.cpu_affinity_mask.set(target);
  scheduler->enqueue_task(&peer, source);
  scheduler->enqueue_task(&current, source);
  const bool moved = balancer->migrate_task(source, target, *scheduler) && current.cpu == target &&
                     peer.cpu == source && scheduler->get_cpu_nr_running(source) == 1 &&
                     scheduler->get_cpu_nr_running(target) == 1;
  scheduler->dequeue_task(&peer);
  scheduler->enqueue_task(&peer, target);
  // Exercise a remote source queue from this CPU, retaining one task there.
  const bool moved_back = balancer->migrate_task(target, source, *scheduler) && current.cpu == source &&
                          peer.cpu == target && scheduler->get_cpu_nr_running(source) == 1 &&
                          scheduler->get_cpu_nr_running(target) == 1;
  scheduler->dequeue_task(&current);
  scheduler->dequeue_task(&peer);

  current.cpu_affinity_mask = peer.cpu_affinity_mask = CpuBitmap::single(source);
  scheduler->enqueue_task(&peer, source);
  scheduler->enqueue_task(&current, source);
  const bool pinned = !balancer->migrate_task(source, target, *scheduler) && current.cpu == source &&
                      peer.cpu == source && scheduler->get_cpu_nr_running(source) == 2 &&
                      scheduler->get_cpu_nr_running(target) == 0;
  scheduler->dequeue_task(&current);
  scheduler->dequeue_task(&peer);
  if (restore_irqs) {
    arch::enable_interrupts();
  }
  ut::expect(retained);
  ut::expect(eligible_peer);
  ut::expect(moved);
  ut::expect(moved_back);
  ut::expect(pinned);
  ut::expect(scheduler->get_cpu_nr_running(source) == 0 && scheduler->get_cpu_nr_running(target) == 0);
}

void register_containers_cases() {
  ut::register_suite("containers", [] {
    ut::register_test("queue_reuse", moss::test::queue_regression::run);
    ut::register_test("ipc_heap_rollback", ipc_heap_rollback);
    ut::register_test("ipc_shared_backing", moss::test::ipc_regression::shared_backing);
    ut::register_test("ipc_shared_lifecycle", moss::test::ipc_regression::shared_lifecycle);
    ut::register_test("ipc_service_lifecycle", moss::test::ipc_regression::service_lifecycle);
    ut::register_test("ipc_ring_wrap", moss::test::ipc_regression::ring_wrap);
    ut::register_test("ipc_ring_geometry", moss::test::ipc_regression::ring_geometry);
    ut::register_test("ownership", container_ownership);
    ut::register_test("release_reuse", container_release_reuse);
    ut::register_test("map_ownership", container_map_ownership);
    ut::register_test("held_reader", container_held_reader);
    ut::register_test("reentry", container_reentry);
  });
}

void register_scheduler_cases() {
  ut::register_suite("scheduler", [] {
    ut::register_test("pelt_large_runtime", moss::test::scheduler_regression::pelt_large_runtime);
    ut::register_test("pelt_partitioned_runtime", moss::test::scheduler_regression::pelt_partitioned_runtime);
    ut::register_test("pelt_half_life", moss::test::scheduler_regression::pelt_half_life);
    ut::register_test("pelt_continuous_normalization", moss::test::scheduler_regression::pelt_continuous_normalization);
    ut::register_test("kernel_stack_initialization", kernel_stack_initialization);
    ut::register_test("cfs_self_selection", [] { scheduler_self_selection(false); });
    ut::register_test("rr_self_selection", [] { scheduler_self_selection(true); });
    ut::register_test("migration_current_owner", migration_current_owner);
  });
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
