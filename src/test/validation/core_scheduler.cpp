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
#include "validation/memory_internal.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;
using moss::test::validation::HeapPressure;

namespace moss::test::validation {
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

void ipc_priority_inheritance() {
  using namespace process;
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  if (!ut::expect(scheduler.get() != nullptr))
    return;

  Thread high(0, 0), low(1, 0), server(2, 0), backend(3, 0), dying(4, 0);
  Thread normal_caller(5, 0), normal_server(6, 0), normal_backend(7, 0);
  PriorityDonation high_call{}, low_call{}, nested{}, cycle{}, death_call{}, normal_call{}, normal_nested{},
      normal_cycle{};
  const auto cpu = arch::get_current_cpu_id();
  high.sched_class = low.sched_class = SchedClass::RealTime;
  high.rt.priority = 80;
  low.rt.priority = 60;
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  auto *original = CfsScheduler::get_current_task();
  CfsScheduler::set_current_task(&high);

  scheduler->enqueue_task(&server, cpu);
  scheduler->enqueue_task(&backend, cpu);
  // Synthetic absolute times verify the min rule without depending on a timer.
  u64 high_deadline = 900;
  ut::expect(scheduler->begin_ipc_call(&high_call, &high, &high_deadline));
  ut::expect(high_deadline == 900 && high_call.deadline_ns == 900);
  scheduler->bind_ipc_server(&high_call, &server);
  ut::expect(server.effective_rt_priority() == 80 && server.rt_on_rq && scheduler->pick_next_task(cpu) == &server);
  ut::expect(!scheduler->rebind_ipc_server(&high_call, &backend, nullptr));
  ut::expect(scheduler->rebind_ipc_server(&high_call, &server, &backend));
  ut::expect(server.effective_rt_priority() == 0 && server.se.rb_on_rq && backend.effective_rt_priority() == 80);
  scheduler->bind_ipc_server(&high_call, nullptr);
  ut::expect(backend.effective_rt_priority() == 0 && backend.se.rb_on_rq);
  scheduler->bind_ipc_server(&high_call, &server);

  scheduler->dequeue_task(&server);
  server.state = ProcessState::Sleeping;
  u64 nested_deadline = 0;
  ut::expect(scheduler->begin_ipc_call(&nested, &server, &nested_deadline));
  ut::expect(nested_deadline == high_deadline && nested.deadline_ns == high_deadline);
  scheduler->bind_ipc_server(&nested, &backend);
  ut::expect(backend.effective_rt_priority() == 80 && backend.rt_on_rq && scheduler->pick_next_task(cpu) == &backend);

  ut::expect(scheduler->begin_ipc_call(&low_call, &low));
  scheduler->bind_ipc_server(&low_call, &server);
  scheduler->end_ipc_call(&high_call);
  ut::expect(server.effective_rt_priority() == 60 && backend.effective_rt_priority() == 60);
  scheduler->end_ipc_call(&low_call);
  ut::expect(server.effective_rt_priority() == 0 && backend.effective_rt_priority() == 0 && !backend.rt_on_rq &&
             backend.se.rb_on_rq);

  scheduler->dequeue_task(&backend);
  backend.state = ProcessState::Sleeping;
  ut::expect(scheduler->begin_ipc_call(&cycle, &backend));
  scheduler->bind_ipc_server(&cycle, &server);
  ut::expect(scheduler->begin_ipc_call(&high_call, &high));
  scheduler->bind_ipc_server(&high_call, &server);
  ut::expect(server.effective_rt_priority() == 80 && backend.effective_rt_priority() == 80);
  scheduler->end_ipc_call(&high_call);
  ut::expect(server.effective_rt_priority() == 0 && backend.effective_rt_priority() == 0);
  scheduler->end_ipc_call(&cycle);
  scheduler->end_ipc_call(&nested);

  ut::expect(scheduler->begin_ipc_call(&death_call, &high));
  scheduler->bind_ipc_server(&death_call, &dying);
  ut::expect(dying.effective_rt_priority() == 80);
  scheduler->forget_ipc_thread(&dying);
  ut::expect(death_call.server == nullptr && dying.ipc_scheduler == nullptr && dying.effective_rt_priority() == 0);
  scheduler->end_ipc_call(&death_call);

  scheduler->set_base_nice(&normal_caller, -10);
  scheduler->set_base_nice(&normal_server, 10);
  scheduler->set_base_nice(&normal_backend, 15);
  scheduler->enqueue_task(&normal_server, cpu);
  scheduler->enqueue_task(&normal_backend, cpu);
  u64 normal_deadline = 600;
  ut::expect(scheduler->begin_ipc_call(&normal_call, &normal_caller, &normal_deadline));
  scheduler->bind_ipc_server(&normal_call, &normal_server);
  ut::expect(normal_server.effective_cfs_nice() == -10 && normal_server.se.rb_on_rq &&
             normal_server.se.weight == cfs_params::nice_to_weight(-10));
  scheduler->dequeue_task(&normal_server);
  normal_server.state = ProcessState::Sleeping;
  u64 shorter_deadline = 500;
  ut::expect(scheduler->begin_ipc_call(&normal_nested, &normal_server, &shorter_deadline));
  ut::expect(shorter_deadline == 500 && normal_nested.deadline_ns == 500);
  scheduler->bind_ipc_server(&normal_nested, &normal_backend);
  ut::expect(normal_backend.effective_cfs_nice() == -10);
  scheduler->dequeue_task(&normal_backend);
  normal_backend.state = ProcessState::Sleeping;
  u64 transitive_deadline = 0;
  ut::expect(scheduler->begin_ipc_call(&normal_cycle, &normal_backend, &transitive_deadline));
  ut::expect(transitive_deadline == shorter_deadline && normal_cycle.deadline_ns == shorter_deadline);
  scheduler->bind_ipc_server(&normal_cycle, &normal_server);
  scheduler->set_base_nice(&normal_caller, -5);
  ut::expect(normal_server.effective_cfs_nice() == -5 && normal_backend.effective_cfs_nice() == -5);
  scheduler->end_ipc_call(&normal_call);
  ut::expect(normal_server.effective_cfs_nice() == 10 && normal_backend.effective_cfs_nice() == 10);
  scheduler->end_ipc_call(&normal_cycle);
  scheduler->end_ipc_call(&normal_nested);
  ut::expect(normal_cycle.deadline_ns == 0 && normal_nested.deadline_ns == 0);
  ut::expect(normal_backend.effective_cfs_nice() == 15 && normal_backend.se.weight == cfs_params::nice_to_weight(15));

  Thread deadline_server(8, 0);
  PriorityDonation first_deadline{}, second_deadline{}, earliest_nested{};
  u64 first = 900, second = 400, requested = 700;
  ut::expect(scheduler->begin_ipc_call(&first_deadline, &high, &first));
  scheduler->bind_ipc_server(&first_deadline, &deadline_server);
  ut::expect(scheduler->begin_ipc_call(&second_deadline, &low, &second));
  scheduler->bind_ipc_server(&second_deadline, &deadline_server);
  ut::expect(scheduler->begin_ipc_call(&earliest_nested, &deadline_server, &requested));
  ut::expect(requested == second && earliest_nested.deadline_ns == second);
  scheduler->end_ipc_call(&earliest_nested);
  scheduler->end_ipc_call(&second_deadline);
  scheduler->end_ipc_call(&first_deadline);

  CfsScheduler::set_current_task(original);
  if (restore_irqs)
    arch::enable_interrupts();
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

void register_scheduler_cases() {
  ut::register_suite("scheduler", [] {
    ut::register_test("pelt_large_runtime", moss::test::scheduler_regression::pelt_large_runtime);
    ut::register_test("pelt_partitioned_runtime", moss::test::scheduler_regression::pelt_partitioned_runtime);
    ut::register_test("pelt_half_life", moss::test::scheduler_regression::pelt_half_life);
    ut::register_test("pelt_continuous_normalization", moss::test::scheduler_regression::pelt_continuous_normalization);
    ut::register_test("kernel_stack_initialization", kernel_stack_initialization);
    ut::register_test("cfs_self_selection", [] { scheduler_self_selection(false); });
    ut::register_test("rr_self_selection", [] { scheduler_self_selection(true); });
    ut::register_test("ipc_priority_inheritance", ipc_priority_inheritance);
    ut::register_test("migration_current_owner", migration_current_owner);
  });
}

} // namespace moss::test::validation
