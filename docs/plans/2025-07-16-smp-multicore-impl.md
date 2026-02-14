# Full SMP Multi-Core Support Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Enable true multi-core scheduling on MOSS — all 8 CPUs independently running tasks with spinlock-protected shared structures, IPI-driven reschedule, and cross-CPU load balancing.

**Architecture:** Ticket SpinLock protects runqueues and allocators. Secondary CPUs initialize GIC/Timer/VBAR, then enter per-CPU scheduling loops. LoadBalancer migrates tasks between CPUs and sends IPI reschedule SGIs to wake idle cores.

**Tech Stack:** ARM64 assembly (WFE/SEV), C++26 modules, GICv2 SGI, QEMU virt 8-core

---

## Reference: Key Files and Locations

| File | Purpose |
|------|---------|
| `src/containers/src/containers.cppm` | Atomic types, PerCpuData — add TicketSpinLock here |
| `src/process/src/process.cppm` | CfsRunqueue (line 591), CfsScheduler (line 1111), LoadBalancer (line 1922) |
| `src/process/src/process.cpp` | Static member definitions |
| `src/mm/src/mm.cppm` | PageFrameAllocator (line 141), RuntimeHeapAllocator (line 413) |
| `src/mm/src/mm.cpp` | Allocator implementations |
| `src/kernel/src/runtime_support.cpp` | kernel_malloc/kernel_free (line 156/186) |
| `src/boot/src/arch/arm64/boot_impl.cpp` | secondary_cpu_entry (line 295), cpu_park (line 251) |
| `src/boot/src/arch/arm64/start_arm64.S` | Exception vectors, IRQ trampoline, per-CPU stacks |
| `src/interrupts/src/interrupts.cppm` | GIC driver, IPI types, send_sgi |
| `src/kernel/src/kernel_main.cpp` | irq_handler_c (line 222) |
| `src/kernel/src/kernel.cppm` | Kernel::run, activate_secondary_cpus call (line 993) |
| `src/hal/intc/src/intc_hal.cppm` | init_cpu_interface (line 217), send_sgi (line 395) |
| `src/hal/timer/src/timer_hal.cppm` | enable(), set_compare(), irq_number() |
| `src/timer/src/timer.cppm` | TimerSubsystem interface |
| `src/aal/src/arch.cppm` | MAX_CPUS=16, barriers, cpu_yield, interrupt control |
| `linker.ld` | Stack allocation (line 56-61) |
| `scripts/run_qemu.py` | QEMU cpu_cores config (line 54) |

## Conventions

- **Build command:** `uv run build.py` (all 6 presets)
- **Quick ARM64 build:** `cmake --workflow --preset arm64-qemu-debug`
- **QEMU test:** `./build/arm64-qemu-debug/run_qemu.sh`
- **Naming:** Classes=CamelCase, functions/vars=lower_case, private members=trailing underscore
- **Commit format:** `[module][subsystem] description` — English, no Co-Authored-By

---

## Task 1: Implement TicketSpinLock

**Files:**
- Modify: `src/containers/src/containers.cppm` (after line 214, before PerCpuCounter)

**Step 1: Add TicketSpinLock and SpinLockGuard classes**

Insert the following block after the `CacheAlignedAtomic` class closing brace (after line 214) and before the `PerCpuCounter` class (line 217). This goes inside the `export namespace moss::kernel::containers {` block:

