// MOSS Process Module - Partition: types
// Base process types, Thread, Process, ProcessManager, user_space

module;

// Architecture detection
#include "arch_detect.h"

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
    Thread* thread;

    ThreadEntry(ThreadId id, Thread* thr) : tid(id), thread(thr) {}

    bool operator==(const ThreadEntry& other) const {
        return tid == other.tid;
    }
};

// Use kernel smart pointers
using moss::kernel::make_unique;
using moss::kernel::unique_ptr;

// Process states
enum class ProcessState : u8 {
  Created = 0,
  Ready = 1,
  Running = 2,
  Blocked = 3,
  Terminated = 4,
  Zombie = 5
};

// Scheduling classes
enum class SchedClass : u8 {
  Normal = 0,
  RealTime = 1,
  Idle = 2,
  Batch = 3
};

// Process priority range
namespace Priority {
inline constexpr i32 MIN_NICE = -20;
inline constexpr i32 MAX_NICE = 19;
inline constexpr i32 DEFAULT_NICE = 0;

inline constexpr u32 MIN_RT_PRIORITY = 1;
inline constexpr u32 MAX_RT_PRIORITY = 99;
inline constexpr u32 DEFAULT_RT_PRIORITY = 50;
} // namespace Priority

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

  constexpr CpuContext() noexcept
      : x{}, sp(0), pc(0), pstate(0), fpsr(0), fpcr(0), v{}, tpidr_el0(0) {}
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
      : rax(0), rbx(0), rcx(0), rdx(0), rsi(0), rdi(0), rbp(0), sp(0),
        r8(0), r9(0), r10(0), r11(0), r12(0), r13(0), r14(0), r15(0),
        pstate(0x202), pc(0), cs(0), ds(0), es(0), fs(0), gs(0), ss(0),
        mxcsr(0), fcw(0) {}
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

  constexpr CpuContext() noexcept : x{}, pc(0), pstate(0), sp(0) {
    x[2] = sp;
  }
};

#else
#error "Unsupported target architecture: please compile on ARM64, x86_64 or RISC-V"
#endif

static_assert(sizeof(CpuContext) <= 1024,
              "CpuContext should fit in reasonable size");

// VMA permission / type flags
namespace VmaFlags {
inline constexpr u32 READ        = 1u << 0;
inline constexpr u32 WRITE       = 1u << 1;
inline constexpr u32 EXEC        = 1u << 2;
inline constexpr u32 DEMAND_ZERO = 1u << 3;  // allocate zero page on first access
} // namespace VmaFlags

// VMA region types (what is this VMA for?)
enum class VmaType : u32 {
  CODE  = 0,
  DATA  = 1,
  BSS   = 2,
  STACK = 3,
  HEAP  = 4,
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
  const moss::kernel::u8* backing_data;    // pointer to ELF data in kernel memory
  moss::kernel::usize backing_offset;      // offset into backing_data for this VMA
  moss::kernel::usize backing_size;        // valid backing data length (rest is zero)

  VmaRegion() noexcept
      : start_addr(0), end_addr(0), flags(0), type(VmaType::DATA),
        backing_data(nullptr), backing_offset(0), backing_size(0) {}

  VmaRegion(moss::kernel::VirtAddr start, moss::kernel::VirtAddr end,
            moss::kernel::u32 region_flags,
            VmaType vma_type = VmaType::DATA,
            const moss::kernel::u8* backing = nullptr,
            moss::kernel::usize b_offset = 0,
            moss::kernel::usize b_size = 0) noexcept
      : start_addr(start), end_addr(end), flags(region_flags), type(vma_type),
        backing_data(backing), backing_offset(b_offset), backing_size(b_size) {
  }

  [[nodiscard]] bool is_demand_zero() const noexcept {
    return (flags & VmaFlags::DEMAND_ZERO) != 0;
  }

  [[nodiscard]] bool has_backing() const noexcept {
    return backing_data != nullptr && backing_size > 0;
  }

  [[nodiscard]] bool contains(VirtAddr addr) const noexcept {
    return addr >= start_addr && addr < end_addr;
  }
};

// Virtual memory address space — per-process PGD + VMA list
struct AddressSpace {
  static constexpr usize MAX_VMAS = 16;  // sufficient for MVP

