# wait()/waitpid() + Zombie State + Dynamic VMA Design

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement complete wait()/waitpid() syscalls with proper Zombie state management, blocking wait queues, and migrate VMA/children tracking from fixed arrays to dynamic RcuList.

**Architecture:** Three orthogonal changes composed together: (1) dynamic VMA via RcuList replaces fixed 16-slot array, (2) parent-child tracking via RcuList + WaitQueue enables blocking wait, (3) sys_exit Zombie transition + sys_waitpid reap cycle completes the process lifecycle.

**Tech Stack:** C++26 freestanding, ARM64 (primary), existing `RcuList<T>` container, CFS scheduler integration.

---

## Current State

### Already Implemented
- COW infrastructure: `SW_COW` bit, `clone_user_page_tables()`, `try_cow_fault()`, page refcounting
- `sys_fork()`: full implementation in `syscall_table.cpp` (250+ lines)
- `sys_exit()`: dequeues thread, frees page tables, terminates process
- `ProcessState::Zombie` enum value exists but is unused
- Demand paging: translation fault → VMA lookup → page alloc → map
- `RcuList<T>`: lock-free linked list with `push_front()`, `remove()`, `find_if()`, `for_each()`

### Problems to Solve
1. **VMA fixed array**: `AddressSpace::vmas[16]` limits VMAs per process to 16
2. **No children tracking**: `parent_pid_` stored on child but parent has no child list
3. **No Zombie state**: `sys_exit()` immediately destroys process, no way for parent to collect exit code
4. **No wait queue**: no mechanism to block a thread and wake it on event
5. **No wait/waitpid**: syscalls 12/13 return ENOSYS

---

## Design

### 1. WaitQueue (New Generic Primitive)

A reusable wait queue for blocking threads on events. Future-proof for pipe, signal, futex.

**Location:** `src/containers/src/containers.cppm` (alongside existing primitives)

```cpp
struct WaitQueueEntry {
    Thread* thread;
};

class WaitQueue {
    RcuList<WaitQueueEntry> waiters_;

public:
    // Block current thread: add to waiters, set Blocked, dequeue from scheduler,
    // call schedule(). Returns when another thread calls wake_one/wake_all.
    void sleep(Thread* current);

    // Wake all blocked threads: set Ready, enqueue in scheduler.
    void wake_all();

    // Wake one blocked thread (FIFO order from RcuList head).
    void wake_one();

    bool has_waiters() const;
};
```

**sleep() flow:**
1. `waiters_.push_front({current})`
2. `current->state = Blocked`
3. Dequeue from CFS scheduler
4. `schedule()` — context switches away
5. On wakeup: `waiters_.remove(self)`, return to caller

**wake_all() flow:**
1. `waiters_.for_each()` — for each entry where `thread->state == Blocked`:
   - `thread->state = Ready`
   - `g_scheduler->enqueue(thread)`

**Note:** `WaitQueue` uses `Thread*` (forward-declared from `moss.process`). Since `containers` module cannot import `moss.process`, the `sleep()`/`wake_all()` implementations will be in the `process` or `kernel` module as bridge functions, while the data structure itself (RcuList of thread pointers) lives in `containers`.

### 2. Dynamic VMA Management

Replace `AddressSpace::vmas[MAX_VMAS]` with `RcuList<VmaRegion>`.

**Changes to `AddressSpace`:**

```cpp
struct AddressSpace {
    PhysAddr pgd_phys;
    u16 asid;
    RcuList<VmaRegion> vmas;              // was: VmaRegion vmas[16] + u32 vma_count
    containers::AtomicSize total_pages;
    containers::AtomicSize resident_pages;

    bool add_vma(VirtAddr start, VirtAddr end, u32 flags, VmaType type,
                 const u8* backing = nullptr, usize offset = 0, usize size = 0);
    const VmaRegion* find_vma(VirtAddr addr) const;
    void remove_vma(VirtAddr start);
    void clone_vmas_from(const AddressSpace& src);
};
```

**Implementation mapping:**

| Old (fixed array) | New (RcuList) |
|---|---|
| `vmas[vma_count++] = region` | `vmas.push_front(region)` |
| `for (i=0; i<vma_count; i++) if (vmas[i].contains(addr))` | `vmas.find_if([addr](auto& v) { return v.contains(addr); })` |
| Loop + copy in fork | `src.vmas.for_each([&](auto& v) { dst.vmas.push_front(v); })` |
| `vma_count = 0` in destructor | `RcuList` destructor handles cleanup |

**Overlap check** in `add_vma()`: `vmas.find_if()` checks no existing VMA overlaps the new range before insertion.

### 3. Parent-Child Tracking

**Process class additions:**

```cpp
struct ChildEntry { ProcessId pid; };

class Process {
    // existing
    ProcessId parent_pid_;

    // new
    RcuList<ChildEntry> children_;
    WaitQueue child_exit_wq_;

    void add_child(ProcessId pid);
    void remove_child(ProcessId pid);
    bool has_children() const;
    Process* find_zombie_child(ProcessId wait_pid) const;
        // wait_pid > 0: find specific zombie child
        // wait_pid == -1: find any zombie child
};
```

**Integration points:**
- `sys_fork()`: after creating child, call `parent->add_child(child_pid)`
- `sys_exit()`: reparent children to init (PID 1)
- `sys_waitpid()`: scan `children_` for zombies

