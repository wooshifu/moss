// MOSS Process Module - Partition: types
// Base process types, Thread, Process, ProcessManager, user_space

export module moss.process:types;

import moss.std;
import moss.types;
import moss.result;
import moss.smart_ptr;
import moss.arch;
import moss.abi;
import moss.containers;
import moss.mm;
import moss.capability;
import moss.logging;

extern "C" void moss_validation_address_space_retiring(moss::kernel::PhysAddr root) noexcept;

// ============================================================================
// process.hpp - Base process types
// ============================================================================
export namespace moss::kernel::process {

namespace log = moss::kernel::logging;

// Forward declarations
struct Thread;
class CfsScheduler;

// Thread entry for LockedList storage
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
// Maps to Linux SCHED_* constants returned by sched_getscheduler().
enum class SchedPolicy : u8 {
  Normal = 0, // SCHED_NORMAL (CFS)
  Fifo = 1,   // SCHED_FIFO   (RT, runs until block/yield/preempted by higher prio)
  RR = 2,     // SCHED_RR     (RT, round-robin within same priority)
  Batch = 3,  // SCHED_BATCH  (CFS, batch-optimized)
  Idle = 5,   // SCHED_IDLE   (lowest priority)
};

// Linux-compatible public ranges: nice -20..19 maps to 40 CFS weights;
// RT priorities 1..99 leave zero for non-RT. Fallback 50 is the middle RT
// priority; the reason for choosing that fallback is not recorded.
namespace priority {
inline constexpr i32 MIN_NICE = -20;
inline constexpr i32 MAX_NICE = 19;
inline constexpr i32 DEFAULT_NICE = 0;

inline constexpr u32 MIN_RT_PRIORITY = 1;
inline constexpr u32 MAX_RT_PRIORITY = 99;
inline constexpr u32 DEFAULT_RT_PRIORITY = 50;
inline constexpr i32 NO_INHERITED_NICE = MAX_NICE + 1;
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

#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X64)
// Architectural FXSAVE64 image. Baseline x87/MMX/SSE state; AVX is not enabled.
struct alignas(16) X86FpState {
  // Architectural reset controls: 0x037f masks x87 exceptions with round-to-
  // nearest/extended precision; MXCSR 0x1f80 masks SSE exceptions and rounds
  // to nearest. They must form a valid fresh FXSAVE image for first dispatch.
  u16 control = 0x037f;
  u16 status = 0;
  u8 tag = 0;
  u8 reserved = 0;
  u16 opcode = 0;
  u64 ip = 0;
  u64 data = 0;
  u32 mxcsr = 0x1f80;
  u32 mxcsr_mask = 0;
  // FXSAVE64 layout: eight 16-byte x87 slots, sixteen 16-byte XMM slots and
  // 96 reserved bytes complete the 512-byte image; offsets are ABI constraints.
  u8 st[128]{};
  u8 xmm[256]{};
  u8 reserved_tail[96]{};
};
static_assert(sizeof(X86FpState) == 512 && __builtin_offsetof(X86FpState, mxcsr) == 24 &&
              __builtin_offsetof(X86FpState, xmm) == 160);

// x64 CPU context
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

  X86FpState fp;
  u64 fs_base;

