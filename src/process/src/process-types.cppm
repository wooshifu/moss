// MOSS Process Module - Partition: types
// Base process types, Thread, Process, ProcessManager, user_space

export module moss.process:types;

import moss.std;
import moss.types;
import moss.result;
import moss.smart_ptr;
import moss.arch;
import moss.containers;
import moss.mm;
import moss.logging;

// ============================================================================
// process.hpp - Base process types
// ============================================================================
export namespace moss::kernel::process {

namespace log = moss::kernel::logging;

// Forward declarations
struct Thread;

// Thread entry for RcuList storage
struct ThreadEntry {
  ThreadId tid;
  Thread *thread;

  ThreadEntry(ThreadId id, Thread *thr) : tid(id), thread(thr) {}

  bool operator==(const ThreadEntry &other) const { return tid == other.tid; }
};

// Use kernel smart pointers
using moss::kernel::make_unique;
using moss::kernel::unique_ptr;

// Process states — modeled after Linux task_struct states.
//
// Sleeping:  TASK_INTERRUPTIBLE   — woken by signal or event (waitpid, read, nanosleep)
// DiskSleep: TASK_UNINTERRUPTIBLE — woken only by event (page I/O, critical sections)
// Blocked:   Legacy alias — code that doesn't care about signal-interruptibility
//            should use Sleeping (the safe default for most waits).
enum class ProcessState : u8 {
  Created = 0,
  Ready = 1,
  Running = 2,
  Sleeping = 3,  // TASK_INTERRUPTIBLE: signals can wake this task
  DiskSleep = 4, // TASK_UNINTERRUPTIBLE: only events wake this task
  Terminated = 5,
  Zombie = 6,
  Stopped = 7, // SIGSTOP/SIGTSTP: task suspended, resumed by SIGCONT
};

// Helper: is the task in any blocked/sleeping state?
constexpr bool is_blocked_state(ProcessState s) noexcept {
  return s == ProcessState::Sleeping || s == ProcessState::DiskSleep || s == ProcessState::Stopped;
}

// Helper: can signals wake this task?
constexpr bool is_signal_wakeable(ProcessState s) noexcept { return s == ProcessState::Sleeping; }

// Scheduling classes (broad category: CFS vs RT vs idle)
enum class SchedClass : u8 { Normal = 0, RealTime = 1, Idle = 2, Batch = 3 };

// Scheduling policy — selects the dispatch algorithm within a class.
// Maps to Linux SCHED_* constants used by sched_setscheduler().
enum class SchedPolicy : u8 {
  Normal = 0, // SCHED_NORMAL (CFS)
  Fifo = 1,   // SCHED_FIFO   (RT, runs until block/yield/preempted by higher prio)
  RR = 2,     // SCHED_RR     (RT, round-robin within same priority)
  Batch = 3,  // SCHED_BATCH  (CFS, batch-optimized)
  Idle = 5,   // SCHED_IDLE   (lowest priority)
};

// Map SchedPolicy → SchedClass for dispatch routing.
constexpr SchedClass policy_to_class(SchedPolicy policy) noexcept {
  switch (policy) {
  case SchedPolicy::Fifo:
  case SchedPolicy::RR:
    return SchedClass::RealTime;
  case SchedPolicy::Batch:
    return SchedClass::Batch;
  case SchedPolicy::Idle:
    return SchedClass::Idle;
  case SchedPolicy::Normal:
  default:
    return SchedClass::Normal;
  }
}

// Process priority range
namespace priority {
inline constexpr i32 MIN_NICE = -20;
inline constexpr i32 MAX_NICE = 19;
inline constexpr i32 DEFAULT_NICE = 0;

inline constexpr u32 MIN_RT_PRIORITY = 1;
inline constexpr u32 MAX_RT_PRIORITY = 99;
inline constexpr u32 DEFAULT_RT_PRIORITY = 50;
} // namespace priority

// RT scheduling parameters
namespace rt_params {
// SCHED_RR default time slice (100ms, matching Linux)
inline constexpr u64 RR_TIMESLICE_NS = 100000000;
} // namespace rt_params

// CPU context structure (multi-architecture support)
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
// ARM64 CPU context
struct alignas(16) CpuContext {
  // General purpose registers x0-x30
  u64 x[31];

  // Stack pointer
  u64 sp;

