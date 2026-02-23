// MOSS进程管理器实现
// 支持用户地址空间管理和ELF程序加载

module moss.process;

import moss.abi;

// Assembly/entry symbols from moss.abi
using moss::abi::context_switch;
using moss::abi::entry::early_debug_print;

// 获取当前时间的辅助函数
static moss::u64 get_current_time() noexcept { return moss::kernel::arch::get_timestamp_counter(); }

namespace moss::kernel::process {

// 进程映射项
struct ProcessEntry {
  ProcessId pid;
  Process *process;

  ProcessEntry(ProcessId id, Process *proc) : pid(id), process(proc) {}

  bool operator==(const ProcessEntry &other) const { return pid == other.pid; }
};

// 全局进程管理器实例
ProcessManager *g_process_manager = nullptr;

// 全局调度器实例
CfsScheduler *g_scheduler = nullptr;

// 全局负载均衡器实例
LoadBalancer *g_load_balancer = nullptr;

// Per-CPU data definitions for CfsScheduler static members
containers::PerCpuData<Thread *> CfsScheduler::current_running_tasks_{};

// Per-CPU bootstrap context for context_switch when no previous task exists
containers::PerCpuData<CpuContext> CfsScheduler::bootstrap_contexts_{};

// Per-CPU exit stack for schedule_after_exit (avoids use-after-free on dead task's kernel stack)
containers::PerCpuData<CfsScheduler::ExitStack> CfsScheduler::exit_stacks_{};

// Process类方法实现
KernelResult<ThreadId> Process::create_thread(VirtAddr entry_point, VirtAddr stack_base, usize stack_size) noexcept {
  if (!address_space_) {
    return KernelResult<ThreadId>{ErrorCode::InvalidState};
  }

  ThreadId tid = allocate_thread_id();
  if (tid == INVALID_THREAD_ID) {
    return KernelResult<ThreadId>{ErrorCode::ResourceExhausted};
  }

  // 创建线程对象
  auto *thread = new Thread(tid, pid_);
  if (!thread) {
    return KernelResult<ThreadId>{ErrorCode::OutOfMemory};
  }

  // 初始化CPU上下文
  thread->context.pc = entry_point;
  thread->context.sp = stack_base + stack_size; // 栈向下增长
  thread->context.pstate = 0x0;                 // 用户模式

  // 设置栈信息
  thread->stack_base = stack_base;
  thread->stack_size = stack_size;

  // 设置线程状态
  thread->state = ProcessState::Ready;
  thread->start_time = get_current_time();

  // 添加到线程列表 (使用RcuList的push_front)
  ThreadEntry entry(tid, thread);
  threads_.push_front(entry);

  // 如果是第一个线程，设置为主线程
  if (thread_count_.load(containers::MemoryOrder::Relaxed) == 0) {
    main_thread_id_ = tid;
  }

  (void)thread_count_.fetch_add(1, containers::MemoryOrder::Relaxed);

  // Enqueue new thread into scheduler run queue
  if (g_scheduler != nullptr) {
    u32 cpu = arch::get_current_cpu_id();
    g_scheduler->enqueue_task(thread, cpu);
    log::klog::info("thread enqueued TID={} PID={} cpu={}", static_cast<u32>(tid), pid_, cpu);
  }

  return KernelResult<ThreadId>{tid};
}

Thread *Process::get_thread(ThreadId tid) const noexcept {
  const ThreadEntry *entry = threads_.find_if([tid](const ThreadEntry &e) { return e.tid == tid; });
  return entry ? entry->thread : nullptr;
}

Thread *Process::get_main_thread() const noexcept { return get_thread(main_thread_id_); }

VoidResult Process::set_address_space(unique_ptr<AddressSpace> as) noexcept {
  if (!as) {
    return VoidResult{ErrorCode::InvalidArgument};
  }

  address_space_ = moss::move(as);
  return VoidResult{};
}

void Process::set_state(ProcessState new_state) noexcept { state_ = new_state; }

void Process::update_cpu_time(u64 user_time, u64 kernel_time) noexcept { stats_.cpu_time += user_time + kernel_time; }

void Process::record_context_switch(bool voluntary) noexcept {
  if (voluntary) {
    ++stats_.voluntary_ctxt_switches;
  } else {
    ++stats_.nonvoluntary_ctxt_switches;
  }
}

void Process::record_page_fault(bool major) noexcept {
  if (major) {
    ++stats_.major_faults;
  } else {
    ++stats_.minor_faults;
  }
}

void Process::cleanup_threads() noexcept {
  threads_.for_each([](const ThreadEntry &entry) {
    if (entry.thread) {
      // Free per-thread kernel stack (allocated in sys_fork)
      if (entry.thread->kernel_stack_base != 0 && entry.thread->kernel_stack_size > 0) {
        constexpr usize PAGE_SIZE = 4096;
        usize order = 0;
        usize pages = entry.thread->kernel_stack_size / PAGE_SIZE;
        while ((1U << order) < pages) {
          ++order;
        }
        (void)mm::free_pages(static_cast<PhysAddr>(entry.thread->kernel_stack_base), order);
      }
      // Mark thread as not on any runqueue to avoid dangling reference
      entry.thread->se.rb_on_rq = false;
      delete entry.thread;
    }
  });
  threads_.clear();
}

ThreadId Process::allocate_thread_id() noexcept {
  // 简化实现：从当前线程数量+1开始分配
  static containers::AtomicU32 next_tid{1000};
  return next_tid.fetch_add(1, containers::MemoryOrder::Relaxed);
}

void Process::register_thread(Thread *thread) noexcept {
  if (!thread) {
    return;
  }

  ThreadEntry entry(thread->tid, thread);
  threads_.push_front(entry);

  // Set main thread if this is the first thread
  if (thread_count_.load(containers::MemoryOrder::Relaxed) == 0) {
    main_thread_id_ = thread->tid;
  }

  (void)thread_count_.fetch_add(1, containers::MemoryOrder::Relaxed);
}

ProcessId Process::find_zombie_child(i64 wait_pid) const noexcept {
  ProcessId found = INVALID_PROCESS_ID;

  children_.for_each([&](ProcessId child_pid) {
    if (found != INVALID_PROCESS_ID) {
      return; // Already found one
    }

    Process *child = g_process_manager->find_process(child_pid);
    if (!child) {
      return;
    }

    if (child->state() != ProcessState::Zombie) {
      return;
    }

    if (wait_pid == -1 || static_cast<ProcessId>(wait_pid) == child_pid) {
      found = child_pid;
    }
  });

  return found;
}

// ProcessManager类方法实现
KernelResult<Process *> ProcessManager::create_process(ProcessId parent_pid) noexcept {
  ProcessId new_pid = allocate_pid();
  if (new_pid == INVALID_PROCESS_ID) {
    return KernelResult<Process *>{ErrorCode::ResourceExhausted};
  }

  auto *process = new Process(new_pid, parent_pid);
  if (!process) {
    return KernelResult<Process *>{ErrorCode::OutOfMemory};
  }

  processes_.insert_or_update(new_pid, process);

  // Initialize per-process signal state (handlers table)
  init_signal_state(process);

  record_fork();
  return KernelResult<Process *>{process};
}

VoidResult ProcessManager::terminate_process(ProcessId pid, i32 exit_code) noexcept {
  Process *const *entry = processes_.find(pid);
  if (!entry) {
    return VoidResult{ErrorCode::NotFound};
  }

  Process *process = *entry;
  process->set_state(ProcessState::Terminated);
  process->set_exit_code(exit_code);

  // 从进程表中移除
  (void)processes_.remove(pid);

  // 释放引用（可能会删除进程对象）
  process->release();

  record_exit();
  return VoidResult{};
}

Process *ProcessManager::find_process(ProcessId pid) const noexcept {
  Process *const *entry = processes_.find(pid);
  return entry ? *entry : nullptr;
}

bool ProcessManager::process_exists(ProcessId pid) const noexcept { return find_process(pid) != nullptr; }

u64 ProcessManager::total_processes() const noexcept { return processes_.size(); }

ProcessId ProcessManager::allocate_pid() noexcept { return next_pid_.fetch_add(1, containers::MemoryOrder::Relaxed); }

// 用户地址空间管理扩展功能
namespace user_space {

// ASID allocator: 8-bit (1-255), ASID 0 reserved for kernel.
// On overflow (>255): global TLB flush + reset counter.
static containers::AtomicU32 next_asid{1};

static u16 allocate_asid() noexcept {
  u32 val = next_asid.fetch_add(1, containers::MemoryOrder::Relaxed);
  if (val > 255) {
    // Wrap around: global TLB flush, reset counter
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
    asm volatile("tlbi vmalle1" ::: "memory");
    asm volatile("dsb sy" ::: "memory");
    asm volatile("isb" ::: "memory");
#endif
    next_asid.store(2, containers::MemoryOrder::Relaxed);
    return 1;
  }
  return static_cast<u16>(val);
}

// Create a real user address space with buddy-allocated PGD
KernelResult<unique_ptr<AddressSpace>> create_user_address_space() noexcept {
  // 1. Allocate a physical page for the user PGD (L0 table)
  auto pgd_result = mm::PageTableManager::allocate_page_table_dynamic();
  if (!pgd_result) {
    return KernelResult<unique_ptr<AddressSpace>>{ErrorCode::OutOfMemory};
  }

  // get_physical_address works on the high-half virtual pointer
  PhysAddr pgd_phys = mm::PageTableManager::get_physical_address(*pgd_result);

  // 2. Copy kernel mappings into user PGD.
  auto *kernel_pgd = mm::PageTableManager::get_kernel_pgd();
  auto *user_pgd = *pgd_result;

#if defined(MOSS_ARCH_RISCV)
  // RISC-V Sv39: kernel_pgd contains 1GB gigapage leaf entries directly at
  // indices [0..3] (identity map) and [256..259] (high-half direct map).
  // Copy all block (leaf) entries and the high-half entries to the user PGD.
  // User PGD entries that are not kernel blocks start empty (demand paging).
  if (kernel_pgd && user_pgd) {
    constexpr usize ENTRIES = mm::PageTable::ENTRIES_PER_TABLE;
    for (usize i = 0; i < ENTRIES; i++) {
      if (kernel_pgd->entries[i].is_valid() && kernel_pgd->entries[i].is_block()) {
        user_pgd->entries[i] = kernel_pgd->entries[i];
      }
    }
  }
#else
  // ARM64/x86_64: kernel PGD[0] → L1 table contains both:
  //   - 1GB block mappings (L1[0..3]) for device/RAM identity map
  //   - L1 entries for user addresses (e.g. L1[8] for 0x200000000)
  //
  // Allocate a SEPARATE L1 (PUD) for each user process and copy ONLY
  // the kernel 1GB block descriptors.  User addresses start with empty
  // L1 entries → translation fault → demand paging.
  if (kernel_pgd && user_pgd && kernel_pgd->entries[0].is_valid()) {
    auto pud_result = mm::PageTableManager::allocate_page_table_dynamic();
    if (!pud_result) {
      mm::free_pages(pgd_phys, 0);
      return KernelResult<unique_ptr<AddressSpace>>{ErrorCode::OutOfMemory};
    }
    auto *user_pud = *pud_result;
    auto *kernel_pud = mm::PageTableManager::get_table_from_physical(kernel_pgd->entries[0].get_phys_addr());

    if (kernel_pud) {
      constexpr usize ENTRIES = mm::PageTable::ENTRIES_PER_TABLE;
      for (usize i = 0; i < ENTRIES; i++) {
        if (kernel_pud->entries[i].is_valid() && kernel_pud->entries[i].is_block()) {
          user_pud->entries[i] = kernel_pud->entries[i];
        }
      }
    }

    PhysAddr pud_phys = mm::PageTableManager::get_physical_address(user_pud);
    user_pgd->entries[0].set_table(pud_phys);
  }
#endif

  // 3. Allocate ASID
  u16 asid = allocate_asid();

  // 4. Create AddressSpace object — on success, ~AddressSpace owns pgd_phys.
  //    On failure, we must free the page table hierarchy manually.
  auto address_space = make_unique<AddressSpace>(pgd_phys, asid);
  if (!address_space) {
    mm::PageTableManager::free_user_page_tables(pgd_phys);
    return KernelResult<unique_ptr<AddressSpace>>{ErrorCode::OutOfMemory};
  }

  return KernelResult<unique_ptr<AddressSpace>>{moss::move(address_space)};
}

// 映射内存区域到用户地址空间
VoidResult map_user_memory(AddressSpace *as, VirtAddr vaddr, [[maybe_unused]] PhysAddr paddr, usize size,
                           u32 flags) noexcept {
  if (!as || vaddr == 0 || size == 0) {
    return VoidResult{ErrorCode::InvalidArgument};
  }

  // Add VMA region via AddressSpace helper
  if (!as->add_vma(vaddr, vaddr + size, flags)) {
    return VoidResult{ErrorCode::AlreadyExists};
  }

  // 更新统计信息
  usize pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  (void)as->total_pages.fetch_add(pages, containers::MemoryOrder::Relaxed);

  return VoidResult{};
}

// 分配用户堆内存
KernelResult<VirtAddr> allocate_user_heap(Process *process, usize size) noexcept {
  if (!process || !process->address_space() || size == 0) {
    return KernelResult<VirtAddr>{ErrorCode::InvalidArgument};
  }

  AddressSpace *as = process->address_space();

  // Use brk_current to track the heap watermark.  First call initializes
  // from HEAP_START; subsequent calls extend from the current break.
  if (as->brk_current == 0) {
    as->brk_base = user_layout::HEAP_START;
    as->brk_current = user_layout::HEAP_START;
  }

  VirtAddr heap_addr = as->brk_current;

  // Physical pages are allocated lazily via demand paging (page fault
  // handler).  Here we only register the VMA so the fault handler knows
  // the access is legitimate.
  u32 flags = vma_flags::READ | vma_flags::WRITE | vma_flags::DEMAND_ZERO;
  auto map_result = map_user_memory(as, heap_addr, 0, size, flags);
  if (!map_result) {
    return KernelResult<VirtAddr>{map_result.error()};
  }

  as->brk_current = heap_addr + size;

  return KernelResult<VirtAddr>{heap_addr};
}

} // namespace user_space

// ============================================================================
// Shared Zombie transition — called by sys_exit and terminate_current_user_process
// ============================================================================

[[noreturn]] void do_exit(Thread *cur, Process *proc, i32 exit_code) noexcept {
  namespace log = moss::kernel::logging;

  ProcessId pid = cur->owner_pid;

  // 1. Mark thread terminated BEFORE dequeue (prevents re-enqueue by scheduler_tick)
  cur->state = ProcessState::Terminated;
  if (g_scheduler) {
    g_scheduler->dequeue_task(cur);
  }

  // 2. Restore TTBR0 to kernel PGD BEFORE freeing user page tables
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
  {
    auto *kpgd = mm::PageTableManager::get_kernel_pgd();
    if (kpgd) {
      u64 kpgd_phys = mm::PageTableManager::get_physical_address(kpgd);
      asm volatile("msr ttbr0_el1, %0" ::"r"(kpgd_phys));
      asm volatile("dsb ish" ::: "memory");
      asm volatile("isb" ::: "memory");
    }
  }
#endif

  // 3. Free user page tables (keeps Process object alive for Zombie)
  auto *as = proc->address_space();
  if (as && as->pgd_phys != 0) {
    mm::PageTableManager::free_user_page_tables(as->pgd_phys);
    as->pgd_phys = 0; // prevent double-free in ~Process
  }

  // 4. Reparent children to init (PID 1)
  Process *init_proc = g_process_manager->find_process(1);
  proc->for_each_child([&](ProcessId child_pid) {
    Process *child = g_process_manager->find_process(child_pid);
    if (child) {
      child->set_parent_pid(1);
      if (init_proc) {
        init_proc->add_child(child_pid);
        // If child is already zombie, wake init's waiters
        if (child->state() == ProcessState::Zombie) {
          init_proc->child_exit_wait_queue().wake_up([](void *thread_ptr) {
            auto *t = static_cast<Thread *>(thread_ptr);
            t->state = ProcessState::Ready;
            if (g_scheduler) {
              g_scheduler->enqueue_task(t, t->wake_cpu);
            }
          });
        }
      }
    }
  });

  // 5. Transition to Zombie state (Process stays in process table)
  proc->set_exit_code(exit_code);
  proc->set_state(ProcessState::Zombie);

  // 6. Wake parent's wait queue so waitpid() can collect us.
  //    Use wake_up() which respects exclusive waiters — only wakes
  //    one exclusive waiter + all non-exclusive ones (avoids thundering herd).
  Process *parent = g_process_manager->find_process(proc->parent_pid());
  if (parent) {
    parent->child_exit_wait_queue().wake_up([](void *thread_ptr) {
      auto *t = static_cast<Thread *>(thread_ptr);
      t->state = ProcessState::Ready;
      if (g_scheduler) {
        g_scheduler->enqueue_task(t, t->wake_cpu);
      }
    });
  }

  log::klog::info("do_exit: PID={} -> Zombie, exit_code={}", pid, exit_code);

  // 7. Hand control to scheduler (never returns)
  if (g_scheduler) {
    g_scheduler->schedule_after_exit();
  }

  // Fallback halt (should never be reached)
  while (true) {
    moss::kernel::arch::cpu_halt();
  }
}

// ============================================================================
// Secondary CPU scheduling loop
// ============================================================================

[[noreturn]] void secondary_cpu_schedule_loop(u32 cpu_id) noexcept {
  early_debug_print("[SEC] CPU entering scheduling loop\n");

  // Unified entry point: secondary CPUs use the same scheduling loop
  // as the BSP, with full context_switch and idle balance support.
  g_scheduler->cpu_startup_entry(cpu_id);
}

} // namespace moss::kernel::process