  constexpr CpuContext() noexcept
      // 0x202 sets fixed RFLAGS bit 1 and IF bit 9; assembly masks IF during
      // the stack transition and the user-return path restores allowed flags.
      : rax(0), rbx(0), rcx(0), rdx(0), rsi(0), rdi(0), rbp(0), sp(0), r8(0), r9(0), r10(0), r11(0), r12(0), r13(0),
        r14(0), r15(0), pstate(0x202), pc(0), cs(0), ds(0), es(0), fs(0), gs(0), ss(0), fp{}, fs_base(0) {}
};
static_assert(__builtin_offsetof(CpuContext, fp) == 160);
static_assert(__builtin_offsetof(CpuContext, fs_base) == 672);

#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV64)
// RISC-V 64 CPU context
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
#error "Unsupported target architecture: please compile on ARM64, x64 or RISC-V 64"
#endif

// One KiB is a software context-size guard, not an architecture frame size;
// its original budget justification is unrecorded. Exact offsets below must
// match context_switch.S, so structure changes require matching assembly edits.
static_assert(sizeof(CpuContext) <= 1024, "CpuContext should fit in reasonable size");
#if defined(MOSS_ARCH_ARM64)
static_assert(__builtin_offsetof(CpuContext, sp) == 248 && __builtin_offsetof(CpuContext, pc) == 256 &&
              __builtin_offsetof(CpuContext, pstate) == 264 && __builtin_offsetof(CpuContext, v) == 288 &&
              __builtin_offsetof(CpuContext, tpidr_el0) == 800);
#elif defined(MOSS_ARCH_RISCV64)
static_assert(__builtin_offsetof(CpuContext, pc) == 256 && __builtin_offsetof(CpuContext, pstate) == 264 &&
              __builtin_offsetof(CpuContext, sp) == 272);
#elif defined(MOSS_ARCH_X64)
static_assert(__builtin_offsetof(CpuContext, sp) == 56 && __builtin_offsetof(CpuContext, pstate) == 128 &&
              __builtin_offsetof(CpuContext, pc) == 136);
#endif

// Canonical user layout; the MMU's runtime USER_MAX selects Sv39/Sv48 bounds.
namespace user_layout {
// Keep heap at/above the 4 GiB kernel-map end, the RX signal stub at 6 GiB,
// raw boot code at 8 GiB and anonymous mmap at 64 GiB so these regions begin
// separately. Exact address-spacing choices are unrecorded; this is layout,
// not a reservation of the intervening physical memory.
inline constexpr VirtAddr CODE_BASE = 0x0000000200000000ULL;  // 8 GiB
inline constexpr VirtAddr HEAP_START = 0x0000000100000000ULL; // 4 GiB, above identity map
// Demand-paged budgets: 32 KiB initial stack, 8 MiB maximum downward growth
// and a 64 KiB ELF exclusion window at the heap base. The HEAP VMA itself
// starts empty and follows brk exactly; the original window sizing evidence is
// not recorded. Changing these values affects admission, not immediate RAM use.
inline constexpr usize STACK_SIZE = 32ULL * 1024;
inline constexpr usize STACK_MAX = 8ULL * 1024 * 1024;
inline constexpr usize HEAP_INIT = 64ULL * 1024;
inline constexpr VirtAddr MMAP_BASE = 0x0000001000000000ULL;      // 64 GiB
inline constexpr VirtAddr SIGRETURN_PAGE = 0x0000000180000000ULL; // kernel-installed RX user stub
#if defined(MOSS_ARCH_RISCV64)
// Initial stack address fits the positive Sv39 half; boot recomputes it from
// the selected MMU mode. Other targets use the positive 48-bit user half.
// Exact stack-top placement within these bounds has no recorded rationale.
constinit inline VirtAddr STACK_TOP = 0x0000003F00000000ULL; // updated during early boot
#else
inline constexpr VirtAddr STACK_TOP = 0x00007FFF00000000ULL;
#endif
} // namespace user_layout

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
  SIGRETURN = 6,
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
  // The VMA retains the Memory Object while borrowing its immutable page list;
  // resident PTEs hold separate frame references until unmap or teardown.
  shared_ptr<capability::Object> memory_object;
  const PhysAddr *shared_pages;
  usize shared_page_count;

  VmaRegion() noexcept
      : start_addr(0), end_addr(0), flags(0), type(VmaType::DATA), backing_data(nullptr), backing_offset(0),
        backing_size(0), memory_object{}, shared_pages(nullptr), shared_page_count(0) {}

  VmaRegion(moss::kernel::VirtAddr start, moss::kernel::VirtAddr end, moss::kernel::u32 region_flags,
            VmaType vma_type = VmaType::DATA, const moss::kernel::u8 *backing = nullptr,
            moss::kernel::usize b_offset = 0, moss::kernel::usize b_size = 0,
            shared_ptr<capability::Object> memory = {}, const PhysAddr *pages = nullptr, usize page_count = 0) noexcept
      : start_addr(start), end_addr(end), flags(region_flags), type(vma_type), backing_data(backing),
        backing_offset(b_offset), backing_size(b_size), memory_object(moss::move(memory)), shared_pages(pages),
        shared_page_count(page_count) {}

  [[nodiscard]] bool is_demand_zero() const noexcept { return (flags & vma_flags::DEMAND_ZERO) != 0; }

  [[nodiscard]] bool has_backing() const noexcept { return backing_data != nullptr && backing_size > 0; }

  [[nodiscard]] bool contains(VirtAddr addr) const noexcept { return addr >= start_addr && addr < end_addr; }

  bool operator==(const VmaRegion &other) const noexcept {
    return start_addr == other.start_addr && end_addr == other.end_addr;
  }
};

namespace user_space {
void release_asid(u16 tag) noexcept;
}

// Virtual memory address space — per-process PGD + VMA list
struct ExecutableImage {
  u8 *data = nullptr;
  usize size = 0;
  ~ExecutableImage() noexcept {
    if (data) {
      (void)mm::RuntimeHeapAllocator::deallocate(data, size);
    }
  }
};

struct AddressSpace {
private:
  containers::IrqSpinLock vm_lock_;

public:
  // Lock order: VM transaction -> VMA list -> MM allocator locks. Keep the
  // AddressSpace owner alive until this guard is destroyed. Transactions may
  // not block, switch address spaces or copy via faultable user addresses.
  // This serializes software state, not hardware-root lifetime. PTE mutators
  // perform their own synchronous TLB invalidation; active-root owners and
  // synchronous copy leases separately protect their corresponding lifetimes.
  class VmTransaction {
    AddressSpace &owner_;

