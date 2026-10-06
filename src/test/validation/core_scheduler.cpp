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

void cpu_runtime_accounting() {
  using namespace process;
  Thread cfs(0, 0), rt(1, 0);
  rt.sched_class = SchedClass::RealTime;

  // Synthetic nanosecond timestamps yield 60 ns, then 50 ns of running time.
  // A backward sample must not move the baseline or double-count the interval.
  constexpr u64 start_ns = 100;
  constexpr u64 first_ns = 160;
  constexpr u64 second_ns = 210;
  Thread *threads[] = {&cfs, &rt};
  for (auto *thread : threads) {
    auto &se = thread->se;
    se.exec_start = start_ns;
    ut::expect(se.charge_runtime(first_ns) == first_ns - start_ns);
    ut::expect(se.cpu_runtime_ns == first_ns - start_ns && se.exec_start == first_ns);
    ut::expect(se.charge_runtime(first_ns) == 0);
    ut::expect(se.charge_runtime(start_ns) == 0);
    ut::expect(se.cpu_runtime_ns == first_ns - start_ns && se.exec_start == first_ns);
    ut::expect(se.charge_runtime(second_ns) == second_ns - first_ns);
    ut::expect(se.cpu_runtime_ns == second_ns - start_ns && se.exec_start == second_ns);
  }
}

void cpu_budget_accounting() {
  using namespace process;
  Thread cfs(0, 0), rt(1, 0);
  rt.sched_class = SchedClass::RealTime;

  // Synthetic 10 ns/20 ns windows expose the exact quota and carry-over debt
  // with small values; their scale is irrelevant to scheduler policy.
  constexpr u64 runtime_ns = 10;
  constexpr u64 period_ns = 2 * runtime_ns;
  auto &cfs_budget = cfs.se.cpu_budget;
  ut::expect(!cfs_budget.exhausted(0)); // A thread has no limit until configured.
  ut::expect(!cfs_budget.configure(0, period_ns, 0));
  ut::expect(!cfs_budget.configure(runtime_ns, 0, 0));
  ut::expect(!cfs_budget.configure(period_ns + 1, period_ns, 0));
  if (!ut::expect(cfs_budget.configure(runtime_ns, period_ns, 0))) {
    return;
  }
  ut::expect(!cfs_budget.configure(period_ns + 1, period_ns, 0));
  ut::expect(cfs.se.charge_runtime(runtime_ns) == runtime_ns);
  ut::expect(cfs_budget.spent_ns == runtime_ns && cfs_budget.exhausted(runtime_ns));
  ut::expect(cfs.se.charge_runtime(runtime_ns) == 0 && cfs.se.charge_runtime(0) == 0);
  ut::expect(cfs_budget.spent_ns == runtime_ns && cfs.se.cpu_runtime_ns == runtime_ns);
  ut::expect(!cfs_budget.exhausted(period_ns) && cfs_budget.spent_ns == 0);
  ut::expect(!cfs_budget.exhausted(2 * period_ns) && cfs_budget.spent_ns == 0);
  // A new dispatch starts at the new window; unused quota did not accumulate.
  cfs.se.exec_start = 2 * period_ns;
  ut::expect(cfs.se.charge_runtime(2 * period_ns + runtime_ns) == runtime_ns);
  ut::expect(cfs_budget.exhausted(2 * period_ns + runtime_ns));

  auto &rt_budget = rt.se.cpu_budget;
  if (!ut::expect(rt_budget.configure(runtime_ns, period_ns, 0))) {
    return;
  }
  // A single RT interval crossing the boundary consumes the new window and
  // carries the old overrun forward; CFS fairness counters cannot erase it.
  const u64 crossed_ns = period_ns + runtime_ns / 2;
  ut::expect(rt.se.charge_runtime(crossed_ns) == crossed_ns);
  ut::expect(rt_budget.period_start_ns == period_ns && rt_budget.spent_ns == runtime_ns + runtime_ns / 2);
  ut::expect(rt_budget.exhausted(crossed_ns));
  ut::expect(!rt_budget.exhausted(2 * period_ns) && rt_budget.spent_ns == runtime_ns / 2);
  ut::expect(cfs_budget.spent_ns == runtime_ns && rt.se.cpu_runtime_ns == crossed_ns);
}