  PhysAddr pgd_phys;       // physical address of the L0 (PGD) page table
  u16 asid;                // Address Space ID (0 = kernel, 1-255 = user)

  VmaRegion vmas[MAX_VMAS];
  u32 vma_count;

  containers::AtomicSize total_pages;
  containers::AtomicSize resident_pages;

  AddressSpace(PhysAddr pgd, u16 asid_val) noexcept
      : pgd_phys(pgd), asid(asid_val), vmas{}, vma_count(0),
        total_pages(0), resident_pages(0) {}

  // Add a VMA region (returns false if overlapping with existing or full)
  bool add_vma(VirtAddr start, VirtAddr end, u32 flags,
               VmaType type = VmaType::DATA,
               const u8* backing = nullptr,
               usize b_offset = 0, usize b_size = 0) noexcept {
    if (vma_count >= MAX_VMAS) return false;

    // Overlap check
    for (u32 i = 0; i < vma_count; i++) {
      if (start < vmas[i].end_addr && end > vmas[i].start_addr) {
        return false;
      }
    }

    vmas[vma_count] = VmaRegion(start, end, flags, type, backing, b_offset, b_size);
    vma_count++;
    return true;
  }

  // Find the VMA containing the given address (const pointer, nullptr if none)
  [[nodiscard]] const VmaRegion* find_vma(VirtAddr addr) const noexcept {
    for (u32 i = 0; i < vma_count; i++) {
      if (vmas[i].contains(addr)) {
        return &vmas[i];
      }
    }
    return nullptr;
  }
};

// CFS scheduling entity
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

  SchedEntity() noexcept
      : vruntime(0), exec_start(0), sum_exec_runtime(0),
        prev_sum_exec_runtime(0), weight(1024), nice(0), prio(120),
        load_weight(1024), load_sum(0), util_sum(0), load_avg(0), util_avg(0) {}
};

// Real-time scheduling entity
struct RtSchedEntity {
  u32 priority;
  u64 runtime;
  u64 deadline;
  u64 period;

  RtSchedEntity() noexcept
      : priority(Priority::DEFAULT_RT_PRIORITY), runtime(0), deadline(0),
        period(0) {}
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

  // Per-thread kernel stack: used as SP_EL1 when handling exceptions
  // from this thread's user-mode execution.  For kernel threads, this
  // is the same as their regular stack.  For user threads, this is a
  // separately allocated 16KB region.
  // kernel_stack_top is the high end (SP initial value, 16-byte aligned).
  VirtAddr kernel_stack_base;   // low address of allocated region
  usize kernel_stack_size;      // size in bytes (typically 16KB)

  Thread(ThreadId id, ProcessId pid) noexcept
      : tid(id), owner_pid(pid), context{}, cpu(0), wake_cpu(0),
        state(ProcessState::Created), sched_class(SchedClass::Normal), se{},
        rt{}, start_time(0), utime(0), stime(0), stack_base(0), stack_size(0),
        wait_queue(0), signal_mask(0), pending_signals(0),
        needs_initial_eret(false), is_user_task(false),
        kernel_stack_base(0), kernel_stack_size(0) {}

  // Returns the top of this thread's kernel stack (for TPIDR_EL1).
  [[nodiscard]] VirtAddr kernel_stack_top() const noexcept {
    return kernel_stack_base + kernel_stack_size;
  }
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

public:
  Process(ProcessId pid, ProcessId parent = INVALID_PROCESS_ID) noexcept
      : pid_(pid), parent_pid_(parent), address_space_(nullptr),
        thread_count_(0), main_thread_id_(INVALID_THREAD_ID),
        state_(ProcessState::Created), exit_code_(0), limits_{}, stats_{},
        ref_count_(1) {}

  ~Process() noexcept {
    cleanup_threads();

    // Free user page tables and demand-paged physical pages.
    // Must happen AFTER TTBR0 is restored to kernel PGD (done in sys_exit
    // and terminate_current_user_process) so we don't free active tables.
    if (address_space_ && address_space_->pgd_phys != 0) {
      mm::PageTableManager::free_user_page_tables(address_space_->pgd_phys);
      address_space_->pgd_phys = 0; // Prevent double-free
    }
  }

  // Non-copyable (deleted copy constructor and copy assignment)
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;