```cpp
// ============================================================================
// Ticket SpinLock — fair, FIFO-ordered mutual exclusion
// ============================================================================

// ARM64-optimized ticket spinlock.
// Uses fetch_add for ticket acquisition (LDAXR/STLXR on ARM64).
// Waiters use cpu_yield() which maps to WFE on ARM64, woken by
// SEV generated implicitly by the store-release in unlock().
class TicketSpinLock {
private:
  AtomicU32 next_ticket_{0};
  AtomicU32 now_serving_{0};

public:
  constexpr TicketSpinLock() noexcept = default;

  TicketSpinLock(const TicketSpinLock &) = delete;
  TicketSpinLock &operator=(const TicketSpinLock &) = delete;

  void lock() noexcept {
    u32 my_ticket = next_ticket_.fetch_add(1, MemoryOrder::Acquire);
    while (now_serving_.load(MemoryOrder::Acquire) != my_ticket) {
      moss::kernel::arch::cpu_yield();  // WFE on ARM64
    }
  }

  void unlock() noexcept {
    now_serving_.fetch_add(1, MemoryOrder::Release);
    // ARM64: store-release generates implicit SEV to wake WFE waiters
  }

  [[nodiscard]] bool try_lock() noexcept {
    u32 current = now_serving_.load(MemoryOrder::Acquire);
    u32 next = current;
    return next_ticket_.compare_exchange_weak(next, current + 1,
                                              MemoryOrder::Acquire,
                                              MemoryOrder::Relaxed);
  }

  [[nodiscard]] bool is_locked() const noexcept {
    return next_ticket_.load(MemoryOrder::Relaxed) !=
           now_serving_.load(MemoryOrder::Relaxed);
  }
};

// IRQ-safe spinlock variant: disables IRQs while held.
// Prevents deadlock when IRQ handler also takes the same lock.
class IrqSpinLock {
private:
  TicketSpinLock inner_;

public:
  constexpr IrqSpinLock() noexcept = default;

  IrqSpinLock(const IrqSpinLock &) = delete;
  IrqSpinLock &operator=(const IrqSpinLock &) = delete;

  void lock() noexcept {
    moss::kernel::arch::disable_interrupts();
    inner_.lock();
  }

  void unlock() noexcept {
    inner_.unlock();
    moss::kernel::arch::enable_interrupts();
  }

  [[nodiscard]] bool try_lock() noexcept {
    moss::kernel::arch::disable_interrupts();
    if (inner_.try_lock()) {
      return true;
    }
    moss::kernel::arch::enable_interrupts();
    return false;
  }
};

// RAII lock guard
template <typename LockType>
class LockGuard {
private:
  LockType &lock_;

public:
  explicit LockGuard(LockType &lock) noexcept : lock_(lock) {
    lock_.lock();
  }
  ~LockGuard() noexcept {
    lock_.unlock();
  }

  LockGuard(const LockGuard &) = delete;
  LockGuard &operator=(const LockGuard &) = delete;
};
```

**Step 2: Build to verify compilation**

Run: `cmake --workflow --preset arm64-qemu-debug`
Expected: BUILD SUCCESS (no new code uses the lock yet, just definition)

**Step 3: Build all 6 presets**

Run: `uv run build.py`
Expected: All 6 presets pass

**Step 4: Commit**

```bash
git add src/containers/src/containers.cppm
git commit -m "[containers][sync] implement TicketSpinLock, IrqSpinLock, and LockGuard

Ticket-based fair spinlock with ARM64 WFE/SEV optimization.
IrqSpinLock variant disables interrupts while held for ISR safety.
LockGuard provides RAII lock management."
```

---

## Task 2: Add SpinLock to CfsRunqueue

The scheduler tick runs from IRQ context and accesses the runqueue. Other CPUs may also access the same runqueue during task migration. We need an `IrqSpinLock` per CfsRunqueue.

**Files:**
- Modify: `src/process/src/process.cppm` (CfsRunqueue class at line 591, CfsScheduler methods)

**Step 1: Add lock member to CfsRunqueue**

In the `CfsRunqueue` class (line 591), add a lock member at the top of the private section (after line 592):

```cpp
// Add as first private member:
  mutable containers::IrqSpinLock lock_;
```

Update the constructor (line 611-614) to remain unchanged — `IrqSpinLock` has a constexpr default constructor.

**Step 2: Add lock/unlock to CfsRunqueue public methods**

Wrap the bodies of `enqueue_task`, `dequeue_task`, `pick_next_task`, and `update_curr_task` with `LockGuard`:

For `enqueue_task` (line 616):
```cpp
  void enqueue_task(Thread *thread) noexcept {
    if (thread == nullptr)
      return;
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    // ... rest of existing body unchanged ...
  }
```