  // Program counter
  u64 pc;

  // Program state register
  u64 pstate;

  // Floating point and NEON registers
  u64 fpsr;
  u64 fpcr;

  // NEON/FP registers (128-bit x 32)
  struct {
    u64 low, high;
  } v[32];

  // Thread pointer register
  u64 tpidr_el0;

  constexpr CpuContext() noexcept : x{}, sp(0), pc(0), pstate(0), fpsr(0), fpcr(0), v{}, tpidr_el0(0) {}
};

#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X86_64)
// x86_64 CPU context
struct alignas(16) CpuContext {
  // General purpose registers
  u64 rax, rbx, rcx, rdx;
  u64 rsi, rdi, rbp;

  // Stack pointer (unified naming)
  u64 sp;

  u64 r8, r9, r10, r11;
  u64 r12, r13, r14, r15;

  // Status register (unified naming)
  u64 pstate;

  // Program counter (unified naming)
  u64 pc;

  // Segment registers
  u16 cs, ds, es, fs, gs, ss;

  // Floating point register state
  u64 mxcsr;
  u64 fcw;

  constexpr CpuContext() noexcept
      : rax(0), rbx(0), rcx(0), rdx(0), rsi(0), rdi(0), rbp(0), sp(0), r8(0), r9(0), r10(0), r11(0), r12(0), r13(0),
        r14(0), r15(0), pstate(0x202), pc(0), cs(0), ds(0), es(0), fs(0), gs(0), ss(0), mxcsr(0), fcw(0) {}
};

#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV)
// RISC-V CPU context
struct alignas(16) CpuContext {
  // General purpose registers x0-x31
  u64 x[32];

  // Program counter (unified naming)
  u64 pc;

  // Status register (unified naming - maps to sstatus)
  u64 pstate;

  // Stack pointer (unified naming - also maps to x[2])
  u64 sp;

  constexpr CpuContext() noexcept : x{}, pc(0), pstate(0), sp(0) { x[2] = sp; }
};

#else
#error "Unsupported target architecture: please compile on ARM64, x86_64 or RISC-V"
#endif

static_assert(sizeof(CpuContext) <= 1024, "CpuContext should fit in reasonable size");

// VMA permission / type flags
namespace vma_flags {
inline constexpr u32 READ = 1U << 0;
inline constexpr u32 WRITE = 1U << 1;
inline constexpr u32 EXEC = 1U << 2;
inline constexpr u32 DEMAND_ZERO = 1U << 3; // allocate zero page on first access
} // namespace vma_flags

// VMA region types (what is this VMA for?)
enum class VmaType : u32 {
  CODE = 0,
  DATA = 1,
  BSS = 2,
  STACK = 3,
  HEAP = 4,
  MMAP = 5,
};

// Virtual Memory Area (VMA) — describes a contiguous region in a process's
// virtual address space.  Backing data (if any) comes from an ELF segment
// held in kernel memory; pages without backing are demand-zeroed.
struct VmaRegion {
  moss::kernel::VirtAddr start_addr;
  moss::kernel::VirtAddr end_addr;
  moss::kernel::u32 flags;
  VmaType type;

  // Lazy backing: ELF segment data source (nullptr = demand-zero only)
  const moss::kernel::u8 *backing_data; // pointer to ELF data in kernel memory
  moss::kernel::usize backing_offset;   // offset into backing_data for this VMA
  moss::kernel::usize backing_size;     // valid backing data length (rest is zero)

  VmaRegion() noexcept
      : start_addr(0), end_addr(0), flags(0), type(VmaType::DATA), backing_data(nullptr), backing_offset(0),
        backing_size(0) {}

  VmaRegion(moss::kernel::VirtAddr start, moss::kernel::VirtAddr end, moss::kernel::u32 region_flags,
            VmaType vma_type = VmaType::DATA, const moss::kernel::u8 *backing = nullptr,
            moss::kernel::usize b_offset = 0, moss::kernel::usize b_size = 0) noexcept
      : start_addr(start), end_addr(end), flags(region_flags), type(vma_type), backing_data(backing),
        backing_offset(b_offset), backing_size(b_size) {}

  [[nodiscard]] bool is_demand_zero() const noexcept { return (flags & vma_flags::DEMAND_ZERO) != 0; }

