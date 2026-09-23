// MOSS进程管理器实现
// 支持用户地址空间管理和ELF程序加载

module moss.process;

import moss.abi;
import moss.hal.mmu;
import moss.vfs;

extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_dispatch_selected() noexcept {}
// The validation image applies real heap pressure at each owned-allocation
// boundary; production keeps the same allocation and rollback path.
extern "C" [[gnu::weak, gnu::noinline]] void
moss_validation_address_space_allocation(bool /*entering*/, bool /*control_block*/,
                                         moss::kernel::usize /*size*/) noexcept {}
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_vm_contended(moss::kernel::PhysAddr /*root*/) noexcept {}
extern "C" [[gnu::weak, gnu::noinline]] void
moss_validation_address_space_retiring(moss::kernel::PhysAddr /*root*/) noexcept {}
// Validation observes an actual child-exit wakeup before wait registers.
extern "C" [[gnu::weak, gnu::noinline]] void
moss_validation_child_exit_notified(moss::kernel::u32 /*child_pid*/, moss::kernel::u32 /*parent_pid*/) noexcept {}

// Assembly/entry symbols from moss.abi
using moss::abi::context_switch;
using moss::abi::entry::early_debug_print;

// 获取当前时间的辅助函数
static moss::u64 get_current_time() noexcept { return moss::kernel::arch::get_timestamp_counter(); }