void cpu_budget_admission() {
  using namespace process;
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  if (!ut::expect(scheduler.get() != nullptr)) {
    return;
  }
  Thread first(0, 0), second(1, 0);
  const auto cpu = arch::get_current_cpu_id();
  first.cpu = second.cpu = cpu;
  // Fractions of one second make capacity and period-mismatch outcomes exact;
  // this synthetic test does not depend on elapsed time or scheduler ticks.
  constexpr u64 period_ns = 1'000'000'000ULL;
  constexpr u64 first_runtime_ns = period_ns * 3 / 4;
  constexpr u64 second_runtime_ns = period_ns / 2;
  ut::expect(scheduler->request_cpu_budget(&first, 0, period_ns) == BudgetProfileResult::Invalid);
  ut::expect(scheduler->request_cpu_budget(&first, period_ns + 1, period_ns) == BudgetProfileResult::Invalid);
  if (!ut::expect(scheduler->request_cpu_budget(&first, first_runtime_ns, period_ns) == BudgetProfileResult::Applied)) {
    return;
  }
  const auto pending = scheduler->cpu_budget_status(&first);
  ut::expect((pending.flags & BudgetProfileStatus::Pending) != 0);
  // The local enqueue is an owner-CPU safe point and installs the request;
  // keep interrupts off until both synthetic contexts leave the runqueue,
  // as in cpu_budget_queue, so a real timer cannot dispatch either one.
  // The existing queue tests cover actual throttling and migration.
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  scheduler->enqueue_task(&first, cpu);
  ut::expect(scheduler->request_cpu_budget(&first, first_runtime_ns, period_ns) ==
             BudgetProfileResult::AlreadyConfigured);
  ut::expect(scheduler->request_cpu_budget(&second, second_runtime_ns, 2 * period_ns) == BudgetProfileResult::Invalid);
  ut::expect(scheduler->request_cpu_budget(&second, second_runtime_ns, period_ns) == BudgetProfileResult::Capacity);
  const auto admitted = scheduler->cpu_budget_status(&first);
  ut::expect(admitted.runtime_ns == first_runtime_ns && admitted.period_ns == period_ns &&
             (admitted.flags & (BudgetProfileStatus::Pending | BudgetProfileStatus::Terminated)) == 0 &&
             first.se.cpu_budget.configured());

  first.state = ProcessState::Terminated;
  scheduler->dequeue_task(&first);
  const auto released = scheduler->cpu_budget_status(&first);
  ut::expect(released.runtime_ns == 0 && released.period_ns == 0 &&
             (released.flags & BudgetProfileStatus::Terminated) != 0 &&
             released.cpu_runtime_ns >= admitted.cpu_runtime_ns);
  ut::expect(scheduler->request_cpu_budget(&second, second_runtime_ns, period_ns) == BudgetProfileResult::Applied);
  scheduler->enqueue_task(&second, cpu);
  const auto replacement = scheduler->cpu_budget_status(&second);
  ut::expect(replacement.runtime_ns == second_runtime_ns && replacement.period_ns == period_ns &&
             (replacement.flags & BudgetProfileStatus::Pending) == 0 && second.se.cpu_budget.configured());
  second.state = ProcessState::Terminated;
  scheduler->dequeue_task(&second);
  if (restore_irqs) {
    arch::enable_interrupts();
  }
}