For `dequeue_task` (line 634):
```cpp
  void dequeue_task(Thread *thread) noexcept {
    if (thread == nullptr)
      return;
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    // ... rest of existing body (from the static dequeue_count line) unchanged ...
  }
```

For `pick_next_task` (line 655):
```cpp
  [[nodiscard]] Thread *pick_next_task() noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    // ... rest of existing body unchanged ...
  }
```

For `update_curr_task` (line 677):
```cpp
  void update_curr_task(Thread *current, u64 delta_exec) noexcept {
    if (current == nullptr)
      return;
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    // ... rest of existing body (from u64 old_vruntime line) unchanged ...
  }
```

Also wrap `should_preempt` (line 708) since it reads rb_leftmost_:
```cpp
  [[nodiscard]] bool should_preempt(Thread *current) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    // ... rest of existing body unchanged ...
  }
```
Note: remove the `const` qualifier from `should_preempt` since `IrqSpinLock::lock()` is non-const.

Also remove `const` from `nr_running()`, `min_vruntime()`, `total_weight()`, `load_avg()`, `util_avg()` if they are called while lock is held, OR keep them const and make the lock `mutable` (already done above with `mutable`).

**Step 3: Build and verify**

Run: `cmake --workflow --preset arm64-qemu-debug`
Expected: BUILD SUCCESS

**Step 4: QEMU smoke test**

Run: `./build/arm64-qemu-debug/run_qemu.sh`
Expected: Same behavior as before — single CPU scheduling still works with locks (no deadlock because IrqSpinLock disables IRQs). Look for `[sched_tick]` output, task switching.

**Step 5: Build all 6 presets**

Run: `uv run build.py`
Expected: All 6 presets pass

**Step 6: Commit**

```bash
git add src/process/src/process.cppm
git commit -m "[process][scheduler] add IrqSpinLock to CfsRunqueue

Protect enqueue_task, dequeue_task, pick_next_task, update_curr_task,
and should_preempt with per-runqueue IrqSpinLock. Required for
multi-CPU concurrent access to shared runqueues."
```

---

## Task 3: Add SpinLock to Heap and Page Allocators

**Files:**
- Modify: `src/mm/src/mm.cppm` (PageFrameAllocator line 141, RuntimeHeapAllocator line 413)
- Modify: `src/mm/src/mm.cpp` (allocator method implementations)

**Step 1: Add lock to PageFrameAllocator**

In `mm.cppm`, add a static lock member to `PageFrameAllocator` (after line 181):

```cpp
    static containers::IrqSpinLock lock_;
```

Note: this requires `import moss.containers;` — check if mm.cppm already imports it. If not, add the import.

**Step 2: Add lock to RuntimeHeapAllocator**

In `mm.cppm`, add a static lock member to `RuntimeHeapAllocator` (after line 463):

```cpp
    static containers::IrqSpinLock lock_;
```

**Step 3: Define static members in mm.cpp**

In `mm.cpp`, add definitions alongside other static members:

```cpp
containers::IrqSpinLock PageFrameAllocator::lock_;
containers::IrqSpinLock RuntimeHeapAllocator::lock_;
```

**Step 4: Wrap PageFrameAllocator::allocate_pages and free_pages**

In `mm.cpp`, wrap `allocate_pages` body:
```cpp
PageAllocResult<PhysAddr> PageFrameAllocator::allocate_pages(usize order) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    // ... existing body ...
}
```

Wrap `free_pages` body similarly.

**Step 5: Wrap RuntimeHeapAllocator::allocate, allocate_aligned, deallocate**

In `mm.cpp`, wrap each method body with `LockGuard<IrqSpinLock>`.

**Step 6: Build and test**

Run: `cmake --workflow --preset arm64-qemu-debug`
Expected: BUILD SUCCESS

Run: `./build/arm64-qemu-debug/run_qemu.sh`
Expected: Same single-CPU behavior, no deadlocks.

