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
void wake_and_migrate_edges() {
  using namespace process;
  unique_ptr<process::CfsScheduler> scheduler(new process::CfsScheduler());
  if (!ut::expect(scheduler.get() != nullptr)) {
    return;
  }
  const auto cpu = arch::get_current_cpu_id();
  Thread task(30, 0);
  task.cpu = cpu;
  task.cpu_affinity_mask.set(cpu);
  task.state = ProcessState::Sleeping;

  scheduler->task_wakeup(nullptr, cpu);
  scheduler->task_wakeup(&task, g_num_cpus);
  ut::expect(task.state == ProcessState::Sleeping);

  task.sleep_handoff.store(2);
  scheduler->task_wakeup(&task, cpu);
  ut::expect(task.state == ProcessState::Sleeping);
  task.sleep_handoff.store(0);

  task.cpu_affinity_mask.set_from_u32(0);
  scheduler->task_wakeup(&task, cpu);
  ut::expect(task.state == ProcessState::Sleeping);
  task.cpu_affinity_mask.set(cpu);

  task.state = ProcessState::Running;
  scheduler->task_wakeup(&task, cpu);
  ut::expect(task.state == ProcessState::Running);

  task.state = ProcessState::Sleeping;
  scheduler->task_wakeup(&task, cpu);
  ut::expect(task.state == ProcessState::Ready && task.se.rb_on_rq);
  scheduler->dequeue_task(&task);
  task.state = ProcessState::Sleeping;

  ut::expect(scheduler->take_next_task(g_num_cpus) == nullptr);
  ut::expect(scheduler->take_next_task(cpu) == nullptr);

  auto *original = CfsScheduler::get_current_task();
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  CfsScheduler::set_current_task(&task);
  task.state = ProcessState::Running;
  task.se.vruntime = 1000;
  scheduler->enqueue_task(&task, cpu);
  task.se.vruntime = 1000;
  scheduler->reset_current_to_min_vruntime();
  ut::expect(task.se.vruntime <= 1000);
  scheduler->dequeue_task(&task);
  ut::expect(!scheduler->migrate_current(g_num_cpus));
  ut::expect(scheduler->migrate_current(cpu));
  CfsScheduler::set_current_task(nullptr);
  scheduler->reset_current_to_min_vruntime();
  CfsScheduler::set_current_task(original);
  if (restore_irqs) {
    arch::enable_interrupts();
  }
  ut::expect(true);
}
void tick_edge_paths() {
  using namespace process;
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  if (!ut::expect(scheduler.get() != nullptr)) {
    return;
  }
  Thread current(10, 0), peer(11, 0);
  const auto cpu = arch::get_current_cpu_id();
  current.cpu = peer.cpu = cpu;
  current.cpu_affinity_mask.set(cpu);
  peer.cpu_affinity_mask.set(cpu);
  current.state = ProcessState::Running;
  current.sched_class = SchedClass::RealTime;
  current.sched_policy = SchedPolicy::RR;
  current.rt.priority = 50;
  current.rt.time_slice_remaining = 0;
  peer.sched_class = SchedClass::RealTime;
  peer.rt.priority = 60;
  peer.state = ProcessState::Ready;

  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  auto *original = CfsScheduler::get_current_task();
  CfsScheduler::set_current_task(&current);

  // A non-running current task is observed but not charged or dispatched.
  current.state = ProcessState::Sleeping;
  scheduler->scheduler_tick();
  ut::expect(current.state == ProcessState::Sleeping);

  // RR slice expiry self-selects when no peer is eligible.
  current.state = ProcessState::Running;
  const auto before = scheduler->total_preemptions();
  scheduler->scheduler_tick();
  ut::expect(scheduler->total_preemptions() > before);
  ut::expect(current.state == ProcessState::Running && !current.se.rb_on_rq);

  // A terminated RT task must not be re-enqueued by a pending preemption.
  current.state = ProcessState::Terminated;
  current.need_resched = false;
  scheduler->scheduler_tick();
  ut::expect(current.state == ProcessState::Terminated && !current.need_resched);

  // A higher-priority RT arrival with preemption blocked marks the request.
  current.state = ProcessState::Running;
  current.preempt_count = 1;
  scheduler->enqueue_task(&peer, cpu);
  scheduler->scheduler_tick();
  ut::expect(current.state == ProcessState::Running && current.need_resched);
  scheduler->dequeue_task(&peer);
  current.need_resched = false;
  current.preempt_count = 0;

  // An RT task waiting behind a CFS current also marks the request.
  current.sched_class = SchedClass::Normal;
  current.rt.priority = 0;
  current.se.vruntime = 10000000ULL;
  current.preempt_count = 1;
  scheduler->enqueue_task(&peer, cpu);
  scheduler->scheduler_tick();
  ut::expect(current.state == ProcessState::Running && current.need_resched);
  scheduler->dequeue_task(&peer);
  current.need_resched = false;

  // A CFS peer with lower vruntime behind a preempt-blocked current requests
  // a reschedule without switching synthetic contexts.
  peer.sched_class = SchedClass::Normal;
  peer.preempt_count = 0;
  peer.se.vruntime = 0;
  peer.state = ProcessState::Ready;
  scheduler->enqueue_task(&peer, cpu);
  scheduler->scheduler_tick();
  ut::expect(current.need_resched);
  scheduler->dequeue_task(&peer);
  current.preempt_count = 0;

  CfsScheduler::set_current_task(original);
  if (restore_irqs) {
    arch::enable_interrupts();
  }
}
void ipc_cycle_and_tree_edges() {
  using namespace process;
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  if (!ut::expect(scheduler.get() != nullptr)) {
    return;
  }
  Thread caller(20, 0), first(21, 0), second(22, 0), waiting(23, 0), server(24, 0);
  first.sched_class = SchedClass::RealTime;
  first.rt.priority = 80;
  second.se.nice.store(15);
  PriorityDonation to_second{}, to_first{}, external{}, internal{}, wait{};
  to_second.caller = &first;
  to_second.server = &second;
  to_first.caller = &second;
  to_first.server = &first;
  external.caller = &caller;
  external.server = &first;
  external.next = &internal;
  internal.caller = &second;
  internal.server = &first;
  second.ipc_wait = &to_first;
  first.ipc_wait = &to_second;
  second.ipc_donors = &to_second;
  first.ipc_donors = &external;
  first.ipc_scheduler = second.ipc_scheduler = scheduler.get();

  // Rebuild a manually linked donation cycle, including an internal donor
  // that must be ignored through the epoch marker and one outside donor.
  scheduler->set_base_nice(&second, 5);
  ut::expect(second.effective_rt_priority() == 80);
  first.ipc_wait = nullptr;
  second.ipc_wait = nullptr;
  first.ipc_donors = nullptr;
  second.ipc_donors = nullptr;
  first.ipc_scheduler = second.ipc_scheduler = nullptr;

  // A thread that is itself waiting must unlink its donation from the server.
  wait.caller = &waiting;
  wait.server = &server;
  waiting.ipc_wait = &wait;
  server.ipc_donors = &wait;
  server.ipc_scheduler = scheduler.get();
  scheduler->forget_ipc_thread(&waiting);
  ut::expect(wait.caller == nullptr && wait.server == nullptr && waiting.ipc_wait == nullptr);
  ut::expect(server.ipc_donors == nullptr && !waiting.ipc_scheduler);
  scheduler->forget_ipc_thread(nullptr);

  // Exhaustive deletion orders over a six-node tree reach every red-black
  // delete-fixup sibling and child color combination.
  constexpr usize nodes = 6;
  Thread tree0(0, 0), tree1(1, 0), tree2(2, 0), tree3(3, 0), tree4(4, 0), tree5(5, 0);
  Thread *tree[nodes] = {&tree0, &tree1, &tree2, &tree3, &tree4, &tree5};
  u32 order[nodes] = {0, 1, 2, 3, 4, 5};
  u32 cursor[nodes]{};
  for (usize i = 0; i < nodes; ++i) {
    tree[i]->se.vruntime = 10 * (i + 1);
  }
  usize permutation = 0;
  bool intact = true;
  for (;;) {
    CfsRunqueue queue;
    for (usize i = 0; i < nodes; ++i) {
      queue.enqueue_task(tree[i]);
    }
    for (usize i = 0; i < nodes; ++i) {
      queue.dequeue_task(tree[order[i]]);
    }
    intact = intact && queue.pick_next_task() == nullptr && !tree[0]->se.rb_on_rq;
    ++permutation;
    // Iterative Heap's algorithm; no allocation in the test fixture.
    usize i = 0;
    while (i < nodes) {
      if (cursor[i] < i) {
        if (i % 2 == 0) {
          const u32 swap = order[0];
          order[0] = order[i];
          order[i] = swap;
        } else {
          const u32 swap = order[cursor[i]];
          order[cursor[i]] = order[i];
          order[i] = swap;
        }
        ++cursor[i];
        break;
      }
      cursor[i] = 0;
      ++i;
    }
    if (i == nodes) {
      break;
    }
  }
  ut::expect(intact);
  ut::expect(permutation == 720);
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
  constexpr usize order = 4;
  // Thread::allocate_kernel_stack uses order 4: sixteen 4 KiB pages (64 KiB).
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

void try_lock_preemption() {
  using namespace process;
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  if (!ut::expect(scheduler.get() != nullptr)) {
    return;
  }
  Thread current(0, 0);
  const auto cpu = arch::get_current_cpu_id();
  current.cpu = cpu;
  current.cpu_affinity_mask = CpuBitmap::single(cpu);
  current.state = ProcessState::Running;
  current.sched_class = SchedClass::RealTime;
  current.sched_policy = SchedPolicy::RR;
  current.rt.priority = priority::DEFAULT_RT_PRIORITY;

  // As in self-selection, a private queue and masked IRQs let the real tick
  // inspect this context without ever dispatching an uninitialized stack.
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  auto *original = CfsScheduler::get_current_task();
  CfsScheduler::set_current_task(&current);
  const auto exercise = [&](auto &inner) {
    containers::TicketSpinLock outer;
    outer.lock();
    const auto switches = scheduler->total_preemptions();
    const bool acquired = inner.try_lock();
    // Two locks must contribute two guards. A failed recursive try must
    // neither leak a guard nor remove the one protecting the current owner.
    const bool nested = acquired && current.preempt_count == 2;
    const bool refused = acquired && !inner.try_lock() && current.preempt_count == 2;
    if (acquired) {
      inner.unlock();
    }
    const bool outer_guarded = current.preempt_count == 1;
    current.rt.time_slice_remaining = 0; // Force RR expiry on this tick, independent of elapsed time.
    current.need_resched = false;
    scheduler->scheduler_tick();
    const bool deferred =
        current.need_resched && current.state == ProcessState::Running && scheduler->total_preemptions() == switches;
    outer.unlock();
    return nested && refused && outer_guarded && deferred && current.preempt_count == 0;
  };
  containers::TicketSpinLock plain;
  containers::IrqSpinLock irq_safe;
  const bool plain_guarded = exercise(plain);
  const bool irq_guarded = exercise(irq_safe);
  const bool irqs_masked = !arch::interrupts_enabled();
  CfsScheduler::set_current_task(original);
  if (restore_irqs) {
    arch::enable_interrupts();
  }
  ut::expect(plain_guarded);
  ut::expect(irq_guarded);
  ut::expect(irqs_masked);
  ut::expect(scheduler->get_cpu_nr_running(cpu) == 0);
}

void reschedule_safe_point() {
  using namespace process;
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  if (!ut::expect(scheduler.get() != nullptr)) {
    return;
  }
  Thread current(0, 0);
  const auto cpu = arch::get_current_cpu_id();
  current.cpu = cpu;
  current.cpu_affinity_mask = CpuBitmap::single(cpu);
  current.state = ProcessState::Running;
  current.sched_class = SchedClass::RealTime;
  current.sched_policy = SchedPolicy::RR;
  current.rt.priority = priority::DEFAULT_RT_PRIORITY;

  // Only self-selection is possible in this private queue. Keep IRQs masked
  // until the real current pointer is restored, as in the tick regressions.
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  auto *original = CfsScheduler::get_current_task();
  CfsScheduler::set_current_task(&current);
  const auto switches = scheduler->total_preemptions();
  current.need_resched = true;
  current.preempt_disable();
  scheduler->check_need_resched();
  const bool guarded = current.need_resched && scheduler->total_preemptions() == switches;
  current.preempt_enable();
  current.state = ProcessState::Sleeping;
  scheduler->check_need_resched();
  const bool blocked =
      current.need_resched && current.state == ProcessState::Sleeping && scheduler->total_preemptions() == switches;
  current.state = ProcessState::Running;
  scheduler->check_need_resched();
  const bool consumed =
      !current.need_resched && current.state == ProcessState::Running && scheduler->total_preemptions() == switches + 1;
  scheduler->check_need_resched();
  const bool once = scheduler->total_preemptions() == switches + 1;
  CfsScheduler::set_current_task(original);
  if (restore_irqs) {
    arch::enable_interrupts();
  }
  ut::expect(guarded);
  ut::expect(blocked);
  ut::expect(consumed);
  ut::expect(once);
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
  ut::expect(scheduler->rebind_ipc_server(&high_call, &backend, nullptr) == IpcBindResult::Stale);
  ut::expect(scheduler->rebind_ipc_server(&high_call, &server, &backend) == IpcBindResult::Bound);
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
  ut::expect(scheduler->bind_ipc_server(&cycle, &server) == IpcBindResult::Deadlock);
  ut::expect(cycle.server == nullptr && server.ipc_donors == nullptr);
  ut::expect(backend.state == ProcessState::Ready && backend.se.rb_on_rq);
  // A real rejected caller resumes in userspace. This synthetic thread has
  // no context to run, so remove its wakeup before checking later donations.
  scheduler->dequeue_task(&backend);
  backend.state = ProcessState::Sleeping;
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
  ut::expect(scheduler->bind_ipc_server(&normal_cycle, &normal_server) == IpcBindResult::Deadlock);
  ut::expect(normal_cycle.server == nullptr);
  ut::expect(normal_backend.state == ProcessState::Ready && normal_backend.se.rb_on_rq);
  scheduler->dequeue_task(&normal_backend);
  normal_backend.state = ProcessState::Sleeping;
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

  Thread front(0, 0), middle(1, 0), back(2, 0), redirect(3, 0), redirect_back(4, 0), late(5, 0), earlier(6, 0),
      back_terminal(7, 0);
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

  ut::expect(scheduler->rebind_ipc_server(&late_call, &front, &redirect) == IpcBindResult::Bound);
  ut::expect(redirect_wait.deadline_ns.load() == late_ns && front_wait.deadline_ns.load() == late_ns &&
             middle_wait.deadline_ns.load() == late_ns);
  ut::expect(front.effective_rt_priority() == 0 && back.effective_rt_priority() == 0 &&
             redirect.effective_rt_priority() == late.rt.priority);

  // Extend the wait chain to a terminal server. A new donor on front must
  // tighten every downstream wait without relying on a deadlocked cycle.
  deadline = 0;
  ut::expect(scheduler->begin_ipc_call(&back_wait, &back, &deadline));
  ut::expect(deadline == late_ns);
  scheduler->bind_ipc_server(&back_wait, &back_terminal);
  earlier.sched_class = SchedClass::RealTime;
  // A distinct higher RT priority makes its later removal observable.
  earlier.rt.priority = late.rt.priority + 1;
  deadline = earlier_ns;
  ut::expect(scheduler->begin_ipc_call(&earlier_call, &earlier, &deadline));
  scheduler->bind_ipc_server(&earlier_call, &front);
  ut::expect(front_wait.deadline_ns.load() == earlier_ns && middle_wait.deadline_ns.load() == earlier_ns &&
             back_wait.deadline_ns.load() == earlier_ns);
  ut::expect(front.effective_rt_priority() == earlier.rt.priority &&
             middle.effective_rt_priority() == earlier.rt.priority &&
             back.effective_rt_priority() == earlier.rt.priority &&
             back_terminal.effective_rt_priority() == earlier.rt.priority);

  scheduler->end_ipc_call(&earlier_call);
  scheduler->end_ipc_call(&late_call);
  ut::expect(front.effective_rt_priority() == 0 && middle.effective_rt_priority() == 0 &&
             back.effective_rt_priority() == 0 && back_terminal.effective_rt_priority() == 0 &&
             redirect.effective_rt_priority() == 0);
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

void ipc_call_graph_limits() {
  using namespace process;
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  if (!ut::expect(scheduler.get() != nullptr)) {
    return;
  }
  Thread *chain[kMaxIpcCallDepth + 2]{};
  bool allocated = true;
  for (usize i = 0; i < kMaxIpcCallDepth + 2; ++i) {
    chain[i] = new Thread(static_cast<ThreadId>(i), 0);
    allocated = allocated && chain[i] != nullptr;
  }
  if (!ut::expect(allocated)) {
    for (auto *thread : chain) {
      delete thread;
    }
    return;
  }
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();

  Thread self(0, 0);
  PriorityDonation call{};
  if (ut::expect(scheduler->begin_ipc_call(&call, &self))) {
    ut::expect(scheduler->bind_ipc_server(&call, &self) == IpcBindResult::Deadlock);
    // A thread waiting for its own Reply cannot make progress; the rejected
    // bind must leave no donor or wait edge for later calls to inherit.
    ut::expect(call.server == nullptr && self.ipc_donors == nullptr);
    ut::expect(scheduler->end_ipc_call(&call) == IpcBindResult::Deadlock);
  }
  ut::expect(self.ipc_wait == nullptr && self.ipc_scheduler == nullptr);

  Thread a(1, 0), b(2, 0), c(3, 0), d(4, 0);
  PriorityDonation ab{}, bc{}, ca{}, recovered{};
  a.sched_class = SchedClass::RealTime;
  a.rt.priority = priority::DEFAULT_RT_PRIORITY;
  if (ut::expect(scheduler->begin_ipc_call(&ab, &a))) {
    ut::expect(scheduler->bind_ipc_server(&ab, &b) == IpcBindResult::Bound);
    if (ut::expect(scheduler->begin_ipc_call(&bc, &b))) {
      ut::expect(scheduler->bind_ipc_server(&bc, &c) == IpcBindResult::Bound);
      if (ut::expect(scheduler->begin_ipc_call(&ca, &c))) {
        ut::expect(scheduler->bind_ipc_server(&ca, &a) == IpcBindResult::Deadlock);
        ut::expect(ca.server == nullptr && a.ipc_donors == nullptr);
        ut::expect(scheduler->end_ipc_call(&ca) == IpcBindResult::Deadlock);
      }
      // Rebinding b to a would close a shorter cycle. Keep the original donor
      // until this rejected call is detached, then permit a fresh call to d.
      ut::expect(scheduler->rebind_ipc_server(&bc, &c, &a) == IpcBindResult::Deadlock);
      ut::expect(bc.server == &c && c.ipc_donors == &bc && c.effective_rt_priority() == a.rt.priority);
      ut::expect(scheduler->end_ipc_call(&bc) == IpcBindResult::Deadlock);
      ut::expect(c.ipc_donors == nullptr && c.effective_rt_priority() == 0);
      if (ut::expect(scheduler->begin_ipc_call(&recovered, &b))) {
        ut::expect(scheduler->bind_ipc_server(&recovered, &d) == IpcBindResult::Bound);
        ut::expect(d.effective_rt_priority() == a.rt.priority);
        scheduler->end_ipc_call(&recovered);
      }
    }
    scheduler->end_ipc_call(&ab);
  }
  ut::expect(a.ipc_wait == nullptr && b.ipc_wait == nullptr && c.ipc_wait == nullptr && a.ipc_donors == nullptr &&
             b.ipc_donors == nullptr && c.ipc_donors == nullptr && d.ipc_donors == nullptr &&
             b.effective_rt_priority() == 0 && d.effective_rt_priority() == 0);

  PriorityDonation edges[kMaxIpcCallDepth + 1]{};
  usize started = 0;
  for (; started < kMaxIpcCallDepth; ++started) {
    if (!ut::expect(scheduler->begin_ipc_call(&edges[started], chain[started]))) {
      break;
    }
    // Sixteen bound edges are allowed; the next edge must account for the
    // longest upstream donor, even when another donor is closer to the leaf.
    if (!ut::expect(scheduler->bind_ipc_server(&edges[started], chain[started + 1]) == IpcBindResult::Bound)) {
      ++started;
      break;
    }
  }
  if (started == kMaxIpcCallDepth) {
    Thread branch(5, 0);
    PriorityDonation branch_call{};
    bool oldest_ended = false;
    if (ut::expect(scheduler->begin_ipc_call(&branch_call, &branch))) {
      ut::expect(scheduler->bind_ipc_server(&branch_call, chain[kMaxIpcCallDepth / 2]) == IpcBindResult::Bound);
      if (ut::expect(scheduler->begin_ipc_call(&edges[started], chain[started]))) {
        ut::expect(scheduler->bind_ipc_server(&edges[started], chain[started + 1]) == IpcBindResult::TooDeep);
        ut::expect(edges[started].server == nullptr && chain[started + 1]->ipc_donors == nullptr);
        ut::expect(scheduler->end_ipc_call(&edges[started]) == IpcBindResult::TooDeep);
        scheduler->end_ipc_call(&edges[0]);
        oldest_ended = true;
        // The rejected call is terminal. Removing the oldest upstream edge
        // permits a new call over the same endpoint without stale state.
        PriorityDonation retry{};
        if (ut::expect(scheduler->begin_ipc_call(&retry, chain[started]))) {
          ut::expect(scheduler->bind_ipc_server(&retry, chain[started + 1]) == IpcBindResult::Bound);
          ut::expect(retry.server == chain[started + 1]);
          scheduler->end_ipc_call(&retry);
        }
      }
      scheduler->end_ipc_call(&branch_call);
    }
    if (!oldest_ended) {
      scheduler->end_ipc_call(&edges[0]);
    }
    for (usize i = started; i > 1; --i) {
      scheduler->end_ipc_call(&edges[i - 1]);
    }
  } else {
    for (usize i = started; i > 0; --i) {
      scheduler->end_ipc_call(&edges[i - 1]);
    }
  }
  for (auto *thread : chain) {
    ut::expect(thread->ipc_wait == nullptr && thread->ipc_donors == nullptr && thread->ipc_scheduler == nullptr);
  }

  if (restore_irqs) {
    arch::enable_interrupts();
  }
  for (auto *thread : chain) {
    delete thread;
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

void periodic_scan_ownership() {
  using namespace process;
  auto *original = CfsScheduler::get_current_task();
  if (!ut::expect(g_scheduler && original && arch::get_current_cpu_id() == 0 && g_num_cpus >= 2)) {
    return;
  }
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  unique_ptr<LoadBalancer> balancer(new LoadBalancer());
  if (!ut::expect(scheduler && balancer)) {
    return;
  }
  // Keep this independent of the implementation's counter: the existing
  // contract is one global scan per eight CPU0 ticks, regardless of SMP size.
  constexpr u32 scan_ticks = 8;
  // The balancer's default 4 ms gate is checked separately from the scheduler's
  // coarser 48 ms callback cadence, using synthetic absolute timestamps.
  constexpr u64 interval_ns = 4'000'000;
  static u32 scans = 0, scan_cpus = 0;
  scans = scan_cpus = 0;
  scheduler->set_balance_callback(+[](u64, CfsScheduler *) {
    ++scans;
    scan_cpus |= 1U << arch::get_current_cpu_id();
  });

  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  const auto original_mask = static_cast<u32>(original->cpu_affinity_mask.low_word());
  // Reuse the production continuation handoff, without changing CPU identity
  // registers. Restore the validation worker's CPU0 affinity before returning.
  g_scheduler->set_task_affinity_mask(original, static_cast<u32>(CpuBitmap::all().low_word()));
  const auto tick = [&](u32 count) {
    Thread current(0, 0);
    current.cpu = arch::get_current_cpu_id();
    current.state = ProcessState::Running;
    current.sched_class = SchedClass::RealTime;
    current.sched_policy = SchedPolicy::RR;
    current.rt.priority = priority::DEFAULT_RT_PRIORITY;
    current.rt.time_slice_remaining = 0; // Force a local reschedule request on every tested CPU.
    current.preempt_disable();           // Never dispatch this synthetic, stackless context.
    const u64 now = timer::TimerSubsystem::instance().now_ns();
    if (now <= cfs_params::SCHED_LATENCY_NS) {
      return false;
    }
    current.se.exec_start = now - cfs_params::SCHED_LATENCY_NS;
    CfsScheduler::set_current_task(&current);
    for (u32 i = 0; i < count; ++i) {
      scheduler->scheduler_tick();
    }
    CfsScheduler::set_current_task(original);
    return current.need_resched && current.state == ProcessState::Running &&
           current.se.cpu_runtime_ns.load() >= cfs_params::SCHED_LATENCY_NS && scheduler->total_preemptions() == 0;
  };
  bool local_ticks = tick(scan_ticks - 1);
  const bool before_eighth = scans == 0;

  // Three ready tasks cross periodic_balance's >2 admission threshold. Their
  // neutral CFS weights exceed its raw load cutoff; no synthetic task executes.
  Thread first(0, 0), second(1, 0), third(2, 0);
  Thread *tasks[] = {&first, &second, &third};
  const auto refill = [&] {
    for (auto *task : tasks) {
      scheduler->dequeue_task(task);
      task->se.load_avg = task->se.weight;
      scheduler->enqueue_task(task, 0);
    }
  };
  refill();
  u32 visited = 1; // Bit zero records the initial CPU0 phase.
  for (u32 cpu = 1; cpu < g_num_cpus; ++cpu) {
    if (!g_scheduler->migrate_current(cpu) || arch::get_current_cpu_id() != cpu) {
      break;
    }
    visited |= 1U << cpu;
    local_ticks = tick(scan_ticks) && local_ticks;
    balancer->periodic_balance(interval_ns, *scheduler);
  }
  const bool peers_ignored = scans == 0 && scheduler->get_cpu_nr_running(0) == 3;
  const bool returned = g_scheduler->migrate_current(0) && arch::get_current_cpu_id() == 0;
  // Reset the queue even on the old implementation. An unauthorized peer scan
  // must neither migrate work nor consume the owner's next interval.
  refill();
  local_ticks = tick(1) && local_ticks;
  const bool eighth = scans == 1 && scan_cpus == 1;
  balancer->periodic_balance(interval_ns, *scheduler);
  const bool owner_migrated = scheduler->get_cpu_nr_running(0) == 2 && scheduler->get_cpu_nr_running(1) == 1;
  refill();
  balancer->periodic_balance(interval_ns, *scheduler);
  balancer->periodic_balance(2 * interval_ns - 1, *scheduler);
  const bool interval_retained = scheduler->get_cpu_nr_running(0) == 3;
  balancer->periodic_balance(2 * interval_ns, *scheduler);
  const bool next_interval = scheduler->get_cpu_nr_running(0) == 2;
  local_ticks = tick(scan_ticks - 1) && local_ticks;
  const bool next_seventh = scans == 1;
  local_ticks = tick(1) && local_ticks;
  for (auto *task : tasks) {
    scheduler->dequeue_task(task);
  }
  g_scheduler->set_task_affinity_mask(original, original_mask);
  if (restore_irqs) {
    arch::enable_interrupts();
  }
  logging::klog::warn("Periodic scan: visited_mask={} scan_cpus={} scans={}", visited, scan_cpus, scans);
  ut::expect(returned && visited == CpuBitmap::all().low_word());
  ut::expect(local_ticks);
  ut::expect(before_eighth);
  ut::expect(peers_ignored);
  ut::expect(eighth);
  ut::expect(owner_migrated);
  ut::expect(interval_retained && next_interval);
  ut::expect(next_seventh && scans == 2 && scan_cpus == 1);
  ut::expect(scheduler->total_context_switches() == 0 && scheduler->get_cpu_nr_running(0) == 0 &&
             scheduler->get_cpu_nr_running(1) == 0);
}

void runqueue_contracts() {
  using namespace process;

  Thread tasks[8] = {{1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 0}, {7, 0}, {8, 0}};
  CfsRunqueue cfs;
  ut::expect(cfs.nr_running() == 0 && cfs.total_weight() == 0 && cfs.pick_next_task() == nullptr);
  cfs.enqueue_task(nullptr);
  cfs.dequeue_task(nullptr);
  ut::expect(cfs_params::nice_to_weight(100) == 1 && cfs_params::nice_to_weight(19) ==
             cfs_params::NICE_TO_WEIGHT[cfs_params::nice_to_weight_index(19)]);
  for (u32 i = 0; i < 8; ++i) {
    tasks[i].state = ProcessState::Ready;
    tasks[i].se.vruntime = (i + 1) * 100;
    tasks[i].se.weight = 1024 + i;
    tasks[i].se.prev_sum_exec_runtime = 0;
    tasks[i].cpu_affinity_mask = CpuBitmap::all();
  }
  for (auto &task : tasks) {
    cfs.enqueue_task(&task);
  }
  ut::expect(cfs.nr_running() == 8 && cfs.total_weight() > 0);
  ut::expect(cfs.pick_next_task() == &tasks[0] && cfs.pick_last_task() == &tasks[7]);
  const u64 min_vruntime = cfs.min_vruntime();
  ut::expect(min_vruntime >= 100);
  cfs.dump_runqueue();

  tasks[0].se.sum_exec_runtime = 1000;
  cfs.update_curr_task(&tasks[0], 1000);
  (void)cfs.should_preempt(&tasks[0]);
  cfs.update_curr_task(nullptr, 10);
  tasks[7].se.prev_sum_exec_runtime = 0;
  tasks[7].se.sum_exec_runtime = cfs_params::SCHED_LATENCY_NS * 2;
  ut::expect(cfs.should_preempt(&tasks[7]));

  cfs.place_entity(&tasks[0], true);
  cfs.place_entity(&tasks[0], false);
  ut::expect(cfs.pick_migration_task(0, &tasks[0]) != nullptr);
  ut::expect(cfs.pick_migration_task(0, &tasks[7]) != nullptr);

  for (auto &task : tasks) {
    cfs.dequeue_task(&task);
    cfs.dequeue_task(&task);
  }
  ut::expect(cfs.nr_running() == 0 && cfs.pick_next_task() == nullptr);

  Thread *random_tasks[32]{};
  for (u32 i = 0; i < 32; ++i) {
    random_tasks[i] = new Thread(1000 + i, 0);
    random_tasks[i]->state = ProcessState::Ready;
    random_tasks[i]->se.vruntime = ((i * 37U) % 101U) + 1U;
    random_tasks[i]->se.weight = 1024U + (i % 8U);
    random_tasks[i]->se.prev_sum_exec_runtime = 0;
    random_tasks[i]->cpu_affinity_mask = CpuBitmap::all();
    cfs.enqueue_task(random_tasks[i]);
  }
  ut::expect(cfs.nr_running() == 32);
  for (u32 i = 0; i < 32; ++i) {
    const u32 index = (i * 13U + 7U) % 32U;
    cfs.dequeue_task(random_tasks[index]);
  }
  ut::expect(cfs.nr_running() == 0 && cfs.pick_next_task() == nullptr);
  for (auto *task : random_tasks) {
    delete task;
  }

  RtRunqueue rt;
  Thread rt_tasks[6] = {{9, 0}, {10, 0}, {11, 0}, {12, 0}, {13, 0}, {14, 0}};
  rt.enqueue_task(nullptr);
  rt.dequeue_task(nullptr);
  for (u32 i = 0; i < 6; ++i) {
    rt_tasks[i].sched_class = SchedClass::RealTime;
    rt_tasks[i].rt.priority = 5;
  }
  rt_tasks[2].rt.priority = 70;
  rt_tasks[3].rt.priority = 70;
  rt_tasks[4].rt.priority = 0;
  rt_tasks[5].rt.priority = 100;
  for (auto &task : rt_tasks) {
    rt.enqueue_task(&task);
  }
  ut::expect(rt.nr_running() == 6 && rt.highest_priority() == 70);
  ut::expect(rt.pick_next_task() == &rt_tasks[2]);
  rt.requeue_task(nullptr);
  rt.requeue_task(&rt_tasks[2]);
  ut::expect(rt.pick_next_task() == &rt_tasks[3]);
  rt.requeue_task(&rt_tasks[2]);
  rt.dequeue_task(&rt_tasks[3]);
  rt.dequeue_task(&rt_tasks[2]);
  rt.dequeue_task(&rt_tasks[4]);
  rt.dequeue_task(&rt_tasks[5]);
  rt.requeue_task(&rt_tasks[0]);
  rt.dequeue_task(&rt_tasks[0]);
  rt.dequeue_task(&rt_tasks[0]);
  rt.dequeue_task(&rt_tasks[1]);
  ut::expect(rt.nr_running() == 0 && rt.highest_priority() == 0 && rt.pick_next_task() == nullptr);
}

void idle_contracts() {
  using namespace process;

  IdleTask idle(0);
  ut::expect(idle.get_cpu_id() == 0 && idle.get_idle_time_ns() == 0);
  idle.enter_idle(100);
  ut::expect(idle.snapshot_idle_time_ns(150) == 50);
  idle.exit_idle(200);
  ut::expect(idle.get_idle_time_ns() == 100);
  idle.snapshot_idle_time_ns(250);
  idle.accumulate_idle_time(7);
  ut::expect(idle.get_idle_time_ns() == 107);
  idle.reset_idle_time();
  ut::expect(idle.get_idle_time_ns() == 0);
  idle.enter_idle(0);
  idle.exit_idle(0);
  ut::expect(idle.get_idle_time_ns() == 0);

  auto *previous = g_idle_tasks.get_cpu(0);
  ut::expect(set_idle_task(0, &idle));
  ut::expect(get_idle_task(0) == &idle);
  ut::expect(!set_idle_task(g_num_cpus, &idle));
  ut::expect(get_idle_task(g_num_cpus) == nullptr);
  ut::expect(!is_cpu_idle(g_num_cpus));
  wakeup_idle_cpu(g_num_cpus);
  ut::expect(create_idle_task(g_num_cpus) == nullptr);
  is_cpu_idle(0);
  wakeup_idle_cpu(0);
  auto *created = create_idle_task(0);
  if (created != nullptr) {
    g_idle_tasks.get_cpu(0) = previous;
    delete created;
  }
  ut::expect(created == nullptr || created->get_cpu_id() == 0);
  set_idle_task(0, previous);
}

void scheduler_lifecycle() {
  using namespace process;

  auto *scheduler = new CfsScheduler();
  if (!ut::expect(scheduler != nullptr)) {
    return;
  }
  auto *current_thread = new Thread(30, 0);
  auto *peer_thread = new Thread(31, 0);
  if (!ut::expect(current_thread != nullptr && peer_thread != nullptr)) {
    return;
  }
  const auto cpu = arch::get_current_cpu_id();
  current_thread->cpu = peer_thread->cpu = cpu;
  current_thread->cpu_affinity_mask = CpuBitmap::single(cpu);
  peer_thread->cpu_affinity_mask = CpuBitmap::single(cpu);
  current_thread->state = ProcessState::Running;
  current_thread->se.exec_start = timer::TimerSubsystem::instance().now_ns();
  peer_thread->se.nice = 0;
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  auto *original = CfsScheduler::get_current_task();
  CfsScheduler::set_current_task(current_thread);

  scheduler->enqueue_task(peer_thread, cpu);
  ut::expect(scheduler->get_cpu_nr_running(cpu) == 1 && scheduler->monitor_runnable(cpu) == 2);
  ut::expect(scheduler->monitor_cpu(peer_thread) == cpu);
  ut::expect(scheduler->has_runnable_tasks(cpu));
  ut::expect(scheduler->pick_next_task(cpu) == peer_thread && scheduler->pick_last_task(cpu) == peer_thread);
  scheduler->set_base_nice(peer_thread, 7);
  ut::expect(peer_thread->se.nice.load() == 7 && peer_thread->se.weight.load() == cfs_params::nice_to_weight(7));
  scheduler->set_base_nice(peer_thread, 100);
  scheduler->reset_current_to_min_vruntime();
  scheduler->replenish_cpu_budgets(current_thread->se.exec_start + 1);
  scheduler->task_blocked(peer_thread);
  ut::expect(peer_thread->state == ProcessState::Sleeping && !peer_thread->se.rb_on_rq);
  scheduler->task_wakeup(peer_thread, cpu);
  ut::expect(peer_thread->state == ProcessState::Ready && peer_thread->se.rb_on_rq);
  scheduler->task_terminate(peer_thread);
  ut::expect(peer_thread->state == ProcessState::Terminated && !peer_thread->se.rb_on_rq);
  scheduler->transition_task_state(peer_thread, ProcessState::Zombie);
  ut::expect(peer_thread->state == ProcessState::Zombie);
  scheduler->transition_task_state(peer_thread, ProcessState::Ready);
  ut::expect(peer_thread->state == ProcessState::Zombie);
  Thread *transitions[8]{};
  for (u32 i = 0; i < 8; ++i) {
    transitions[i] = new Thread(static_cast<ThreadId>(61 + i), 0);
    if (!ut::expect(transitions[i] != nullptr)) {
      return;
    }
  }
  transitions[0]->state = ProcessState::Created;
  scheduler->transition_task_state(transitions[0], ProcessState::Ready);
  ut::expect(transitions[0]->state == ProcessState::Ready);
  scheduler->transition_task_state(transitions[0], ProcessState::Sleeping);
  ut::expect(transitions[0]->state == ProcessState::Sleeping);
  scheduler->transition_task_state(transitions[0], ProcessState::DiskSleep);
  scheduler->transition_task_state(transitions[0], ProcessState::Stopped);
  scheduler->transition_task_state(transitions[0], ProcessState::Terminated);
  ut::expect(transitions[0]->state == ProcessState::Terminated);
  scheduler->transition_task_state(transitions[0], ProcessState::Zombie);
  ut::expect(transitions[0]->state == ProcessState::Zombie);
  scheduler->transition_task_state(transitions[0], ProcessState::Ready);
  ut::expect(transitions[0]->state == ProcessState::Zombie);
  transitions[1]->state = ProcessState::Running;
  scheduler->transition_task_state(transitions[1], ProcessState::Ready);
  scheduler->transition_task_state(transitions[1], ProcessState::DiskSleep);
  scheduler->transition_task_state(transitions[1], ProcessState::Stopped);
  scheduler->transition_task_state(transitions[1], ProcessState::Terminated);
  ut::expect(transitions[1]->state == ProcessState::Terminated);
  scheduler->transition_task_state(transitions[2], ProcessState::Ready);
  scheduler->transition_task_state(transitions[3], ProcessState::Running);
  ut::expect(transitions[2]->state == ProcessState::Ready && transitions[3]->state == ProcessState::Created);
  scheduler->transition_task_state(nullptr, ProcessState::Ready);
  scheduler->set_base_nice(nullptr, 0);
  scheduler->task_blocked(nullptr);
  scheduler->task_wakeup(nullptr, cpu);
  scheduler->task_terminate(nullptr);
  scheduler->update_current(nullptr, 10);
  ut::expect(!scheduler->should_preempt_current(nullptr));
  scheduler->dequeue_task(nullptr);
  scheduler->enqueue_task(nullptr, cpu);
  scheduler->place_entity(nullptr, cpu, false);
  scheduler->reset_current_to_min_vruntime();
  scheduler->update_current(current_thread, 1000);
  scheduler->dump_runqueue(cpu);
  (void)scheduler->get_cpu_load(cpu);
  ut::expect(scheduler->get_cpu_nr_running(cpu) == 0);
  ut::expect(scheduler->get_cpu_nr_running(g_num_cpus) == 0);
  ut::expect(!scheduler->has_runnable_tasks(g_num_cpus));
  ut::expect(scheduler->pick_next_task(g_num_cpus) == nullptr && scheduler->pick_last_task(g_num_cpus) == nullptr);
  scheduler->dequeue_task(nullptr);
  scheduler->place_entity(nullptr, cpu, true);
  ut::expect(scheduler->get_cpu_min_vruntime(g_num_cpus) == 0);

  // Invalid and stale scheduler inputs must return without mutating queues.
  PriorityDonation donation{};
  auto *ipc_thread = new Thread(70, 0);
  if (!ut::expect(ipc_thread != nullptr)) {
    return;
  }
  ut::expect(!scheduler->begin_ipc_call(nullptr, nullptr));
  ipc_thread->ipc_wait = &donation;
  ut::expect(!scheduler->begin_ipc_call(&donation, ipc_thread));
  ipc_thread->ipc_wait = nullptr;
  scheduler->arm_ipc_deadline_timer(nullptr, nullptr);
  scheduler->arm_ipc_deadline_timer(&donation, nullptr);
  ut::expect(scheduler->bind_ipc_server(nullptr, peer_thread) == IpcBindResult::Stale);
  ut::expect(scheduler->bind_ipc_server(&donation, peer_thread) == IpcBindResult::Stale);
  ut::expect(scheduler->rebind_ipc_server(nullptr, nullptr, nullptr) == IpcBindResult::Stale);
  ut::expect(scheduler->rebind_ipc_server(&donation, nullptr, nullptr) == IpcBindResult::Stale);
  ut::expect(scheduler->end_ipc_call(nullptr) == IpcBindResult::Stale);
  scheduler->set_base_nice(peer_thread, priority::MIN_NICE - 1);
  scheduler->set_base_nice(peer_thread, priority::MAX_NICE + 1);
  ut::expect(!scheduler->migrate_ready_task(g_num_cpus, cpu));
  ut::expect(!scheduler->migrate_ready_task(cpu, g_num_cpus));
  ut::expect(!scheduler->migrate_ready_task(cpu, cpu));
  ut::expect(scheduler->get_current_task_on_cpu(g_num_cpus) == nullptr);
  scheduler->set_idle_task(g_num_cpus, nullptr);
  scheduler->set_idle_task(cpu, nullptr);
  ut::expect(scheduler->get_idle_task(g_num_cpus) == nullptr);
  scheduler->replenish_cpu_budgets(timer::TimerSubsystem::instance().now_ns());
  peer_thread->state = ProcessState::Terminated;
  ut::expect(scheduler->request_cpu_budget(peer_thread, 1, 10) == BudgetProfileResult::Invalid);
  peer_thread->state = ProcessState::Running;
  ut::expect(scheduler->request_cpu_budget(nullptr, 1, 10) == BudgetProfileResult::Invalid);
  ut::expect(scheduler->request_cpu_budget(peer_thread, 0, 10) == BudgetProfileResult::Invalid);
  ut::expect(scheduler->request_cpu_budget(peer_thread, 20, 10) == BudgetProfileResult::Invalid);

  CfsScheduler::set_current_task(original);
  if (restore_irqs) {
    arch::enable_interrupts();
  }
}

void process_surfaces() {
  using namespace process;
  constexpr usize page = moss::kernel::PAGE_SIZE;

  Process proc(77);
  proc.set_name("probe");
  proc.set_command("probe command", 13);
  ut::expect(proc.name()[0] == 'p');
  auto *thread = new Thread(Process::allocate_thread_id(), proc.pid());
  if (!ut::expect(thread != nullptr && proc.register_thread(thread).has_value())) {
    return;
  }
  ut::expect(proc.thread_count() == 1 && proc.get_main_thread() == thread);
  proc.update_cpu_time(10, 20);
  proc.record_context_switch(true);
  proc.record_context_switch(false);
  proc.record_page_fault(true);
  proc.record_page_fault(false);
  moss::abi::DomainInfo info{};
  proc.monitor_snapshot(info);
  ut::expect(info.threads == 1 && info.uid == 0 && info.state == static_cast<u32>(ProcessState::Created));
  ut::expect(proc.try_begin_exec());
  auto *blocked = new Thread(Process::allocate_thread_id(), proc.pid());
  ut::expect(!proc.register_thread(blocked));
  delete blocked;
  proc.finish_exec();
  proc.cleanup_files();

  auto as = shared_ptr<AddressSpace>(new AddressSpace(0, 0));
  ut::expect(!proc.set_address_space(shared_ptr<AddressSpace>{}));
  ut::expect(proc.set_address_space(as));
  ut::expect(proc.address_space().get() == as.get());
  ut::expect(user_space::map_user_memory(nullptr, user_layout::CODE_BASE, 0, page, vma_flags::READ).error() ==
             ErrorCode::InvalidArgument);
  ut::expect(user_space::map_user_memory(as.get(), user_layout::CODE_BASE, 0, 0, vma_flags::READ).error() ==
             ErrorCode::InvalidArgument);
  ut::expect(user_space::map_user_memory(as.get(), USER_MAX, 0, page, vma_flags::READ).error() ==
             ErrorCode::InvalidArgument);
  ut::expect(user_space::map_user_memory(as.get(), user_layout::CODE_BASE, 0, page, vma_flags::READ).has_value());
  ut::expect(user_space::map_user_memory(as.get(), user_layout::CODE_BASE, 0, page, vma_flags::READ).error() ==
             ErrorCode::AlreadyExists);
  ut::expect(user_space::allocate_user_heap(nullptr, page).error() == ErrorCode::InvalidArgument);
  ut::expect(user_space::allocate_user_heap(&proc, 0).error() == ErrorCode::InvalidArgument);
  auto heap = user_space::allocate_user_heap(&proc, page);
  ut::expect(heap.has_value() && *heap == user_layout::HEAP_START);
  ut::expect(user_space::allocate_user_heap(&proc, page).has_value());
  proc.clear_address_space();
  ut::expect(!proc.address_space());

  auto real = user_space::create_user_address_space();
  if (ut::expect(real.has_value())) {
    auto owned = *real;
    ut::expect(owned->add_vma(user_layout::CODE_BASE, user_layout::CODE_BASE + page,
                              vma_flags::READ | vma_flags::WRITE | vma_flags::DEMAND_ZERO));
    ut::expect(owned->resolve_fault(user_layout::CODE_BASE, mm::UserFaultAccess::Read, false));
    ut::expect(owned->resolve_fault(user_layout::CODE_BASE, mm::UserFaultAccess::Write, false));
    ut::expect(!owned->resolve_fault(user_layout::CODE_BASE + 2 * page, mm::UserFaultAccess::Read, false));
    ut::expect(owned->add_vma(user_layout::STACK_TOP - user_layout::STACK_SIZE, user_layout::STACK_TOP,
                              vma_flags::READ | vma_flags::WRITE, VmaType::STACK));
    VirtAddr grown = 0;
    ut::expect(owned->resolve_fault(user_layout::STACK_TOP - user_layout::STACK_SIZE - page,
                                    mm::UserFaultAccess::Read, false, &grown));
    ut::expect(grown == user_layout::STACK_TOP - user_layout::STACK_SIZE - page);
    ut::expect(!owned->resolve_fault(USER_MAX, mm::UserFaultAccess::Read, false));
  }

  Process bad(78);
  ut::expect(bad.create_thread(user_layout::CODE_BASE, user_layout::CODE_BASE, page).error() ==
             ErrorCode::InvalidState);
  user_space::release_asid(0);
  user_space::release_asid(256);

  ProcessManager manager;
  auto first = manager.create_process();
  ut::expect(first.has_value() && manager.total_processes() == 1);
  if (first) {
    auto first_proc = *first;
    ut::expect(manager.process_exists(first_proc->pid()));
    ut::expect(manager.find_process(first_proc->pid()).get() == first_proc.get());
    ut::expect(manager.terminate_process(first_proc->pid(), 3).has_value());
    ut::expect(manager.total_processes() == 0 && !manager.process_exists(first_proc->pid()));
  }
  ut::expect(!manager.terminate_process(INVALID_PROCESS_ID, 0));
}

void load_balancer_contracts() {
  using namespace process;

  CfsScheduler scheduler;
  LoadBalancer balancer;
  Thread tasks[4] = {{40, 0}, {41, 0}, {42, 0}, {43, 0}};
  const auto current_cpu = arch::get_current_cpu_id();
  const u32 other_cpu = (current_cpu + 1) % g_num_cpus;
  for (u32 i = 0; i < 4; ++i) {
    tasks[i].cpu = other_cpu;
    tasks[i].state = ProcessState::Ready;
    tasks[i].se.load_avg = 100;
    tasks[i].se.vruntime = 100 + i;
    tasks[i].cpu_affinity_mask = CpuBitmap::all();
  }
  for (u32 i = 0; i < 3; ++i) {
    scheduler.enqueue_task(&tasks[i], other_cpu);
  }
  ut::expect(balancer.get_stats(g_num_cpus).migrations_count == 0);
  ut::expect(balancer.get_stats(other_cpu).migrations_count == 0);
  balancer.set_policy(BalancePolicy::Aggressive);
  balancer.set_policy(BalancePolicy::NUMA_Aware);
  balancer.set_policy(static_cast<BalancePolicy>(99));
  balancer.set_policy(BalancePolicy::Conservative);
  ut::expect(!balancer.migrate_task(g_num_cpus, other_cpu, scheduler));
  ut::expect(!balancer.migrate_task(other_cpu, other_cpu, scheduler));
  ut::expect(balancer.migrate_task(other_cpu, current_cpu, scheduler));
  ut::expect(balancer.get_stats(other_cpu).migrations_count == 1);
  ut::expect(balancer.get_global_stats().migrations_count >= 1);

  ut::expect(balancer.idle_balance(g_num_cpus, scheduler) == false);
  ut::expect(balancer.select_cpu_for_task(nullptr, scheduler) == 0);
  Thread affinity(44, 0);
  affinity.cpu = other_cpu;
  affinity.cpu_affinity_mask = CpuBitmap::single(other_cpu);
  ut::expect(balancer.select_cpu_for_task(&affinity, scheduler) == other_cpu);
  balancer.set_policy(BalancePolicy::NUMA_Aware);
  ut::expect(balancer.select_cpu_for_task(&affinity, scheduler) == other_cpu);
  balancer.set_policy(BalancePolicy::Conservative);

  for (u32 i = 0; i < 3; ++i) {
    scheduler.dequeue_task(&tasks[i]);
    tasks[i].se.load_avg = 100;
    tasks[i].state = ProcessState::Ready;
    scheduler.enqueue_task(&tasks[i], current_cpu);
  }
  balancer.periodic_balance(0, scheduler);
  balancer.periodic_balance(8000000, scheduler);
  for (u32 i = 0; i < 3; ++i) {
    scheduler.dequeue_task(&tasks[i]);
  }

  // A valid idle CPU reaches busiest selection, benefit accounting and the
  // scheduler-owned migration path.
  for (u32 i = 0; i < 3; ++i) {
    tasks[i].cpu = other_cpu;
    tasks[i].state = ProcessState::Ready;
    tasks[i].se.load_avg = 100;
    tasks[i].se.vruntime = 100 + i;
    scheduler.enqueue_task(&tasks[i], other_cpu);
  }
  ut::expect(balancer.idle_balance(current_cpu, scheduler));
  auto *saved_balancer = process::g_load_balancer;
  auto *saved_scheduler = process::g_scheduler;
  process::g_load_balancer = &balancer;
  process::g_scheduler = &scheduler;
  process::try_idle_balance(current_cpu);
  process::g_load_balancer = saved_balancer;
  process::g_scheduler = saved_scheduler;
  for (u32 i = 0; i < 3; ++i) {
    scheduler.dequeue_task(&tasks[i]);
  }
}

void runqueue_edges() {
  using namespace process;
  CfsRunqueue queue;
  queue.enqueue_task(nullptr);
  queue.dequeue_task(nullptr);
  ut::expect(queue.pick_next_task() == nullptr);
  ut::expect(queue.pick_last_task() == nullptr);
  ut::expect(queue.pick_migration_task(0, nullptr) == nullptr);
  ut::expect(queue.nr_running() == 0 && queue.total_weight() == 0);

  Thread first(300, 0), middle(301, 0), last(302, 0);
  Thread *tasks[] = {&first, &middle, &last};
  for (u32 i = 0; i < 3; ++i) {
    tasks[i]->se.vruntime = 10ULL * (i + 1);
    tasks[i]->se.weight = 1024;
    tasks[i]->state = ProcessState::Ready;
    tasks[i]->cpu_affinity_mask.set(0);
    queue.enqueue_task(tasks[i]);
  }
  ut::expect(queue.nr_running() == 3);
  ut::expect(queue.pick_next_task() == &first);
  ut::expect(queue.pick_last_task() == &last);

  last.cpu_affinity_mask.set_from_u32(0);
  middle.state = ProcessState::Running;
  ut::expect(queue.pick_migration_task(1, &first) == nullptr);
  middle.state = ProcessState::Ready;
  middle.cpu_affinity_mask.set(1);
  ut::expect(queue.pick_migration_task(1, &first) == &middle);

  ut::expect(!queue.should_preempt(nullptr));
  ut::expect(!queue.should_preempt(&first));
  first.se.sum_exec_runtime = first.se.prev_sum_exec_runtime + cfs_params::SCHED_LATENCY_NS;
  last.se.vruntime = first.se.vruntime + cfs_params::SCHED_LATENCY_NS;
  ut::expect(queue.should_preempt(&last));
  last.se.vruntime = 0;
  first.se.sum_exec_runtime = first.se.prev_sum_exec_runtime;
  middle.se.vruntime = cfs_params::SCHED_LATENCY_NS;
  ut::expect(queue.should_preempt(&middle));

  queue.update_curr_task(nullptr, 1);
  queue.update_curr_task(&first, 0);
  first.se.vruntime = 100;
  queue.update_curr_task(&first, 1000);
  ut::expect(first.se.sum_exec_runtime == 1000);
  queue.place_entity(&first, true);
  const auto forked = first.se.vruntime;
  queue.place_entity(&first, false);
  ut::expect(first.se.vruntime <= forked);
  queue.dump_runqueue();
  queue.dequeue_task(&first);
  queue.dequeue_task(&middle);
  queue.dequeue_task(&last);
  queue.dequeue_task(&first);
  ut::expect(queue.nr_running() == 0);

  RtRunqueue rt;
  Thread zero(310, 0), one(311, 0), two(312, 0);
  zero.rt.priority = 0;
  one.rt.priority = 40;
  two.rt.priority = 40;
  rt.enqueue_task(&zero);
  rt.enqueue_task(&one);
  rt.enqueue_task(&two);
  ut::expect(rt.nr_running() == 3);
  rt.requeue_task(nullptr);
  rt.requeue_task(&zero);
  ut::expect(rt.pick_next_task() != nullptr);
  rt.requeue_task(&one);
  ut::expect(rt.pick_next_task() != nullptr);
  rt.dequeue_task(&one);
  rt.dequeue_task(&two);
  zero.rt.priority = 40;
  rt.requeue_task(&zero);
  rt.dequeue_task(&zero);
  ut::expect(true);
}

void budget_edges() {
  using namespace process;
  unique_ptr<CfsScheduler> scheduler(new CfsScheduler());
  if (!ut::expect(scheduler.get() != nullptr)) {
    return;
  }
  Thread task(320, 0);
  ut::expect(scheduler->request_cpu_budget(nullptr, 1, 10) == BudgetProfileResult::Invalid);
  ut::expect(scheduler->request_cpu_budget(&task, 0, 10) == BudgetProfileResult::Invalid);
  ut::expect(scheduler->request_cpu_budget(&task, 11, 10) == BudgetProfileResult::Invalid);
  auto &timers = timer::TimerSubsystem::instance();
  const bool was_initialized = timers.is_initialized();
  if (was_initialized) {
    timers.shutdown();
  }
  ut::expect(scheduler->request_cpu_budget(&task, 1, 10) == BudgetProfileResult::ClockUnavailable);
  if (!timers.is_initialized()) {
    ut::expect(timers.initialize().has_value());
  }
  ut::expect(scheduler->request_cpu_budget(&task, 1, 10) == BudgetProfileResult::Applied);
  ut::expect(scheduler->request_cpu_budget(&task, 1, 10) == BudgetProfileResult::AlreadyConfigured);
  task.state = ProcessState::Terminated;
  ut::expect(scheduler->request_cpu_budget(&task, 1, 10) == BudgetProfileResult::Invalid);
  const auto status = scheduler->cpu_budget_status(&task);
  ut::expect((status.flags & BudgetProfileStatus::Terminated) != 0);
  (void)scheduler->cpu_budget_status(nullptr);
  (void)scheduler->monitor_runnable(g_num_cpus);
  (void)scheduler->monitor_cpu(&task);
  scheduler->replenish_cpu_budgets(0);
  ut::expect(scheduler->get_cpu_load(g_num_cpus) == 0);
  ut::expect(scheduler->get_cpu_nr_running(g_num_cpus) == 0);
  send_reschedule_ipi(g_num_cpus);
  send_reschedule_ipi(arch::get_current_cpu_id());
  ut::expect(true);
}

void scheduler_state_matrix() {
  using namespace process;
  CfsScheduler scheduler;
  Thread task(500, 0);
  const ProcessState states[] = {ProcessState::Created, ProcessState::Ready,    ProcessState::Running,
                                 ProcessState::Sleeping, ProcessState::DiskSleep, ProcessState::Terminated,
                                 ProcessState::Zombie,   ProcessState::Stopped};
  u32 valid = 0;
  for (const auto from : states) {
    for (const auto to : states) {
      task.state = from;
      scheduler.transition_task_state(&task, to);
      if (task.state == to && from != to) {
        ++valid;
      }
    }
  }
  scheduler.transition_task_state(nullptr, ProcessState::Ready);
  ut::expect(valid == 18);

  const auto cpu = arch::get_current_cpu_id();
  task.cpu = cpu;
  task.cpu_affinity_mask.set(cpu);
  scheduler.enqueue_task(nullptr, cpu);
  scheduler.enqueue_task(&task, g_num_cpus);
  scheduler.dequeue_task(nullptr);
  ut::expect(scheduler.take_next_task(g_num_cpus) == nullptr);
  ut::expect(!scheduler.migrate_ready_task(g_num_cpus, cpu));
  ut::expect(!scheduler.migrate_ready_task(cpu, g_num_cpus));
  ut::expect(!scheduler.migrate_ready_task(cpu, cpu));
  ut::expect(scheduler.pick_last_task(g_num_cpus) == nullptr);
  ut::expect(scheduler.get_cpu_load(g_num_cpus) == 0);
  ut::expect(scheduler.get_cpu_nr_running(g_num_cpus) == 0);
  scheduler.dump_runqueue(g_num_cpus);
  scheduler.update_current(nullptr, 1);
  ut::expect(!scheduler.should_preempt_current(nullptr));
  scheduler.place_entity(nullptr, cpu, false);
  scheduler.place_entity(&task, g_num_cpus, false);
  ut::expect(scheduler.get_cpu_min_vruntime(g_num_cpus) == 0);
  scheduler.set_idle_task(g_num_cpus, nullptr);

  Thread budget_a(620, 0), budget_b(621, 0), budget_c(622, 0);
  budget_a.state = ProcessState::Ready;
  budget_b.state = ProcessState::Ready;
  budget_c.state = ProcessState::Ready;
  ut::expect(scheduler.request_cpu_budget(nullptr, 1, 1) == BudgetProfileResult::Invalid);
  ut::expect(scheduler.request_cpu_budget(&budget_a, 0, 1) == BudgetProfileResult::Invalid);
  ut::expect(scheduler.request_cpu_budget(&budget_a, 2, 1) == BudgetProfileResult::Invalid);
  budget_a.state = ProcessState::Terminated;
  ut::expect(scheduler.request_cpu_budget(&budget_a, 1, 2) == BudgetProfileResult::Invalid);
  budget_a.state = ProcessState::Ready;
  ut::expect(scheduler.request_cpu_budget(&budget_a, 1, 2) == BudgetProfileResult::Applied);
  ut::expect(scheduler.request_cpu_budget(&budget_a, 1, 2) == BudgetProfileResult::AlreadyConfigured);
  ut::expect(scheduler.request_cpu_budget(&budget_b, 1, 3) == BudgetProfileResult::Invalid);
  ut::expect(scheduler.request_cpu_budget(&budget_b, 2, 2) == BudgetProfileResult::Capacity);
  CfsScheduler empty_scheduler;
  ut::expect(empty_scheduler.request_cpu_budget(&budget_c, 1, 2) == BudgetProfileResult::Applied);

  Thread ipc_caller(630, 0), ipc_server(631, 0), ipc_next(632, 0), ipc_donor(633, 0);
  PriorityDonation donor_call{}, active_call{}, stale_call{};
  donor_call.caller = &ipc_donor;
  donor_call.deadline_ns.store(50);
  ipc_donor.ipc_wait = &donor_call;
  ipc_caller.ipc_donors = &donor_call;
  ut::expect(!scheduler.begin_ipc_call(nullptr, &ipc_caller));
  ut::expect(!scheduler.begin_ipc_call(&active_call, nullptr));
  u64 inherited_deadline = 0;
  ut::expect(scheduler.begin_ipc_call(&active_call, &ipc_caller, &inherited_deadline));
  ut::expect(inherited_deadline == 50 && active_call.deadline_ns.load() == 50);
  ut::expect(!scheduler.begin_ipc_call(&stale_call, &ipc_caller));
  ut::expect(scheduler.bind_ipc_server(nullptr, &ipc_server) == IpcBindResult::Stale);
  ut::expect(scheduler.rebind_ipc_server(nullptr, &ipc_server, &ipc_next) == IpcBindResult::Stale);
  stale_call.caller = &ipc_donor;
  ut::expect(scheduler.bind_ipc_server(&stale_call, &ipc_server) == IpcBindResult::Stale);
  ut::expect(scheduler.bind_ipc_server(&active_call, nullptr) == IpcBindResult::Bound);
  ut::expect(scheduler.bind_ipc_server(&active_call, &ipc_server) == IpcBindResult::Bound);
  ut::expect(scheduler.bind_ipc_server(&active_call, &ipc_server) == IpcBindResult::Bound);
  ut::expect(scheduler.rebind_ipc_server(&active_call, &ipc_server, &ipc_next) == IpcBindResult::Bound);
  u64 final_deadline = 0;
  (void)scheduler.end_ipc_call(&active_call, &final_deadline);
  ut::expect(final_deadline == 50 && active_call.deadline_ns.load() == 0);
  (void)scheduler.end_ipc_call(nullptr);
  scheduler.forget_ipc_thread(nullptr);
  scheduler.set_base_nice(nullptr, 0);
  scheduler.set_base_nice(&ipc_server, priority::MIN_NICE - 1);
  scheduler.set_base_nice(&ipc_server, priority::MAX_NICE + 1);
  scheduler.set_base_nice(&ipc_server, 10);
  scheduler.set_base_nice(&ipc_server, -10);
  scheduler.reset_current_to_min_vruntime();

  Thread timer_target(640, 0);
  PriorityDonation timer_call{};
  timer::HrTimer timer_probe;
  timer_probe.init(timer::TimerMode::OneShot, [](void *) noexcept {}, nullptr);
  ut::expect(scheduler.begin_ipc_call(&timer_call, &timer_target));
  scheduler.arm_ipc_deadline_timer(nullptr, nullptr);
  scheduler.arm_ipc_deadline_timer(&stale_call, &timer_probe);
  scheduler.arm_ipc_deadline_timer(&timer_call, nullptr);
  scheduler.arm_ipc_deadline_timer(&timer_call, &timer_probe);
  if (static_cast<bool>(timer_probe.reserve())) {
    timer_call.deadline_ns.store(timer::TimerSubsystem::instance().now_ns() + 1'000'000);
    scheduler.arm_ipc_deadline_timer(&timer_call, &timer_probe);
    timer_probe.cancel_sync();
  }
  scheduler.end_ipc_call(&timer_call);
  ut::expect(true);
}

void cfs_tree_internal_edges() {
  using namespace process;
  CfsRunqueue queue;
  Thread tasks[12] = {{601, 0}, {602, 0}, {603, 0}, {604, 0}, {605, 0}, {606, 0},
                      {607, 0}, {608, 0}, {609, 0}, {610, 0}, {611, 0}, {612, 0}};
  for (u32 i = 0; i < 12; ++i) {
    tasks[i].se.vruntime = (i * 17U) % 37U;
    tasks[i].se.weight = 1024;
    tasks[i].state = ProcessState::Ready;
    queue.enqueue_task(&tasks[i]);
  }
  ut::expect(queue.nr_running() == 12);
  ut::expect(queue.pick_next_task() != nullptr);
  for (u32 i = 0; i < 12; ++i) {
    queue.dequeue_task(&tasks[(i * 5U) % 12U]);
  }
  ut::expect(queue.nr_running() == 0 && queue.pick_next_task() == nullptr);

  RtRunqueue rt;
  Thread rt_one(650, 0), rt_two(651, 0), rt_three(652, 0);
  rt_one.sched_class = SchedClass::RealTime;
  rt_two.sched_class = SchedClass::RealTime;
  rt_one.rt.priority = 40;
  rt_two.rt.priority = 40;
  rt.enqueue_task(&rt_one);
  rt.enqueue_task(&rt_two);
  rt.requeue_task(nullptr);
  rt.requeue_task(&rt_one);
  ut::expect(rt.pick_next_task() == &rt_two);
  rt.requeue_task(&rt_two);
  ut::expect(rt.pick_next_task() == &rt_one);
  rt.dequeue_task(&rt_one);
  rt.dequeue_task(&rt_two);
  rt_three.rt.priority = 40;
  rt.enqueue_task(&rt_three);
  rt.requeue_task(&rt_three);
  rt.dequeue_task(&rt_three);
  ut::expect(rt.nr_running() == 0);
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
    ut::register_test("try_lock_preemption", try_lock_preemption);
    ut::register_test("reschedule_safe_point", reschedule_safe_point);
    ut::register_test("periodic_scan_ownership", periodic_scan_ownership);
    ut::register_test("cpu_runtime_accounting", cpu_runtime_accounting);
    ut::register_test("cpu_budget_accounting", cpu_budget_accounting);
    ut::register_test("cpu_budget_admission", cpu_budget_admission);
    ut::register_test("cfs_cpu_budget_queue", [] { cpu_budget_queue(false); });
    ut::register_test("rt_cpu_budget_queue", [] { cpu_budget_queue(true); });
    ut::register_test("ipc_priority_inheritance", ipc_priority_inheritance);
    ut::register_test("ipc_deadline_tightening", ipc_deadline_tightening);
    ut::register_test("ipc_call_graph_limits", ipc_call_graph_limits);
    ut::register_test("migration_current_owner", migration_current_owner);
    ut::register_test("runqueue_contracts", runqueue_contracts);
    ut::register_test("idle_contracts", idle_contracts);
    ut::register_test("scheduler_lifecycle", scheduler_lifecycle);
    ut::register_test("process_surfaces", process_surfaces);
    ut::register_test("load_balancer_contracts", load_balancer_contracts);
    ut::register_test("runqueue_edges", runqueue_edges);
    ut::register_test("budget_edges", budget_edges);
    ut::register_test("ipc_cycle_and_tree_edges", ipc_cycle_and_tree_edges);
    ut::register_test("tick_edge_paths", tick_edge_paths);
    ut::register_test("wake_and_migrate_edges", wake_and_migrate_edges);
    ut::register_test("scheduler_state_matrix", scheduler_state_matrix);
    ut::register_test("cfs_tree_internal_edges", cfs_tree_internal_edges);
  });
}

} // namespace moss::test::validation