  public:
    explicit VmTransaction(AddressSpace &owner) noexcept;
    ~VmTransaction() noexcept;
    VmTransaction(const VmTransaction &) = delete;
    VmTransaction &operator=(const VmTransaction &) = delete;
  };
  [[nodiscard]] VmTransaction lock_vm() noexcept { return VmTransaction(*this); }

  PhysAddr pgd_phys; // physical address of the L0 (PGD) page table
  u16 asid;          // Address Space ID (0 = kernel, 1-255 = user)

  shared_ptr<ExecutableImage> executable_image; // Shared by fork; outlives lazy VMA backing.
  containers::LockedList<VmaRegion> vmas;       // dynamic VMA list (was: fixed array)

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

  // The final owner releases tables and ASID together, including fork rollback
  // and readers which outlive exec/exit. CfsScheduler's per-CPU root owner is
  // released only after switching away; references do not serialize PTE mutation.
  ~AddressSpace() noexcept {
    if (pgd_phys != 0) {
      moss_validation_address_space_retiring(pgd_phys);
      mm::PageTableManager::free_user_page_tables(pgd_phys);
      pgd_phys = 0;
    }
    user_space::release_asid(asid);
  }

  // Non-copyable (page tables are unique resources)
  AddressSpace(const AddressSpace &) = delete;
  AddressSpace &operator=(const AddressSpace &) = delete;

  // Retain this AddressSpace for the whole call. Fault resolution consumes one
  // VMA/root/image context; it never returns a borrowed backing pointer.
  [[nodiscard]] bool resolve_fault(VirtAddr address, mm::UserFaultAccess access, bool cow_only,
                                   VirtAddr *grown_stack = nullptr) noexcept;

  // Bind copies to this owned version, independently of the CPU's active root.
  // A short VM transaction keeps each resolved page mapped through its copy;
  // writers also exclude fork so a physical alias cannot bypass newly set COW.
  // The entire buffer is not atomic, and kernel buffers must remain valid.
  [[nodiscard]] usize copy_from_user(void *destination, VirtAddr source, usize size) noexcept;
  [[nodiscard]] usize copy_to_user(VirtAddr destination, const void *source, usize size) noexcept;

private:
  [[nodiscard]] bool resolve_fault_locked(VirtAddr address, mm::UserFaultAccess access, bool cow_only,
                                          VirtAddr *grown_stack) noexcept;
  [[nodiscard]] usize transfer_user_pages(VirtAddr address, void *kernel_output, const void *kernel_input, usize size,
                                          mm::UserFaultAccess access) noexcept;

public:
  [[nodiscard]] static bool valid_vma_range(VirtAddr start, VirtAddr end, VmaType type) noexcept {
    if (start > end || ((start | end) & (PAGE_SIZE - 1)) != 0) {
      return false;
    }
    // brk_base is fixed by the user layout. Accepting a HEAP elsewhere would
    // let its VMA diverge from the break fields used by sys_brk and fork.
    if (type == VmaType::HEAP && start != user_layout::HEAP_START) {
      return false;
    }
    constexpr auto stub = user_layout::SIGRETURN_PAGE;
    // An empty HEAP region is the explicit brk==brk_base state. No other VMA
    // may be empty because it would claim metadata without authorizing bytes.
    if (start == end) {
      // One byte validates the sentinel address itself without granting a span.
      constexpr usize address_probe_bytes = 1;
      return type == VmaType::HEAP && mm::PageTableManager::is_user_range(start, address_probe_bytes) &&
             (start < stub || start >= stub + PAGE_SIZE);
    }
    if (!mm::PageTableManager::is_user_range(start, end - start)) {
      return false;
    }
    if (type == VmaType::SIGRETURN) {
      return start == stub && end == stub + PAGE_SIZE;
    }
    return end <= stub || start >= stub + PAGE_SIZE;
  }

