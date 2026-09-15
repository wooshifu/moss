# MOSS Full SMP Multi-Core Support Design

**Date:** 2025-07-16
**Status:** Approved
**Scope:** Full SMP — SpinLock, secondary CPU init, per-CPU scheduling, IPI reschedule, load balancing, 8-core QEMU verification

---

## 1. Problem Statement

MOSS has extensive SMP infrastructure (per-CPU runqueues, IPI framework, PSCI boot, LoadBalancer skeleton) but only CPU 0 actually runs tasks. Secondary CPUs boot via PSCI, enter `cpu_park()`, and sleep forever. Three root causes block true multi-core execution:

1. **No SpinLock primitive** — shared data structures (runqueues, allocators) have no concurrent-access protection
2. **Secondary CPUs don't initialize subsystems** — no GIC CPU interface, no timer, no exception vectors
3. **No per-CPU scheduling loop** — secondary CPUs never enter the scheduler

## 2. Existing Infrastructure Inventory

| Component | Status | Location |
|-----------|--------|----------|
| `PerCpuData<T>`, `PerCpuAtomicCounter<T>`, `PerCpuWorkQueue<T>` | ✅ Complete | containers.cppm |
| `CfsScheduler` with `PerCpuData<CfsRunqueue>` | ✅ Complete | process.cppm |
| `current_running_tasks_[MAX_CPUS]` | ✅ Complete | process.cppm |
| `bootstrap_contexts_[MAX_CPUS]` | ✅ Complete | process.cppm |
| `GenericInterruptController` + `send_sgi()` | ✅ Complete | interrupts.cppm |
| `SimpleHardwareIpi` (Reschedule/Ping/WakeUp) | ✅ Complete | interrupts.cppm |
| PSCI `CPU_ON` → `secondary_cpu_entry()` → `cpu_park()` | ✅ Boot works | boot_impl.cpp |
| `LoadBalancer` with migration queue | ✅ Skeleton | process.cppm |
| Exception vectors + IRQ trampoline | ✅ Complete | start_arm64.S |
| Linker script: 128KB stack (8 CPU × 16KB) | ✅ Complete | linker.ld |
| SpinLock | ❌ Missing | — |
| Secondary CPU subsystem init | ❌ Missing | — |
| Per-CPU scheduling loop | ❌ Missing | — |

## 3. Architecture Design

### 3.1 Overall Architecture

```
┌──────────────────────────────────────────────────────────┐
│                      CPU 0 (BSP)                          │
│  ┌──────────┐  ┌────────────┐  ┌──────────────┐          │
│  │ CfsRq[0] │  │ Timer[0]   │  │ GIC CPU IF   │          │
│  │ (locked) │  │ sched_tick │  │ (initialized)│          │
│  └──────────┘  └────────────┘  └──────────────┘          │
│       │             │                  │                   │
│       ▼             ▼                  ▼                   │
│  scheduler_tick() → preempt? → context_switch()           │
│                                                           │
│              IPI (SGI 0: Reschedule)                      │
│                    ────────────────────►                   │
│                                                           │
│  ┌──────────────────────────────────────────────┐         │
│  │              LoadBalancer                      │        │
│  │  idle_balance() / periodic_balance()          │        │
│  │  → migrate_task(from_cpu, to_cpu)             │        │
│  │  → IPI reschedule target CPU                  │        │
│  └──────────────────────────────────────────────┘         │
└──────────────────────────────────────────────────────────┘
                         │ same design ×8
┌──────────────────────────────────────────────────────────┐
│                      CPU 1-7 (AP)                         │
│  secondary_cpu_init():                                    │
│    1. Set VBAR_EL1 (exception vectors)                    │
│    2. init_cpu_interface() (GIC)                          │
│    3. Enable CPACR_EL1 (FP/NEON)                         │
│    4. Start per-CPU timer tick                            │
│    5. Enter per_cpu_schedule_loop() (WFI idle)            │
│                                                           │
│  Timer IRQ / IPI Reschedule → scheduler_tick() → preempt  │
└──────────────────────────────────────────────────────────┘
```

### 3.2 Ticket SpinLock

ARM64-optimized ticket spinlock using LDAXR/STLXR + WFE/SEV:

```cpp
class TicketSpinLock {
  AtomicU32 next_ticket_{0};
  AtomicU32 now_serving_{0};
public:
  void lock() noexcept {
    u32 my_ticket = next_ticket_.fetch_add(1, Acquire);
    while (now_serving_.load(Acquire) != my_ticket) {
      arch::cpu_yield();  // WFE on ARM64
    }
  }
  void unlock() noexcept {
    now_serving_.fetch_add(1, Release);
    // SEV on ARM64 wakes WFE waiters
  }
};
```

Location: `containers.cppm`, alongside existing synchronization primitives.

### 3.3 Lock Placement