  [[nodiscard]] bool has_backing() const noexcept { return backing_data != nullptr && backing_size > 0; }

  [[nodiscard]] bool contains(VirtAddr addr) const noexcept { return addr >= start_addr && addr < end_addr; }

  bool operator==(const VmaRegion &other) const noexcept {
    return start_addr == other.start_addr && end_addr == other.end_addr;
  }
};

// Virtual memory address space — per-process PGD + VMA list
struct AddressSpace {
  PhysAddr pgd_phys; // physical address of the L0 (PGD) page table
  u16 asid;          // Address Space ID (0 = kernel, 1-255 = user)

  containers::RcuList<VmaRegion> vmas; // dynamic VMA list (was: fixed array)

  containers::AtomicSize total_pages;
  containers::AtomicSize resident_pages;

  // Program break for brk() syscall — tracks the heap boundary.
  // brk_base is the initial heap start (= HEAP_START), brk_current is
  // the current program break.  Expanding brk_current extends the HEAP
  // VMA; demand paging allocates physical pages lazily on access.
  VirtAddr brk_base{0};
  VirtAddr brk_current{0};

  // Next free virtual address for anonymous mmap allocations.
  // Starts at MMAP_BASE and advances upward as regions are mapped.
  VirtAddr mmap_next{0};

  AddressSpace(PhysAddr pgd, u16 asid_val) noexcept
      : pgd_phys(pgd), asid(asid_val), vmas{}, total_pages(0), resident_pages(0) {}

  // Destructor: free page table hierarchy if still owned.
  // This ensures no PGD/PUD/PMD/PTE leak when an AddressSpace is
  // dropped without going through do_exit (e.g. fork error paths).
  ~AddressSpace() noexcept {
    if (pgd_phys != 0) {
      mm::PageTableManager::free_user_page_tables(pgd_phys);
      pgd_phys = 0;
    }
  }

  // Non-copyable (page tables are unique resources)
  AddressSpace(const AddressSpace &) = delete;
  AddressSpace &operator=(const AddressSpace &) = delete;

  // Add a VMA region (returns false if overlapping with existing)
  bool add_vma(VirtAddr start, VirtAddr end, u32 flags, VmaType type = VmaType::DATA, const u8 *backing = nullptr,
               usize b_offset = 0, usize b_size = 0) noexcept {
    // Overlap check via RcuList traversal
    const VmaRegion *overlap =
        vmas.find_if([start, end](const VmaRegion &v) { return start < v.end_addr && end > v.start_addr; });
    if (overlap) {
      return false;
    }

    vmas.push_front(VmaRegion(start, end, flags, type, backing, b_offset, b_size));
    return true;
  }

  // Find the VMA containing the given address (const pointer, nullptr if none)
  [[nodiscard]] const VmaRegion *find_vma(VirtAddr addr) const noexcept {
    return vmas.find_if([addr](const VmaRegion &v) { return v.contains(addr); });
  }

  // Remove a VMA by exact start/end match (used by munmap).
  // Returns true if the VMA was found and removed.
  bool remove_vma(VirtAddr start, VirtAddr end) noexcept {
    // RcuList::remove uses VmaRegion::operator== which compares start_addr and end_addr
    return vmas.remove(VmaRegion(start, end, 0));
  }
};

// CFS scheduling entity — includes embedded RB-tree node fields so that
// enqueue/dequeue never needs pool allocation (mirrors Linux sched_entity).
struct SchedEntity {
  u64 vruntime;
  u64 exec_start;
  u64 sum_exec_runtime;
  u64 prev_sum_exec_runtime;

  u32 weight;
  i32 nice;
  u32 prio;

  u32 load_weight;
  u64 load_sum;
  u64 util_sum;
  u64 load_avg;
  u64 util_avg;

  // ── Embedded RB-tree node (replaces CfsRunqueue::node_pool_) ────────
  // Intrusive design: the node lives inside the entity, eliminating
  // fixed-size pool limits and per-enqueue allocation overhead.
  //
  // Layout requirement: the first 5 fields (rb_data..rb_red) must be
  // binary-compatible with RbNode<Thread> { T* data; RbNode *left, *right,
  // *parent; bool red; }.  CfsRunqueue reinterpret_casts &rb_data as
  // RbNode<Thread>* for O(1) node access.
  //
  // rb_on_rq is separate state (not part of the RB node) and is placed
  // BEFORE the RB fields to avoid any layout/padding interference.
  bool rb_on_rq; // true when this node is inserted in a CfsRunqueue