namespace moss::kernel::process {

AddressSpace::VmTransaction::VmTransaction(AddressSpace &owner) noexcept : owner_(owner) {
  if (!owner_.vm_lock_.try_lock()) {
    // Observe real contention without changing the production lock algorithm.
    moss_validation_vm_contended(owner_.pgd_phys);
    owner_.vm_lock_.lock();
  }
}

AddressSpace::VmTransaction::~VmTransaction() noexcept { owner_.vm_lock_.unlock(); }

bool AddressSpace::resolve_fault(VirtAddr address, mm::UserFaultAccess access, bool cow_only,
                                 VirtAddr *grown_stack) noexcept {
  auto transaction = lock_vm();
  return resolve_fault_locked(address, access, cow_only, grown_stack);
}

bool AddressSpace::resolve_fault_locked(VirtAddr address, mm::UserFaultAccess access, bool cow_only,
                                        VirtAddr *grown_stack) noexcept {
  static_assert(static_cast<u32>(mm::UserFaultAccess::Read) == vma_flags::READ);
  static_assert(static_cast<u32>(mm::UserFaultAccess::Write) == vma_flags::WRITE);
  static_assert(static_cast<u32>(mm::UserFaultAccess::Execute) == vma_flags::EXEC);
  if (grown_stack) {
    *grown_stack = 0;
  }
  auto vma = find_vma(address);
  if (!vma && !cow_only) {
    const VirtAddr new_start = address & ~(static_cast<VirtAddr>(PAGE_SIZE) - 1);
    const VirtAddr limit = user_layout::STACK_TOP - user_layout::STACK_MAX;
    if (address < limit || !valid_vma_range(new_start, user_layout::STACK_TOP, VmaType::STACK) ||
        !vmas.update_if([&](const VmaRegion &v) { return v.type == VmaType::STACK && address < v.start_addr; },
                        [&](VmaRegion &v) { v.start_addr = new_start; })) {
      return false;
    }
    if (grown_stack) {
      *grown_stack = new_start;
    }
    vma = find_vma(address);
  }
  if (!vma) {
    return false;
  }
  const mm::UserFaultContext context{.root = pgd_phys,
                                     .flags = vma->flags,
                                     .start = vma->start_addr,
                                     .backing = vma->backing_data,
                                     .backing_offset = vma->backing_offset,
                                     .backing_size = vma->backing_size,
                                     .shared_page = vma->shared_page};
  return cow_only ? mm::resolve_user_cow_fault(context, address)
                  : mm::resolve_user_demand_fault(context, address, access);
}

Thread *Thread::try_create(ThreadId id, ProcessId pid) noexcept {
  auto *storage = moss::abi::bridge::moss_heap_allocate(sizeof(Thread), alignof(Thread));
  return storage ? new (storage) Thread(id, pid) : nullptr;
}

Thread::~Thread() {
  if (kernel_stack_base != 0 && kernel_stack_size > 0) {
    usize order = 0;
    const usize pages = kernel_stack_size / PAGE_SIZE;
    while ((1U << order) < pages) {
      ++order;
    }
    (void)mm::free_pages(static_cast<PhysAddr>(kernel_stack_base), order);
  }
}

VoidResult Thread::allocate_kernel_stack() noexcept {
  if (kernel_stack_base || kernel_stack_size) {
    return VoidResult{ErrorCode::InvalidState};
  }
  // 2 阶分配得到 4 个 4 KiB 页，即固定 16 KiB 异常/系统调用栈；
  // 实测栈深度的选值依据尚未记录，扩大该值会增加每线程常驻内存。
  constexpr usize order = 2;
  auto block = mm::allocate_pages(order);
  if (!block) {
    return VoidResult{ErrorCode::OutOfMemory};
  }
  constexpr usize size = PAGE_SIZE << order;
  const auto base = static_cast<VirtAddr>(*block); // Identity-mapped stack.
  auto *words = reinterpret_cast<u64 *>(base);
  // Initialize the entire recycled region, not just the initial trap frame.
  for (usize i = 0; i < size / sizeof(u64); ++i) {
    words[i] = 0;
  }
  kernel_stack_base = base;
  kernel_stack_size = size;
  return {};
}

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

extern "C" bool moss_io_wait_interrupted() noexcept {
  auto *thread = current_thread();
  if (!thread || !signal_pending(thread)) {
    return false;
  }
  const u64 pending = thread->pending_signals.load() & (~thread->signal_mask.load() | sig::UNCATCHABLE_MASK);
  if (pending & sig::UNCATCHABLE_MASK) {
    return true;
  }
  auto proc = g_process_manager ? g_process_manager->find_process(thread->owner_pid) : shared_ptr<Process>{};
  auto *state = proc ? get_signal_state(proc.get()) : nullptr;
  for (u32 signo = 1; signo < sig::NSIG; ++signo) {
    if (!(pending & sig::sigmask(signo))) {
      continue;
    }
    const auto handler = state ? state->actions[signo].handler : SIG_DFL;
    if (handler == SIG_IGN) {
      continue;
    }
    const auto action = default_action(signo);
    if (handler != SIG_DFL || (action != SigDefault::Ignore && action != SigDefault::Continue)) {
      return true;
    }
  }
  return false;
}

extern "C" void *moss_prepare_io_wait() noexcept {
  auto *thread = g_scheduler ? g_scheduler->prepare_sleep() : nullptr;
  // Publish Sleeping before checking pending signals. Earlier signals are
  // caught here; later senders participate in the scheduler's sleep handoff.
  if (thread && moss_io_wait_interrupted()) {
    g_scheduler->task_wakeup(thread, thread->wake_cpu);
  }
  return thread;
}

extern "C" void moss_commit_io_wait() noexcept { g_scheduler->commit_sleep(); }

extern "C" void moss_wake_io_waiter(void *opaque) noexcept {
  auto *thread = static_cast<Thread *>(opaque);
  if (g_scheduler && thread) {
    g_scheduler->task_wakeup(thread, thread->wake_cpu);
  }
}

extern "C" void moss_signal_broken_pipe() noexcept { (void)send_signal(current_thread(), sig::SIGPIPE); }

// 全局负载均衡器实例
LoadBalancer *g_load_balancer = nullptr;

// Per-CPU data definitions for CfsScheduler static members
containers::PerCpuData<Thread *> CfsScheduler::current_running_tasks_{};

// Per-CPU bootstrap context for context_switch when no previous task exists
containers::PerCpuData<CpuContext> CfsScheduler::bootstrap_contexts_{};
// Slot storage lasts for the kernel's lifetime; each owner is explicitly reset
// on a root switch. Suppress only exit-time destruction: this freestanding
// kernel has no __cxa_atexit, and must not release a CPU's live root at shutdown.
[[clang::no_destroy]] containers::PerCpuData<shared_ptr<AddressSpace>> CfsScheduler::active_address_spaces_{};

void CfsScheduler::use_address_space(shared_ptr<AddressSpace> next) noexcept {
  const bool interrupts = arch::interrupts_enabled();
  arch::disable_interrupts();
  auto *kernel = mm::PageTableManager::get_kernel_pgd();
  if (!next && !kernel) {
    arch::kernel_panic("kernel page table unavailable");
  }
  const PhysAddr physical = next ? next->pgd_phys : mm::PageTableManager::get_physical_address(kernel);
  if (!physical) {
    arch::kernel_panic("address-space root unavailable");
  }
  [[maybe_unused]] const u16 asid = next ? next->asid : 0;
  // The parameter retains the outgoing owner through the hardware write.
  // Publish the incoming pin first, with local IRQs/preemption excluded; no
  // Process lock or VM lock may be held across the subsequent final release.
  active_address_spaces_.get_local().swap(next);
#if defined(MOSS_ARCH_ARM64)
  // TTBR0_EL1 stores the ASID above the table PA, starting at bit 48.
  const u64 root = physical | (static_cast<u64>(asid) << 48);
  asm volatile("msr ttbr0_el1, %0; dsb ish; isb" ::"r"(root) : "memory");
#elif defined(MOSS_ARCH_X64)
  asm volatile("mov %0, %%cr3" ::"r"(physical) : "memory");
#elif defined(MOSS_ARCH_RISCV64)
  const u64 root = hal::mmu::make_satp_value(physical, asid);
  asm volatile("csrw satp, %0; sfence.vma" ::"r"(root) : "memory");
#endif
  next.reset();
  if (interrupts) {
    arch::enable_interrupts();
  }
}

shared_ptr<AddressSpace> CfsScheduler::active_address_space() noexcept {
  const bool interrupts = arch::interrupts_enabled();
  arch::disable_interrupts();
  auto held = active_address_spaces_.get_local();
  if (interrupts) {
    arch::enable_interrupts();
  }
  return held;
}

// Legacy storage: exit restores bootstrap_contexts_, not these old exit stacks.
containers::PerCpuData<CfsScheduler::ExitStack> CfsScheduler::exit_stacks_{};

// Process类方法实现
void Process::cleanup_files() noexcept {
  auto *table = static_cast<vfs::FdTable *>(fd_table_);
  fd_table_ = nullptr;
  if (table) {
    table->close_all();
    delete table;
  }
}

KernelResult<ThreadId> Process::create_thread(VirtAddr entry_point, VirtAddr stack_base, usize stack_size) noexcept {
  if (!address_space()) {
    return KernelResult<ThreadId>{ErrorCode::InvalidState};
  }

  ThreadId tid = allocate_thread_id();
  if (tid == INVALID_THREAD_ID) {
    return KernelResult<ThreadId>{ErrorCode::ResourceExhausted};
  }

  // 创建线程对象
  auto *thread = Thread::try_create(tid, pid_);
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

  auto registered = register_thread(thread);
  if (!registered) {
    delete thread;
    return KernelResult<ThreadId>{registered.error()};
  }

  // Enqueue new thread into scheduler run queue
  if (g_scheduler != nullptr) {
    u32 cpu = arch::get_current_cpu_id();
    g_scheduler->enqueue_task(thread, cpu);
    log::klog::info("thread enqueued TID={} PID={} cpu={}", static_cast<u32>(tid), pid_, cpu);
  }

  return KernelResult<ThreadId>{tid};
}

Thread *Process::get_thread(ThreadId tid) const noexcept {
  auto entry = threads_.find_if([tid](const ThreadEntry &e) { return e.tid == tid; });
  return entry ? entry->thread : nullptr;
}

Thread *Process::get_main_thread() const noexcept { return get_thread(main_thread_id_); }

VoidResult Process::set_address_space(shared_ptr<AddressSpace> as) noexcept {
  if (!as) {
    return VoidResult{ErrorCode::InvalidArgument};
  }

  {
    containers::LockGuard<containers::IrqSpinLock> guard(address_space_lock_);
    address_space_.swap(as);
  }
  // The retired owner may free an entire page-table tree. Do not hold the
  // publication lock across destruction or block readers of the new version.
  // CPUs retain their installed versions separately; publishing here does not
  // switch another CPU's root or finish shared-exec thread coordination.
  return VoidResult{};
}

void Process::clear_address_space() noexcept {
  shared_ptr<AddressSpace> retired;
  {
    containers::LockGuard<containers::IrqSpinLock> guard(address_space_lock_);
    address_space_.swap(retired);
  }
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
      // Mark thread as not on any runqueue to avoid dangling reference
      entry.thread->se.rb_on_rq = false;
      delete entry.thread;
    }
  });
  threads_.clear();
}