  // Admission policy is independent of whether a user page is resident.
  // Mutators require a VM transaction, or an unpublished/exclusively owned
  // space. The list lock alone cannot serialize metadata with PTE changes.
  bool add_vma(VirtAddr start, VirtAddr end, u32 flags, VmaType type = VmaType::DATA, const u8 *backing = nullptr,
               usize b_offset = 0, usize b_size = 0, shared_ptr<capability::Object> memory = {},
               const PhysAddr *pages = nullptr, usize page_count = 0) noexcept {
    constexpr u32 allowed = vma_flags::READ | vma_flags::WRITE | vma_flags::EXEC | vma_flags::DEMAND_ZERO;
    // Bootstrap image mappings also use this path; no caller may publish a
    // VMA that is writable and executable at the same time. A shared VMA must
    // cover exactly the retained object's page list before faults index it.
    if (!valid_vma_range(start, end, type) || (flags & ~allowed) != 0 ||
        (flags & (vma_flags::WRITE | vma_flags::EXEC)) == (vma_flags::WRITE | vma_flags::EXEC) ||
        (type == VmaType::SIGRETURN && flags != (vma_flags::READ | vma_flags::EXEC)) ||
        (static_cast<bool>(memory) != (pages != nullptr)) || (!memory && page_count != 0) ||
        (memory &&
         (memory->type() != capability::ObjectType::Memory || type != VmaType::MMAP || page_count == 0 ||
          (end - start) / PAGE_SIZE != page_count || (flags & (vma_flags::EXEC | vma_flags::DEMAND_ZERO)) != 0))) {
      return false;
    }
    return vmas.push_front_unless(
        [start, end, type](const VmaRegion &v) {
          // Empty heap sentinels do not overlap by interval arithmetic, so
          // reject a second HEAP explicitly to keep one authoritative break.
          return (type == VmaType::HEAP && v.type == VmaType::HEAP) || (start < v.end_addr && end > v.start_addr);
        },
        start, end, flags, type, backing, b_offset, b_size, moss::move(memory), pages, page_count);
  }

  // Resize one exact VMA without a check/update race. before_update runs while
  // the VMA list is locked, allowing brk shrink to revoke resident pages before
  // the shorter authorization is published. It must not call back into vmas.
  template <typename Func>
  bool resize_vma(VirtAddr start, VirtAddr old_end, VirtAddr new_end, VmaType type, Func before_update) noexcept {
    if (!valid_vma_range(start, new_end, type)) {
      return false;
    }
    return vmas.update_if_unless(
        [start, old_end, type](const VmaRegion &v) {
          return v.start_addr == start && v.end_addr == old_end && v.type == type;
        },
        [start, new_end](const VmaRegion &v) { return start < v.end_addr && new_end > v.start_addr; },
        [&](VmaRegion &v) {
          before_update(static_cast<const VmaRegion &>(v));
          v.end_addr = new_end;
        });
  }

  // Copy metadata while locked; callers never borrow a list node.
  [[nodiscard]] containers::Optional<VmaRegion> find_vma(VirtAddr addr) const noexcept {
    if (!mm::PageTableManager::is_user_range(addr, 1)) {
      return {};
    }
    return vmas.find_if([addr](const VmaRegion &v) { return v.contains(addr); });
  }

  // Metadata check only: this neither pins pages nor recovers a CPU access fault.
  [[nodiscard]] bool allows_user_access(VirtAddr start, usize length, u32 required_flags) const noexcept {
    if (length == 0) {
      return true;
    }
    if (!mm::PageTableManager::is_user_range(start, length)) {
      return false;
    }
    const VirtAddr end = start + length;
    // ponytail: linear VMA lookup; use an interval index if VMA counts make scans costly.
    while (start < end) {
      auto vma = find_vma(start);
      if (!vma || (vma->flags & required_flags) != required_flags || vma->end_addr <= start) {
        return false;
      }
      start = vma->end_addr < end ? vma->end_addr : end;
    }
    return true;
  }

  // Remove a VMA by exact start/end match (used by munmap).
  // Returns true if the VMA was found and removed.
  bool remove_vma(VirtAddr start, VirtAddr end) noexcept {
    // LockedList::remove uses VmaRegion::operator== which compares start_addr and end_addr
    return vmas.remove(VmaRegion(start, end, 0));
  }
};

// CFS scheduling entity — includes embedded RB-tree node fields so that
// enqueue/dequeue never needs pool allocation (mirrors Linux sched_entity).
struct SchedEntity {
  u64 vruntime;
  // Monotonic nanoseconds at dispatch/last charge. This and cpu_runtime_ns
  // use the calibrated clocksource; raw ISA counters have different rates.
  u64 exec_start;
  // Elapsed time while dispatched for both RT and CFS, including sub-tick time.
  // CFS sum_exec_runtime can be capped for fairness and is not a budget meter.
  u64 cpu_runtime_ns;
  u64 sum_exec_runtime;
  u64 prev_sum_exec_runtime;

  moss::atomic<u32> weight;
  moss::atomic<i32> nice;
  u32 prio;

  moss::atomic<u32> load_weight;
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
      // Neutral nice=0 uses weight 1024; prio 120 is the retained Linux-style
      // normal-priority default. The exact reason for retaining 120 is unrecorded.
      : vruntime(0), exec_start(0), cpu_runtime_ns(0), sum_exec_runtime(0), prev_sum_exec_runtime(0), weight(1024),
        nice(0), prio(120), load_weight(1024), load_sum(0), util_sum(0), load_avg(0), util_avg(0), rb_on_rq(false),
        rb_data(nullptr), rb_left(nullptr), rb_right(nullptr), rb_parent(nullptr), rb_red(true) {}