**Step 7: Build all 6 presets**

Run: `uv run build.py`
Expected: All 6 presets pass

**Step 8: Commit**

```bash
git add src/mm/src/mm.cppm src/mm/src/mm.cpp
git commit -m "[mm][allocator] add IrqSpinLock to PageFrameAllocator and RuntimeHeapAllocator

Protect allocate_pages, free_pages, allocate, allocate_aligned,
and deallocate with IrqSpinLock for multi-core safety."
```

---

## Task 4: Secondary CPU Subsystem Initialization

Currently `secondary_cpu_entry()` → `cpu_park()` → WFI forever. Transform this into a full initialization sequence.

**Files:**
- Modify: `src/boot/src/arch/arm64/boot_impl.cpp` (secondary_cpu_entry at line 295, cpu_park at line 251)
- Modify: `src/boot/src/arch/arm64/start_arm64.S` (per-CPU stack size at line 183)
- Modify: `src/process/src/process.cppm` (add `per_cpu_schedule_loop` declaration)
- Modify: `src/process/src/process.cpp` (add `per_cpu_schedule_loop` definition)
- Modify: `linker.ld` (stack size at line 59)

**Step 1: Increase per-CPU stack size**

In `linker.ld` line 56-59, change:
```
    /* 栈空间 - 多CPU支持：256KB总空间，每CPU 32KB，支持 MAX_CPUS=8 */
    .stack (NOLOAD) : ALIGN(4K) {
        _stack_bottom = .;
        . += 256K;  /* 256KB栈空间支持8个CPU (每CPU 32KB) */
        _stack_top = .;
    }
```

In `start_arm64.S` line 183, change per-CPU stack size:
```asm
    mov     x1, #0x8000           // 每个CPU 32KB栈空间
```

**Step 2: Rewrite secondary_cpu_entry in boot_impl.cpp**

Replace `secondary_cpu_entry()` (lines 295-352) with:

```cpp
extern "C" [[noreturn]] void secondary_cpu_entry() noexcept {
    // 1. Read CPU ID from hardware
    u64 mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    u32 cpu_id = static_cast<u32>(mpidr & 0xFF);

    volatile u32 *uart_base = reinterpret_cast<volatile u32 *>(moss::kernel::platform::uart_base());
    uart_base[0] = 'S';
    uart_base[0] = 'M';
    uart_base[0] = 'P';
    uart_base[0] = '0' + static_cast<u32>(cpu_id % 10);
    uart_base[0] = 10;

    // 2. Set exception vectors (same as CPU 0)
    extern char exception_vectors[];
    asm volatile("msr vbar_el1, %0" :: "r"(exception_vectors));
    asm volatile("isb");

    // 3. Enable FP/NEON access
    u64 cpacr = (3ULL << 20);
    asm volatile("msr cpacr_el1, %0" :: "r"(cpacr));
    asm volatile("isb");

    // 4. Initialize GIC CPU interface for this CPU
    const auto &plat = moss::fdt::get_platform_info();
    moss::kernel::VirtAddr gic_cpu_base =
        (plat.dtb_valid && plat.intc.valid)
            ? static_cast<moss::kernel::VirtAddr>(plat.intc.cpu_base)
            : moss::kernel::platform::intc_cpu_base();
    (void)moss::kernel::hal::intc::init_cpu_interface(gic_cpu_base);

    // 5. Enable per-CPU timer
    moss::kernel::hal::timer::enable();
    // Set compare far in the future to avoid spurious interrupt
    u64 counter_now = moss::kernel::hal::timer::read_counter();
    moss::kernel::hal::timer::set_compare(
        counter_now + moss::kernel::timer::TimerSubsystem::instance().clocksource().ns_to_cycles(1000000000ULL));

    // 6. Mark CPU as online
    asm volatile("dmb sy" ::: "memory");
    mark_cpu_online(cpu_id);
    asm volatile("dmb sy" ::: "memory");
    asm volatile("sev" ::: "memory");

    uart_base[0] = 'R';
    uart_base[0] = 'D';
    uart_base[0] = 'Y';
    uart_base[0] = '0' + static_cast<u32>(cpu_id % 10);
    uart_base[0] = 10;

    // 7. Enable IRQs and enter scheduling loop (never returns)
    asm volatile("msr daifclr, #2" ::: "memory");
    moss::kernel::process::secondary_cpu_schedule_loop(cpu_id);
}
```