### 4. Zombie State Transition (Modified sys_exit)

**Current sys_exit flow:**
```
exit(code) → thread.state=Terminated → dequeue → restore kernel TTBR0
           → terminate_process(pid, code) → remove from table → release() → ~Process() → free page tables
           → schedule_after_exit()
```

**New sys_exit flow:**
```
exit(code) → release user resources:
               - free user page tables (keeps Process object alive)
               - clear address space VMA list
           → process.state = Zombie
           → process.exit_code = code
           → reparent children:
               for each child in children_:
                 child.parent_pid = 1 (init)
                 init_process.add_child(child.pid)
                 if child is Zombie: init_process.child_exit_wq_.wake_all()
           → notify parent:
               parent.child_exit_wq_.wake_all()
           → thread.state = Terminated
           → dequeue from scheduler
           → schedule_after_exit()
```

**Key difference:** Process stays in process table as Zombie. Only `sys_waitpid()` removes it.

### 5. sys_waitpid Implementation

```
sys_waitpid(pid_t wait_pid, int* wstatus_ptr, int options):

  current = get_current_thread()
  proc = get_current_process()

  if !proc->has_children():
    return -ECHILD

  loop:
    // Scan for matching zombie
    zombie = proc->find_zombie_child(wait_pid)

    if zombie found:
      child_pid = zombie->pid()
      exit_code = zombie->exit_code()
      proc->remove_child(child_pid)
      g_process_manager->remove_process(child_pid)  // remove from table
      zombie->release()                               // trigger destructor

      if wstatus_ptr != nullptr:
        // Write exit status to user space (via copy_to_user or direct write)
        *wstatus_ptr = (exit_code & 0xFF) << 8    // WEXITSTATUS encoding

      return child_pid

    // No zombie found
    if wait_pid > 0 and pid not in children_:
      return -ECHILD    // specified child doesn't exist

    if options & WNOHANG:
      return 0          // non-blocking, nothing ready

    // Block: sleep on parent's wait queue
    proc->child_exit_wq_.sleep(current)
    // Woken up — loop back and rescan

    // Check if we still have children (might have been reaped by another thread)
    if !proc->has_children():
      return -ECHILD
```

**Multi-thread safety:** Multiple threads can call `waitpid()` concurrently. All block on the same `child_exit_wq_`. When a child exits, `wake_all()` wakes all waiters. Each waiter rescans the zombie list — first to find a match wins (RcuList removal is atomic). Losers loop back to sleep.

**WNOHANG:** Non-blocking mode. Returns 0 if no zombie available.

**Status encoding:** Linux-compatible `WEXITSTATUS` format: `(exit_code & 0xFF) << 8`.

### 6. Reparenting

When a process exits, its children must be reparented to init (PID 1):

```cpp
// In sys_exit(), before setting Zombie:
Process* init = g_process_manager->find_process(1);

current_process->children_.for_each([&](const ChildEntry& ce) {
    Process* child = g_process_manager->find_process(ce.pid);
    if (child) {
        child->set_parent_pid(1);
        if (init) {
            init->add_child(ce.pid);
            // If child is already Zombie, wake init's waiters
            if (child->state() == ProcessState::Zombie) {
                init->child_exit_wq_.wake_all();
            }
        }
    }
});
```

---

## Files to Modify

| File | Changes |
|------|---------|
| `src/containers/src/containers.cppm` | Add `WaitQueue` data structure (RcuList of thread pointers) |
| `src/process/src/process-types.cppm` | AddressSpace: `RcuList<VmaRegion>` replaces fixed array; Process: add `children_`, `child_exit_wq_`, `add_child()`, `remove_child()`, `find_zombie_child()` |
| `src/process/src/process.cpp` | Implement `add_vma()`/`find_vma()` as RcuList operations; implement `add_child()`/`remove_child()`/`find_zombie_child()`; update `create_user_address_space()` |
| `src/kernel/src/syscall_table.cpp` | Modify `sys_exit()` for Zombie + reparenting + wake; implement `sys_waitpid()` with blocking |
| `src/kernel/src/kernel-syscall_table.cppm` | Mark SYS_WAITPID as implemented |
| `src/kernel/src/kernel_main.cpp` | Update `demand_page_lookup` bridge to use RcuList-based `find_vma()` |
| `src/kernel/src/elf_loader.cpp` | `add_vma()` calls — interface unchanged, internal implementation changes |
| `src/mm/src/page_fault.cpp` | No changes needed — uses `demand_page_lookup` bridge which adapts internally |

## Testing

1. **fork + exit**: Parent forks child, child exits → child becomes Zombie
2. **waitpid specific**: Parent forks, child exits, parent `waitpid(child_pid)` → collects exit code
3. **waitpid -1**: Parent forks 3 children, children exit in order, parent `waitpid(-1)` × 3
4. **waitpid blocking**: Parent calls `waitpid(-1)` before child exits → blocks → child exits → parent wakes and collects
5. **WNOHANG**: Parent calls `waitpid(-1, WNOHANG)` with no exited children → returns 0
6. **reparenting**: Grandparent → Parent → Child; Parent exits → Child reparented to init
7. **multi-thread wait**: Two threads in same process call `waitpid(-1)` → both block → one child exits → one thread collects, other re-blocks
8. **VMA dynamic**: Process with > 16 VMAs (e.g., many mmap regions) works correctly
