# Process Lifecycle & Real Scheduler Design

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix the six broken links in the process lifecycle chain so that user processes can cleanly exit and the scheduler can preemptively switch between kernel tasks.

**Architecture:** Bottom-up repair of the existing Process/Thread/CFS infrastructure. No new modules — only filling in gaps in process.cppm, kernel.cppm, syscall_table.cpp, and start_arm64.S.

**Tech Stack:** C++26 modules, ARM64 assembly (context_switch.S, start_arm64.S), CFS scheduler.

---

## Current State — Six Broken Links

| # | Gap | File | Impact |
|---|-----|------|--------|
| 1 | `create_init_process()` bypasses ProcessManager | kernel.cppm | PID=1 never in process table |
| 2 | `current_thread()` hardcoded to nullptr | process.cppm:450 | Can't identify running task |
| 3 | `terminate_process()` doesn't dequeue from scheduler | process.cpp:190 | Zombie threads in runqueue |
| 4 | sys_exit returns to user via eret | syscall_table.cpp + start_arm64.S | Dead process resumes |
| 5 | `scheduler_tick()` is a no-op | process.cppm:1630 | No preemption |
| 6 | `context_switch()` asm never called | context_switch.S | No kernel task switching |

## Design — Five Steps

### Step 1: Process Registration

**File:** `src/kernel/src/kernel.cppm` — `create_init_process()`

Replace the raw `new Thread(1000, 1)` with proper ProcessManager calls:

```
process_manager_->create_process(0)        // creates Process(PID=1, parent=0)
process->allocate_thread_id()              // returns TID=1000
new Thread(tid, pid)                       // creates Thread(1000, 1)
process->add_thread(thread)                // registers in Process::threads_
```

Then set up the thread's CpuContext (entry point, stack, pstate=0 for EL0t) and enqueue via scheduler as before. The key difference: PID=1 is now in the ProcessManager hash map.

### Step 2: Current Task Tracking

**File:** `src/process/src/process.cppm` — `current_thread()` and `current_process()`

`CfsScheduler::get_current_task()` already maintains a per-CPU `current_running_tasks_[]` array and is updated by `set_current_task()` in context_switch_to_task(). Wire the convenience functions:

```cpp
[[nodiscard]] inline Thread *current_thread() noexcept {
    return CfsScheduler::get_current_task();
}
[[nodiscard]] inline Process *current_process() noexcept {
    Thread *t = current_thread();
    if (!t || !g_process_manager) return nullptr;
    return g_process_manager->find_process(t->owner_pid);
}
```

### Step 3: Process Exit — Complete Chain

**Files:** `syscall_table.cpp`, `process.cpp`, `process.cppm`

#### 3a. Fix sys_exit to use real current task

Replace hardcoded `current_pid = 1` with:
```cpp
Thread *cur = CfsScheduler::get_current_task();
ProcessId current_pid = cur ? cur->owner_pid : INVALID_PROCESS_ID;
```

#### 3b. Enhance terminate_process to dequeue thread

After marking Process as terminated, iterate its threads and dequeue each from the scheduler:
```cpp
process->for_each_thread([&](Thread* thread) {
    scheduler_->dequeue_task(thread);
    thread->state = ProcessState::Terminated;
});
```

#### 3c. sys_exit must be [[noreturn]]

After termination, sys_exit must NOT return to the assembly eret path. Instead:
```cpp
// Pick next task and switch to it
g_scheduler->schedule_after_exit();
// Never returns — either eret to user or ret to kernel context
```

#### 3d. New `schedule_after_exit()` in CfsScheduler

```cpp
[[noreturn]] void schedule_after_exit() noexcept {
    u32 cpu = get_current_cpu_id();
    Thread *next = pick_next_task(cpu);
    if (next) {
        dequeue_task(next);
        context_switch_to_task(next);
        // If kernel task returns, re-enqueue and loop
        enqueue_task(next, cpu);
    }
    // No tasks: enter idle
    while (true) { arch::cpu_idle_once(); }
}
```

### Step 4: Real Scheduler Tick

**File:** `src/process/src/process.cppm` — `scheduler_tick()`

Replace the no-op with actual scheduling logic:

```cpp
void scheduler_tick() noexcept {
    tick_count_++;
    u32 cpu = get_current_cpu_id();
    Thread *curr = get_current_task();

    if (curr == nullptr) return;  // idle, nothing to preempt

    // Update vruntime for current task
    u64 now = get_current_time();
    u64 delta = (now > curr->se.last_update) ? (now - curr->se.last_update) : 1000;
    update_current(curr, delta);

    // Check if preemption needed
    CfsRunqueue &rq = runqueues_.get_cpu(cpu);
    if (rq.nr_running() > 0 && rq.should_preempt(curr)) {
        // Re-enqueue current, pick next
        enqueue_task(curr, cpu);
        Thread *next = pick_next_task(cpu);
        if (next && next != curr) {
            dequeue_task(next);
            context_switch_to_task(next);
            // If kernel task returned, continue
        }
    }

    // Periodic status log
    if (tick_count_ % 500 == 0) { /* log */ }
}
```

**Note:** For user tasks, `context_switch_to_task` calls `switch_to_user` which does `eret` and never returns. For kernel tasks, it calls `context_switch()` which saves/restores register state and returns to the caller (the tick handler), which then returns from the IRQ. This is the standard preemptive scheduling model.

### Step 5: Kernel Context Switch Integration

**Files:** `process.cppm`, possibly `context_switch.S`

#### 5a. Declare context_switch in C++

Add to global module fragment:
```cpp
extern "C" void context_switch(void* prev_context, void* next_context);
```

#### 5b. Use it in context_switch_to_task

Replace the direct `test_task_entry()` call for kernel tasks:
```cpp
void context_switch_to_task(Thread *task) noexcept {
    Thread *prev = get_current_task();
    set_current_task(task);
    task->state = ProcessState::Running;
    record_context_switch();

    if (task->tid == 1000) {
        switch_to_user(&task->context, task->stack_base + task->stack_size - 16);
    } else {
        // Kernel-to-kernel context switch
        if (prev != nullptr && prev != task) {
            context_switch(&prev->context, &task->context);
        }
    }
}
```

#### 5c. Initialize kernel task contexts properly

Test tasks (TID 1001-1020) need their CpuContext.pc set to `test_task_entry` and initial SP set to their stack top. The `context_switch` assembly will `ret` to the address saved in the context's LR (x30), which for a fresh task is the entry point.

## Verification

| Scenario | Expected Output |
|----------|----------------|
| QEMU ARM64 boot | "Hello from userspace!" → clean exit → scheduler picks next kernel task |
| sys_exit return | No eret back to user; scheduler takes over |
| Timer tick preemption | `[sched_tick]` shows switches > 0 and multiple TIDs running |
| All 6 presets | Zero errors under -Weverything -Werror |

## Files Changed

| File | Changes |
|------|---------|
| `src/kernel/src/kernel.cppm` | create_init_process via ProcessManager |
| `src/process/src/process.cppm` | current_thread(), scheduler_tick(), schedule_after_exit(), context_switch_to_task() |
| `src/process/src/process.cpp` | terminate_process dequeues threads |
| `src/kernel/src/syscall_table.cpp` | sys_exit uses current_thread(), [[noreturn]] |
| `src/boot/src/arch/arm64/start_arm64.S` | Possible: mark sys_exit as noreturn in dispatch |