**Step 3: Add secondary_cpu_schedule_loop declaration to process.cppm**

Near the `extern CfsScheduler *g_scheduler;` line (around line 1867), add:

```cpp
// Secondary CPU scheduling entry point — called from boot_impl.cpp
[[noreturn]] void secondary_cpu_schedule_loop(u32 cpu_id) noexcept;
```

**Step 4: Add secondary_cpu_schedule_loop definition to process.cpp**

Add at the end of process.cpp:

```cpp
[[noreturn]] void secondary_cpu_schedule_loop(u32 cpu_id) noexcept {
    namespace log = moss::kernel::logging;

    log::klog::info("CPU{}: entering scheduling loop", cpu_id);

    // Arm per-CPU scheduler tick timer
    static timer::HrTimer secondary_ticks[MAX_CPUS];
    secondary_ticks[cpu_id].init(
        timer::TimerMode::Periodic,
        [](void* data) {
            auto* sched = static_cast<CfsScheduler*>(data);
            sched->scheduler_tick();
        },
        g_scheduler);
    secondary_ticks[cpu_id].start_relative(CfsParams::SCHED_LATENCY_NS);

    log::klog::info("CPU{}: scheduler tick armed, entering idle", cpu_id);

    // Main scheduling loop
    while (true) {
        Thread *next = g_scheduler->pick_next_task(cpu_id);
        if (next != nullptr) {
            g_scheduler->dequeue_task(next);
            CfsScheduler::set_current_task(next);
            next->state = ProcessState::Running;
            next->se.exec_start = arch::get_timestamp_counter();

#if defined(MOSS_ARCH_ARM64)
            CpuContext *prev_ctx = &CfsScheduler::bootstrap_context(cpu_id);
            arch::disable_interrupts();
            context_switch(prev_ctx, &next->context);
            arch::enable_interrupts();
#endif
            // Returned — task was preempted. Clear and retry.
            CfsScheduler::set_current_task(nullptr);
        } else {
            arch::cpu_idle_once();
        }
    }
}
```

**Step 5: Add bootstrap_context accessor to CfsScheduler**

In `process.cppm`, add a public static accessor (near line 1528):

```cpp
  static CpuContext& bootstrap_context(u32 cpu) noexcept {
    return bootstrap_contexts_[cpu % MAX_CPUS];
  }
```

**Step 6: Add necessary extern declarations in boot_impl.cpp**

At the top of boot_impl.cpp in the `extern "C"` block (near line 15), ensure these are declared:

```cpp
extern char exception_vectors[];
```

Add the needed imports at the module level. Since boot_impl.cpp is `module moss.boot;`, it already imports timer/process modules. Verify `import moss.hal.intc;` and `import moss.hal.timer;` and `import moss.timer;` and `import moss.process;` are imported.

**Step 7: Update cpu_park to not be used**

Delete or simplify `cpu_park()` body since `secondary_cpu_entry` no longer calls it. Simplest: leave it as-is since it's now dead code.

**Step 8: Build and test**

Run: `cmake --workflow --preset arm64-qemu-debug`

Fix any compilation errors. Common issues:
- Missing imports in boot_impl.cpp
- `clocksource()` accessor may need to be added to TimerSubsystem
- `scheduler_tick()` may need to be made public on CfsScheduler

Run: `./build/arm64-qemu-debug/run_qemu.sh`
Expected: Secondary CPUs print "SMP0", "SMP1", etc. and "RDY1", "RDY2", etc. Then enter scheduling loops. May see `[sched_tick]` from multiple CPUs.

**Step 9: Build all 6 presets**