  void *rb_data;   // Thread* back-pointer (void* to avoid circular dep)
  void *rb_left;   // RbNode* left child
  void *rb_right;  // RbNode* right child
  void *rb_parent; // RbNode* parent
  bool rb_red;     // RB colour (true = red)

  SchedEntity() noexcept
      : vruntime(0), exec_start(0), sum_exec_runtime(0), prev_sum_exec_runtime(0), weight(1024), nice(0), prio(120),
        load_weight(1024), load_sum(0), util_sum(0), load_avg(0), util_avg(0), rb_on_rq(false), rb_data(nullptr),
        rb_left(nullptr), rb_right(nullptr), rb_parent(nullptr), rb_red(true) {}
};

// Real-time scheduling entity — per-thread RT state.
// Used by SCHED_FIFO and SCHED_RR policies.
struct RtSchedEntity {
  u32 priority;             // 1-99 (higher = more important, opposite of nice)
  u64 time_slice_remaining; // SCHED_RR: remaining ns in current quantum

  RtSchedEntity() noexcept
      : priority(priority::DEFAULT_RT_PRIORITY), time_slice_remaining(rt_params::RR_TIMESLICE_NS) {}
};

// Thread structure
struct Thread {
  ThreadId tid;
  ProcessId owner_pid;

  CpuContext context;
  u32 cpu;
  u32 wake_cpu;

  ProcessState state;
  SchedClass sched_class;
  SchedPolicy sched_policy;
  SchedEntity se;
  RtSchedEntity rt;

  u64 start_time;
  u64 utime;
  u64 stime;

  VirtAddr stack_base;
  usize stack_size;

  VirtAddr wait_queue;
  u64 signal_mask;
  u64 pending_signals;

  // True when a user-mode task has not yet entered EL0 (needs switch_to_user
  // + eret).  After the first eret, timer-IRQ preemption saves/restores via
  // context_switch + irq_trampoline eret, so this is set false.
  bool needs_initial_eret;

  // Permanent flag: true for tasks that run in EL0 (user-mode).
  // Unlike needs_initial_eret (which is cleared after first eret),
  // this persists for the task's lifetime and drives TTBR0 switching
  // on every re-dispatch after preemption.
  bool is_user_task;

  // TIF_NEED_RESCHED — set when preemption is needed,
  // checked at safe points (syscall return, IRQ return).
  bool need_resched{false};

  // Preemption nesting counter.  When > 0, the scheduler must not
  // context-switch this task away (it holds a spinlock or is in a
  // critical section).  Incremented by preempt_disable / spinlock
  // acquire, decremented by preempt_enable / spinlock release.
  u32 preempt_count{0};

  // CPU affinity bitmap: bit N set means task may run on CPU N.
  // Default: all CPUs allowed (set from g_num_cpus at thread creation).
  CpuBitmap cpu_affinity_mask{CpuBitmap::all()};

  // Per-thread kernel stack: used as SP_EL1 when handling exceptions
  // from this thread's user-mode execution.  For kernel threads, this
  // is the same as their regular stack.  For user threads, this is a
  // separately allocated 16KB region.
  // kernel_stack_top is the high end (SP initial value, 16-byte aligned).
  VirtAddr kernel_stack_base; // low address of allocated region
  usize kernel_stack_size;    // size in bytes (typically 16KB)

  // RT run queue intrusive list pointer (next task at same priority).
  // Used by RtRunqueue; nullptr when not enqueued in an RT queue.
  Thread *rt_next_{nullptr};

  Thread(ThreadId id, ProcessId pid) noexcept
      : tid(id), owner_pid(pid), context{}, cpu(0), wake_cpu(0), state(ProcessState::Created),
        sched_class(SchedClass::Normal), sched_policy(SchedPolicy::Normal), se{}, rt{}, start_time(0), utime(0),
        stime(0), stack_base(0), stack_size(0), wait_queue(0), signal_mask(0), pending_signals(0),
        needs_initial_eret(false), is_user_task(false), need_resched(false), cpu_affinity_mask(CpuBitmap::all()),
        kernel_stack_base(0), kernel_stack_size(0) {
    // Point the embedded RB node back to this Thread (set once, immutable).
    se.rb_data = static_cast<void *>(this);
  }