  [[nodiscard]] u64 charge_runtime(u64 now_ns) noexcept {
    // A stalled or slightly backward cross-CPU clock must not charge time or
    // move the start backward, which would charge the same interval twice.
    if (now_ns <= exec_start) {
      return 0;
    }
    const u64 elapsed = now_ns - exec_start;
    cpu_runtime_ns += elapsed;
    exec_start = now_ns;
    return elapsed;
  }
};

// Real-time scheduling entity — per-thread RT state.
// Used by SCHED_FIFO and SCHED_RR policies.
struct RtSchedEntity {
  u32 priority;             // 1-99 (higher = more important, opposite of nice)
  u64 time_slice_remaining; // SCHED_RR: remaining ns in current quantum

  RtSchedEntity() noexcept
      : priority(priority::DEFAULT_RT_PRIORITY), time_slice_remaining(rt_params::RR_TIMESLICE_NS) {}
};

// PendingCall owns this record. The scheduler links it into thread lists only
// while the call is active, so call completion and thread teardown can detach
// either side without leaving a pointer to a retired reply capability.
struct PriorityDonation {
  Thread *caller{nullptr};
  Thread *server{nullptr};
  PriorityDonation *next{nullptr};
  // The absolute deadline follows this wait dependency through Reply handoff.
  u64 deadline_ns{0};
};

// Thread structure
struct Thread {
  static constexpr u64 WAIT_STATUS_MASK = (1ULL << (sizeof(u32) * 8)) - 1;

  ThreadId tid;
  ProcessId owner_pid;

  CpuContext context;
  u32 cpu;
  u32 wake_cpu;

  moss::atomic<ProcessState> state;
  // 0: no handoff, 1: preparing to block, 2: wake requested before context save.
  moss::atomic<u32> sleep_handoff{0};
  containers::IrqSpinLock sleep_lock;
  // High word is a generation; low word is the Linux-compatible wait status.
  // Keeping the generation when consumed prevents a failed copyout from
  // restoring an old event over a later identical STOP/CONT event.
  moss::atomic<u64> wait_status_event{0};
  // State stays Stopped through the wake handoff; this suppresses duplicate
  // continued events from multiple SIGCONT senders. Protected by sleep_lock.
  bool job_stopped{false};
  SchedClass sched_class;
  SchedPolicy sched_policy;
  SchedEntity se;
  RtSchedEntity rt;
  moss::atomic<u32> inherited_rt_priority{0};
  moss::atomic<i32> inherited_cfs_nice{priority::NO_INHERITED_NICE};
  PriorityDonation *ipc_wait{nullptr};
  PriorityDonation *ipc_donors{nullptr};
  CfsScheduler *ipc_scheduler{nullptr};
  u64 ipc_cycle_epoch{0};

  u64 start_time;
  u64 utime;
  u64 stime;

  VirtAddr stack_base;
  usize stack_size;

  VirtAddr wait_queue;
  moss::atomic<u64> signal_mask;
  moss::atomic<u64> pending_signals;

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
  moss::atomic<bool> need_resched{false};

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

  // Borrowed only during syscall dispatch or the user-return checkpoint.
  // Nested kernel exceptions must not replace this user frame.
  moss::abi::TrapFrame *trap_frame{nullptr};
  VirtAddr active_signal_frame{0};
  // A caught signal can restart only the syscall interrupted at this return checkpoint.
  u64 restart_syscall_number{0};
  u64 restart_syscall_arg0{0};
  bool restart_syscall_pending{false};

  // RT run queue intrusive list pointer (next task at same priority).
  // Used by RtRunqueue; nullptr when not enqueued in an RT queue.
  Thread *rt_next_{nullptr};
  bool rt_on_rq{false};

  // Signal alternate stack (sigaltstack)
  VirtAddr alt_stack_sp{0}; // alternate stack base address
  usize alt_stack_size{0};  // alternate stack size in bytes
  u32 alt_stack_flags{2};   // SS_DISABLE=2 by default
  bool on_alt_stack{false}; // true when executing handler on altstack

  Thread(ThreadId id, ProcessId pid) noexcept
      : tid(id), owner_pid(pid), context{}, cpu(0), wake_cpu(0), state(ProcessState::Created),
        sched_class(SchedClass::Normal), sched_policy(SchedPolicy::Normal), se{}, rt{}, start_time(0), utime(0),
        stime(0), stack_base(0), stack_size(0), wait_queue(0), signal_mask(0), pending_signals(0),
        needs_initial_eret(false), is_user_task(false), need_resched(false), cpu_affinity_mask(CpuBitmap::all()),
        kernel_stack_base(0), kernel_stack_size(0), trap_frame(nullptr), alt_stack_sp(0), alt_stack_size(0),
        alt_stack_flags(2), on_alt_stack(false) {
    // Point the embedded RB node back to this Thread (set once, immutable).
    se.rb_data = static_cast<void *>(this);
  }