void cpu_budget_queue(bool realtime) {
  using namespace process;
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  if (!ut::expect(scheduler.get() != nullptr)) {
    return;
  }
  Thread limited(0, 0), peer(1, 0), caller(2, 0), sleeper(3, 0);
  const auto cpu = arch::get_current_cpu_id();
  // A synthetic minute keeps the real clock inside the first window while
  // this local-queue test forces a refill at the precise period boundary.
  constexpr u64 period_ns = 60'000'000'000ULL;
  constexpr u64 runtime_ns = period_ns / 2;
  const u64 anchor_ns = timer::TimerSubsystem::instance().now_ns();
  if (!ut::expect(anchor_ns > 0)) {
    return;
  }
  if (realtime) {
    limited.sched_class = SchedClass::RealTime;
    limited.sched_policy = SchedPolicy::Fifo;
  }
  caller.sched_class = SchedClass::RealTime;
  caller.state = ProcessState::Running;
  caller.cpu = cpu;
  limited.se.exec_start = anchor_ns;
  if (!ut::expect(limited.se.cpu_budget.configure(runtime_ns, period_ns, anchor_ns))) {
    return;
  }
  ut::expect(limited.se.charge_runtime(anchor_ns + runtime_ns) == runtime_ns);
  sleeper.se.exec_start = anchor_ns;
  if (!ut::expect(sleeper.se.cpu_budget.configure(runtime_ns, period_ns, anchor_ns))) {
    return;
  }
  ut::expect(sleeper.se.charge_runtime(anchor_ns + runtime_ns) == runtime_ns);

  // Keep synthetic contexts off the dispatch path, as in self-selection tests.
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  auto *original = CfsScheduler::get_current_task();
  CfsScheduler::set_current_task(&caller);
  scheduler->enqueue_task(&limited, cpu);
  scheduler->enqueue_task(&peer, cpu);
  scheduler->enqueue_task(&sleeper, cpu);
  const bool sleeper_parked = sleeper.budget_parked && !sleeper.se.rb_on_rq && !sleeper.rt_on_rq;
  scheduler->dequeue_task(&sleeper);
  sleeper.state = ProcessState::Sleeping;
  // Each exhausted Ready thread has made exactly one transition into the
  // budget wait list; status counts that transition, not later tick checks.
  const bool gated = limited.budget_parked && !limited.se.rb_on_rq && !limited.rt_on_rq && sleeper_parked &&
                     scheduler->cpu_budget_status(&limited).throttle_count == 1 &&
                     scheduler->get_cpu_nr_running(cpu) == 1 && scheduler->pick_next_task(cpu) == &peer;

  bool donation_safe = true;
  if (!realtime) {
    PriorityDonation donation{};
    if (ut::expect(scheduler->begin_ipc_call(&donation, &caller))) {
      scheduler->bind_ipc_server(&donation, &limited);
      donation_safe = limited.effective_rt_priority() == caller.effective_rt_priority() && limited.budget_parked &&
                      !limited.se.rb_on_rq && !limited.rt_on_rq && scheduler->pick_next_task(cpu) == &peer;
      scheduler->end_ipc_call(&donation);
    } else {
      donation_safe = false;
    }
  }

  scheduler->replenish_cpu_budgets(anchor_ns + period_ns);
  const bool replenished = !limited.budget_parked && (limited.se.rb_on_rq || limited.rt_on_rq) &&
                           scheduler->get_cpu_nr_running(cpu) == 2 &&
                           (!realtime || scheduler->pick_next_task(cpu) == &limited);
  const bool sleeper_blocked =
      sleeper.state == ProcessState::Sleeping && !sleeper.budget_parked && !sleeper.se.rb_on_rq && !sleeper.rt_on_rq;
  bool dispatch_gated = true;
  if (realtime && replenished) {
    // The RT task wins selection, then a fresh sub-tick charge must keep it
    // parked at dispatch while the unbudgeted peer remains eligible.
    limited.se.exec_start = anchor_ns + period_ns;
    ut::expect(limited.se.charge_runtime(anchor_ns + period_ns + runtime_ns) == runtime_ns);
    dispatch_gated = scheduler->take_next_task(cpu) == &peer && limited.budget_parked && !limited.rt_on_rq &&
                     scheduler->get_cpu_nr_running(cpu) == 0;
  }
  scheduler->dequeue_task(&limited);
  scheduler->dequeue_task(&peer);
  scheduler->dequeue_task(&sleeper);

  bool yield_charged = true;
  if (!realtime) {
    Thread yielding(4, 0);
    // One nanosecond behind the already sampled clock guarantees that the
    // current thread has spent its one-nanosecond quota at enqueue.
    constexpr u64 yield_runtime_ns = 1;
    const u64 yield_start_ns = anchor_ns - yield_runtime_ns;
    yielding.se.exec_start = yield_start_ns;
    yielding.cpu = cpu;
    yielding.state = ProcessState::Running;
    if (ut::expect(yielding.se.cpu_budget.configure(yield_runtime_ns, period_ns, yield_start_ns))) {
      CfsScheduler::set_current_task(&yielding);
      scheduler->enqueue_task(&yielding, cpu);
      yield_charged = yielding.budget_parked && !yielding.se.rb_on_rq && !yielding.rt_on_rq &&
                      yielding.se.cpu_budget.spent_ns >= yield_runtime_ns;
      scheduler->dequeue_task(&yielding);
      CfsScheduler::set_current_task(&caller);
    } else {
      yield_charged = false;
    }
  }

  bool affinity_moved = true;
  if (!realtime && g_num_cpus >= 2) {
    Thread affinity(5, 0);
    const u32 target = (cpu + 1) % g_num_cpus;
    affinity.se.exec_start = anchor_ns;
    if (ut::expect(affinity.se.cpu_budget.configure(runtime_ns, period_ns, anchor_ns))) {
      ut::expect(affinity.se.charge_runtime(anchor_ns + runtime_ns) == runtime_ns);
      scheduler->enqueue_task(&affinity, cpu);
      const bool parked = affinity.budget_parked && affinity.cpu == cpu && !affinity.se.rb_on_rq;
      // A parked task can lose its old CPU while it waits for the next period.
      {
        containers::LockGuard<containers::IrqSpinLock> guard(affinity.sleep_lock);
        scheduler->set_task_affinity_mask(&affinity, CpuBitmap::single(target).low_word());
      }
      scheduler->replenish_cpu_budgets(anchor_ns + period_ns);
      affinity_moved = parked && affinity.cpu == target && affinity.cpu_affinity_mask.test(affinity.cpu) &&
                       affinity.se.rb_on_rq && scheduler->get_cpu_nr_running(cpu) == 0 &&
                       scheduler->get_cpu_nr_running(target) == 1 && scheduler->pick_next_task(target) == &affinity;
      scheduler->dequeue_task(&affinity);
    } else {
      affinity_moved = false;
    }
  }

  Thread tick_task(6, 0);
  const u64 tick_anchor_ns = timer::TimerSubsystem::instance().now_ns();
  // Starting one nanosecond before the sampled clock makes the tick's live
  // charge reach this quota even at the clock's coarsest resolution.
  constexpr u64 tick_runtime_ns = 1;
  bool tick_deferred = false;
  if (tick_anchor_ns > 0 &&
      ut::expect(tick_task.se.cpu_budget.configure(tick_runtime_ns, period_ns, tick_anchor_ns - tick_runtime_ns))) {
    tick_task.se.exec_start = tick_anchor_ns - tick_runtime_ns;
    tick_task.cpu = cpu;
    tick_task.state = ProcessState::Running;
    tick_task.preempt_count = 1; // Hold off context switching while the tick charges CPU time.
    if (realtime) {
      tick_task.sched_class = SchedClass::RealTime;
      tick_task.sched_policy = SchedPolicy::Fifo;
    } else {
      scheduler->enqueue_task(&peer, cpu);
    }
    CfsScheduler::set_current_task(&tick_task);
    scheduler->scheduler_tick();
    tick_deferred = tick_task.need_resched && tick_task.state == ProcessState::Running &&
                    tick_task.se.cpu_runtime_ns >= tick_runtime_ns &&
                    tick_task.se.cpu_budget.exhausted(timer::TimerSubsystem::instance().now_ns()) &&
                    scheduler->total_preemptions() == 0;
    CfsScheduler::set_current_task(&caller);
    if (!realtime) {
      scheduler->dequeue_task(&peer);
    }
  }
  CfsScheduler::set_current_task(original);
  if (restore_irqs) {
    arch::enable_interrupts();
  }
  ut::expect(gated);
  ut::expect(donation_safe);
  ut::expect(replenished);
  ut::expect(sleeper_blocked);
  ut::expect(dispatch_gated);
  ut::expect(yield_charged);
  ut::expect(affinity_moved);
  ut::expect(tick_deferred);
  ut::expect(scheduler->get_cpu_nr_running(cpu) == 0);
}