  // Returns the top of this thread's kernel stack (for TPIDR_EL1).
  [[nodiscard]] VirtAddr kernel_stack_top() const noexcept { return kernel_stack_base + kernel_stack_size; }

  // Preemption control — called by spinlock acquire/release.
  void preempt_disable() noexcept { ++preempt_count; }
  void preempt_enable() noexcept {
    if (preempt_count > 0) {
      --preempt_count;
    }
  }
  [[nodiscard]] bool is_preemptible() const noexcept { return preempt_count == 0; }
};

// Process control block
class Process {
private:
  ProcessId pid_;
  ProcessId parent_pid_;

  unique_ptr<AddressSpace> address_space_;

  containers::RcuList<ThreadEntry> threads_;
  containers::AtomicCounter<u32> thread_count_;
  ThreadId main_thread_id_;

  ProcessState state_;
  i32 exit_code_;

  struct {
    u64 max_memory;
    u64 max_cpu_time;
    u32 max_threads;
    u32 max_files;
  } limits_;

  struct {
    u64 memory_usage;
    u64 cpu_time;
    u32 minor_faults;
    u32 major_faults;
    u32 voluntary_ctxt_switches;
    u32 nonvoluntary_ctxt_switches;
  } stats_;

  mutable containers::AtomicU32 ref_count_;

  // VFS: per-process file descriptor table (vfs::FdTable*)
  // Stored as void* to avoid circular dependency on moss.vfs
  void *fd_table_ = nullptr;

  // Process name (like Linux task_struct.comm), set by execve
  char name_[16]{};

  // Process group and session IDs (POSIX job control).
  // Default: pgid = pid (each process is its own group leader),
  //          sid  = parent's sid (inherited on fork, set by setsid).
  ProcessId pgid_;
  ProcessId sid_;

  // Children tracking for wait()/waitpid()
  containers::RcuList<ProcessId> children_;
  containers::WaitQueue child_exit_wq_;

  // POSIX process credentials.
  // Default: root (0,0). Inherited from parent on fork, set by execve.
  u32 uid_{0};
  u32 gid_{0};
  u32 euid_{0};
  u32 egid_{0};

public:
  Process(ProcessId pid, ProcessId parent = INVALID_PROCESS_ID) noexcept
      : pid_(pid), parent_pid_(parent), address_space_(nullptr), thread_count_(0), main_thread_id_(INVALID_THREAD_ID),
        state_(ProcessState::Created), exit_code_(0), limits_{}, stats_{}, ref_count_(1), pgid_(pid), sid_(0),
        children_{}, child_exit_wq_{} {}

  ~Process() noexcept {
    cleanup_threads();
    // Page table cleanup is handled by ~AddressSpace (via unique_ptr).
    // do_exit() zeroes pgd_phys early to avoid freeing active tables;
    // if that didn't happen (error path), ~AddressSpace frees them now.
  }

  // Non-copyable (deleted copy constructor and copy assignment)
  Process(const Process &) = delete;
  Process &operator=(const Process &) = delete;

  // Process objects are heap-allocated and accessed via pointer; move is not needed.
  Process(Process &&) = delete;
  Process &operator=(Process &&) = delete;

  // Basic property access
  [[nodiscard]] ProcessId pid() const noexcept { return pid_; }
  [[nodiscard]] ProcessId parent_pid() const noexcept { return parent_pid_; }
  [[nodiscard]] ProcessState state() const noexcept { return state_; }
  [[nodiscard]] i32 exit_code() const noexcept { return exit_code_; }

  // Process group / session accessors (POSIX job control)
  [[nodiscard]] ProcessId pgid() const noexcept { return pgid_; }
  [[nodiscard]] ProcessId sid() const noexcept { return sid_; }
  void set_pgid(ProcessId pgid) noexcept { pgid_ = pgid; }
  void set_sid(ProcessId sid) noexcept { sid_ = sid; }