  [[nodiscard]] static Thread *try_create(ThreadId id, ProcessId pid) noexcept;
  ~Thread();

  // Own an initialized 16KB stack, including before registration in a Process.
  [[nodiscard]] VoidResult allocate_kernel_stack() noexcept;

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

  [[nodiscard]] u32 effective_rt_priority() const noexcept {
    const u32 base = sched_class == SchedClass::RealTime ? rt.priority : 0;
    const u32 inherited = inherited_rt_priority.load();
    return base > inherited ? base : inherited;
  }

  [[nodiscard]] i32 effective_cfs_nice() const noexcept {
    const i32 base = se.nice.load();
    const i32 inherited = inherited_cfs_nice.load();
    return base < inherited ? base : inherited;
  }

  void publish_wait_status(u32 status) noexcept {
    u64 current = wait_status_event.load();
    u64 next;
    do {
      next = ((current + WAIT_STATUS_MASK + 1) & ~WAIT_STATUS_MASK) | status;
    } while (!wait_status_event.compare_exchange_weak(current, next));
  }
};

// Owned by Process, independent of the numeric PID. Handler 0 is SIG_DFL.
struct Sigaction {
  VirtAddr handler{0};
  u64 mask{0};
  u32 flags{0};
};

struct SignalState {
  Sigaction actions[32]{}; // standard signals 1..31; slot 0 is unused
};

// Process control block
class Process {
private:
  ProcessId pid_;
  ProcessId parent_pid_;
  // Fork fixes membership before publication. Exec and orphaning do not
  // change it, so a failed service cannot lose an unregistered descendant.
  shared_ptr<capability::Object> domain_scope_;
  // Assigned during boot before this Process is runnable. Forked children
  // must not inherit the fatal supervisor identity from their parent.
  bool initial_supervisor_{false};
  bool domain_factory_source_{false};
  capability::Table capabilities_;

  // Only publication/acquisition uses this short IRQ-safe lock. Copy the owner
  // under it, but destroy retired spaces outside it: teardown takes MM locks.
  mutable containers::IrqSpinLock address_space_lock_;
  shared_ptr<AddressSpace> address_space_;

  containers::LockedList<ThreadEntry> threads_;
  containers::AtomicCounter<u32> thread_count_;
  // Exec admits one registered thread; pending registrations reserve the count
  // before publishing, so a concurrent registrar cannot slip past the gate.
  moss::atomic<bool> exec_in_progress_{false};
  ThreadId main_thread_id_;

  // Publishing Zombie makes both fields visible to a waiter on another CPU.
  moss::atomic<ProcessState> state_;
  i32 exit_code_;
  u32 terminating_signal_ = 0;
  containers::WaitQueue domain_exit_wq_;

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

  // VFS: per-process file descriptor table (vfs::FdTable*)
  // Stored as void* to avoid circular dependency on moss.vfs
  void *fd_table_ = nullptr;
  // Child exit and signal delivery can read dispositions on another CPU.
  // The lock protects coherent action snapshots; user copies stay outside it.
  mutable containers::IrqSpinLock signal_state_lock_;
  friend bool send_signal(Thread *thread, u32 signo) noexcept;
  SignalState signal_state_{};
  void discard_pending_signals_locked(u64 mask) noexcept;

  // Process name (like Linux task_struct.comm), set by execve
  char name_[16]{}; // Linux-style comm: up to 15 bytes plus a terminating NUL.

  // Process group and session IDs (POSIX job control).
  // Default: pgid = pid (each process is its own group leader),
  //          sid  = parent's sid (inherited on fork, set by setsid).
  // A child may change groups on another CPU while its parent scans waitpid.
  moss::atomic<ProcessId> pgid_;
  ProcessId sid_;

  // Children tracking for wait()/waitpid()
  containers::LockedList<ProcessId> children_;
  containers::WaitQueue child_exit_wq_;

  // POSIX process credentials.
  // Init defaults to root. Fork copies all four IDs; this static exec profile
  // preserves them and does not implement set-user-ID/set-group-ID binaries.
  u32 uid_{0};
  u32 gid_{0};
  u32 euid_{0};
  u32 egid_{0};

public:
  Process(ProcessId pid, ProcessId parent = INVALID_PROCESS_ID,
          shared_ptr<capability::Object> domain_scope = {}) noexcept
      : pid_(pid), parent_pid_(parent), domain_scope_(moss::move(domain_scope)), address_space_(nullptr),
        thread_count_(0), main_thread_id_(INVALID_THREAD_ID), state_(ProcessState::Created), exit_code_(0),
        domain_exit_wq_{}, limits_{}, stats_{}, pgid_(pid), sid_(0), children_{}, child_exit_wq_{} {}