Run: `uv run build.py`
Expected: All 6 presets pass (x86_64/RISC-V stubs may need `#if defined(MOSS_ARCH_ARM64)` guards)

**Step 10: Commit**

```bash
git add linker.ld src/boot/src/arch/arm64/start_arm64.S \
        src/boot/src/arch/arm64/boot_impl.cpp \
        src/process/src/process.cppm src/process/src/process.cpp \
        src/timer/src/timer.cppm
git commit -m "[smp][boot] initialize secondary CPUs with GIC, timer, and scheduling loop

Secondary CPUs now initialize exception vectors, GIC CPU interface,
FP/NEON, and per-CPU timer before entering scheduling loop.
Increased per-CPU stack to 32KB (256KB total for 8 CPUs)."
```

---

## Task 5: Distribute Tasks Across All CPUs

Currently tasks are distributed to `i % 4`. Update to distribute across all available CPUs and make secondary CPUs actually pick up their tasks.

**Files:**
- Modify: `src/process/src/process.cppm` (create_test_task around line 1479)

**Step 1: Update task distribution**

In `create_test_task()`, line 1479, change:
```cpp
      u32 target_cpu = i % 4;
```
to:
```cpp
      u32 target_cpu = i % 8;  // Distribute across all 8 CPUs
```

**Step 2: Update status log**

In line 1490, the log already says `MAX_CPUS` — verify it shows 8.

**Step 3: Build and QEMU test**

Run: `cmake --workflow --preset arm64-qemu-debug && ./build/arm64-qemu-debug/run_qemu.sh`
Expected: `[sched_tick]` logs from multiple CPUs. Tasks run on different CPUs.

**Step 4: Commit**

```bash
git add src/process/src/process.cppm
git commit -m "[process][scheduler] distribute test tasks across all 8 CPUs

Changed task CPU assignment from i%4 to i%8 to utilize all cores."
```

---

## Task 6: IPI Reschedule Integration

When a task is enqueued on a remote CPU (e.g., by load balancer or after exit), send an IPI SGI to wake that CPU from WFI.

**Files:**
- Modify: `src/process/src/process.cppm` (CfsScheduler::enqueue_task)
- Modify: `src/kernel/src/kernel_main.cpp` (irq_handler_c — handle SGI)

**Step 1: Send IPI on remote enqueue**

In `CfsScheduler::enqueue_task` (line 1126), after the existing `enqueue_task` call, add IPI notification:

```cpp
  void enqueue_task(Thread *thread, u32 cpu) noexcept {
    if (thread == nullptr || cpu >= MAX_CPUS)
      return;

    runqueues_.get_cpu(cpu).enqueue_task(thread);
    thread->cpu = cpu;
    thread->state = ProcessState::Ready;

    // If enqueuing to a remote CPU, send IPI to wake it from WFI
    u32 current_cpu = get_current_cpu_id();
    if (cpu != current_cpu && interrupts::g_gic) {
      (void)interrupts::g_gic->send_sgi(
          static_cast<u32>(interrupts::IpiSgiId::Reschedule),
          1u << cpu);
    }
  }
```

**Step 2: Handle SGI in irq_handler_c**

In `kernel_main.cpp` `irq_handler_c` (line 222), add SGI handling:

```cpp
void irq_handler_c(void) noexcept {
  irq_count++;

  namespace intc_hal = ::moss::kernel::hal::intc;
  namespace timer_hal = ::moss::kernel::hal::timer;

  u64 gicc_base = ::moss::kernel::platform::intc_cpu_base();
  u32 ack_val = intc_hal::ack_irq(gicc_base);
  u32 irq = intc_hal::irq_from_ack(ack_val);

  if (intc_hal::is_spurious(irq)) {
    return;
  }

  intc_hal::eoi(gicc_base, ack_val);

  if (irq < 16) {
    // SGI (Software Generated Interrupt) — IPI
    // SGI 0 = Reschedule: no action needed, returning from IRQ
    // will check for runnable tasks in the schedule loop.
    // Other SGIs: ignore for now.
    return;
  }

  // PPI/SPI — timer interrupt
  timer_hal::ack_interrupt();
  ::moss::kernel::timer::TimerSubsystem::instance().handle_interrupt();
}
```