ThreadId Process::allocate_thread_id() noexcept {
  // 独立的全局原子 32 位计数器，从 1000 起步；不是“当前线程数+1”。起始值的
  // 依据尚未记录，调用方不能靠它推断 init TID（idle 也会消耗 ID）。
  static containers::AtomicU32 next_tid{1000};
  return next_tid.fetch_add(1, containers::MemoryOrder::Relaxed);
}

VoidResult Process::register_thread(Thread *thread) noexcept {
  if (!thread) {
    return VoidResult{ErrorCode::InvalidArgument};
  }

  if (exec_in_progress_.load()) {
    return VoidResult{ErrorCode::InvalidState};
  }
  // Reserve the count before publishing the thread. Exec either observes the
  // pending peer and refuses, or wins the gate and makes us withdraw it.
  const u32 previous_count = thread_count_.fetch_add(1);
  if (exec_in_progress_.load()) {
    (void)thread_count_.fetch_sub(1);
    return VoidResult{ErrorCode::InvalidState};
  }

  ThreadEntry entry(thread->tid, thread);
  if (!threads_.try_push_front(entry)) {
    (void)thread_count_.fetch_sub(1);
    return VoidResult{ErrorCode::OutOfMemory};
  }

  // Set main thread if this is the first thread
  if (previous_count == 0) {
    main_thread_id_ = thread->tid;
  }

  return {};
}