void ipc_priority_inheritance() {
  using namespace process;
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  if (!ut::expect(scheduler.get() != nullptr)) {
    return;
  }

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
  ut::expect(high_deadline == 900 && high_call.deadline_ns.load() == 900);
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
  ut::expect(nested_deadline == high_deadline && nested.deadline_ns.load() == high_deadline);
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
  ut::expect(shorter_deadline == 500 && normal_nested.deadline_ns.load() == 500);
  scheduler->bind_ipc_server(&normal_nested, &normal_backend);
  ut::expect(normal_backend.effective_cfs_nice() == -10);
  scheduler->dequeue_task(&normal_backend);
  normal_backend.state = ProcessState::Sleeping;
  u64 transitive_deadline = 0;
  ut::expect(scheduler->begin_ipc_call(&normal_cycle, &normal_backend, &transitive_deadline));
  ut::expect(transitive_deadline == shorter_deadline && normal_cycle.deadline_ns.load() == shorter_deadline);
  scheduler->bind_ipc_server(&normal_cycle, &normal_server);
  scheduler->set_base_nice(&normal_caller, -5);
  ut::expect(normal_server.effective_cfs_nice() == -5 && normal_backend.effective_cfs_nice() == -5);
  scheduler->end_ipc_call(&normal_call);
  ut::expect(normal_server.effective_cfs_nice() == 10 && normal_backend.effective_cfs_nice() == 10);
  scheduler->end_ipc_call(&normal_cycle);
  scheduler->end_ipc_call(&normal_nested);
  ut::expect(normal_cycle.deadline_ns.load() == 0 && normal_nested.deadline_ns.load() == 0);
  ut::expect(normal_backend.effective_cfs_nice() == 15 && normal_backend.se.weight == cfs_params::nice_to_weight(15));

  Thread deadline_server(8, 0);
  PriorityDonation first_deadline{}, second_deadline{}, earliest_nested{};
  u64 first = 900, second = 400, requested = 700;
  ut::expect(scheduler->begin_ipc_call(&first_deadline, &high, &first));
  scheduler->bind_ipc_server(&first_deadline, &deadline_server);
  ut::expect(scheduler->begin_ipc_call(&second_deadline, &low, &second));
  scheduler->bind_ipc_server(&second_deadline, &deadline_server);
  ut::expect(scheduler->begin_ipc_call(&earliest_nested, &deadline_server, &requested));
  ut::expect(requested == second && earliest_nested.deadline_ns.load() == second);
  scheduler->end_ipc_call(&earliest_nested);
  scheduler->end_ipc_call(&second_deadline);
  scheduler->end_ipc_call(&first_deadline);

  CfsScheduler::set_current_task(original);
  if (restore_irqs) {
    arch::enable_interrupts();
  }
  ut::expect(scheduler->get_cpu_nr_running(cpu) == 0);
}