  Process(Process &&other) noexcept
      : pid_(other.pid_), parent_pid_(other.parent_pid_),
        address_space_(moss::move(other.address_space_)),
        threads_{},
        thread_count_(0),
        main_thread_id_(other.main_thread_id_), state_(other.state_),
        exit_code_(other.exit_code_), limits_(other.limits_),
        stats_(other.stats_), ref_count_(1) {
    other.pid_ = INVALID_PROCESS_ID;
  }

  // Basic property access
  [[nodiscard]] ProcessId pid() const noexcept { return pid_; }
  [[nodiscard]] ProcessId parent_pid() const noexcept { return parent_pid_; }
  [[nodiscard]] ProcessState state() const noexcept { return state_; }
  [[nodiscard]] i32 exit_code() const noexcept { return exit_code_; }

  // Thread management
  [[nodiscard]] KernelResult<ThreadId> create_thread(VirtAddr entry_point,
                                                     VirtAddr stack_base,
                                                     usize stack_size) noexcept;

  [[nodiscard]] Thread *get_thread(ThreadId tid) const noexcept;
  [[nodiscard]] Thread *get_main_thread() const noexcept;

  [[nodiscard]] u32 thread_count() const noexcept {
    return thread_count_.load(containers::MemoryOrder::Relaxed);
  }

  // Memory management
  [[nodiscard]] VoidResult
  set_address_space(unique_ptr<AddressSpace> as) noexcept;
  [[nodiscard]] AddressSpace *address_space() const noexcept {
    return address_space_.get();
  }

  // Process state management
  void set_state(ProcessState new_state) noexcept;
  void set_exit_code(i32 code) noexcept { exit_code_ = code; }

  // Statistics update
  void update_cpu_time(u64 user_time, u64 kernel_time) noexcept;
  void record_context_switch(bool voluntary) noexcept;
  void record_page_fault(bool major) noexcept;

  // Reference counting
  void add_ref() const noexcept {
    (void)ref_count_.fetch_add(1, containers::MemoryOrder::Relaxed);
  }

  void release() const noexcept {
    if (ref_count_.fetch_sub(1, containers::MemoryOrder::AcqRel) == 1) {
      delete this;
    }
  }

  [[nodiscard]] u32 ref_count() const noexcept {
    return ref_count_.load(containers::MemoryOrder::Acquire);
  }

  // Allocate a globally unique thread ID (static atomic counter).
  // Public so that fork() and other kernel code can create threads directly.
  [[nodiscard]] static ThreadId allocate_thread_id() noexcept;

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

  [[nodiscard]] KernelResult<Process *>
  create_process(ProcessId parent_pid = INVALID_PROCESS_ID) noexcept;
  [[nodiscard]] VoidResult terminate_process(ProcessId pid,
                                             i32 exit_code) noexcept;

  [[nodiscard]] Process *find_process(ProcessId pid) const noexcept;
  [[nodiscard]] bool process_exists(ProcessId pid) const noexcept;

  [[nodiscard]] KernelResult<ProcessId> sys_fork() noexcept;
  [[nodiscard]] VoidResult sys_exit(i32 exit_code) noexcept;
  [[nodiscard]] KernelResult<ProcessId> sys_wait(ProcessId pid) noexcept;

  [[nodiscard]] u64 total_processes() const noexcept;
  [[nodiscard]] u64 total_context_switches() const noexcept {
    return total_context_switches_;
  }
  [[nodiscard]] u64 total_forks() const noexcept { return total_forks_; }
  [[nodiscard]] u64 total_exits() const noexcept { return total_exits_; }

  template <typename Func> void for_each_process(Func &&func) const {
    processes_.for_each(
        [&func](const auto &entry) { func(entry.key, entry.value); });
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

[[nodiscard]] inline u32 current_cpu() noexcept {
  return arch::get_current_cpu_id();
}

// User address space management extensions
namespace user_space {

[[nodiscard]] KernelResult<unique_ptr<AddressSpace>> create_user_address_space() noexcept;

[[nodiscard]] KernelResult<Process*> create_process_from_elf(const u8* elf_data, usize elf_size) noexcept;

[[nodiscard]] VoidResult map_user_memory(AddressSpace* as, VirtAddr vaddr, PhysAddr paddr,
                          usize size, u32 flags) noexcept;

[[nodiscard]] KernelResult<VirtAddr> allocate_user_heap(Process* process, usize size) noexcept;

} // namespace user_space

} // namespace moss::kernel::process