bool Process::try_begin_exec() noexcept {
  if (thread_count_.load() != 1) {
    return false;
  }
  bool expected = false;
  if (!exec_in_progress_.compare_exchange_strong(expected, true)) {
    return false;
  }
  // A registrar may have reserved a count between the first check and gate.
  // It either backs out after observing the gate or forces this retry to fail.
  if (thread_count_.load() == 1) {
    return true;
  }
  exec_in_progress_.store(false);
  return false;
}

void Process::finish_exec() noexcept { exec_in_progress_.store(false); }

ProcessId Process::find_zombie_child(i64 wait_pid) const noexcept {
  ProcessId found = INVALID_PROCESS_ID;

  children_.for_each([&](ProcessId child_pid) {
    if (found != INVALID_PROCESS_ID) {
      return; // Already found one
    }

    auto child = g_process_manager->find_process(child_pid);
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
KernelResult<shared_ptr<Process>> ProcessManager::create_process(ProcessId parent_pid) noexcept {
  ProcessId new_pid = allocate_pid();
  if (new_pid == INVALID_PROCESS_ID) {
    return KernelResult<shared_ptr<Process>>{ErrorCode::ResourceExhausted};
  }

  auto process = shared_ptr<Process>::try_make(moss::abi::bridge::moss_heap_allocate, new_pid, parent_pid);
  if (!process) {
    return KernelResult<shared_ptr<Process>>{ErrorCode::OutOfMemory};
  }

  if (!processes_.try_insert_or_update(new_pid, process)) {
    return KernelResult<shared_ptr<Process>>{ErrorCode::OutOfMemory};
  }

  record_fork();
  return KernelResult<shared_ptr<Process>>{process};
}

VoidResult ProcessManager::terminate_process(ProcessId pid, i32 exit_code) noexcept {
  auto entry = processes_.extract(pid);
  if (!entry) {
    return VoidResult{ErrorCode::NotFound};
  }

  auto process = *entry;
  process->set_state(ProcessState::Terminated);
  process->set_exit_code(exit_code);

  record_exit();
  return VoidResult{};
}

shared_ptr<Process> ProcessManager::find_process(ProcessId pid) const noexcept {
  auto entry = processes_.find(pid);
  return entry ? *entry : shared_ptr<Process>{};
}

bool ProcessManager::process_exists(ProcessId pid) const noexcept { return static_cast<bool>(find_process(pid)); }

u64 ProcessManager::total_processes() const noexcept { return processes_.size(); }

ProcessId ProcessManager::allocate_pid() noexcept { return next_pid_.fetch_add(1, containers::MemoryOrder::Relaxed); }

// 用户地址空间管理扩展功能
namespace user_space {

// ponytail: 255 simultaneous address-space leases. Use per-CPU generations
// before supporting more concurrent spaces; never recycle a live owner's tag.
static containers::IrqSpinLock asid_lock;
static bool asid_used[256]{true}; // tag 0 belongs to the kernel

static u16 allocate_asid() noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(asid_lock);
  for (u16 tag = 1; tag < 256; ++tag) {
    if (!asid_used[tag]) {
      asid_used[tag] = true;
      return tag;
    }
  }
  return 0;
}

void release_asid(u16 tag) noexcept {
  if (tag == 0 || tag >= 256) {
    return;
  }
  containers::LockGuard<containers::IrqSpinLock> guard(asid_lock);
#if defined(MOSS_ARCH_ARM64)
  // ARM64 TLBI 的 ASID 位于操作数 [63:48]，故左移 48，而不是页号位移。
  // Finish invalidation on every CPU before another space can take this tag.
  asm volatile("dsb ishst; tlbi aside1is, %0; dsb ish; isb" : : "r"(static_cast<u64>(tag) << 48) : "memory");
#endif
  // RV64 flushes on every SATP switch; x86 does not enable PCID.
  asid_used[tag] = false;
}

// Page-table ownership and architecture layout belong to the MM module.
KernelResult<shared_ptr<AddressSpace>> create_user_address_space() noexcept {
  auto tables = mm::PageTableManager::create_user_page_tables();
  if (!tables) {
    return KernelResult<shared_ptr<AddressSpace>>{tables.error()};
  }
  const u16 asid = allocate_asid();
  if (asid == 0) {
    mm::PageTableManager::free_user_page_tables(*tables);
    return KernelResult<shared_ptr<AddressSpace>>{ErrorCode::ResourceExhausted};
  }
  // A zero root/tag owns no MM resources. Keep that inert state until both
  // fallible SharedPtr allocations succeed: try_make destroys the object when
  // its control block fails, so publishing tables earlier would double-free
  // them in the explicit rollback below.
  bool control_block = false;
  auto address_space = shared_ptr<AddressSpace>::try_make(
      [&](usize size, usize alignment) -> void * {
        moss_validation_address_space_allocation(true, control_block, size);
        auto storage = mm::RuntimeHeapAllocator::allocate_aligned(size, alignment);
        moss_validation_address_space_allocation(false, control_block, size);
        control_block = true; // try_make allocates the object, then its control block.
        return storage ? *storage : nullptr;
      },
      PhysAddr{0}, u16{0});
  if (!address_space) {
    release_asid(asid);
    mm::PageTableManager::free_user_page_tables(*tables);
    return KernelResult<shared_ptr<AddressSpace>>{ErrorCode::OutOfMemory};
  }
  address_space->pgd_phys = *tables;
  address_space->asid = asid;
  return KernelResult<shared_ptr<AddressSpace>>{moss::move(address_space)};
}

// 映射内存区域到用户地址空间
VoidResult map_user_memory(AddressSpace *as, VirtAddr vaddr, [[maybe_unused]] PhysAddr paddr, usize size,
                           u32 flags) noexcept {
  if (!as || size == 0 || !mm::PageTableManager::is_user_range(vaddr, size)) {
    return VoidResult{ErrorCode::InvalidArgument};
  }
  auto transaction = as->lock_vm();

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
  auto as = process ? process->address_space() : shared_ptr<AddressSpace>{};
  if (!as || size == 0) {
    return KernelResult<VirtAddr>{ErrorCode::InvalidArgument};
  }
  auto transaction = as->lock_vm();

  constexpr u32 flags = vma_flags::READ | vma_flags::WRITE | vma_flags::DEMAND_ZERO;
  // Use brk_current to track the heap watermark. The empty VMA represents the
  // initial break without authorizing the old HEAP_INIT reservation window.
  if (as->brk_current == 0) {
    as->brk_base = user_layout::HEAP_START;
    as->brk_current = user_layout::HEAP_START;
    if (!as->add_vma(as->brk_base, as->brk_base, flags, VmaType::HEAP)) {
      return KernelResult<VirtAddr>{ErrorCode::AlreadyExists};
    }
  }

  VirtAddr heap_addr = as->brk_current;
  if (size > USER_MAX - heap_addr || !mm::PageTableManager::is_user_range(heap_addr, size)) {
    return KernelResult<VirtAddr>{ErrorCode::InvalidArgument};
  }
  const VirtAddr page_mask = static_cast<VirtAddr>(PAGE_SIZE) - 1;
  const VirtAddr old_end = (heap_addr + page_mask) & ~page_mask;
  const VirtAddr new_break = heap_addr + size;
  const VirtAddr new_end = (new_break + page_mask) & ~page_mask;
  if (old_end != new_end && !as->resize_vma(as->brk_base, old_end, new_end, VmaType::HEAP, [](const VmaRegion &) {})) {
    return KernelResult<VirtAddr>{ErrorCode::AlreadyExists};
  }

  // Physical pages remain lazy; total_pages records newly authorized pages.
  (void)as->total_pages.fetch_add((new_end - old_end) / PAGE_SIZE, containers::MemoryOrder::Relaxed);
  as->brk_current = new_break;

  return KernelResult<VirtAddr>{heap_addr};
}

} // namespace user_space

// ============================================================================
// Shared Zombie transition — called by sys_exit and terminate_current_user_process
// ============================================================================

[[noreturn]] void do_exit(Thread *cur, shared_ptr<Process> proc, i32 exit_code) noexcept {
  namespace log = moss::kernel::logging;

  ProcessId pid = cur->owner_pid;

  if (proc->is_initial_supervisor()) {
    // Its authority and service graph cannot be rebuilt from a zombie or a
    // new PID. Reset before ordinary process teardown can leave services
    // running without their initial supervisor.
    log::klog::error("initial system supervisor exited: code={}; resetting system", exit_code);
    arch::system_reset();
  }

  // 1. Mark thread terminated BEFORE dequeue (prevents re-enqueue by scheduler_tick)
  cur->state = ProcessState::Terminated;
  if (g_scheduler) {
    g_scheduler->dequeue_task(cur);
  }

  // Release descriptors before publishing exit: other processes must observe
  // pipe EOF and recover file-pool capacity without first reaping this zombie.
  proc->cleanup_files();
  proc->capabilities().clear();

  // 2. Restore page table base to kernel PGD BEFORE freeing user page tables
  CfsScheduler::use_kernel_address_space();

  // 3. Detach before publishing Zombie, preserving any in-flight reader's
  // tables and ASID until its final release. Never free through a borrowed
  // pointer or leave a local owning snapshot on this non-returning exit stack.
  proc->clear_address_space();

  // 4. Reparent children to init (PID 1)
  auto init_proc = g_process_manager->find_process(1);
  proc->for_each_child([&](ProcessId child_pid) {
    auto child = g_process_manager->find_process(child_pid);
    if (child) {
      child->set_parent_pid(1);
      if (init_proc) {
        init_proc->add_child(child_pid);
        // If child is already zombie, wake init's waiters
        if (child->state() == ProcessState::Zombie) {
          send_signal(init_proc->get_main_thread(), sig::SIGCHLD);
          init_proc->child_exit_wait_queue().wake_up([](void *thread_ptr) {
            auto *t = static_cast<Thread *>(thread_ptr);
            if (g_scheduler) {
              g_scheduler->task_wakeup(t, t->wake_cpu);
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
  auto parent = g_process_manager->find_process(proc->parent_pid());
  if (parent) {
    // Publish child status before SIGCHLD and wakeup: the returning waitpid
    // checkpoint must observe both the zombie and its notification.
    send_signal(parent->get_main_thread(), sig::SIGCHLD);
    log::klog::info("do_exit: PID={} waking parent PID={}", pid, proc->parent_pid());
    parent->child_exit_wait_queue().wake_up([](void *thread_ptr) {
      auto *t = static_cast<Thread *>(thread_ptr);
      log::klog::info("do_exit: wake waiter TID={} state->{}", static_cast<u32>(t->tid), "Ready");
      if (g_scheduler) {
        g_scheduler->task_wakeup(t, t->wake_cpu);
      }
    });
    moss_validation_child_exit_notified(pid, proc->parent_pid());
  } else {
    log::klog::error("do_exit: PID={} parent PID={} NOT FOUND", pid, proc->parent_pid());
  }

  log::klog::info("do_exit: PID={} -> Zombie, exit_code={}", pid, exit_code);

  // 7. Hand control to scheduler (never returns)
  parent.reset();
  init_proc.reset();
  if (g_scheduler) {
    g_scheduler->schedule_after_exit(moss::move(proc));
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