  // POSIX credentials
  [[nodiscard]] u32 uid() const noexcept { return uid_; }
  [[nodiscard]] u32 gid() const noexcept { return gid_; }
  [[nodiscard]] u32 euid() const noexcept { return euid_; }
  [[nodiscard]] u32 egid() const noexcept { return egid_; }
  void set_uid(u32 uid) noexcept {
    uid_ = uid;
    euid_ = uid;
  }
  void set_gid(u32 gid) noexcept {
    gid_ = gid;
    egid_ = gid;
  }

  // Process name (set by execve, inherited by fork)
  [[nodiscard]] const char *name() const noexcept { return name_; }
  void set_name(const char *n) noexcept {
    usize i = 0;
    while (i < 15 && n[i] != '\0') {
      name_[i] = n[i];
      ++i;
    }
    name_[i] = '\0';
  }

  // Thread management
  [[nodiscard]] KernelResult<ThreadId> create_thread(VirtAddr entry_point, VirtAddr stack_base,
                                                     usize stack_size) noexcept;

  [[nodiscard]] Thread *get_thread(ThreadId tid) const noexcept;
  [[nodiscard]] Thread *get_main_thread() const noexcept;

  [[nodiscard]] u32 thread_count() const noexcept { return thread_count_.load(containers::MemoryOrder::Relaxed); }

  // Memory management
  [[nodiscard]] VoidResult set_address_space(unique_ptr<AddressSpace> as) noexcept;
  [[nodiscard]] AddressSpace *address_space() const noexcept { return address_space_.get(); }

  // Process state management
  void set_state(ProcessState new_state) noexcept;
  void set_exit_code(i32 code) noexcept { exit_code_ = code; }

  // Statistics update
  void update_cpu_time(u64 user_time, u64 kernel_time) noexcept;
  void record_context_switch(bool voluntary) noexcept;
  void record_page_fault(bool major) noexcept;

  // Reference counting
  void add_ref() const noexcept { (void)ref_count_.fetch_add(1, containers::MemoryOrder::Relaxed); }

  void release() const noexcept {
    if (ref_count_.fetch_sub(1, containers::MemoryOrder::AcqRel) == 1) {
      delete this;
    }
  }

  [[nodiscard]] u32 ref_count() const noexcept { return ref_count_.load(containers::MemoryOrder::Acquire); }

  // Allocate a globally unique thread ID (static atomic counter).
  // Public so that fork() and other kernel code can create threads directly.
  [[nodiscard]] static ThreadId allocate_thread_id() noexcept;

  // Register an externally-created thread into this process's thread list.
  // Used by fork() which builds a Thread manually instead of create_thread().
  void register_thread(Thread *thread) noexcept;

  // ── Children tracking (for wait/waitpid) ──────────────────────────

  void add_child(ProcessId child_pid) { children_.push_front(child_pid); }

  void remove_child(ProcessId child_pid) {
    containers::RcuReadLock lock;
    children_.remove(child_pid);
  }

  [[nodiscard]] bool has_children() const noexcept { return !children_.empty(); }

  // Find a zombie child matching wait_pid:
  //   wait_pid > 0  → specific child
  //   wait_pid == -1 → any zombie child
  // Returns PID of found zombie, or INVALID_PROCESS_ID if none.
  [[nodiscard]] ProcessId find_zombie_child(i64 wait_pid) const noexcept;

  // Check if a specific PID is in this process's children list
  [[nodiscard]] bool is_child(ProcessId pid) const noexcept { return children_.find(pid) != nullptr; }

  // Access wait queue for child exit notification
  containers::WaitQueue &child_exit_wait_queue() noexcept { return child_exit_wq_; }

  // Iterate children (for reparenting in sys_exit)
  template <typename Func> void for_each_child(Func func) const { children_.for_each(func); }

  // Parent PID setter (for reparenting)
  void set_parent_pid(ProcessId pid) noexcept { parent_pid_ = pid; }

  // VFS file descriptor table access (void* to avoid circular dependency)
  [[nodiscard]] void *fd_table() const noexcept { return fd_table_; }
  void set_fd_table(void *fdt) noexcept { fd_table_ = fdt; }

private:
  void cleanup_threads() noexcept;
};

// Process manager
class ProcessManager {
private:
  containers::RcuHashMap<ProcessId, Process *> processes_;
  containers::AtomicCounter<ProcessId> next_pid_;

  containers::PerCpuAtomicCounter<u64> total_context_switches_;
  containers::PerCpuAtomicCounter<u64> total_forks_;
  containers::PerCpuAtomicCounter<u64> total_exits_;

public:
  ProcessManager() noexcept : next_pid_(1) {}