**Step 3: Build and test**

Run: `cmake --workflow --preset arm64-qemu-debug && ./build/arm64-qemu-debug/run_qemu.sh`
Expected: When tasks are enqueued to remote CPUs, those CPUs wake up and schedule them.

**Step 4: Build all 6 presets**

Run: `uv run build.py`
Expected: All 6 presets pass

**Step 5: Commit**

```bash
git add src/process/src/process.cppm src/kernel/src/kernel_main.cpp
git commit -m "[smp][ipi] send SGI reschedule IPI on remote CPU enqueue

When a task is enqueued to a CPU different from the current one,
send SGI 0 (Reschedule) to wake the target CPU from WFI.
irq_handler_c now distinguishes SGI (IPI) from PPI/SPI (timer)."
```

---

## Task 7: Activate Load Balancer

The existing `LoadBalancer` has `idle_balance()` and `periodic_balance()` but they're never called. Wire them into the scheduling loops.

**Files:**
- Modify: `src/process/src/process.cppm` (scheduler_tick, LoadBalancer::select_migration_candidate)
- Modify: `src/process/src/process.cpp` (secondary_cpu_schedule_loop)

**Step 1: Call periodic_balance from scheduler_tick**

In `scheduler_tick()` (around line 1625, the periodic status log block), add load balancing:

```cpp
    // Periodic load balance (every ~100 ticks = ~600ms)
    if (tick_count_ % 100 == 50 && ::moss::kernel::process::g_load_balancer) {
      ::moss::kernel::process::g_load_balancer->periodic_balance(
          get_current_time(), *this);
    }
```

**Step 2: Add idle_balance to secondary_cpu_schedule_loop**

In `process.cpp` `secondary_cpu_schedule_loop`, update the idle branch:

```cpp
        } else {
            // Try to steal work from busy CPUs
            if (g_load_balancer) {
                g_load_balancer->idle_balance(cpu_id, *g_scheduler);
            }
            arch::cpu_idle_once();
        }
```

**Step 3: Implement select_migration_candidate**

The current implementation returns `nullptr`. Replace (line 2163-2167):

```cpp
  [[nodiscard]] Thread *select_migration_candidate(
      u32 cpu, CfsScheduler &scheduler) const noexcept {
    // Pick the task with highest vruntime (least urgent) from the source CPU
    Thread *candidate = scheduler.pick_next_task(cpu);
    if (candidate == nullptr) return nullptr;

    // Don't migrate the only task or user-mode tasks
    if (scheduler.get_cpu_nr_running(cpu) <= 1) return nullptr;
    if (candidate->tid == 1000) return nullptr;  // Don't migrate init

    return candidate;
  }
```

Note: `pick_next_task` returns the leftmost (lowest vruntime). For migration we ideally want the rightmost (highest vruntime) — but that requires a new method. For now, just migrate whatever is available. This is safe because `steal_task` already checks `migration_cost_`.

**Step 4: Add g_load_balancer global pointer**

Check if `g_load_balancer` is already declared/defined. It's used in `kernel.cppm` as `load_balancer_` member. Add a global pointer:

In `process.cppm` (near `extern CfsScheduler *g_scheduler;` line 1867):
```cpp
extern LoadBalancer *g_load_balancer;
```

In `process.cpp` add:
```cpp
LoadBalancer *g_load_balancer = nullptr;
```

In `kernel.cppm` where `load_balancer_` is created (around line 981), add:
```cpp
    ::moss::kernel::process::g_load_balancer = load_balancer_;
```

**Step 5: Build and test**

Run: `cmake --workflow --preset arm64-qemu-debug && ./build/arm64-qemu-debug/run_qemu.sh`
Expected: Tasks should migrate between CPUs. Look for tasks appearing on CPUs they weren't originally assigned to.

**Step 6: Build all 6 presets**