| Data Structure | Lock Type | Rationale |
|---------------|-----------|-----------|
| `CfsRunqueue` | TicketSpinLock per-queue | enqueue/dequeue/pick_next under lock |
| `RuntimeHeapAllocator` | TicketSpinLock | Global heap: new/delete from any CPU |
| `PageFrameAllocator` | TicketSpinLock | Physical page allocation from any CPU |
| `interrupt_table_` | Existing RCU | Read-heavy, keep lock-free reads |
| `current_running_tasks_[]` | No lock needed | Per-CPU exclusive write |
| `TimerSubsystem` queue | TicketSpinLock | Timer enqueue from any CPU |

### 3.4 Secondary CPU Initialization

Transform `cpu_park()` into `secondary_cpu_init()`:

```
secondary_cpu_entry()
  → secondary_cpu_init(cpu_id):
    1. msr vbar_el1, exception_vectors    // Exception handling
    2. mov cpacr_el1, #(3<<20)            // FP/NEON access
    3. hal::intc::init_cpu_interface()     // GIC per-CPU
    4. hal::timer::enable()               // Per-CPU timer
    5. hal::timer::set_compare(far_future) // No spurious IRQ
    6. msr daifclr, #2                    // Enable IRQs
    7. per_cpu_schedule_loop(cpu_id)       // Never returns
```

### 3.5 Per-CPU Scheduling Loop

Each secondary CPU runs:

```cpp
[[noreturn]] void per_cpu_schedule_loop(u32 cpu_id) noexcept {
  // Arm per-CPU scheduler tick timer
  HrTimer tick;
  tick.init(TimerMode::Periodic, scheduler_tick_callback, scheduler);
  tick.start_relative(SCHED_LATENCY_NS);

  while (true) {
    Thread *next = pick_next_task(cpu_id);
    if (next) {
      dequeue_task(next);
      context_switch_to_task(next);
    } else {
      // Try idle balance (steal from busy CPUs)
      load_balancer->idle_balance(cpu_id, *scheduler);
      // Still nothing? Sleep until IRQ
      arch::cpu_idle_once();
    }
  }
}
```

### 3.6 IPI Reschedule Integration

When LoadBalancer migrates a task to CPU N:

1. `lock(rq[target_cpu])` → `enqueue_task(task, target_cpu)` → `unlock(rq[target_cpu])`
2. `send_sgi(SGI_RESCHEDULE, 1 << target_cpu)` — wake target CPU
3. Target CPU IRQ handler: if SGI_RESCHEDULE → set `need_resched` flag
4. On return from IRQ → check flag → `schedule()`

### 3.7 Load Balancer Activation

The existing `LoadBalancer` class has `idle_balance()` and periodic balance hooks. Integration points:

- **idle_balance**: Called from per-CPU schedule loop when local queue empty
- **periodic_balance**: Called from `scheduler_tick()` every N ticks
- **task migration**: `dequeue_task(thread)` on source CPU, `enqueue_task(thread, target_cpu)` on target CPU, both under respective runqueue locks

### 3.8 QEMU Configuration

- Change `cpu_cores` from 4 to 8 in QEMU config
- Update linker.ld stack from 128KB to 256KB (8 CPU × 32KB)
- Update `MAX_CPUS` from 16 to 16 (already sufficient)
- Task creation: distribute across 8 CPUs (`i % 8` instead of `i % 4`)

## 4. Implementation Steps

| Step | Description | Files | Risk |
|------|-------------|-------|------|
| 1 | Implement TicketSpinLock + SpinLockGuard | containers.cppm | Low |
| 2 | Add SpinLock to CfsRunqueue | process.cppm | Medium |
| 3 | Add SpinLock to heap + page allocator | mm.cppm | Medium |
| 4 | Secondary CPU subsystem init | boot_impl.cpp, process.cppm | High |
| 5 | Per-CPU scheduling loop | process.cppm | Medium |
| 6 | IPI reschedule integration | interrupts.cppm, process.cppm, kernel_main.cpp | Medium |
| 7 | Load balancer activation | process.cppm | Medium |
| 8 | 8-core build + QEMU verification | build.py, run_qemu.py, linker.ld | Low |

## 5. Verification Criteria

| Signal | Expected |
|--------|----------|
| QEMU 8-core boot | All CPUs print initialization logs |
| `[sched_tick]` per CPU | Each CPU shows independent tick output |
| Multi-CPU task interleaving | Task logs from different CPUs interleaved |
| Load balancing | Idle CPUs steal tasks from busy CPUs |
| No deadlocks | System runs 30+ seconds without hang |
| All 6 presets compile | ARM64/x64/RISC-V 64 × debug/release |

## 6. Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Deadlock from nested locks | Strict lock ordering: always acquire rq locks in CPU-ID order |
| IRQ handler takes spinlock | Use `disable_interrupts()` + lock (irq-safe spinlock variant) |
| Cache coherency on ARM64 | Ticket spinlock uses acquire/release semantics; WFE/SEV for wakeup |
| Timer not firing on secondary CPUs | Each CPU enables its own PPI timer independently |
| Stack overflow with 8 CPUs | Increase per-CPU stack to 32KB (256KB total) |