  [[nodiscard]] KernelResult<Process *> create_process(ProcessId parent_pid = INVALID_PROCESS_ID) noexcept;
  [[nodiscard]] VoidResult terminate_process(ProcessId pid, i32 exit_code) noexcept;

  [[nodiscard]] Process *find_process(ProcessId pid) const noexcept;
  [[nodiscard]] bool process_exists(ProcessId pid) const noexcept;

  [[nodiscard]] u64 total_processes() const noexcept;
  [[nodiscard]] u64 total_context_switches() const noexcept { return total_context_switches_; }
  [[nodiscard]] u64 total_forks() const noexcept { return total_forks_; }
  [[nodiscard]] u64 total_exits() const noexcept { return total_exits_; }

  template <typename Func> void for_each_process(Func &&func) const {
    processes_.for_each([&func](const auto &entry) { func(entry.key, entry.value); });
  }

private:
  [[nodiscard]] ProcessId allocate_pid() noexcept;
  void record_fork() noexcept { (void)total_forks_.fetch_add_local(1); }
  void record_exit() noexcept { (void)total_exits_.fetch_add_local(1); }
};

// Global process manager instance
extern ProcessManager *g_process_manager;

// Convenience functions (defined after CfsScheduler — see :scheduler partition)
[[nodiscard]] Thread *current_thread() noexcept;
[[nodiscard]] Process *current_process() noexcept;

[[nodiscard]] inline u32 current_cpu() noexcept { return arch::get_current_cpu_id(); }

// Shared Zombie transition: tear down a dying process and hand control to the
// scheduler.  Called by both sys_exit (syscall) and terminate_current_user_process
// (fatal page fault bridge).  This function never returns.
//
// Preconditions:
//   - `cur` is the currently running thread (will be marked Terminated)
//   - `proc` is the Process owning `cur` (will transition to Zombie)
//   - Caller must have already validated cur/proc are non-null
[[noreturn]] void do_exit(Thread *cur, Process *proc, i32 exit_code) noexcept;

// Canonical user-space virtual address layout.
// All components that create user VMAs should reference these constants
// instead of hardcoding addresses.
//
// RISC-V: STACK_TOP is a runtime variable set by init_riscv_address_layout()
// based on detected MMU mode (Sv39: 252GB, Sv48: 128TB-4GB).
// ARM64/x86_64 always use 48-bit VA (128TB user space).
// CODE_BASE, HEAP_START, MMAP_BASE are below 64GB and identical for all modes.
namespace user_layout {
inline constexpr VirtAddr CODE_BASE = 0x0000000200000000ULL;  // 8GB — above kernel identity map
inline constexpr VirtAddr HEAP_START = 0x0000000100000000ULL; // 4GB
inline constexpr usize STACK_SIZE = 32ULL * 1024;             // 32KB default user stack
inline constexpr usize STACK_MAX = 8ULL * 1024 * 1024;        // 8MB max stack (auto-growth limit)
inline constexpr usize HEAP_INIT = 64ULL * 1024;              // 64KB initial heap
inline constexpr VirtAddr MMAP_BASE = 0x0000001000000000ULL;  // 64GB — anonymous mmap region start

#if defined(MOSS_ARCH_RISCV)
// Runtime variable — set by init_riscv_address_layout() during early boot.
// Sv39: 0x3F00000000 (252GB), Sv48: 0x7FFF00000000 (128TB - 4GB)
constinit inline VirtAddr STACK_TOP = 0x0000003F00000000ULL; // Sv39 default
#else
inline constexpr VirtAddr STACK_TOP = 0x00007FFF00000000ULL; // 128TB boundary - 4GB
#endif
} // namespace user_layout

// User address space management extensions
namespace user_space {

[[nodiscard]] KernelResult<unique_ptr<AddressSpace>> create_user_address_space() noexcept;

[[nodiscard]] VoidResult map_user_memory(AddressSpace *as, VirtAddr vaddr, PhysAddr paddr, usize size,
                                         u32 flags) noexcept;

[[nodiscard]] KernelResult<VirtAddr> allocate_user_heap(Process *process, usize size) noexcept;

} // namespace user_space

} // namespace moss::kernel::process