void ipc_deadline_tightening() {
  using namespace process;
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  if (!ut::expect(scheduler.get() != nullptr)) {
    return;
  }
  // Priority recomputation sends reschedule IPIs; mutate these synthetic wait
  // graphs with interrupts disabled. The threads never enter a run queue.
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();

  Thread front(0, 0), middle(1, 0), back(2, 0), redirect(3, 0), redirect_back(4, 0), late(5, 0), earlier(6, 0);
  PriorityDonation front_wait{}, middle_wait{}, back_wait{}, redirect_wait{}, late_call{}, earlier_call{}, fresh{};
  // These are synthetic absolute nanoseconds, not clock samples: each value
  // distinguishes an inherited limit from a later, strictly earlier donor.
  constexpr u64 front_ns = 900;
  constexpr u64 middle_ns = 800;
  constexpr u64 late_ns = 500;
  constexpr u64 earlier_ns = 300;
  u64 deadline = front_ns;
  ut::expect(scheduler->begin_ipc_call(&front_wait, &front, &deadline));
  scheduler->bind_ipc_server(&front_wait, &middle);
  deadline = middle_ns;
  ut::expect(scheduler->begin_ipc_call(&middle_wait, &middle, &deadline));
  scheduler->bind_ipc_server(&middle_wait, &back);
  deadline = 0;
  ut::expect(scheduler->begin_ipc_call(&redirect_wait, &redirect, &deadline));
  scheduler->bind_ipc_server(&redirect_wait, &redirect_back);
  ut::expect(redirect_wait.deadline_ns.load() == 0);

  late.sched_class = SchedClass::RealTime;
  late.rt.priority = priority::DEFAULT_RT_PRIORITY;
  deadline = late_ns;
  ut::expect(scheduler->begin_ipc_call(&late_call, &late, &deadline));
  scheduler->bind_ipc_server(&late_call, &front);
  ut::expect(front_wait.deadline_ns.load() == late_ns && middle_wait.deadline_ns.load() == late_ns);
  ut::expect(front.effective_rt_priority() == late.rt.priority && back.effective_rt_priority() == late.rt.priority);

  ut::expect(scheduler->rebind_ipc_server(&late_call, &front, &redirect));
  ut::expect(redirect_wait.deadline_ns.load() == late_ns && front_wait.deadline_ns.load() == late_ns &&
             middle_wait.deadline_ns.load() == late_ns);
  ut::expect(front.effective_rt_priority() == 0 && back.effective_rt_priority() == 0 &&
             redirect.effective_rt_priority() == late.rt.priority);

  // Back now calls front, closing the dependency cycle. A new external donor
  // must tighten each wait once, then stop when it revisits the same deadline.
  deadline = 0;
  ut::expect(scheduler->begin_ipc_call(&back_wait, &back, &deadline));
  ut::expect(deadline == late_ns);
  scheduler->bind_ipc_server(&back_wait, &front);
  earlier.sched_class = SchedClass::RealTime;
  // A distinct higher RT priority makes its later removal observable.
  earlier.rt.priority = late.rt.priority + 1;
  deadline = earlier_ns;
  ut::expect(scheduler->begin_ipc_call(&earlier_call, &earlier, &deadline));
  scheduler->bind_ipc_server(&earlier_call, &back);
  ut::expect(front_wait.deadline_ns.load() == earlier_ns && middle_wait.deadline_ns.load() == earlier_ns &&
             back_wait.deadline_ns.load() == earlier_ns);
  ut::expect(front.effective_rt_priority() == earlier.rt.priority &&
             middle.effective_rt_priority() == earlier.rt.priority &&
             back.effective_rt_priority() == earlier.rt.priority);

  scheduler->end_ipc_call(&earlier_call);
  scheduler->end_ipc_call(&late_call);
  ut::expect(front.effective_rt_priority() == 0 && middle.effective_rt_priority() == 0 &&
             back.effective_rt_priority() == 0 && redirect.effective_rt_priority() == 0);
  // Donor removal restores priority, but a live call cannot regain time it
  // already promised to an earlier donor; only its own completion clears it.
  ut::expect(front_wait.deadline_ns.load() == earlier_ns && middle_wait.deadline_ns.load() == earlier_ns &&
             back_wait.deadline_ns.load() == earlier_ns && redirect_wait.deadline_ns.load() == late_ns);
  scheduler->end_ipc_call(&back_wait);
  scheduler->end_ipc_call(&middle_wait);
  scheduler->end_ipc_call(&front_wait);
  scheduler->end_ipc_call(&redirect_wait);
  ut::expect(front_wait.deadline_ns.load() == 0 && middle_wait.deadline_ns.load() == 0 &&
             back_wait.deadline_ns.load() == 0 && redirect_wait.deadline_ns.load() == 0);
  deadline = 0;
  ut::expect(scheduler->begin_ipc_call(&fresh, &front, &deadline));
  ut::expect(deadline == 0 && fresh.deadline_ns.load() == 0);
  scheduler->end_ipc_call(&fresh);

  Thread waiting(7, 0), downstream(8, 0), new_donor(9, 0);
  PriorityDonation waiting_call{}, incoming_call{};
  timer::HrTimer deadline_timer;
  moss::atomic<u64> fired_ns{0};
  deadline_timer.init(
      timer::TimerMode::OneShot,
      [](void *data) noexcept {
        static_cast<moss::atomic<u64> *>(data)->store(timer::TimerSubsystem::instance().now_ns(),
                                                      moss::memory_order_release);
      },
      &fired_ns);
  if (ut::expect(static_cast<bool>(deadline_timer.reserve()))) {
    auto &clock = timer::TimerSubsystem::instance();
    // The original 5 s timer cannot fire during the 200 ms observation. A
    // later 5 ms donor deadline must move its already armed expiry while the
    // waiting thread stays blocked. The 200 ms bound matches timer_capacity's
    // QEMU timer check; it is not a measured worst-case dispatch latency.
    constexpr u64 original_delay_ns = 5'000'000'000ULL;
    constexpr u64 tighter_delay_ns = 5'000'000ULL;
    constexpr u64 observation_ns = 200'000'000ULL;
    const u64 original_deadline = clock.now_ns() + original_delay_ns;
    deadline = original_deadline;
    ut::expect(scheduler->begin_ipc_call(&waiting_call, &waiting, &deadline));
    scheduler->arm_ipc_deadline_timer(&waiting_call, &deadline_timer);
    scheduler->bind_ipc_server(&waiting_call, &downstream);
    const u64 tightened_deadline = clock.now_ns() + tighter_delay_ns;
    deadline = tightened_deadline;
    ut::expect(scheduler->begin_ipc_call(&incoming_call, &new_donor, &deadline));
    scheduler->bind_ipc_server(&incoming_call, &waiting);
    ut::expect(waiting_call.deadline_ns.load() == tightened_deadline);

    arch::enable_interrupts();
    const u64 observation_end = clock.now_ns() + observation_ns;
    ut::expect(observation_end < original_deadline);
    while (fired_ns.load(moss::memory_order_acquire) == 0 && clock.now_ns() < observation_end) {
      arch::cpu_yield();
    }
    arch::disable_interrupts();
    scheduler->end_ipc_call(&incoming_call);
    scheduler->end_ipc_call(&waiting_call);
    deadline_timer.cancel_sync();
    const u64 observed_ns = fired_ns.load(moss::memory_order_acquire);
    ut::expect(observed_ns >= tightened_deadline && observed_ns < observation_end);
  }
  if (restore_irqs) {
    arch::enable_interrupts();
  }
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
    ut::register_test("cpu_runtime_accounting", cpu_runtime_accounting);
    ut::register_test("cpu_budget_accounting", cpu_budget_accounting);
    ut::register_test("cpu_budget_admission", cpu_budget_admission);
    ut::register_test("cfs_cpu_budget_queue", [] { cpu_budget_queue(false); });
    ut::register_test("rt_cpu_budget_queue", [] { cpu_budget_queue(true); });
    ut::register_test("ipc_priority_inheritance", ipc_priority_inheritance);
    ut::register_test("ipc_deadline_tightening", ipc_deadline_tightening);
    ut::register_test("migration_current_owner", migration_current_owner);
  });
}

} // namespace moss::test::validation