Run: `uv run build.py`
Expected: All 6 presets pass

**Step 7: Commit**

```bash
git add src/process/src/process.cppm src/process/src/process.cpp \
        src/kernel/src/kernel.cppm
git commit -m "[smp][scheduler] activate load balancer with idle and periodic balance

Wire LoadBalancer::periodic_balance into scheduler_tick (every 100 ticks).
Wire idle_balance into secondary CPU schedule loop.
Implement select_migration_candidate to pick tasks for migration.
Add g_load_balancer global pointer set during kernel init."
```

---

## Task 8: 8-Core QEMU Configuration and Final Verification

**Files:**
- Modify: `scripts/run_qemu.py` (line 54, cpu_cores)
- Modify: `src/boot/src/arch/arm64/start_arm64.S` (line 291, CPU ID validation)

**Step 1: Update QEMU CPU count**

In `scripts/run_qemu.py` line 54, change:
```python
    cpu_cores: int = 4  # 从 CMake MOSS_CPU_CORES 变量读取，默认 4
```
to:
```python
    cpu_cores: int = 8  # 从 CMake MOSS_CPU_CORES 变量读取，默认 8
```

**Step 2: Update CPU ID validation in assembly**

In `start_arm64.S` line 291, the check `cmp x19, #8` is already correct for 8 CPUs. No change needed.

**Step 3: Build all 6 presets**

Run: `uv run build.py`
Expected: All 6 presets pass

**Step 4: Full QEMU verification**

Run: `./build/arm64-qemu-debug/run_qemu.sh`

Expected output signals:
1. **8 CPU boot messages**: `[0]`, `[1]`, `[2]`, ... `[7]` from assembly boot
2. **Secondary CPU init**: `SMP1`, `SMP2`, ..., `SMP7` and `RDY1`, ..., `RDY7`
3. **Per-CPU scheduling**: `CPU0: entering scheduling loop`, ..., `CPU7: entering scheduling loop`
4. **Multi-CPU sched_tick**: `[sched_tick]` logs with different `CPU=` values
5. **Task interleaving**: Tasks running on multiple CPUs (TID logs from different CPUs)
6. **Preemption**: `preemptions=` count increasing on multiple CPUs
7. **No deadlocks**: System runs 30+ seconds without hang
8. **Userspace still works**: `"Hello from userspace!"` appears

**Step 5: Run for 30+ seconds to verify stability**

Let QEMU run for 30 seconds. Check:
- No kernel panics
- No stack corruption (no crashes)
- Multiple CPUs showing activity in `[sched_tick]` logs
- `switches=` and `preemptions=` counts increasing

**Step 6: Commit**

```bash
git add scripts/run_qemu.py
git commit -m "[smp][config] set QEMU to 8-core configuration

Update default cpu_cores to 8 for full multi-core testing."
```

**Step 7: Final integration commit (tag)**

```bash
git tag -a smp-v1.0 -m "Full SMP multi-core support: 8 CPUs with spinlocks, per-CPU scheduling, IPI, and load balancing"
```

---

## Troubleshooting Guide

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Deadlock on boot | Lock ordering violation — IRQ handler takes lock already held | Use IrqSpinLock (disables IRQs) for all runqueue locks |
| Secondary CPU crashes | Missing VBAR_EL1 setup | Ensure `msr vbar_el1` is before any IRQ can fire |
| Timer not ticking on CPUs 1-7 | GIC CPU interface not initialized | Verify `init_cpu_interface()` in secondary_cpu_entry |
| Tasks only on CPU 0 | Tasks not distributed to other CPUs | Check `i % 8` in create_test_task |
| Heap corruption | Missing allocator lock | Verify LockGuard in RuntimeHeapAllocator methods |
| Build fails on x86_64/RISC-V | ARM64-specific code without `#ifdef` | Guard ARM64 asm with `#if defined(MOSS_ARCH_ARM64)` |
| WFI never wakes | SEV not sent after SGI | Verify send_sgi in enqueue_task for remote CPUs |
