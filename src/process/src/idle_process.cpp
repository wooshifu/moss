// Idle process implementation - module implementation unit for moss.process

module moss.process;

namespace moss::kernel::process {

// 全局Per-CPU idle任务数组
moss::kernel::containers::PerCpuData<IdleTask *> g_idle_tasks{nullptr};

// 标记哪些CPU正在运行idle
static moss::kernel::containers::PerCpuData<bool> g_cpu_idle_status{false};

IdleTask::IdleTask(u32 cpu_id) noexcept
    : Thread(Process::allocate_thread_id(), ProcessId{0}), cpu_id_(cpu_id), idle_time_ns_(0), last_idle_start_(0) {

  // idle任务的调度参数设置
  this->se.nice = 19;    // 最低优先级
  this->se.weight = 15;  // 最小权重
  this->se.vruntime = 0; // 虚拟运行时间为0

  // idle任务绑定到特定CPU
  this->cpu = cpu_id;
  this->wake_cpu = cpu_id;

  // idle任务状态为可运行
  this->state = ProcessState::Ready;
  this->sched_class = SchedClass::Idle;
}

[[noreturn]] void IdleTask::run() const noexcept {
  namespace log = moss::kernel::logging;
  log::klog::info("idle task started on CPU{}", cpu_id_);

  // 进入Linux风格的do_idle循环
  do_idle(cpu_id_);
}

[[noreturn]] void do_idle(u32 cpu_id) noexcept {
  // 标记当前CPU进入idle状态
  g_cpu_idle_status.get_cpu(cpu_id) = true;

  while (true) {
    // 🔧 Linux风格的idle循环核心机制

    // 1. Reschedule check is handled by timer IRQ -> scheduler_tick()
    //    which performs context_switch() preemption when needed.

    // 2. 电源管理：进入低功耗状态 (多架构支持)
    arch::memory_barrier();      // 数据同步屏障
    arch::cpu_halt();            // 等待中断 - CPU进入低功耗状态
    arch::instruction_barrier(); // 指令同步屏障

    // 3. WFI returns when an interrupt fires (timer or IPI).
    //    The timer IRQ handler calls scheduler_tick() which does
    //    context_switch() if a higher-priority task is runnable.
    //    If no preemption occurred, we simply loop back to WFI.
  }
}

IdleTask *create_idle_task(u32 cpu_id) noexcept {
  if (cpu_id >= moss::kernel::g_num_cpus) {
    return nullptr;
  }

  // 检查是否已经创建过
  if (g_idle_tasks.get_cpu(cpu_id) != nullptr) {
    return g_idle_tasks.get_cpu(cpu_id);
  }

  // 为idle任务分配内存
  // 使用标准的new操作符，后续可以集成更完整的内存分配器
  namespace log = moss::kernel::logging;

  auto *idle_task = new IdleTask(cpu_id);
  if (idle_task == nullptr) {
    log::klog::error("failed to allocate idle task for CPU{}", cpu_id);
    return nullptr;
  }

  // 存储到Per-CPU数组
  g_idle_tasks.get_cpu(cpu_id) = idle_task;

  log::klog::info("idle task created for CPU{} TID={}", cpu_id, static_cast<u32>(idle_task->tid));
  return idle_task;
}

bool is_cpu_idle(u32 cpu_id) noexcept {
  if (cpu_id >= moss::kernel::g_num_cpus) {
    return false;
  }
  return g_cpu_idle_status.get_cpu(cpu_id);
}

void wakeup_idle_cpu(u32 cpu_id) noexcept {
  if (cpu_id >= moss::kernel::g_num_cpus) {
    return;
  }

  // 标记CPU不再idle
  g_cpu_idle_status.get_cpu(cpu_id) = false;

  // Note: Actual IPI wakeup (SGI 0) is sent by the scheduler's
  // try_idle_balance() or enqueue_task() via the interrupts module.
  // This function only marks the status; the caller is responsible
  // for sending the IPI if needed.
}

} // namespace moss::kernel::process