  ~Process() noexcept {
    cleanup_threads();
    cleanup_files();
    // Dropping this Process's owner must not invalidate held AddressSpace
    // snapshots. do_exit() detaches it earlier, after switching hardware roots.
  }

  // Non-copyable (deleted copy constructor and copy assignment)
  Process(const Process &) = delete;
  Process &operator=(const Process &) = delete;

  // Process objects are heap-allocated and accessed via pointer; move is not needed.
  Process(Process &&) = delete;
  Process &operator=(Process &&) = delete;

  // Basic property access
  [[nodiscard]] ProcessId pid() const noexcept { return pid_; }
  void designate_initial_supervisor() noexcept { initial_supervisor_ = true; }
  [[nodiscard]] bool is_initial_supervisor() const noexcept { return initial_supervisor_; }
  void designate_domain_factory_source() noexcept { domain_factory_source_ = true; }
  [[nodiscard]] bool is_domain_factory_source() const noexcept { return domain_factory_source_; }
  [[nodiscard]] capability::Table &capabilities() noexcept { return capabilities_; }
  [[nodiscard]] const capability::Table &capabilities() const noexcept { return capabilities_; }
  [[nodiscard]] ProcessId parent_pid() const noexcept { return parent_pid_; }
  [[nodiscard]] const shared_ptr<capability::Object> &domain_scope() const noexcept { return domain_scope_; }
  [[nodiscard]] ProcessState state() const noexcept { return state_; }
  containers::WaitQueue &domain_exit_wait_queue() noexcept { return domain_exit_wq_; }
  [[nodiscard]] i32 exit_code() const noexcept { return exit_code_; }
  [[nodiscard]] u32 terminating_signal() const noexcept { return terminating_signal_; }
  // The Linux-compatible wait ABI puts normal exit codes in bits 8..15 and
  // fatal signals in the low bits. Keep the cause so _exit(-signo) stays normal.
  [[nodiscard]] i32 wait_status() const noexcept {
    return terminating_signal_ ? static_cast<i32>(terminating_signal_)
                               : static_cast<i32>((static_cast<u32>(exit_code_) & 0xffU) << 8);
  }
  [[nodiscard]] Sigaction signal_action(u32 signo) const noexcept;
  [[nodiscard]] bool try_replace_signal_action(u32 signo, const Sigaction &expected, const Sigaction &desired) noexcept;
  void inherit_signal_actions_from(const Process &parent) noexcept;
  void reset_signal_actions_for_exec() noexcept;

  // Process group / session accessors (POSIX job control)
  [[nodiscard]] ProcessId pgid() const noexcept { return pgid_.load(); }
  [[nodiscard]] ProcessId sid() const noexcept { return sid_; }
  void set_pgid(ProcessId pgid) noexcept { pgid_.store(pgid); }
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
  void inherit_credentials(const Process &parent) noexcept {
    uid_ = parent.uid_;
    gid_ = parent.gid_;
    euid_ = parent.euid_;
    egid_ = parent.egid_;
  }

  // Process name (set by execve, inherited by fork)
  [[nodiscard]] const char *name() const noexcept { return name_; }
  void set_name(const char *n) noexcept {
    usize i = 0;
    // Reserve the sixteenth byte for NUL even when the input is longer.
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
  [[nodiscard]] bool try_begin_exec() noexcept;
  void finish_exec() noexcept;

  // Memory management
  [[nodiscard]] VoidResult set_address_space(shared_ptr<AddressSpace> as) noexcept;
  void clear_address_space() noexcept;
  // Retains this published version, which may differ from a CPU's installed
  // version until an explicit root switch. Publication does not rebind readers.
  // This is lifetime protection, not a VMA/PTE transaction lock or page pin.
  [[nodiscard]] shared_ptr<AddressSpace> address_space() const noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(address_space_lock_);
    return address_space_;
  }

  // Process state management
  void set_state(ProcessState new_state) noexcept;
  void set_exit_code(i32 code) noexcept { exit_code_ = code; }
  void set_exit_status(i32 code, u32 signal) noexcept {
    exit_code_ = code;
    terminating_signal_ = signal;
  }

  // Statistics update
  void update_cpu_time(u64 user_time, u64 kernel_time) noexcept;
  void record_context_switch(bool voluntary) noexcept;
  void record_page_fault(bool major) noexcept;

  // Allocate a globally unique thread ID (static atomic counter).
  // Public so that fork() and other kernel code can create threads directly.
  [[nodiscard]] static ThreadId allocate_thread_id() noexcept;

  // Register an externally-created thread into this process's thread list.
  // Used by fork() which builds a Thread manually instead of create_thread().
  // Ownership transfers only on success.
  [[nodiscard]] VoidResult register_thread(Thread *thread) noexcept;

  // ── Children tracking (for wait/waitpid) ──────────────────────────

  void add_child(ProcessId child_pid) { children_.push_front(child_pid); }

  [[nodiscard]] bool try_add_child(ProcessId child_pid) { return children_.try_push_front(child_pid); }

  void remove_child(ProcessId child_pid) { children_.remove(child_pid); }

  [[nodiscard]] bool has_children() const noexcept { return !children_.empty(); }

  // Find a zombie child matching wait_pid and the selected process group.
  // Returns PID of found zombie, or INVALID_PROCESS_ID if none.
  [[nodiscard]] ProcessId find_zombie_child(i64 wait_pid, ProcessId target_pgid) const noexcept;

  // Check if a specific PID is in this process's children list
  [[nodiscard]] bool is_child(ProcessId pid) const noexcept { return static_cast<bool>(children_.find(pid)); }

  // Access wait queue for child exit notification
  containers::WaitQueue &child_exit_wait_queue() noexcept { return child_exit_wq_; }

  // Iterate children (for reparenting in sys_exit)
  template <typename Func> void for_each_child(Func func) const { children_.for_each_snapshot(func); }

  // wait4 rechecks with IRQs masked; avoid snapshot allocation there. The
  // callback must not modify or reenter this child list.
  template <typename Func> void for_each_child_locked(Func func) const { children_.for_each(func); }

  // Parent PID setter (for reparenting)
  void set_parent_pid(ProcessId pid) noexcept { parent_pid_ = pid; }

  // VFS file descriptor table access (void* to avoid circular dependency)
  [[nodiscard]] void *fd_table() const noexcept { return fd_table_; }
  // Takes ownership of a heap-allocated vfs::FdTable (init/fork only).
  void set_fd_table(void *fdt) noexcept { fd_table_ = fdt; }
  void cleanup_files() noexcept;

private:
  void cleanup_threads() noexcept;
};

// Process manager
class ProcessManager {
private:
  containers::LockedHashMap<ProcessId, shared_ptr<Process>> processes_;
  containers::AtomicCounter<ProcessId> next_pid_;

  containers::PerCpuAtomicCounter<u64> total_context_switches_;
  containers::PerCpuAtomicCounter<u64> total_forks_;
  containers::PerCpuAtomicCounter<u64> total_exits_;

public:
  // PID zero is reserved for kernel/idle ownership; the first process is init (1).
  ProcessManager() noexcept : next_pid_(1) {}

  [[nodiscard]] KernelResult<shared_ptr<Process>>
  create_process(ProcessId parent_pid = INVALID_PROCESS_ID, shared_ptr<capability::Object> domain_scope = {}) noexcept;
  [[nodiscard]] VoidResult terminate_process(ProcessId pid, i32 exit_code) noexcept;

  [[nodiscard]] shared_ptr<Process> find_process(ProcessId pid) const noexcept;
  [[nodiscard]] bool process_exists(ProcessId pid) const noexcept;

  [[nodiscard]] u64 total_processes() const noexcept;
  [[nodiscard]] u64 total_context_switches() const noexcept { return total_context_switches_; }
  [[nodiscard]] u64 total_forks() const noexcept { return total_forks_; }
  [[nodiscard]] u64 total_exits() const noexcept { return total_exits_; }

  template <typename Func> void for_each_process(Func &&func) const {
    processes_.for_each_snapshot([&func](const auto &entry) { func(entry.key, entry.value.get()); });
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
[[nodiscard]] shared_ptr<Process> current_process() noexcept;

[[nodiscard]] inline u32 current_cpu() noexcept { return arch::get_current_cpu_id(); }

// Shared Zombie transition: tear down a dying process and hand control to the
// scheduler.  Called by both sys_exit (syscall) and terminate_current_user_process
// (fatal page fault bridge).  This function never returns.
//
// Preconditions:
//   - `cur` is the currently running thread (will be marked Terminated)
//   - `proc` is the Process owning `cur` (will transition to Zombie)
//   - Caller must have already validated cur/proc are non-null
// Fatal callers retain a shell-style 128 + signo diagnostic code but pass
// signo separately so wait4 does not mistake it for a normal exit code.
[[noreturn]] void do_exit(Thread *cur, shared_ptr<Process> proc, i32 exit_code, u32 terminating_signal = 0) noexcept;

// User address space management extensions
namespace user_space {

[[nodiscard]] KernelResult<shared_ptr<AddressSpace>> create_user_address_space() noexcept;

[[nodiscard]] VoidResult map_user_memory(AddressSpace *as, VirtAddr vaddr, PhysAddr paddr, usize size,
                                         u32 flags) noexcept;

[[nodiscard]] KernelResult<VirtAddr> allocate_user_heap(Process *process, usize size) noexcept;

} // namespace user_space

} // namespace moss::kernel::process
