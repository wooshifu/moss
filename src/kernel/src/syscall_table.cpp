// MOSS内核系统调用表实现
// 提供完整的系统调用处理和分发机制

module;

#include <moss/domain_spawn.h>
#include <moss/startup_auxv.h>

module moss.kernel;

import moss.abi;
import moss.vfs;

// Assembly symbols from moss.abi
using moss::abi::context_switch;
using moss::abi::switch_to_user;

// Only the validation image overrides this observation point. It can hold the
// caller after arming a real timer without replacing expiry or scheduling.
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_sleep_armed(void * /*unused*/) noexcept {}
// Validation may apply real heap pressure at a fork allocation boundary.
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_fork_metadata(unsigned /*unused*/,
                                                                           bool /*unused*/) noexcept {}
// Validation may apply real heap pressure at an exec preparation boundary.
// The observer never substitutes an allocation result in production.
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_exec_allocation(unsigned /*unused*/, bool /*unused*/,
                                                                             moss::kernel::usize /*unused*/) noexcept {}
// Validation can replace the published source between pathname and vector reads.
extern "C" [[gnu::weak, gnu::noinline]] void
moss_validation_exec_source_snapshot(moss::kernel::PhysAddr /*unused*/, moss::kernel::VirtAddr /*unused*/) noexcept {}
// Validation can force child exit after wait's first scan and before registration.
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_wait_before_register(moss::kernel::u32 /*parent_pid*/,
                                                                                  long /*wait_pid*/) noexcept {}
// Validation can replace a signal action after oldact is copied, before publication.
extern "C" [[gnu::weak, gnu::noinline]] void
moss_validation_sigaction_before_replace(moss::kernel::u32 /*pid*/, moss::kernel::u32 /*signo*/) noexcept {}

namespace moss::kernel::syscall {

// 全局系统调用统计
SyscallStats g_syscall_stats = {.total_syscalls = 0,
                                .successful_syscalls = 0,
                                .failed_syscalls = 0,
                                .unimplemented_syscalls = 0,
                                .invalid_syscalls = 0};

// 系统调用处理函数实现
namespace handlers {
namespace log = moss::kernel::logging;

namespace exec_allocation_stage {
// Keep these values synchronized with the validation override. Each stage
// surrounds one independently fallible owned resource in preparation order.
inline constexpr unsigned ARGUMENTS = 0;
inline constexpr unsigned MUTABLE_IMAGE_OBJECT = 1;
inline constexpr unsigned MUTABLE_IMAGE_CONTROL = 2;
inline constexpr unsigned MUTABLE_IMAGE_BYTES = 3;
inline constexpr unsigned ADDRESS_SPACE = 4;
inline constexpr unsigned VMA_NODE = 5;
} // namespace exec_allocation_stage

// ── User pointer validation (copy_from_user / copy_to_user) ────────────
//
// Shared process uaccess owns VMA admission and fault-contained copies.
// The syscall layer translates its failure to the Moss EFAULT return value.

/// Retain the calling process's published AddressSpace, or an empty owner.
static shared_ptr<process::AddressSpace> get_current_address_space() noexcept {
  using namespace moss::kernel::process;
  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return {};
  }
  auto proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : shared_ptr<Process>{};
  if (!proc) {
    return {};
  }
  return proc->address_space();
}

/// Check the user address domain and every VMA covering the complete range.
static bool validate_user_range(u64 user_addr, usize len, u32 required_flags) noexcept {
  if (len == 0) {
    return true;
  }
  auto as = get_current_address_space();
  return as && as->allows_user_access(user_addr, len, required_flags);
}

/// Copy `len` bytes from validated user address to a kernel buffer.
/// Returns 0 on success, -EFAULT if the range is invalid.
static long copy_from_user(void *kernel_dst, u64 user_src, usize len) noexcept {
  return process::copy_from_user(kernel_dst, user_src, len) == 0 ? 0 : -errc::EFAULT;
}

/// Copy `len` bytes from a kernel buffer to a validated user address.
/// Returns 0 on success, -EFAULT if the range is invalid.
static long copy_to_user(u64 user_dst, const void *kernel_src, usize len) noexcept {
  return process::copy_to_user(user_dst, kernel_src, len) == 0 ? 0 : -errc::EFAULT;
}

/// Copy a NUL-terminated string from user space into a kernel buffer.
/// Check each byte through the shared copy policy, including across VMAs.
/// No NUL within the bounded buffer is an error, not a truncated pathname.
static long copy_string_from_user(char *kernel_dst, u64 user_src, usize max_len,
                                  process::AddressSpace *source_as = nullptr) noexcept {
  if (!mm::PageTableManager::is_user_range(user_src, 1) || max_len == 0) {
    return -errc::EFAULT;
  }
  for (usize i = 0; i < max_len; ++i) {
    if (source_as ? source_as->copy_from_user(&kernel_dst[i], user_src + i, 1) != 0
                  : copy_from_user(&kernel_dst[i], user_src + i, 1) < 0) {
      return -errc::EFAULT;
    }
    if (kernel_dst[i] == '\0') {
      return 0;
    }
  }
  return -errc::ENAMETOOLONG;
}

// 基础系统调用处理函数
long sys_debug_print(long arg0, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                     long /*unused*/) noexcept {
  if (arg0 == 0) {
    return -errc::EINVAL;
  }
  // Keep diagnostic strings within a 256-byte stack snapshot including NUL.
  // The exact budget is unrecorded; oversized input fails rather than truncates.
  char buf[256];
  const long copied = copy_string_from_user(buf, static_cast<u64>(arg0), sizeof(buf));
  if (copied < 0) {
    return copied;
  }
  moss::kernel::hal::uart::puts(buf);
  return 0;
}

long sys_exit(long exit_code, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
              long /*unused*/) noexcept {
  using namespace moss::kernel::process;
  namespace log = moss::kernel::logging;

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    log::klog::error("sys_exit: no current thread");
    while (true) {
      ::moss::kernel::arch::cpu_yield();
    }
  }

  ProcessId pid = cur->owner_pid;
  auto proc = g_process_manager ? g_process_manager->find_process(pid) : shared_ptr<Process>{};
  if (!proc) {
    log::klog::error("sys_exit: process not found PID={}", pid);
    while (true) {
      ::moss::kernel::arch::cpu_yield();
    }
  }

  // Delegate to shared Zombie transition (never returns)
  do_exit(cur, moss::move(proc), static_cast<i32>(exit_code));
}

long sys_getpid(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                long /*unused*/) noexcept {
  using namespace moss::kernel::process;
  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return -errc::ESRCH;
  }
  return static_cast<long>(cur->owner_pid);
}

long sys_getppid(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                 long /*unused*/) noexcept {
  using namespace moss::kernel::process;
  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return -errc::ESRCH;
  }
  auto proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : shared_ptr<Process>{};
  if (!proc) {
    return -errc::ESRCH;
  }
  return static_cast<long>(proc->parent_pid());
}

long sys_getuid(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                long /*unused*/) noexcept {
  using namespace moss::kernel::process;
  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return 0;
  }
  auto proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : shared_ptr<Process>{};
  return proc ? static_cast<long>(proc->uid()) : 0;
}

long sys_getgid(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                long /*unused*/) noexcept {
  using namespace moss::kernel::process;
  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return 0;
  }
  auto proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : shared_ptr<Process>{};
  return proc ? static_cast<long>(proc->gid()) : 0;
}

long sys_geteuid(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                 long /*unused*/) noexcept {
  auto proc = process::current_process();
  return proc ? static_cast<long>(proc->euid()) : -errc::ESRCH;
}

long sys_getegid(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                 long /*unused*/) noexcept {
  auto proc = process::current_process();
  return proc ? static_cast<long>(proc->egid()) : -errc::ESRCH;
}

long sys_arch_prctl(long operation, long address, long /*unused*/, long /*unused*/, long /*unused*/,
                    long /*unused*/) noexcept {
#if defined(MOSS_ARCH_X64)
  auto *thread = process::CfsScheduler::get_current_task();
  if (!thread) {
    return -errc::ESRCH;
  }
  // ARCH_* operations and IA32_FS_BASE (0xC0000100) match the native x64
  // TLS contract. WRMSR/RDMSR split the 64-bit base into two 32-bit words.
  if (operation == 0x1002) { // ARCH_SET_FS
    auto base = static_cast<u64>(address);
    if (base != 0 && !mm::PageTableManager::is_user_range(base, 1)) {
      return -errc::EINVAL;
    }
    const bool restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();
    thread->context.fs_base = base;
    asm volatile("wrmsr" ::"c"(0xC0000100U), "a"(static_cast<u32>(base)), "d"(static_cast<u32>(base >> 32)) : "memory");
    if (restore_irqs) {
      arch::enable_interrupts();
    }
    return 0;
  }
  if (operation == 0x1003) { // ARCH_GET_FS
    u32 low = 0, high = 0;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000100U));
    u64 base = static_cast<u64>(low) | (static_cast<u64>(high) << 32);
    return copy_to_user(static_cast<u64>(address), &base, sizeof(base));
  }
  return -errc::EINVAL;
#else
  (void)operation;
  (void)address;
  return -errc::ENOSYS;
#endif
}

// fork() — create a child process with COW-shared address space.
// Child returns 0, parent returns child PID.
class DomainObject final : public capability::Object {
public:
  // Hold the specific process incarnation after its diagnostic PID can be
  // reused; an old capability must never address a later process by number.
  shared_ptr<process::Process> process;
  explicit DomainObject(shared_ptr<process::Process> target) noexcept
      : Object(capability::ObjectType::Domain), process(moss::move(target)) {}
};

class DomainScopeObject final : public capability::Object {
  moss::atomic<bool> closed_{false};

public:
  DomainScopeObject() noexcept : Object(capability::ObjectType::DomainScope) {}
  void close() noexcept { closed_.store(true); }
  [[nodiscard]] bool closed() const noexcept { return closed_.load(); }
};

class DomainFactoryObject final : public capability::Object {
public:
  DomainFactoryObject() noexcept : Object(capability::ObjectType::DomainFactory) {}
};

inline constexpr u32 kFullDomainRights = capability::rights::DOMAIN_TERMINATE | capability::rights::DOMAIN_INSPECT |
                                         capability::rights::DOMAIN_OBSERVE | capability::rights::DOMAIN_SIGNAL |
                                         capability::rights::TRANSFER | capability::rights::DUPLICATE;
inline constexpr u32 kFullDomainScopeRights =
    capability::rights::DOMAIN_SCOPE_ASSIGN | capability::rights::DOMAIN_SCOPE_TERMINATE |
    capability::rights::DOMAIN_SCOPE_INSPECT | capability::rights::TRANSFER | capability::rights::DUPLICATE;
inline constexpr u32 kFactoryRights =
    capability::rights::DOMAIN_SPAWN | capability::rights::TRANSFER | capability::rights::DUPLICATE;

struct DomainExitStatus {
  i32 code;
  u32 signal;
};
static_assert(sizeof(DomainExitStatus) == 8);

static long copy_domain_exit_status(const process::Process &target, u64 address) noexcept {
  const u32 signal = target.terminating_signal();
  const DomainExitStatus status{signal ? 0 : target.exit_code(), signal};
  return copy_to_user(address, &status, sizeof(status)) < 0 ? -errc::EFAULT : 0;
}

static long domain_cap_error(ErrorCode error) noexcept {
  if (error == ErrorCode::NotFound)
    return -errc::EBADF;
  if (error == ErrorCode::PermissionDenied)
    return -errc::EACCES;
  if (error == ErrorCode::ResourceExhausted)
    return -errc::EMFILE;
  if (error == ErrorCode::OutOfMemory)
    return -errc::ENOMEM;
  return -errc::EINVAL;
}

static long do_fork(u64 domain_cap_out_addr, const capability::ForkSelection *selected, usize selected_count,
                    bool inherit_marked, u64 scope_handle) noexcept {
  using namespace moss::kernel::process;
  namespace log = moss::kernel::logging;

  if (domain_cap_out_addr && !validate_user_range(domain_cap_out_addr, sizeof(Handle), vma_flags::WRITE)) {
    return -errc::EFAULT;
  }

  // 1. Get current thread and process
  Thread *parent_thread = CfsScheduler::get_current_task();
  if (!parent_thread) {
    log::klog::error("sys_fork: no current thread");
    return -errc::EAGAIN;
  }

  auto parent_proc =
      g_process_manager ? g_process_manager->find_process(parent_thread->owner_pid) : shared_ptr<Process>{};
  auto parent_as = parent_proc ? parent_proc->address_space() : shared_ptr<AddressSpace>{};
  if (!parent_as) {
    log::klog::error("sys_fork: no parent process or address space");
    return -errc::EAGAIN;
  }

  auto scope = parent_proc->domain_scope();
  if (scope_handle) {
    // Existing members cannot escape containment by selecting another scope.
    if (scope)
      return -errc::EACCES;
    auto selected_scope =
        parent_proc->capabilities().lookup(static_cast<Handle>(scope_handle), capability::ObjectType::DomainScope,
                                           capability::rights::DOMAIN_SCOPE_ASSIGN);
    if (!selected_scope)
      return domain_cap_error(selected_scope.error());
    scope = *selected_scope;
  }
  if (scope && static_cast<DomainScopeObject *>(scope.get())->closed())
    return -errc::EACCES;

  // The live entry owns the frame; never infer it from a stack-top offset.
  auto *frame = parent_thread->trap_frame;
  if (!frame || !frame->from_user()) {
    return -errc::EAGAIN;
  }
  const u64 user_pc = frame->pc;
  const u64 user_sp = frame->sp;

  // 3. Create child process
  // A native domain has no POSIX parent or wait status. The returned PID is
  // diagnostic; only the domain capability retains authority after exit.
  const bool native_domain = domain_cap_out_addr != 0;
  auto child_proc_result =
      g_process_manager->create_process(native_domain ? INVALID_PROCESS_ID : parent_proc->pid(), scope);
  if (!child_proc_result) {
    log::klog::error("sys_fork: create_process failed");
    return -errc::ENOMEM;
  }
  auto child_proc = *child_proc_result;
  child_proc->inherit_credentials(*parent_proc);

  // Helper: clean up the child process on error (removes from process
  // table and triggers ~Process which frees address space, threads, etc.)
  auto cleanup_child = [&](Process *cp) {
    if (!domain_cap_out_addr)
      parent_proc->remove_child(cp->pid());
    if (g_process_manager) {
      (void)g_process_manager->terminate_process(cp->pid(), -1);
    }
  };

  // 4. Create child address space (new PGD + ASID)
  auto child_as_result = user_space::create_user_address_space();
  if (!child_as_result) {
    log::klog::error("sys_fork: create_user_address_space failed");
    cleanup_child(child_proc.get());
    return -errc::ENOMEM;
  }
  auto child_as = moss::move(*child_as_result);

  // The child is unpublished. Hold only the parent's VM transaction through
  // PTE/ref changes, TLB invalidation and the matching VMA/cursor snapshot.
  // Release it before process-table cleanup or any later scheduling work.
  const auto cloned = [&]() -> VoidResult {
    auto transaction = parent_as->lock_vm();
    auto tables = mm::PageTableManager::clone_user_page_tables(parent_as->pgd_phys, child_as->pgd_phys);
    if (!tables) {
      return tables;
    }

    // clone_user_page_tables synchronously invalidated every CPU after its
    // COW commit. Do not reload a raw root here outside the ownership path.

    // 7. Copy VMAs from parent to child via LockedList iteration
    child_as->executable_image = parent_as->executable_image;
    bool vmas_copied = true;
    parent_as->vmas.for_each([&](const process::VmaRegion &vma) {
      if (!vmas_copied) {
        return;
      }
      moss_validation_fork_metadata(0, true);
      vmas_copied = child_as->add_vma(vma.start_addr, vma.end_addr, vma.flags, vma.type, vma.backing_data,
                                      vma.backing_offset, vma.backing_size, vma.memory_object, vma.shared_page);
      moss_validation_fork_metadata(0, false);
    });
    if (!vmas_copied) {
      return VoidResult{ErrorCode::OutOfMemory};
    }
    // The allocation cursors belong to the cloned address space too. Leaving
    // them zero breaks the first new mmap/brk allocation after fork.
    child_as->mmap_next = parent_as->mmap_next;
    child_as->brk_base = parent_as->brk_base;
    child_as->brk_current = parent_as->brk_current;
    return {};
  }();
  if (!cloned) {
    cleanup_child(child_proc.get());
    return cloned.error() == ErrorCode::OutOfMemory ? -errc::ENOMEM : -errc::EFAULT;
  }

  // 8. Bind address space to child process
  auto set_result = child_proc->set_address_space(moss::move(child_as));
  if (!set_result) {
    log::klog::error("sys_fork: set_address_space failed");
    // The by-value owner releases an uninstalled space on failure.
    cleanup_child(child_proc.get());
    return -errc::ENOMEM;
  }

  // 9. Create child thread
  ThreadId child_tid = Process::allocate_thread_id();
  moss_validation_fork_metadata(1, true);
  auto *child_thread = Thread::try_create(child_tid, child_proc->pid());
  moss_validation_fork_metadata(1, false);
  if (!child_thread) {
    log::klog::error("sys_fork: thread allocation failed");
    cleanup_child(child_proc.get());
    return -errc::ENOMEM;
  }

  // Convert user GP state to the scheduler's initial-return context. ARM64 x0
  // and RISC-V a0/x10 carry fork results; RISC-V x0 is absent from TrapFrame,
  // hence its GPR-to-CpuContext index offset of one.
  auto &context = child_thread->context;
#if defined(MOSS_ARCH_ARM64)
  for (u32 i = 0; i < moss::abi::TrapFrame::GPR_COUNT; ++i) {
    context.x[i] = frame->gpr(i);
  }
  context.x[0] = 0;
  asm volatile("mrs %0, tpidr_el0" : "=r"(context.tpidr_el0));
#elif defined(MOSS_ARCH_RISCV64)
  for (u32 i = 0; i < moss::abi::TrapFrame::GPR_COUNT; ++i) {
    context.x[i + 1] = frame->gpr(i);
  }
  context.x[10] = 0;
#elif defined(MOSS_ARCH_X64)
  // Kernel C++ does not use FP/SIMD; capture the caller's live state.
  asm volatile("fxsave64 %0" : "=m"(context.fp)::"memory");
  u32 fs_low = 0, fs_high = 0;
  asm volatile("rdmsr" : "=a"(fs_low), "=d"(fs_high) : "c"(0xC0000100U));
  context.fs_base = static_cast<u64>(fs_low) | (static_cast<u64>(fs_high) << 32);
  context.rbx = frame->rbx;
  context.rcx = frame->rcx;
  context.rdx = frame->rdx;
  context.rsi = frame->rsi;
  context.rdi = frame->rdi;
  context.rbp = frame->rbp;
  context.r8 = frame->r8;
  context.r9 = frame->r9;
  context.r10 = frame->r10;
  context.r11 = frame->r11;
  context.r12 = frame->r12;
  context.r13 = frame->r13;
  context.r14 = frame->r14;
  context.r15 = frame->r15;
  context.rax = 0;
#endif
  context.pc = user_pc;
  context.sp = user_sp;
  context.pstate = frame->user_status();

  child_thread->stack_base = parent_thread->stack_base;
  child_thread->stack_size = parent_thread->stack_size;
  child_thread->needs_initial_eret = true;
  child_thread->is_user_task = true;
  child_thread->sched_class = SchedClass::Normal;
  // A child inherits the configured nice value, not an IPC boost that may
  // temporarily change the parent's dispatch weight.
  child_thread->se.nice.store(parent_thread->se.nice.load());
  child_thread->se.weight.store(cfs_params::nice_to_weight(child_thread->se.nice.load()));
  child_thread->se.load_weight.store(child_thread->se.weight.load());
  child_thread->cpu_affinity_mask = parent_thread->cpu_affinity_mask;
  child_thread->state = ProcessState::Ready;

  // 11. Allocate per-thread kernel stack (16KB)
  auto kstack_result = child_thread->allocate_kernel_stack();
  if (!kstack_result) {
    log::klog::error("sys_fork: kernel stack alloc failed");
    delete child_thread;
    cleanup_child(child_proc.get());
    return -errc::ENOMEM;
  }
  // 12. Register child thread in child process's thread list
  moss_validation_fork_metadata(2, true);
  auto registered = child_proc->register_thread(child_thread);
  moss_validation_fork_metadata(2, false);
  if (!registered) {
    delete child_thread;
    cleanup_child(child_proc.get());
    return -errc::ENOMEM;
  }

  // 12b. Clone VFS fd table from parent to child
  if (parent_proc->fd_table() != nullptr) {
    auto *parent_fdt = static_cast<moss::kernel::vfs::FdTable *>(parent_proc->fd_table());
    auto *child_fdt = parent_fdt->clone();
    if (!child_fdt) {
      cleanup_child(child_proc.get());
      return -errc::ENOMEM;
    }
    child_proc->set_fd_table(child_fdt);
  }

  // Compatibility fork can use native domain authority while retaining only
  // capabilities the parent explicitly marked for inheritance.
  auto copied_caps = inherit_marked ? parent_proc->capabilities().clone_inheritable_to(child_proc->capabilities())
                                    : parent_proc->capabilities().clone_selected_to(child_proc->capabilities(),
                                                                                    selected, selected_count);
  if (!copied_caps) {
    cleanup_child(child_proc.get());
    return domain_cap_out_addr ? domain_cap_error(copied_caps.error()) : -errc::EAGAIN;
  }

  // 12c. Inherit process name from parent
  child_proc->set_name(parent_proc->name());

  // 12d. Inherit process group and session from parent (POSIX semantics)
  child_proc->set_pgid(parent_proc->pgid());
  child_proc->set_sid(parent_proc->sid());

  // Fork duplicates dispositions, mask and the user altstack/context, but not
  // pending notifications. The child's COW stack owns any active signal frame.
  child_proc->inherit_signal_actions_from(*parent_proc);
  child_thread->signal_mask = parent_thread->signal_mask.load();
  child_thread->alt_stack_sp = parent_thread->alt_stack_sp;
  child_thread->alt_stack_size = parent_thread->alt_stack_size;
  child_thread->alt_stack_flags = parent_thread->alt_stack_flags;
  child_thread->on_alt_stack = parent_thread->on_alt_stack;
  child_thread->active_signal_frame = parent_thread->active_signal_frame;

  // Only POSIX fork publishes a parent-child relationship for waitpid.
  if (!native_domain) {
    moss_validation_fork_metadata(3, true);
    const bool child_registered = parent_proc->try_add_child(child_proc->pid());
    moss_validation_fork_metadata(3, false);
    if (!child_registered) {
      cleanup_child(child_proc.get());
      return -errc::ENOMEM;
    }
  }

  Handle installed_domain_handle = 0;
  if (domain_cap_out_addr) {
    auto object =
        shared_ptr<capability::Object>::try_make<DomainObject>(moss::abi::bridge::moss_heap_allocate, child_proc);
    if (!object) {
      cleanup_child(child_proc.get());
      return -errc::ENOMEM;
    }
    auto handle = parent_proc->capabilities().install(moss::move(object), kFullDomainRights);
    if (!handle) {
      cleanup_child(child_proc.get());
      return domain_cap_error(handle.error());
    }
    installed_domain_handle = *handle;
    // The child is not runnable yet. If copying the handle faults, remove
    // both the authority and its unpublished child before returning.
    if (copy_to_user(domain_cap_out_addr, &*handle, sizeof(*handle)) < 0) {
      (void)parent_proc->capabilities().close(*handle);
      cleanup_child(child_proc.get());
      return -errc::EFAULT;
    }
  }

  // A close racing with fork must catch a child that was inserted after the
  // terminator's process-table scan but before it became runnable.
  if (scope && static_cast<DomainScopeObject *>(scope.get())->closed()) {
    if (installed_domain_handle)
      (void)parent_proc->capabilities().close(installed_domain_handle);
    cleanup_child(child_proc.get());
    return -errc::EACCES;
  }

  // 14. Enqueue child into scheduler (scatter across CPUs via load balancer)
  child_proc->set_state(ProcessState::Running);
  if (g_scheduler) {
    u32 target_cpu = arch::get_current_cpu_id();
    if (g_load_balancer) {
      target_cpu = g_load_balancer->select_cpu_for_task(child_thread, *g_scheduler);
    }
    // Place child vruntime: fork penalty so parent runs first (returns child PID)
    g_scheduler->place_entity(child_thread, target_cpu, /*is_fork=*/true);
    g_scheduler->enqueue_task(child_thread, target_cpu);
  }

  // 15. Parent returns child PID
  return static_cast<long>(child_proc->pid());
}

long sys_fork(long, long, long, long, long, long) noexcept { return do_fork(0, nullptr, 0, true, 0); }

long sys_fork_domain(long cap_out_addr, long, long, long, long, long) noexcept {
  return cap_out_addr ? do_fork(static_cast<u64>(cap_out_addr), nullptr, 0, false, 0) : -errc::EFAULT;
}

long sys_fork_domain_inherit(long cap_out_addr, long, long, long, long, long) noexcept {
  return cap_out_addr ? do_fork(static_cast<u64>(cap_out_addr), nullptr, 0, true, 0) : -errc::EFAULT;
}

static long fork_domain_selected(long cap_out_addr, long handles_addr, long count_arg, long scope_handle) noexcept {
  if (!cap_out_addr)
    return -errc::EFAULT;
  if (count_arg < 0 || static_cast<usize>(count_arg) > capability::Table::capacity())
    return -errc::EINVAL;
  capability::ForkSelection selected[capability::Table::capacity()]{};
  const auto count = static_cast<usize>(count_arg);
  if (count != 0 &&
      (!handles_addr || copy_from_user(selected, static_cast<u64>(handles_addr), count * sizeof(selected[0])) < 0))
    return -errc::EFAULT;
  return do_fork(static_cast<u64>(cap_out_addr), selected, count, false, static_cast<u64>(scope_handle));
}

long sys_fork_domain_select(long cap_out_addr, long handles_addr, long count_arg, long, long, long) noexcept {
  // Existing three-argument callers do not initialize a fourth syscall register.
  return fork_domain_selected(cap_out_addr, handles_addr, count_arg, 0);
}

long sys_fork_domain_scoped(long cap_out_addr, long handles_addr, long count_arg, long scope_handle, long,
                            long) noexcept {
  return scope_handle > 0 ? fork_domain_selected(cap_out_addr, handles_addr, count_arg, scope_handle) : -errc::EINVAL;
}

long sys_domain_id(long handle, long, long, long, long, long) noexcept {
  auto caller = process::current_process();
  if (!caller)
    return -errc::ESRCH;
  auto object = caller->capabilities().lookup(static_cast<Handle>(handle), capability::ObjectType::Domain,
                                              capability::rights::DOMAIN_INSPECT);
  if (!object)
    return domain_cap_error(object.error());
  return static_cast<long>(static_cast<DomainObject *>((*object).get())->process->pid());
}

long sys_domain_same(long left, long right, long, long, long, long) noexcept {
  auto caller = process::current_process();
  if (!caller)
    return -errc::ESRCH;
  auto first = caller->capabilities().lookup(static_cast<Handle>(left), capability::ObjectType::Domain,
                                             capability::rights::DOMAIN_INSPECT);
  if (!first)
    return domain_cap_error(first.error());
  auto second = caller->capabilities().lookup(static_cast<Handle>(right), capability::ObjectType::Domain,
                                              capability::rights::DOMAIN_INSPECT);
  if (!second)
    return domain_cap_error(second.error());
  // SYS_DOMAIN_SELF creates a fresh wrapper for each handle. Compare the
  // retained process incarnation, not wrapper identity or a reusable PID.
  const auto *left_domain = static_cast<DomainObject *>((*first).get())->process.get();
  const auto *right_domain = static_cast<DomainObject *>((*second).get())->process.get();
  return left_domain == right_domain ? 1 : 0;
}

long sys_domain_self(long, long, long, long, long, long) noexcept {
  auto caller = process::current_process();
  if (!caller)
    return -errc::ESRCH;
  // Ordinary do_exit clears the caller's table before publishing exit,
  // breaking this self-handle cycle. Initial-supervisor exit resets the system.
  auto object = shared_ptr<capability::Object>::try_make<DomainObject>(moss::abi::bridge::moss_heap_allocate, caller);
  if (!object)
    return -errc::ENOMEM;
  auto handle = caller->capabilities().install(moss::move(object), kFullDomainRights);
  return handle ? static_cast<long>(*handle) : domain_cap_error(handle.error());
}

long sys_domain_scope_create(long, long, long, long, long, long) noexcept {
  auto caller = process::current_process();
  if (!caller)
    return -errc::ESRCH;
  auto scope = shared_ptr<capability::Object>::try_make<DomainScopeObject>(moss::abi::bridge::moss_heap_allocate);
  if (!scope)
    return -errc::ENOMEM;
  auto handle = caller->capabilities().install(moss::move(scope), kFullDomainScopeRights);
  return handle ? static_cast<long>(*handle) : domain_cap_error(handle.error());
}

long sys_domain_scope_terminate(long handle, long, long, long, long, long) noexcept {
  auto caller = process::current_process();
  if (!caller)
    return -errc::ESRCH;
  auto object = caller->capabilities().lookup(static_cast<Handle>(handle), capability::ObjectType::DomainScope,
                                              capability::rights::DOMAIN_SCOPE_TERMINATE);
  if (!object)
    return domain_cap_error(object.error());
  auto *scope = static_cast<DomainScopeObject *>((*object).get());
  scope->close();
  // ponytail: recovery scans an O(process count) snapshot; track members per
  // scope only if recovery latency grows with the deployed process count.
  process::g_process_manager->for_each_process([&](ProcessId, process::Process *target) {
    if (target->domain_scope().get() != scope || target->state() == process::ProcessState::Terminated ||
        target->state() == process::ProcessState::Zombie)
      return;
    if (auto *thread = target->get_main_thread())
      (void)process::send_signal(thread, process::sig::SIGKILL);
  });
  return 0;
}

long sys_domain_scope_status(long handle, long, long, long, long, long) noexcept {
  auto caller = process::current_process();
  if (!caller)
    return -errc::ESRCH;
  auto object = caller->capabilities().lookup(static_cast<Handle>(handle), capability::ObjectType::DomainScope,
                                              capability::rights::DOMAIN_SCOPE_INSPECT);
  if (!object)
    return domain_cap_error(object.error());
  long live = 0;
  process::g_process_manager->for_each_process([&](ProcessId, process::Process *target) {
    if (target->domain_scope().get() == (*object).get() && target->state() != process::ProcessState::Terminated &&
        target->state() != process::ProcessState::Zombie)
      ++live;
  });
  return live;
}

long sys_domain_scope_contains(long scope_handle, long domain_handle, long, long, long, long) noexcept {
  auto caller = process::current_process();
  if (!caller)
    return -errc::ESRCH;
  auto scope = caller->capabilities().lookup(static_cast<Handle>(scope_handle), capability::ObjectType::DomainScope,
                                             capability::rights::DOMAIN_SCOPE_INSPECT);
  if (!scope)
    return domain_cap_error(scope.error());
  auto domain = caller->capabilities().lookup(static_cast<Handle>(domain_handle), capability::ObjectType::Domain,
                                              capability::rights::DOMAIN_INSPECT);
  if (!domain)
    return domain_cap_error(domain.error());
  auto *target = static_cast<DomainObject *>((*domain).get())->process.get();
  return target->domain_scope().get() == (*scope).get() ? 1 : 0;
}

long sys_domain_factory(long, long, long, long, long, long) noexcept {
  auto caller = process::current_process();
  if (!caller)
    return -errc::ESRCH;
  if (!caller->is_domain_factory_source())
    return -errc::EACCES;
  auto factory = shared_ptr<capability::Object>::try_make<DomainFactoryObject>(moss::abi::bridge::moss_heap_allocate);
  if (!factory)
    return -errc::ENOMEM;
  auto handle = caller->capabilities().install(moss::move(factory), kFactoryRights);
  return handle ? static_cast<long>(*handle) : domain_cap_error(handle.error());
}

long sys_domain_spawn(long factory_handle, long image_addr, long, long, long, long) noexcept {
  using namespace moss::kernel::process;
  static_assert(PAGE_SIZE == MOSS_DOMAIN_PAGE_BYTES);
  if (!image_addr)
    return -errc::EFAULT;
  auto caller = current_process();
  if (!caller || !g_process_manager || !g_scheduler)
    return -errc::ESRCH;
  auto factory = caller->capabilities().lookup(static_cast<Handle>(factory_handle),
                                               capability::ObjectType::DomainFactory, capability::rights::DOMAIN_SPAWN);
  if (!factory)
    return domain_cap_error(factory.error());
  // A delegated factory must not let a service create domains outside its
  // recovery scope.
  auto scope = caller->domain_scope();
  if (scope && static_cast<DomainScopeObject *>(scope.get())->closed())
    return -errc::EACCES;
  auto source_space = caller->address_space();
  if (!source_space)
    return -errc::ESRCH;
  // Image metadata, page bytes, and inherited-handle selection must all come
  // from one retained caller address-space version across concurrent exec.
  auto read_source = [&](void *destination, u64 address, usize size) {
    return source_space->copy_from_user(destination, address, size) == 0;
  };
  moss_domain_spawn image{};
  if (!read_source(&image, static_cast<u64>(image_addr), sizeof(image)))
    return -errc::EFAULT;

  // A single construction call is bounded until per-domain memory quotas are
  // available. The image still reaches 16 MiB without a kernel ELF parser.
  constexpr usize MAX_IMAGE_PAGES = 4096;
  if (!image.pages || image.page_count == 0 || image.page_count > MAX_IMAGE_PAGES ||
      image.capability_count > capability::Table::capacity() || (image.capability_count && !image.capabilities) ||
      image.stack_size > user_layout::STACK_SIZE || (image.stack_size && !image.stack_source) ||
      image.stack_pointer < user_layout::STACK_TOP - user_layout::STACK_SIZE ||
      image.stack_pointer >= user_layout::STACK_TOP || (image.stack_pointer & 7) != 0 ||
      image.stack_size > user_layout::STACK_TOP - image.stack_pointer)
    return -errc::EINVAL;
  if (image.pages > ~u64{0} - image.page_count * sizeof(moss_domain_page) ||
      (image.capability_count &&
       image.capabilities > ~u64{0} - image.capability_count * sizeof(capability::ForkSelection)) ||
      (image.stack_size && image.stack_source > ~u64{0} - image.stack_size))
    return -errc::EFAULT;

  capability::ForkSelection selected[capability::Table::capacity()]{};
  if (image.capability_count &&
      !read_source(selected, image.capabilities, image.capability_count * sizeof(selected[0])))
    return -errc::EFAULT;

  auto created = user_space::create_user_address_space();
  if (!created)
    return -errc::ENOMEM;
  auto space = moss::move(*created);
  const VirtAddr stack_bottom = user_layout::STACK_TOP - user_layout::STACK_SIZE;
  if (!space->add_vma(user_layout::SIGRETURN_PAGE, user_layout::SIGRETURN_PAGE + PAGE_SIZE,
                      vma_flags::READ | vma_flags::EXEC, VmaType::SIGRETURN, moss::abi::signal::trampoline(), 0,
                      moss::abi::signal::trampoline_size()) ||
      !space->add_vma(stack_bottom, user_layout::STACK_TOP, vma_flags::READ | vma_flags::WRITE | vma_flags::DEMAND_ZERO,
                      VmaType::STACK) ||
      !space->add_vma(user_layout::HEAP_START, user_layout::HEAP_START,
                      vma_flags::READ | vma_flags::WRITE | vma_flags::DEMAND_ZERO, VmaType::HEAP))
    return -errc::ENOMEM;
  space->brk_base = space->brk_current = user_layout::HEAP_START;
  space->mmap_next = user_layout::MMAP_BASE;

  // Every page is private to the new address space. If any copy or page-table
  // allocation fails, its owner frees all previously mapped pages on return.
  auto install_page = [&](VirtAddr address, u32 flags, VmaType type, u64 source, usize count, usize destination_offset,
                          bool add_region) -> long {
    auto frame = mm::allocate_pages(0);
    if (!frame)
      return -errc::ENOMEM;
    auto *bytes = reinterpret_cast<u8 *>(phys_to_virt(*frame));
    __builtin_memset(bytes, 0, PAGE_SIZE);
    if (count && !read_source(bytes + destination_offset, source, count)) {
      (void)mm::free_pages(*frame, 0);
      return -errc::EFAULT;
    }
#if defined(MOSS_ARCH_ARM64)
    if (flags & vma_flags::EXEC) {
      // Direct-map writes must reach the point of coherency before another
      // CPU's first user dispatch invalidates its instruction cache.
      u64 ctr = 0;
      asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
      const usize line_bytes = usize{4} << ((ctr >> 16) & 0xf);
      for (usize offset = 0; offset < PAGE_SIZE; offset += line_bytes)
        arch::flush_cache_line(phys_to_virt(*frame) + offset);
      arch::data_sync_barrier();
    }
#endif
    if (add_region && !space->add_vma(address, address + PAGE_SIZE, flags, type)) {
      (void)mm::free_pages(*frame, 0);
      return -errc::ENOMEM;
    }
    const u64 permissions = (flags & vma_flags::WRITE)  ? hal::mmu::page_perms::USER_RW
                            : (flags & vma_flags::EXEC) ? hal::mmu::page_perms::USER_RX
                                                        : hal::mmu::page_perms::USER_RO;
    if (!mm::PageTableManager::map_user_page(space->pgd_phys, address, *frame, permissions)) {
      if (add_region)
        (void)space->remove_vma(address, address + PAGE_SIZE);
      (void)mm::free_pages(*frame, 0);
      return -errc::ENOMEM;
    }
    (void)space->resident_pages.fetch_add(1, containers::MemoryOrder::Relaxed);
    return 0;
  };

  bool entry_in_image = false;
  for (usize i = 0; i < image.page_count; ++i) {
    moss_domain_page page{};
    if (!read_source(&page, image.pages + i * sizeof(page), sizeof(page)))
      return -errc::EFAULT;
    constexpr u64 allowed = MOSS_DOMAIN_PAGE_READ | MOSS_DOMAIN_PAGE_WRITE | MOSS_DOMAIN_PAGE_EXEC;
    if ((page.address & (PAGE_SIZE - 1)) != 0 || !mm::PageTableManager::is_user_range(page.address, PAGE_SIZE) ||
        (page.flags & ~allowed) != 0 || (page.flags & MOSS_DOMAIN_PAGE_READ) == 0 ||
        (page.flags & (MOSS_DOMAIN_PAGE_WRITE | MOSS_DOMAIN_PAGE_EXEC)) ==
            (MOSS_DOMAIN_PAGE_WRITE | MOSS_DOMAIN_PAGE_EXEC) ||
        page.size > PAGE_SIZE || (page.size && (!page.source || page.source > ~u64{0} - page.size)) ||
        (page.address >= user_layout::HEAP_START && page.address < user_layout::HEAP_START + user_layout::HEAP_INIT) ||
        space->find_vma(page.address))
      return -errc::EINVAL;
    const u32 flags = vma_flags::READ | ((page.flags & MOSS_DOMAIN_PAGE_WRITE) ? vma_flags::WRITE : 0U) |
                      ((page.flags & MOSS_DOMAIN_PAGE_EXEC) ? vma_flags::EXEC : 0U);
    if (page.flags & MOSS_DOMAIN_PAGE_EXEC)
      entry_in_image |= image.entry >= page.address && image.entry - page.address < PAGE_SIZE;
    if (long error = install_page(page.address, flags, (flags & vma_flags::EXEC) ? VmaType::CODE : VmaType::DATA,
                                  page.source, static_cast<usize>(page.size), 0, true);
        error < 0)
      return error;
  }
  if (!entry_in_image)
    return -errc::EINVAL;

  const VirtAddr stack_end = image.stack_pointer + image.stack_size;
  for (VirtAddr address = image.stack_pointer & ~(VirtAddr{PAGE_SIZE} - 1); address < stack_end; address += PAGE_SIZE) {
    const VirtAddr begin = address > image.stack_pointer ? address : image.stack_pointer;
    const VirtAddr end = address + PAGE_SIZE < stack_end ? address + PAGE_SIZE : stack_end;
    if (long error =
            install_page(address, vma_flags::READ | vma_flags::WRITE, VmaType::STACK,
                         image.stack_source + begin - image.stack_pointer, end - begin, begin - address, false);
        error < 0)
      return error;
  }

  auto created_process = g_process_manager->create_process(INVALID_PROCESS_ID, scope);
  if (!created_process)
    return -errc::ENOMEM;
  auto child = *created_process;
  auto rollback = [&](long error) -> long {
    (void)g_process_manager->terminate_process(child->pid(), -1);
    return error;
  };
  if (!child->set_address_space(space))
    return rollback(-errc::ENOMEM);
  const ThreadId tid = Process::allocate_thread_id();
  if (tid == INVALID_THREAD_ID)
    return rollback(-errc::EAGAIN);
  auto *thread = Thread::try_create(tid, child->pid());
  if (!thread)
    return rollback(-errc::ENOMEM);
  thread->context.pc = image.entry;
  thread->context.sp = image.stack_pointer;
#if defined(MOSS_ARCH_ARM64)
  thread->context.x[0] = image.arg0;
  thread->context.x[1] = image.arg1;
  thread->context.x[2] = image.arg2;
#elif defined(MOSS_ARCH_RISCV64)
  thread->context.x[10] = image.arg0;
  thread->context.x[11] = image.arg1;
  thread->context.x[12] = image.arg2;
#elif defined(MOSS_ARCH_X64)
  thread->context.rdi = image.arg0;
  thread->context.rsi = image.arg1;
  thread->context.rdx = image.arg2;
  thread->context.pstate = 0x202; // RFLAGS bit 1 is required; bit 9 enables user interrupts.
#endif
  thread->stack_base = stack_bottom;
  thread->stack_size = user_layout::STACK_SIZE;
  thread->needs_initial_eret = true;
  thread->is_user_task = true;
  thread->state = ProcessState::Ready;
  if (!thread->allocate_kernel_stack() || !child->register_thread(thread)) {
    delete thread;
    return rollback(-errc::ENOMEM);
  }
  auto copied = caller->capabilities().clone_selected_to(child->capabilities(), selected, image.capability_count);
  if (!copied)
    return rollback(domain_cap_error(copied.error()));
  auto object = shared_ptr<capability::Object>::try_make<DomainObject>(moss::abi::bridge::moss_heap_allocate, child);
  if (!object)
    return rollback(-errc::ENOMEM);
  auto handle = caller->capabilities().install(moss::move(object), kFullDomainRights);
  if (!handle)
    return rollback(domain_cap_error(handle.error()));
  // Scope closure can pass its process-table scan during image preparation.
  // Recheck after insertion so a late child cannot escape that recovery unit.
  if (scope && static_cast<DomainScopeObject *>(scope.get())->closed()) {
    (void)caller->capabilities().close(*handle);
    return rollback(-errc::EACCES);
  }

  // After authority publication no preparation may fail. The child has no
  // POSIX parent; its diagnostic PID retires on exit while the handle remains.
  child->set_state(ProcessState::Running);
  const u32 cpu =
      g_load_balancer ? g_load_balancer->select_cpu_for_task(thread, *g_scheduler) : arch::get_current_cpu_id();
  g_scheduler->place_entity(thread, cpu, /*is_fork=*/true);
  g_scheduler->enqueue_task(thread, cpu);
  return static_cast<long>(*handle);
}

long sys_domain_layout(long layout_addr, long, long, long, long, long) noexcept {
  if (!layout_addr)
    return -errc::EFAULT;
  const moss_domain_layout layout{PAGE_SIZE,
                                  mm::PageTableManager::KERNEL_IDENTITY_END,
                                  USER_MAX,
                                  process::user_layout::STACK_TOP,
                                  process::user_layout::STACK_SIZE,
                                  process::user_layout::STACK_MAX,
                                  process::user_layout::HEAP_START,
                                  process::user_layout::HEAP_INIT,
                                  process::user_layout::SIGRETURN_PAGE,
                                  process::user_layout::MMAP_BASE};
  return copy_to_user(static_cast<u64>(layout_addr), &layout, sizeof(layout));
}

long sys_domain_terminate(long handle, long, long, long, long, long) noexcept {
  auto caller = process::current_process();
  if (!caller)
    return -errc::ESRCH;
  auto object = caller->capabilities().lookup(static_cast<Handle>(handle), capability::ObjectType::Domain,
                                              capability::rights::DOMAIN_TERMINATE);
  if (!object)
    return domain_cap_error(object.error());
  auto &target = *static_cast<DomainObject *>((*object).get())->process;
  if (target.state() != process::ProcessState::Running)
    return -errc::ESRCH; // Retired domains cannot accept new control requests.
  auto *thread = target.get_main_thread();
  if (!thread)
    return -errc::ESRCH;
  // The current single-thread process implementation uses uncatchable SIGKILL
  // as its exit wakeup; authorization comes solely from this domain handle.
  return process::send_signal(thread, process::sig::SIGKILL) ? 0 : -errc::ESRCH;
}

long sys_domain_signal(long handle, long signo, long, long, long, long) noexcept {
  if (signo < 0 || signo >= process::sig::NSIG)
    return -errc::EINVAL;
  auto caller = process::current_process();
  if (!caller)
    return -errc::ESRCH;
  auto object = caller->capabilities().lookup(static_cast<Handle>(handle), capability::ObjectType::Domain,
                                              capability::rights::DOMAIN_SIGNAL);
  if (!object)
    return domain_cap_error(object.error());
  auto &target = *static_cast<DomainObject *>((*object).get())->process;
  if (target.state() != process::ProcessState::Running)
    return -errc::ESRCH;
  auto *thread = target.get_main_thread();
  if (!thread)
    return -errc::ESRCH;
  return signo == 0 || process::send_signal(thread, static_cast<u32>(signo)) ? 0 : -errc::ESRCH;
}

long sys_domain_wait(long handle, long, long, long, long, long) noexcept {
  auto caller = process::current_process();
  if (!caller)
    return -errc::ESRCH;
  auto object = caller->capabilities().lookup(static_cast<Handle>(handle), capability::ObjectType::Domain,
                                              capability::rights::DOMAIN_OBSERVE);
  if (!object)
    return domain_cap_error(object.error());
  auto &target = *static_cast<DomainObject *>((*object).get())->process;
  if (&target == caller.get())
    return -errc::EINVAL;
  auto *cur = process::CfsScheduler::get_current_task();
  if (!cur || !process::g_scheduler)
    return -errc::ESRCH;
  auto exited = [&] {
    const auto state = target.state();
    return state == process::ProcessState::Zombie || state == process::ProcessState::Terminated;
  };
  while (!exited()) {
    if (moss::abi::bridge::moss_io_wait_interrupted())
      return -errc::EINTR;
    // Prepare before registration, then recheck after registration: exit on
    // another CPU must neither miss this waiter nor wake it before handoff.
    const bool restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();
    (void)moss::abi::bridge::moss_prepare_io_wait();
    target.domain_exit_wait_queue().add_waiter(static_cast<void *>(cur));
    if (exited())
      process::g_scheduler->task_wakeup(cur, cur->wake_cpu);
    process::g_scheduler->commit_sleep();
    target.domain_exit_wait_queue().remove_waiter(static_cast<void *>(cur));
    if (restore_irqs)
      arch::enable_interrupts();
  }
  return 0;
}

long sys_domain_wait_any(long handles_addr, long count_arg, long, long, long, long) noexcept {
  if (count_arg <= 0 || static_cast<usize>(count_arg) > capability::Table::capacity())
    return -errc::EINVAL;
  if (!handles_addr)
    return -errc::EFAULT;
  const usize count = static_cast<usize>(count_arg);
  Handle handles[capability::Table::capacity()]{};
  if (copy_from_user(handles, static_cast<u64>(handles_addr), count * sizeof(handles[0])) < 0)
    return -errc::EFAULT;

  auto caller = process::current_process();
  auto *cur = process::CfsScheduler::get_current_task();
  if (!caller || !cur || !process::g_scheduler)
    return -errc::ESRCH;
  shared_ptr<process::Process> targets[capability::Table::capacity()]{};
  for (usize i = 0; i < count; ++i) {
    auto object =
        caller->capabilities().lookup(handles[i], capability::ObjectType::Domain, capability::rights::DOMAIN_OBSERVE);
    if (!object)
      return domain_cap_error(object.error());
    auto target = static_cast<DomainObject *>((*object).get())->process;
    if (target.get() == caller.get())
      return -errc::EINVAL;
    // Aliased handles would register this sleeper twice on one queue; each
    // result index must name a distinct domain.
    for (usize j = 0; j < i; ++j) {
      if (targets[j].get() == target.get())
        return -errc::EINVAL;
    }
    targets[i] = moss::move(target);
  }

  auto exited_index = [&]() -> usize {
    for (usize i = 0; i < count; ++i) {
      const auto state = targets[i]->state();
      if (state == process::ProcessState::Zombie || state == process::ProcessState::Terminated)
        return i;
    }
    return count;
  };
  for (;;) {
    const usize exited = exited_index();
    if (exited != count)
      return static_cast<long>(exited);
    if (moss::abi::bridge::moss_io_wait_interrupted())
      return -errc::EINTR;

    usize registered = 0;
    for (; registered < count; ++registered) {
      if (!targets[registered]->domain_exit_wait_queue().try_add_waiter(static_cast<void *>(cur))) {
        for (usize i = 0; i < registered; ++i)
          targets[i]->domain_exit_wait_queue().remove_waiter(static_cast<void *>(cur));
        return -errc::ENOMEM;
      }
    }
    // Register before preparing sleep, then recheck after preparation. Exits
    // before preparation are observed by the recheck; later wakeups use the
    // scheduler's prepared-sleeper handoff.
    const bool restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();
    (void)moss::abi::bridge::moss_prepare_io_wait();
    if (exited_index() != count)
      process::g_scheduler->task_wakeup(cur, cur->wake_cpu);
    process::g_scheduler->commit_sleep();
    if (restore_irqs)
      arch::enable_interrupts();
    for (usize i = 0; i < count; ++i)
      targets[i]->domain_exit_wait_queue().remove_waiter(static_cast<void *>(cur));
  }
}

// Exit status has its own syscall: older wait callers leave unused argument
// registers unspecified, so extending their argument list breaks the ABI.
long sys_domain_status(long handle, long status_addr, long, long, long, long) noexcept {
  if (!status_addr)
    return -errc::EFAULT;
  auto caller = process::current_process();
  if (!caller)
    return -errc::ESRCH;
  auto object = caller->capabilities().lookup(static_cast<Handle>(handle), capability::ObjectType::Domain,
                                              capability::rights::DOMAIN_OBSERVE);
  if (!object)
    return domain_cap_error(object.error());
  auto &target = *static_cast<DomainObject *>((*object).get())->process;
  const auto state = target.state();
  if (state != process::ProcessState::Zombie && state != process::ProcessState::Terminated)
    return -errc::EAGAIN;
  return copy_domain_exit_status(target, static_cast<u64>(status_addr));
}

static long do_execve(long pathname_addr, long argv_addr, long envp_addr, Handle startup_cap) noexcept {
  using namespace moss::kernel::process;
  using namespace moss::kernel::elf;
  Thread *cur = g_scheduler ? CfsScheduler::get_current_task() : nullptr;
  auto proc = cur && g_process_manager ? g_process_manager->find_process(cur->owner_pid) : shared_ptr<Process>{};
  auto source_as = proc ? proc->address_space() : shared_ptr<AddressSpace>{};
  if (!source_as) {
    return -errc::ESRCH;
  }
  if (!proc->try_begin_exec()) {
    return -errc::EAGAIN;
  }
  struct ExecReservation {
    Process *owner;
    ~ExecReservation() noexcept {
      if (owner) {
        owner->finish_exec();
      }
    }
    void finish() noexcept {
      owner->finish_exec();
      owner = nullptr;
    }
  } reservation{proc.get()};

  // All exec inputs must come from one published version, even if another
  // thread replaces the Process address space during preparation.
  // Exec retains its narrower 256-byte path limit (including NUL), separate
  // from the VFS full-path limit; its exact original sizing is unrecorded.
  char pathname[256];
  if (long error = copy_string_from_user(pathname, static_cast<u64>(pathname_addr), sizeof(pathname), source_as.get());
      error < 0) {
    return error;
  }
  moss_validation_exec_source_snapshot(source_as->pgd_phys, static_cast<VirtAddr>(argv_addr));

  // Bounded native exec contract: argv + envp together have at most 128
  // strings and 16 KiB of bytes including their terminators. Never truncate.
  // These are Moss limits, not POSIX ARG_MAX; exact sizing evidence is
  // unrecorded. Increasing them changes snapshot memory and startup stack use.
  constexpr usize MAX_STRINGS = 128;
  constexpr usize STRING_BYTES = 16384;
  struct Arguments {
    char strings[STRING_BYTES]{};
    usize offsets[MAX_STRINGS]{};
    // The selected startup capability adds one auxiliary tag/value pair.
    u64 vector[MAX_STRINGS + 7]{};
    LoadPlan load_plan{};
    usize count{}, used{}, argc{};
  };
  // The syscall kernel stack is only 16 KiB; keep the snapshot in owned heap memory.
  moss_validation_exec_allocation(exec_allocation_stage::ARGUMENTS, true, sizeof(Arguments));
  auto args_storage = mm::RuntimeHeapAllocator::allocate_aligned(sizeof(Arguments), alignof(Arguments));
  moss_validation_exec_allocation(exec_allocation_stage::ARGUMENTS, false, sizeof(Arguments));
  if (!args_storage) {
    return -errc::ENOMEM;
  }
  // Ordinary new panics on kernel-heap OOM; placement construction preserves
  // exec's recoverable contract, and unique_ptr uses the compatible delete path.
  unique_ptr<Arguments> args{new (*args_storage) Arguments{}};
  auto capture = [&](long address) -> long {
    if (!address) {
      return 0;
    }
    for (usize index = 0;; ++index) {
      const u64 base = static_cast<u64>(address);
      const usize offset = index * sizeof(u64);
      if (base > ~u64{0} - offset) {
        return -errc::EFAULT;
      }
      u64 pointer = 0;
      if (source_as->copy_from_user(&pointer, base + offset, sizeof(pointer)) != 0) {
        return -errc::EFAULT;
      }
      if (!pointer) {
        return 0;
      }
      if (args->count == MAX_STRINGS || args->used == STRING_BYTES) {
        return -errc::E2BIG;
      }
      const long error =
          copy_string_from_user(args->strings + args->used, pointer, STRING_BYTES - args->used, source_as.get());
      if (error < 0) {
        return error == -errc::ENAMETOOLONG ? -errc::E2BIG : error;
      }
      args->offsets[args->count++] = args->used;
      while (args->strings[args->used++]) {
      }
    }
  };
  if (long error = capture(argv_addr); error < 0) {
    return error;
  }
  args->argc = args->count;
  if (long error = capture(envp_addr); error < 0) {
    return error;
  }
  source_as.reset();

  shared_ptr<ExecutableImage> image;
  const u8 *image_data;
  usize image_size;
  {
    containers::LockGuard<containers::IrqSpinLock> guard(vfs::namespace_lock);
    auto *files = static_cast<vfs::FdTable *>(proc->fd_table());
    vfs::VfsError error = vfs::VfsError::NoEntry;
    auto *dentry = vfs::resolve_path_locked(pathname, &error, files ? files->working_directory() : nullptr,
                                            proc->euid(), proc->egid());
    if (!dentry || !dentry->inode) {
      return -static_cast<long>(error);
    }
    auto *inode = dentry->inode;
    if (inode->type != vfs::FileType::Regular || !vfs::can_access(*inode, proc->euid(), proc->egid(), 1)) {
      return -errc::EACCES;
    }
    if (!inode->data || !inode->size) {
      return -errc::ENOEXEC;
    }
    image_data = inode->data;
    image_size = inode->size;
    if (inode->ramfs_mutable) {
      // ponytail: snapshot mutable executables once; use shared file pages if
      // copying large images becomes costly. Immutable CPIO stays borrowed.
      bool allocating_image_object = true;
      image = shared_ptr<ExecutableImage>::try_make([&](usize bytes, usize alignment) -> void * {
        // SharedPtr owns two independent allocations. Expose each boundary so
        // validation can prove that a missing control block releases the object.
        const unsigned stage = allocating_image_object ? exec_allocation_stage::MUTABLE_IMAGE_OBJECT
                                                       : exec_allocation_stage::MUTABLE_IMAGE_CONTROL;
        allocating_image_object = false;
        moss_validation_exec_allocation(stage, true, bytes);
        auto storage = mm::RuntimeHeapAllocator::allocate_aligned(bytes, alignment);
        moss_validation_exec_allocation(stage, false, bytes);
        return storage ? *storage : nullptr;
      });
      if (!image) {
        return -errc::ENOMEM;
      }
      moss_validation_exec_allocation(exec_allocation_stage::MUTABLE_IMAGE_BYTES, true, image_size);
      auto allocation = mm::RuntimeHeapAllocator::allocate(image_size);
      moss_validation_exec_allocation(exec_allocation_stage::MUTABLE_IMAGE_BYTES, false, image_size);
      if (!allocation) {
        return -errc::ENOMEM;
      }
      image->data = static_cast<u8 *>(*allocation);
      image->size = image_size;
      __builtin_memcpy(image->data, image_data, image_size);
      image_data = image->data;
    }
  }
  const VirtAddr stack_bottom = user_layout::STACK_TOP - user_layout::STACK_SIZE;
  const LoadRange reserved_ranges[] = {
      {.start = user_layout::SIGRETURN_PAGE, .end = user_layout::SIGRETURN_PAGE + PAGE_SIZE},
      {.start = stack_bottom, .end = user_layout::STACK_TOP},
      {.start = user_layout::HEAP_START, .end = user_layout::HEAP_START + user_layout::HEAP_INIT},
  };
  const LoadPlanPolicy load_policy = {
      .page_size = PAGE_SIZE,
      .user_begin = mm::PageTableManager::KERNEL_IDENTITY_END,
      .user_end = USER_MAX,
      .reserved = reserved_ranges,
      .reserved_count = sizeof(reserved_ranges) / sizeof(reserved_ranges[0]),
  };
  if (!build_load_plan(image_data, image_size, load_policy, args->load_plan)) {
    return -errc::ENOEXEC;
  }
  // From this point onward the plan is immutable: validation and VMA backing
  // intervals cannot diverge through a second round of ELF arithmetic.
  const LoadPlan &load_plan = args->load_plan;

  moss_validation_exec_allocation(exec_allocation_stage::ADDRESS_SPACE, true, sizeof(AddressSpace));
  auto created = user_space::create_user_address_space();
  moss_validation_exec_allocation(exec_allocation_stage::ADDRESS_SPACE, false, sizeof(AddressSpace));
  if (!created) {
    return -errc::ENOMEM;
  }
  auto prepared = moss::move(*created);
  prepared->executable_image = image;
  auto add_prepared_vma = [&](VirtAddr start, VirtAddr end, u32 flags, VmaType type, const u8 *backing,
                              usize backing_offset, usize backing_size) {
    moss_validation_exec_allocation(exec_allocation_stage::VMA_NODE, true, sizeof(VmaRegion));
    const bool added = prepared->add_vma(start, end, flags, type, backing, backing_offset, backing_size);
    moss_validation_exec_allocation(exec_allocation_stage::VMA_NODE, false, sizeof(VmaRegion));
    return added;
  };
  for (usize i = 0; i < load_plan.segment_count; ++i) {
    const auto &segment = load_plan.segments[i];
    u32 flags = segment.backing_size ? 0 : vma_flags::DEMAND_ZERO;
    if (segment.flags & PF_R) {
      flags |= vma_flags::READ;
    }
    if (segment.flags & PF_W) {
      flags |= vma_flags::WRITE;
    }
    if (segment.flags & PF_X) {
      flags |= vma_flags::EXEC;
    }
    const u8 *backing = segment.backing_size ? image_data + segment.backing_offset : nullptr;
    if (!add_prepared_vma(segment.page_start, segment.page_end, flags,
                          segment.flags & PF_X ? VmaType::CODE : VmaType::DATA, backing, 0, segment.backing_size)) {
      return -errc::ENOMEM;
    }
  }
  if (!add_prepared_vma(user_layout::SIGRETURN_PAGE, user_layout::SIGRETURN_PAGE + PAGE_SIZE,
                        vma_flags::READ | vma_flags::EXEC, VmaType::SIGRETURN, moss::abi::signal::trampoline(), 0,
                        moss::abi::signal::trampoline_size()) ||
      !add_prepared_vma(stack_bottom, user_layout::STACK_TOP,
                        vma_flags::READ | vma_flags::WRITE | vma_flags::DEMAND_ZERO, VmaType::STACK, nullptr, 0, 0) ||
      !add_prepared_vma(user_layout::HEAP_START, user_layout::HEAP_START,
                        vma_flags::READ | vma_flags::WRITE | vma_flags::DEMAND_ZERO, VmaType::HEAP, nullptr, 0, 0)) {
    return -errc::ENOMEM;
  }
  prepared->brk_base = prepared->brk_current = user_layout::HEAP_START;
  prepared->mmap_next = user_layout::MMAP_BASE;

  // Native C entry registers point into one canonical startup vector. The
  // optional handle is auxiliary metadata, so a caller's envp stays exact.
  const usize argc = args->argc;
  // Leave 16 bytes below the exclusive stack top; 7/15 are alignment masks
  // for 8-byte pointer words and the native 16-byte startup stack boundary.
  const VirtAddr strings_base = (user_layout::STACK_TOP - 16 - args->used) & ~VirtAddr{7};
  const usize vector_bytes = (args->count + 5 + (startup_cap ? 2 : 0)) * sizeof(u64);
  const VirtAddr vector_base = (strings_base - vector_bytes) & ~VirtAddr{15};
  const VirtAddr argv_base = vector_base + sizeof(u64);
  const VirtAddr envp_base = argv_base + (argc + 1) * sizeof(u64);
  VirtAddr user_sp = vector_base;
#if defined(MOSS_ARCH_X64)
  user_sp -= sizeof(u64); // Synthetic return slot: native C entry requires RSP % 16 == 8.
#endif
  if (user_sp < stack_bottom) {
    return -errc::E2BIG;
  }
  args->vector[0] = argc;
  for (usize i = 0; i < args->count; ++i) {
    args->vector[1 + i + (i >= argc ? 1 : 0)] = strings_base + args->offsets[i];
  }
  if (startup_cap) {
    args->vector[args->count + 3] = MOSS_AT_STARTUP_CAP;
    args->vector[args->count + 4] = startup_cap;
  }

  // Populate inactive stack pages through the kernel's physical mapping.
  // Failure frees only this prepared image; the caller remains runnable.
  for (VirtAddr va = user_sp & ~(VirtAddr{PAGE_SIZE} - 1); va < user_layout::STACK_TOP; va += PAGE_SIZE) {
    auto page = mm::allocate_pages(0);
    if (!page) {
      return -errc::ENOMEM;
    }
    __builtin_memset(reinterpret_cast<void *>(phys_to_virt(*page)), 0, PAGE_SIZE);
    if (!mm::PageTableManager::map_user_page(prepared->pgd_phys, va, *page, hal::mmu::page_perms::USER_RW)) {
      (void)mm::free_pages(*page, 0);
      return -errc::ENOMEM;
    }
    (void)prepared->resident_pages.fetch_add(1, containers::MemoryOrder::Relaxed);
  }
  auto write_stack = [&](VirtAddr address, const void *source, usize size) {
    const auto *bytes = static_cast<const u8 *>(source);
    while (size) {
      auto *pte = mm::PageTableManager::get_user_pte(prepared->pgd_phys, address);
      if (!pte || !pte->is_valid()) {
        return false;
      }
      const usize offset = address & (PAGE_SIZE - 1);
      const usize chunk = size < PAGE_SIZE - offset ? size : PAGE_SIZE - offset;
      __builtin_memcpy(reinterpret_cast<void *>(phys_to_virt(pte->get_phys_addr()) + offset), bytes, chunk);
      address += chunk;
      bytes += chunk;
      size -= chunk;
    }
    return true;
  };
  if (!write_stack(strings_base, args->strings, args->used) || !write_stack(vector_base, args->vector, vector_bytes)) {
    return -errc::EFAULT;
  }

  // This is the last fallible step. A single-threaded exec owns its table,
  // and the selected handle must survive close_uninheritable at commit.
  if (startup_cap) {
    auto kept = proc->capabilities().set_keep_on_exec(startup_cap, true);
    if (!kept)
      return domain_cap_error(kept.error());
  }

  // Commit: no remaining fallible preparation. IRQs stay masked while the
  // local hardware root and Process ownership change before user return.
  // Other CPUs retain their installed versions independently; the old tree is
  // freed only after its final software reader and hardware-root owner leave.
  arch::disable_interrupts();
  CfsScheduler::use_address_space(prepared);
  (void)proc->set_address_space(moss::move(prepared)); // Non-null ownership transfer cannot fail.
  if (auto *files = static_cast<vfs::FdTable *>(proc->fd_table())) {
    files->close_on_exec();
  }
  proc->capabilities().close_uninheritable();
  const char *basename = pathname;
  for (const char *p = pathname; *p; ++p) {
    if (*p == '/') {
      basename = p + 1;
    }
  }
  proc->set_name(basename);
  proc->reset_signal_actions_for_exec();
  cur->alt_stack_sp = cur->alt_stack_size = 0;
  cur->alt_stack_flags = ss_flags::SS_DISABLE;
  cur->on_alt_stack = false;
  cur->active_signal_frame = 0;
  cur->trap_frame = nullptr;
  cur->context = CpuContext{};
  cur->context.pc = load_plan.entry;
  cur->context.sp = user_sp;
#if defined(MOSS_ARCH_ARM64)
  cur->context.x[0] = argc;
  cur->context.x[1] = argv_base;
  cur->context.x[2] = envp_base;
  if (cur->kernel_stack_base) {
    asm volatile("msr tpidr_el1, %0" ::"r"(cur->kernel_stack_top()));
  }
#elif defined(MOSS_ARCH_RISCV64)
  cur->context.x[10] = argc;
  cur->context.x[11] = argv_base;
  cur->context.x[12] = envp_base;
  if (cur->kernel_stack_base) {
    arch::set_user_kernel_stack(cur->kernel_stack_top());
  }
#elif defined(MOSS_ARCH_X64)
  cur->context.rdi = argc;
  cur->context.rsi = argv_base;
  cur->context.rdx = envp_base;
  cur->context.pstate = 0x202; // RFLAGS bit 1 is required; bit 9 enables user interrupts.
  if (cur->kernel_stack_base) {
    moss::abi::x64::set_kernel_stack(cur->kernel_stack_top());
  }
#endif
  cur->needs_initial_eret = false;
  cur->state = ProcessState::Running;
  cur->stack_base = stack_bottom;
  cur->stack_size = user_layout::STACK_SIZE;
  // switch_to_user abandons this kernel frame; its destructor will not run.
  reservation.finish();
  args.reset();
  proc.reset();
  switch_to_user(&cur->context, cur->context.sp);
  while (true) {
    ::moss::kernel::arch::cpu_halt();
  }
}

long sys_execve(long pathname_addr, long argv_addr, long envp_addr, long, long, long) noexcept {
  return do_execve(pathname_addr, argv_addr, envp_addr, 0);
}

long sys_execve_cap(long pathname_addr, long argv_addr, long envp_addr, long startup_cap, long, long) noexcept {
  return startup_cap > 0 ? do_execve(pathname_addr, argv_addr, envp_addr, static_cast<Handle>(startup_cap))
                         : -errc::EBADF;
}

// wait4(pid, wstatus, options, rusage) — wait for child process state change
// pid > 0: specific child; -1: any child; 0: caller's process group;
// pid < -1: children in process group -pid.
// options use the Linux-compatible abi-bits/wait.h values.
long sys_wait4(long wait_pid, long wstatus_addr, long options, long /*unused*/, long /*unused*/,
               long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  constexpr long WNOHANG = 1;
  constexpr long WUNTRACED = 2;
  constexpr long WCONTINUED = 8;
  if ((options & ~(WNOHANG | WUNTRACED | WCONTINUED)) != 0) {
    return -errc::EINVAL;
  }
  // Bound both signs before negating a process-group selector (including LONG_MIN).
  constexpr long MAX_PID = static_cast<long>(~ProcessId{0});
  if (wait_pid > MAX_PID || wait_pid < -MAX_PID) {
    return -errc::ECHILD;
  }

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return -errc::EINVAL;
  }

  auto proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : shared_ptr<Process>{};
  if (!proc) {
    return -errc::EINVAL;
  }

  // Resolve pid==0 once so every scan during this wait uses the same group.
  const ProcessId target_pgid = wait_pid == 0 ? proc->pgid() : (wait_pid < -1 ? static_cast<ProcessId>(-wait_pid) : 0);
  auto matches_child = [&](ProcessId child_pid, const Process &child) {
    if (wait_pid == -1) {
      return true;
    }
    if (wait_pid > 0) {
      return static_cast<ProcessId>(wait_pid) == child_pid;
    }
    return child.pgid() == target_pgid;
  };
  auto has_matching_child = [&]() {
    bool found = false;
    proc->for_each_child_locked([&](ProcessId child_pid) {
      if (found) {
        return;
      }
      auto child = g_process_manager->find_process(child_pid);
      found = child && matches_child(child_pid, *child);
    });
    return found;
  };

  struct JobEvent {
    shared_ptr<Process> child;
    Thread *thread = nullptr;
    u64 value = 0;
  };
  auto find_job_event = [&]() -> JobEvent {
    JobEvent found{};
    if ((options & (WUNTRACED | WCONTINUED)) == 0) {
      return found;
    }
    proc->for_each_child_locked([&](ProcessId child_pid) {
      if (found.thread) {
        return;
      }
      auto child = g_process_manager->find_process(child_pid);
      if (!child || child->state() == ProcessState::Zombie || !matches_child(child_pid, *child)) {
        return;
      }
      auto *thread = child->get_main_thread();
      const u64 event = thread ? thread->wait_status_event.load() : 0;
      const u32 status = static_cast<u32>(event);
      if (((options & WUNTRACED) && (status & 0xff) == 0x7f) || ((options & WCONTINUED) && status == 0xffff)) {
        // Retain the process so another waiter cannot free this thread during
        // reservation and copyout.
        found = {child, thread, event};
      }
    });
    return found;
  };

  while (true) {
    // Scan for matching zombie child
    ProcessId zombie_pid = proc->find_zombie_child(wait_pid, target_pgid);

    if (zombie_pid != INVALID_PROCESS_ID) {
      // Found a zombie — reap it
      auto zombie = g_process_manager->find_process(zombie_pid);
      if (!zombie || !matches_child(zombie_pid, *zombie)) {
        // Another waiter may have reaped it, or it may have changed groups.
        continue;
      }

      i32 child_exit_code = zombie->exit_code();
      ProcessId result_pid = zombie->pid();

      // Write the published wait status only after Zombie is observed.
      if (wstatus_addr != 0) {
        int wstatus = zombie->wait_status();
        if (copy_to_user(static_cast<u64>(wstatus_addr), &wstatus, sizeof(wstatus)) < 0) {
          return -errc::EFAULT;
        }
      }

      // Commit reaping only after copyout succeeds. EFAULT must leave the
      // exit status available for a retry with a valid userspace destination.
      proc->remove_child(zombie_pid);
      (void)g_process_manager->terminate_process(zombie_pid, child_exit_code);

      return static_cast<long>(result_pid);
    }

    if (auto job = find_job_event(); job.thread) {
      const u64 cleared = job.value & ~Thread::WAIT_STATUS_MASK;
      u64 expected = job.value;
      // Reserve before copyout so a competing waiter cannot report the same
      // event, and a failed consume cannot leave an unreported user status.
      if (!job.thread->wait_status_event.compare_exchange_strong(expected, cleared)) {
        continue;
      }
      const int status = static_cast<int>(static_cast<u32>(job.value));
      if (wstatus_addr != 0 && copy_to_user(static_cast<u64>(wstatus_addr), &status, sizeof(status)) < 0) {
        expected = cleared;
        if (job.thread->wait_status_event.compare_exchange_strong(expected, job.value)) {
          // Another waiter may have slept while this event was reserved.
          proc->child_exit_wait_queue().wake_up([](void *waiting) {
            auto *task = static_cast<Thread *>(waiting);
            if (g_scheduler) {
              g_scheduler->task_wakeup(task, task->wake_cpu);
            }
          });
        }
        return -errc::EFAULT;
      }
      return static_cast<long>(job.child->pid());
    }

    // Other children do not keep a wait for this selected set alive.
    if (!has_matching_child()) {
      return -errc::ECHILD;
    }

    // WNOHANG: non-blocking, return 0
    if (options & WNOHANG) {
      return 0;
    }

    if (moss::abi::bridge::moss_io_wait_interrupted()) {
      return -errc::EINTR;
    }

    if (!g_scheduler) {
      return -errc::ESRCH;
    }

    moss_validation_wait_before_register(proc->pid(), wait_pid);
    // Publish a prepared sleeper before registering it. An exit on another
    // CPU must not enqueue us until bootstrap owns our saved context.
    const bool restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();
    // The shared helper self-wakes if a signal arrived after the check above.
    (void)moss::abi::bridge::moss_prepare_io_wait();
    proc->child_exit_wait_queue().add_waiter(static_cast<void *>(cur), /*exclusive=*/true);
    // An exit may have happened after the first scan but before registration.
    // Recheck after publishing the waiter so neither side can miss the other.
    if (proc->find_zombie_child(wait_pid, target_pgid) != INVALID_PROCESS_ID || find_job_event().thread ||
        !has_matching_child()) {
      g_scheduler->task_wakeup(cur, cur->wake_cpu);
    }
    g_scheduler->commit_sleep();
    proc->child_exit_wait_queue().remove_waiter(static_cast<void *>(cur));
    if (restore_irqs) {
      arch::enable_interrupts();
    }

  }
}

// waitpid(pid, wstatus, options) — thin wrapper over wait4
long sys_waitpid(long pid, long wstatus, long options, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  return sys_wait4(pid, wstatus, options, 0, 0, 0);
}

static bool pid_signal_requires_capability(const process::Process &target, ProcessId caller_pid) noexcept {
  // A parentless native domain has no POSIX peer authority. Its own ordinary
  // fork children retain parent-directed signals until the compatibility
  // service owns that relationship. PID 1 has no such exception: losing the
  // initial supervisor resets the whole system.
  return target.parent_pid() == INVALID_PROCESS_ID && target.pid() != caller_pid &&
         (target.is_initial_supervisor() || !target.is_child(caller_pid));
}

// kill(pid, sig) — send signal to process.
//   pid > 0:  send to one POSIX-visible process
//   pid == 0: send to POSIX-visible members of the caller's group
//   pid == -1: send to all POSIX-visible processes (except init) — simplified
//   pid < -1: send to POSIX-visible members of group |pid|
long sys_kill(long pid_arg, long sig_arg, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  if (sig_arg < 0 || sig_arg >= sig::NSIG) {
    return -errc::EINVAL;
  }
  auto signo = static_cast<u32>(sig_arg);

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur || !g_process_manager || !g_scheduler) {
    return -errc::ESRCH;
  }

  auto pid = static_cast<i64>(pid_arg);
  // ProcessId is u32. Reject truncating aliases (and LONG_MIN before negation).
  if (pid > 0xffffffffLL || pid < -0xffffffffLL) {
    return -errc::ESRCH;
  }

  if (pid > 0) {
    // Send to specific process
    auto target = g_process_manager->find_process(static_cast<ProcessId>(pid));
    if (!target) {
      return -errc::ESRCH;
    }
    if (pid_signal_requires_capability(*target, cur->owner_pid)) {
      return -errc::EPERM;
    }
    Thread *main_thread = target->get_main_thread();
    if (!main_thread) {
      return -errc::ESRCH;
    }
    if (signo == 0) {
      return 0; // Existence probe, after target lookup and without wakeup.
    }
    if (!send_signal(main_thread, signo)) {
      return -errc::EPERM;
    }
    return 0;
  }

  if (pid == 0) {
    // Send to all processes in caller's process group
    auto caller = g_process_manager->find_process(cur->owner_pid);
    if (!caller) {
      return -errc::ESRCH;
    }
    ProcessId my_pgid = caller->pgid();
    bool sent = false;
    g_process_manager->for_each_process([&](ProcessId, Process *proc) {
      if (proc->pgid() == my_pgid && !pid_signal_requires_capability(*proc, cur->owner_pid)) {
        Thread *thr = proc->get_main_thread();
        if (thr && (signo == 0 || send_signal(thr, signo))) {
          sent = true;
        }
      }
    });
    return sent ? 0 : -errc::ESRCH;
  }

  if (pid < -1) {
    // Send to process group |pid|
    auto target_pgid = static_cast<ProcessId>(-pid);
    bool sent = false;
    g_process_manager->for_each_process([&](ProcessId, Process *proc) {
      if (proc->pgid() == target_pgid && !pid_signal_requires_capability(*proc, cur->owner_pid)) {
        Thread *thr = proc->get_main_thread();
        if (thr && (signo == 0 || send_signal(thr, signo))) {
          sent = true;
        }
      }
    });
    return sent ? 0 : -errc::ESRCH;
  }

  // pid == -1: send to all POSIX-visible processes except init.
  bool sent = false;
  g_process_manager->for_each_process([&](ProcessId proc_pid, Process *proc) {
    if (proc_pid <= 1 || pid_signal_requires_capability(*proc, cur->owner_pid)) {
      return;
    }
    Thread *thr = proc->get_main_thread();
    if (thr && (signo == 0 || send_signal(thr, signo))) {
      sent = true;
    }
  });
  return sent ? 0 : -errc::ESRCH;
}

// ── Process group / session syscalls (POSIX job control) ──────────

// getpgid(pid) — returns process group ID.
// pid==0 means "calling process".
long sys_getpgid(long pid_arg, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                 long /*unused*/) noexcept {
  using namespace moss::kernel::process;
  Thread *cur = CfsScheduler::get_current_task();
  if (!cur || !g_process_manager) {
    return -errc::ESRCH;
  }

  ProcessId target = (pid_arg == 0) ? cur->owner_pid : static_cast<ProcessId>(pid_arg);
  auto proc = g_process_manager->find_process(target);
  if (!proc) {
    return -errc::ESRCH;
  }
  return static_cast<long>(proc->pgid());
}

// getpgrp() — equivalent to getpgid(0).
long sys_getpgrp(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                 long /*unused*/) noexcept {
  return sys_getpgid(0, 0, 0, 0, 0, 0);
}

// getsid(pid) — returns session ID.
// pid==0 means "calling process".
long sys_getsid(long pid_arg, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                long /*unused*/) noexcept {
  using namespace moss::kernel::process;
  Thread *cur = CfsScheduler::get_current_task();
  if (!cur || !g_process_manager) {
    return -errc::ESRCH;
  }

  ProcessId target = (pid_arg == 0) ? cur->owner_pid : static_cast<ProcessId>(pid_arg);
  auto proc = g_process_manager->find_process(target);
  if (!proc) {
    return -errc::ESRCH;
  }
  return static_cast<long>(proc->sid());
}

static void change_process_group(process::Process &target, ProcessId pgid) noexcept {
  if (target.pgid() == pgid) {
    return;
  }
  target.set_pgid(pgid);
  auto parent = process::g_process_manager->find_process(target.parent_pid());
  if (!parent) {
    return;
  }
  // Every group-selecting waiter must recheck whether it still has a matching child.
  parent->child_exit_wait_queue().for_each_waiter([](void *waiting) {
    auto *thread = static_cast<process::Thread *>(waiting);
    if (process::g_scheduler) {
      process::g_scheduler->task_wakeup(thread, thread->wake_cpu);
    }
  });
}

// setpgid(pid, pgid) — set process group of `pid` to `pgid`.
// pid==0 → calling process;  pgid==0 → use pid as new pgid.
// POSIX restrictions: can only set own or child's pgid, child must not
// have called execve, and target pgid must exist in caller's session.
// Simplified: allow setting own or child's pgid within same session.
long sys_setpgid(long pid_arg, long pgid_arg, long /*unused*/, long /*unused*/, long /*unused*/,
                 long /*unused*/) noexcept {
  using namespace moss::kernel::process;
  Thread *cur = CfsScheduler::get_current_task();
  if (!cur || !g_process_manager) {
    return -errc::ESRCH;
  }

  auto caller = g_process_manager->find_process(cur->owner_pid);
  if (!caller) {
    return -errc::ESRCH;
  }

  ProcessId target_pid = (pid_arg == 0) ? cur->owner_pid : static_cast<ProcessId>(pid_arg);
  ProcessId new_pgid = (pgid_arg == 0) ? target_pid : static_cast<ProcessId>(pgid_arg);

  auto target = g_process_manager->find_process(target_pid);
  if (!target) {
    return -errc::ESRCH;
  }

  // Must be self or a child
  if (target_pid != cur->owner_pid && !caller->is_child(target_pid)) {
    return -errc::ESRCH;
  }

  // Must be in the same session
  if (target->sid() != caller->sid()) {
    return -errc::EPERM;
  }

  change_process_group(*target, new_pgid);
  return 0;
}

// setpgrp() — equivalent to setpgid(0, 0).
long sys_setpgrp(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                 long /*unused*/) noexcept {
  return sys_setpgid(0, 0, 0, 0, 0, 0);
}

// setsid() — create a new session.
// Fails with EPERM if the caller is already a process group leader.
long sys_setsid(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                long /*unused*/) noexcept {
  using namespace moss::kernel::process;
  Thread *cur = CfsScheduler::get_current_task();
  if (!cur || !g_process_manager) {
    return -errc::ESRCH;
  }

  auto proc = g_process_manager->find_process(cur->owner_pid);
  if (!proc) {
    return -errc::ESRCH;
  }

  // POSIX: cannot setsid() if already a process group leader (pgid == pid)
  // Exception: allow if also session leader (already own session)
  if (proc->pgid() == proc->pid() && proc->sid() != proc->pid()) {
    return -errc::EPERM;
  }

  // Become session leader and process group leader
  proc->set_sid(proc->pid());
  change_process_group(*proc, proc->pid());
  return static_cast<long>(proc->sid());
}

// ── Signal handling syscalls ─────────────────────────────────────

// User-space sigaction structure (must match userspace/syscall.h layout)
struct UserSigaction {
  unsigned long handler; // function pointer or SIG_DFL(0)/SIG_IGN(1)
  unsigned long mask;    // signals to block during handler
  unsigned long flags;   // SA_RESTART etc.
};

// sigaction(signo, act, oldact) — set signal handler.
// act: pointer to UserSigaction (or nullptr to query only)
// oldact: pointer to receive old action (or nullptr)
long sys_sigaction(long sig_arg, long act_addr, long oldact_addr, long /*unused*/, long /*unused*/,
                   long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  if (sig_arg <= 0 || sig_arg >= sig::NSIG) {
    return -errc::EINVAL;
  }
  auto signo = static_cast<u32>(sig_arg);

  // Cannot change SIGKILL or SIGSTOP handlers
  if (signo == sig::SIGKILL || signo == sig::SIGSTOP) {
    return -errc::EINVAL;
  }

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur || !g_process_manager) {
    return -errc::ESRCH;
  }

  auto proc = g_process_manager->find_process(cur->owner_pid);
  if (!proc) {
    return -errc::ESRCH;
  }

  Sigaction desired{};
  if (act_addr != 0) {
    UserSigaction kact;
    if (copy_from_user(&kact, static_cast<u64>(act_addr), sizeof(kact)) < 0) {
      return -errc::EFAULT;
    }
    if ((kact.flags & ~static_cast<u64>(sa_flags::SA_ONSTACK | sa_flags::SA_RESTART | sa_flags::SA_NOCLDSTOP |
                                        sa_flags::SA_NOCLDWAIT)) != 0) {
      return -errc::EINVAL;
    }
    desired = Sigaction{static_cast<VirtAddr>(kact.handler), kact.mask & ~sig::UNCATCHABLE_MASK,
                        static_cast<u32>(kact.flags)};
  }

  for (;;) {
    const Sigaction previous = proc->signal_action(signo);
    if (oldact_addr != 0) {
      const UserSigaction kold{previous.handler, previous.mask, previous.flags};
      if (copy_to_user(static_cast<u64>(oldact_addr), &kold, sizeof(kold)) < 0) {
        return -errc::EFAULT;
      }
    }
    if (act_addr == 0) {
      return 0;
    }
    if (oldact_addr != 0) {
      moss_validation_sigaction_before_replace(proc->pid(), signo);
    }
    // User copy cannot run under the IRQ spinlock. Retry if another writer
    // changed the action so oldact always describes the action we replace.
    if (proc->try_replace_signal_action(signo, previous, desired)) {
      return 0;
    }
  }
}

// sigprocmask(how, set, oldset) — modify thread's signal mask.
// how: 0=SIG_BLOCK, 1=SIG_UNBLOCK, 2=SIG_SETMASK
long sys_sigprocmask(long how, long set_addr, long oldset_addr, long /*unused*/, long /*unused*/,
                     long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return -errc::ESRCH;
  }

  // Return old mask if requested
  if (oldset_addr != 0) {
    u64 old_mask = cur->signal_mask;
    if (copy_to_user(static_cast<u64>(oldset_addr), &old_mask, sizeof(old_mask)) < 0) {
      return -errc::EFAULT;
    }
  }

  // Modify mask if set is provided
  if (set_addr != 0) {
    u64 new_set = 0;
    if (copy_from_user(&new_set, static_cast<u64>(set_addr), sizeof(new_set)) < 0) {
      return -errc::EFAULT;
    }
    // SIGKILL and SIGSTOP can never be blocked
    new_set &= ~sig::UNCATCHABLE_MASK;

    switch (how) {
    case 0: // SIG_BLOCK
      cur->signal_mask |= new_set;
      break;
    case 1: // SIG_UNBLOCK
      cur->signal_mask &= ~new_set;
      break;
    case 2: // SIG_SETMASK
      cur->signal_mask = new_set;
      break;
    default:
      return -errc::EINVAL;
    }
  }

  return 0;
}

// sigreturn() — restore interrupted context from signal frame on user stack.
long sys_sigreturn(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                   long /*unused*/) noexcept {
  using namespace moss::kernel::process;
  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return -errc::EFAULT;
  }
  return do_sigreturn(cur);
}

// sigaltstack(ss, old_ss) — set/query alternate signal stack.
// ss:     pointer to UserStack describing new alternate stack (or 0 to query only)
// old_ss: pointer to receive current alternate stack state (or 0 to skip)

// User-space sigaltstack structure
struct UserStack {
  unsigned long ss_sp;
  unsigned long ss_size;
  unsigned long ss_flags;
};

long sys_sigaltstack(long ss_addr, long old_ss_addr, long /*unused*/, long /*unused*/, long /*unused*/,
                     long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return -errc::ESRCH;
  }

  // Return current altstack if requested
  if (old_ss_addr != 0) {
    UserStack old_ss;
    old_ss.ss_sp = cur->alt_stack_sp;
    old_ss.ss_size = cur->alt_stack_size;
    old_ss.ss_flags = cur->alt_stack_flags;
    if (cur->on_alt_stack) {
      old_ss.ss_flags |= ss_flags::SS_ONSTACK;
    }
    if (copy_to_user(static_cast<u64>(old_ss_addr), &old_ss, sizeof(old_ss)) < 0) {
      return -errc::EFAULT;
    }
  }

  // Set new altstack if provided
  if (ss_addr != 0) {
    // Cannot change altstack while executing on it
    if (cur->on_alt_stack) {
      return -errc::EPERM;
    }
    UserStack new_ss;
    if (copy_from_user(&new_ss, static_cast<u64>(ss_addr), sizeof(new_ss)) < 0) {
      return -errc::EFAULT;
    }
    if ((new_ss.ss_flags & ~static_cast<u64>(ss_flags::SS_DISABLE)) != 0) {
      return -errc::EINVAL;
    }
    if (new_ss.ss_flags & ss_flags::SS_DISABLE) {
      cur->alt_stack_sp = 0;
      cur->alt_stack_size = 0;
      cur->alt_stack_flags = ss_flags::SS_DISABLE;
    } else {
      // Native admission floor of 2048 bytes (MINSIGSTKSZ); it is not a guarantee
      // that arbitrary handler locals or nested signal frames fit this stack.
      if (new_ss.ss_size < 2048) { // MINSIGSTKSZ
        return -errc::ENOMEM;
      }
      if (!validate_user_range(new_ss.ss_sp, new_ss.ss_size, vma_flags::WRITE)) {
        return -errc::EFAULT;
      }
      cur->alt_stack_sp = static_cast<VirtAddr>(new_ss.ss_sp);
      cur->alt_stack_size = static_cast<usize>(new_ss.ss_size);
      cur->alt_stack_flags = 0;
    }
  }

  return 0;
}

// ── VFS-backed file system calls ─────────────────────────────────

/// Helper: get the calling process's VFS fd_table (void*).
static void *get_current_fd_table() noexcept {
  using namespace moss::kernel::process;
  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return nullptr;
  }
  auto proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : shared_ptr<Process>{};
  return proc ? proc->fd_table() : nullptr;
}

long sys_open(long pathname_addr, long flags, long mode, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  void *fdt = get_current_fd_table();
  if (!fdt) {
    return -errc::EBADF;
  }

  if (flags < 0 || static_cast<u64>(flags) > 0xffffffffULL) {
    return -errc::EINVAL;
  }
  // The third argument is absent for open(path, flags) without O_CREAT.
  if (flags & vfs::O_CREAT) {
    if (mode < 0 || static_cast<u64>(mode) > 0xffffffffULL) {
      return -errc::EINVAL;
    }
  } else {
    mode = 0;
  }
  char path_buf[vfs::MAX_PATH_LEN];
  const long copied = copy_string_from_user(path_buf, static_cast<u64>(pathname_addr), sizeof(path_buf));
  if (copied < 0) {
    return copied;
  }

  auto proc = process::current_process();
  if (!proc) {
    return -errc::ESRCH;
  }
  return moss::kernel::vfs::syscall::do_open(fdt, path_buf, static_cast<u32>(flags), static_cast<u32>(mode),
                                             proc->euid(), proc->egid());
}

long sys_close(long fd, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  void *fdt = get_current_fd_table();
  if (!fdt) {
    return -errc::EBADF;
  }

  return moss::kernel::vfs::syscall::do_close(fdt, fd);
}

long sys_read(long fd, long buf_addr, long count, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  using namespace moss::kernel::process;
  void *fdt = get_current_fd_table();
  if (!fdt) {
    return -errc::EBADF;
  }

  if (buf_addr == 0 || count <= 0) {
    return -errc::EINVAL;
  }

  // Validate user buffer is writable before passing to VFS layer
  if (!validate_user_range(static_cast<u64>(buf_addr), static_cast<usize>(count), vma_flags::WRITE)) {
    return -errc::EFAULT;
  }

  auto buffer = moss::kernel::vfs::OutputBuffer::user(static_cast<u64>(buf_addr), static_cast<usize>(count),
                                                      process::copy_to_user);
  return moss::kernel::vfs::syscall::do_read(fdt, fd, buffer);
}

long sys_write(long fd, long buf_addr, long count, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  using namespace moss::kernel::process;
  void *fdt = get_current_fd_table();
  if (!fdt) {
    return -errc::EBADF;
  }

  if (buf_addr == 0 || count <= 0) {
    return -errc::EINVAL;
  }

  // Validate user buffer is readable before passing to VFS layer
  if (!validate_user_range(static_cast<u64>(buf_addr), static_cast<usize>(count), vma_flags::READ)) {
    return -errc::EFAULT;
  }

  auto buffer = moss::kernel::vfs::InputBuffer::user(static_cast<u64>(buf_addr), static_cast<usize>(count),
                                                     process::copy_from_user);
  return moss::kernel::vfs::syscall::do_write(fdt, fd, buffer);
}

// ── Additional VFS syscalls (dup, dup2, pipe, lseek, fstat) ────

long sys_lseek(long fd, long offset, long whence, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  if (whence < 0 || whence > 2) {
    return -errc::EINVAL;
  }
  void *fdt = get_current_fd_table();
  if (!fdt) {
    return -errc::EBADF;
  }
  return moss::kernel::vfs::syscall::do_lseek(fdt, fd, static_cast<i64>(offset), static_cast<u32>(whence));
}

long sys_getdents(long fd, long buffer, long size, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  if (size < static_cast<long>(sizeof(vfs::DirEntry))) {
    return -errc::EINVAL;
  }
  if (!validate_user_range(static_cast<u64>(buffer), sizeof(vfs::DirEntry), process::vma_flags::WRITE)) {
    return -errc::EFAULT;
  }
  return vfs::syscall::do_getdents(
      get_current_fd_table(), fd,
      vfs::OutputBuffer::user(static_cast<u64>(buffer), sizeof(vfs::DirEntry), process::copy_to_user));
}

long sys_stat(long path_addr, long stat_addr, long /*unused*/, long /*unused*/, long /*unused*/,
              long /*unused*/) noexcept {
  char path[vfs::MAX_PATH_LEN];
  if (long error = copy_string_from_user(path, static_cast<u64>(path_addr), sizeof(path)); error < 0) {
    return error;
  }
  vfs::Stat status{};
  auto proc = process::current_process();
  if (!proc) {
    return -errc::ESRCH;
  }
  long result = vfs::syscall::do_stat(path, &status, proc->fd_table(), proc->euid(), proc->egid());
  return result < 0 ? result : copy_to_user(static_cast<u64>(stat_addr), &status, sizeof(status));
}

long sys_getcwd(long buffer, long size, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  if (size <= 0) {
    return -errc::EINVAL;
  }
  return vfs::syscall::do_getcwd(
      get_current_fd_table(),
      vfs::OutputBuffer::user(static_cast<u64>(buffer), static_cast<usize>(size), process::copy_to_user));
}

long sys_access(long path_addr, long mode, long /*unused*/, long /*unused*/, long /*unused*/,
                long /*unused*/) noexcept {
  if (mode < 0 || mode > 7) {
    return -errc::EINVAL;
  }
  char path[vfs::MAX_PATH_LEN];
  if (long error = copy_string_from_user(path, static_cast<u64>(path_addr), sizeof(path)); error < 0) {
    return error;
  }
  auto proc = process::current_process();
  if (!proc) {
    return -errc::ESRCH;
  }
  // access(), unlike open()/chdir(), checks the real credentials.
  return vfs::syscall::do_access(proc->fd_table(), path, mode, proc->uid(), proc->gid());
}

long sys_chdir(long path_addr, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
               long /*unused*/) noexcept {
  char path[vfs::MAX_PATH_LEN];
  if (long error = copy_string_from_user(path, static_cast<u64>(path_addr), sizeof(path)); error < 0) {
    return error;
  }
  auto proc = process::current_process();
  if (!proc) {
    return -errc::ESRCH;
  }
  return vfs::syscall::do_chdir(proc->fd_table(), path, proc->euid(), proc->egid());
}

long sys_mkdir(long path_addr, long mode, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  char path[vfs::MAX_PATH_LEN];
  if (long error = copy_string_from_user(path, static_cast<u64>(path_addr), sizeof(path)); error < 0) {
    return error;
  }
  if (mode < 0 || mode > 0777) {
    return -errc::EINVAL;
  }
  auto proc = process::current_process();
  if (!proc) {
    return -errc::ESRCH;
  }
  return vfs::syscall::do_mkdir(path, static_cast<u32>(mode), proc->euid(), proc->egid(), proc->fd_table());
}

long sys_rmdir(long path_addr, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
               long /*unused*/) noexcept {
  char path[vfs::MAX_PATH_LEN];
  if (long error = copy_string_from_user(path, static_cast<u64>(path_addr), sizeof(path)); error < 0) {
    return error;
  }
  auto proc = process::current_process();
  if (!proc) {
    return -errc::ESRCH;
  }
  return vfs::syscall::do_rmdir(path, proc->fd_table(), proc->euid(), proc->egid());
}

long sys_unlink(long path_addr, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                long /*unused*/) noexcept {
  char path[vfs::MAX_PATH_LEN];
  if (long error = copy_string_from_user(path, static_cast<u64>(path_addr), sizeof(path)); error < 0) {
    return error;
  }
  auto proc = process::current_process();
  if (!proc) {
    return -errc::ESRCH;
  }
  return vfs::syscall::do_unlink(path, proc->fd_table(), proc->euid(), proc->egid());
}

long sys_rename(long old_path, long new_path, long /*unused*/, long /*unused*/, long /*unused*/,
                long /*unused*/) noexcept {
  char old_name[vfs::MAX_PATH_LEN], new_name[vfs::MAX_PATH_LEN];
  if (long error = copy_string_from_user(old_name, static_cast<u64>(old_path), sizeof(old_name)); error < 0) {
    return error;
  }
  if (long error = copy_string_from_user(new_name, static_cast<u64>(new_path), sizeof(new_name)); error < 0) {
    return error;
  }
  auto proc = process::current_process();
  if (!proc) {
    return -errc::ESRCH;
  }
  return vfs::syscall::do_rename(old_name, new_name, proc->fd_table(), proc->euid(), proc->egid());
}

long sys_fstat(long fd, long stat_buf_addr, long /*unused*/, long /*unused*/, long /*unused*/,
               long /*unused*/) noexcept {
  void *fdt = get_current_fd_table();
  if (!fdt) {
    return -errc::EBADF;
  }
  if (stat_buf_addr == 0) {
    return -errc::EFAULT;
  }
  // Use kernel-stack buffer, then copy to validated user address
  moss::kernel::vfs::Stat kstat{};
  long ret = moss::kernel::vfs::syscall::do_fstat(fdt, fd, &kstat);
  if (ret < 0) {
    return ret;
  }
  if (copy_to_user(static_cast<u64>(stat_buf_addr), &kstat, sizeof(kstat)) < 0) {
    return -errc::EFAULT;
  }
  return 0;
}

long sys_ioctl(long fd, long command, long argument, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  return moss::kernel::vfs::syscall::do_ioctl(get_current_fd_table(), fd, command, static_cast<u64>(argument));
}

long sys_uname(long address, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
               long /*unused*/) noexcept {
  // Native ABI: six zero-padded 65-byte fields: system, node, release,
  // version, machine, domain. No hostname/domain mutation is implemented.
  static constexpr char identity[6][65] = {
      "Moss", "moss", MOSS_KERNEL_RELEASE, MOSS_KERNEL_VERSION, MOSS_KERNEL_MACHINE, ""};
  return copy_to_user(static_cast<u64>(address), identity, sizeof(identity));
}

long sys_fcntl(long fd, long command, long argument, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  return moss::kernel::vfs::syscall::do_fcntl(get_current_fd_table(), fd, command, argument);
}

long sys_dup(long oldfd, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  void *fdt = get_current_fd_table();
  if (!fdt) {
    return -errc::EBADF;
  }
  return moss::kernel::vfs::syscall::do_dup(fdt, oldfd);
}

long sys_dup2(long oldfd, long newfd, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  void *fdt = get_current_fd_table();
  if (!fdt) {
    return -errc::EBADF;
  }
  return moss::kernel::vfs::syscall::do_dup2(fdt, oldfd, newfd);
}

long sys_pipe(long pipefd_addr, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
              long /*unused*/) noexcept {
  void *fdt = get_current_fd_table();
  if (!fdt) {
    return -errc::EBADF;
  }
  if (pipefd_addr == 0) {
    return -errc::EFAULT;
  }
  auto buffer = moss::kernel::vfs::OutputBuffer::user(static_cast<u64>(pipefd_addr), 2 * sizeof(long),
                                                      moss::kernel::process::copy_to_user);
  return moss::kernel::vfs::syscall::do_pipe(fdt, buffer);
}

// 内存管理系统调用

// Moss anonymous mmap constants (same numeric values on each supported ISA).
constexpr long PROT_READ = 0x1;
constexpr long PROT_WRITE = 0x2;
constexpr long PROT_EXEC = 0x4;
constexpr long MAP_PRIVATE = 0x02;
constexpr long MAP_ANONYMOUS = 0x20;

long sys_mmap(long addr, long length, long prot, long flags, long fd, long offset) noexcept {
  using namespace moss::kernel::process;

  // Only support MAP_ANONYMOUS | MAP_PRIVATE (no file-backed mmap yet)
  if (length <= 0) {
    return -errc::EINVAL;
  }
  if (!(flags & MAP_ANONYMOUS)) {
    return -errc::ENOSYS;
  }
  if (flags != (MAP_ANONYMOUS | MAP_PRIVATE) || (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0 || fd != -1 ||
      offset != 0) {
    return -errc::EINVAL;
  }
  // Anonymous memory has no approved immutable code version. Fail closed
  // until a capability-checked executable Memory Object path exists.
  if (prot & PROT_EXEC) {
    return -errc::EACCES;
  }

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return -errc::ESRCH;
  }
  auto proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : shared_ptr<Process>{};
  if (!proc) {
    return -errc::ESRCH;
  }
  auto as = proc->address_space();
  if (!as) {
    return -errc::ENOMEM;
  }

  auto transaction = as->lock_vm();
  // Page-align length upward
  auto map_len = static_cast<usize>(length);
  if (map_len > USER_MAX - mm::PageTableManager::KERNEL_IDENTITY_END) {
    return -errc::ENOMEM;
  }
  map_len = (map_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  // Choose mapping address
  VirtAddr map_addr = 0;
  if (addr == 0) {
    map_addr = as->mmap_next;
  } else {
    // Use hint address (page-aligned), not MAP_FIXED
    map_addr = static_cast<VirtAddr>(static_cast<usize>(addr)) & ~(static_cast<VirtAddr>(PAGE_SIZE) - 1);
  }
  if (!mm::PageTableManager::is_user_range(map_addr, map_len) ||
      !AddressSpace::valid_vma_range(map_addr, map_addr + map_len, VmaType::MMAP)) {
    return -errc::EINVAL;
  }

  // Convert prot flags to VMA flags
  u32 vflags = vma_flags::DEMAND_ZERO;
  if (prot & PROT_READ) {
    vflags |= vma_flags::READ;
  }
  if (prot & PROT_WRITE) {
    vflags |= vma_flags::WRITE;
  }
  // A valid hint never replaces an existing VMA. On collision try the cursor;
  // MAP_FIXED and unknown flags were rejected above, not silently downgraded.
  if (!as->add_vma(map_addr, map_addr + map_len, vflags, VmaType::MMAP)) {
    map_addr = as->mmap_next;
    if (addr == 0 || !mm::PageTableManager::is_user_range(map_addr, map_len) ||
        !as->add_vma(map_addr, map_addr + map_len, vflags, VmaType::MMAP)) {
      return -errc::ENOMEM;
    }
  }

  // Advance mmap cursor past this mapping
  VirtAddr map_end = map_addr + map_len;
  if (map_end > as->mmap_next) {
    as->mmap_next = map_end;
  }

  return static_cast<long>(map_addr);
}

long sys_munmap(long addr, long length, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  auto map_addr = static_cast<VirtAddr>(static_cast<usize>(addr));
  if (map_addr & (PAGE_SIZE - 1)) {
    return -errc::EINVAL;
  }
  if (length <= 0) {
    return -errc::EINVAL;
  }
  auto map_len = static_cast<usize>(length);
  if (map_len > USER_MAX - mm::PageTableManager::KERNEL_IDENTITY_END) {
    return -errc::EINVAL;
  }
  map_len = (map_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  if (!mm::PageTableManager::is_user_range(map_addr, map_len) ||
      !AddressSpace::valid_vma_range(map_addr, map_addr + map_len, VmaType::MMAP)) {
    return -errc::EINVAL;
  }

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return -errc::ESRCH;
  }
  auto proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : shared_ptr<Process>{};
  if (!proc) {
    return -errc::ESRCH;
  }
  auto as = proc->address_space();
  if (!as) {
    return -errc::EINVAL;
  }

  auto transaction = as->lock_vm();
  // Find VMA containing the unmap address
  auto vma = as->find_vma(map_addr);
  if (!vma) {
    return -errc::EINVAL;
  }

  // Only support unmapping entire VMAs (no partial unmap / VMA splitting)
  if (map_addr != vma->start_addr || map_addr + map_len != vma->end_addr) {
    return -errc::EINVAL;
  }

  VirtAddr vma_start = vma->start_addr;
  VirtAddr vma_end = vma->end_addr;

  // Unmap all pages that have been demand-paged into the range
  for (VirtAddr va = vma_start; va < vma_end; va += PAGE_SIZE) {
    mm::PageTableManager::unmap_user_page(as->pgd_phys, va);
  }

  // Remove VMA from address space
  as->remove_vma(vma_start, vma_end);
  return 0;
}

long sys_mprotect(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                  long /*unused*/) noexcept {
  log::klog::warn("syscall: mprotect() not implemented");
  return -errc::ENOSYS;
}

long sys_brk(long addr, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return -errc::ESRCH;
  }
  auto proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : shared_ptr<Process>{};
  if (!proc) {
    return -errc::ESRCH;
  }
  auto as = proc->address_space();
  if (!as) {
    return -errc::ENOMEM;
  }

  auto transaction = as->lock_vm();
  // brk(0): query current program break
  if (addr == 0) {
    return static_cast<long>(as->brk_current);
  }

  auto new_brk = static_cast<VirtAddr>(static_cast<usize>(addr));

  // Reject addresses below heap base (Linux returns current brk on failure)
  if (new_brk < as->brk_base) {
    return static_cast<long>(as->brk_current);
  }

  // Bound brk to 16 MiB per process. Exact budget sizing is unrecorded; mmap
  // is separate, and brk failure returns the current break rather than -errno.
  constexpr usize MAX_HEAP = 16ULL * 1024 * 1024;
  if (new_brk - as->brk_base > MAX_HEAP || !mm::PageTableManager::is_user_range(as->brk_base, new_brk - as->brk_base)) {
    return static_cast<long>(as->brk_current);
  }

  const VirtAddr page_mask = static_cast<VirtAddr>(PAGE_SIZE) - 1;
  const VirtAddr old_end = (as->brk_current + page_mask) & ~page_mask;
  const VirtAddr new_end = (new_brk + page_mask) & ~page_mask;
  if (!AddressSpace::valid_vma_range(as->brk_base, new_end, VmaType::HEAP)) {
    return static_cast<long>(as->brk_current);
  }

  if (old_end != new_end) {
    // Keep VMA conflict validation, resident-page revocation and metadata
    // publication in one list transaction. A failed growth leaves both the
    // old VMA and brk_current untouched.
    const bool resized = as->resize_vma(as->brk_base, old_end, new_end, VmaType::HEAP, [&](const VmaRegion &vma) {
      if (new_end >= vma.end_addr) {
        return;
      }
      for (VirtAddr va = new_end; va < vma.end_addr; va += PAGE_SIZE) {
        mm::PageTableManager::unmap_user_page(as->pgd_phys, va);
      }
    });
    if (!resized) {
      return static_cast<long>(as->brk_current);
    }
  }

  // Sub-page shrink retains the containing page. Hardware cannot revoke only
  // its tail; a later growth within that page may observe the retained bytes.
  as->brk_current = new_brk;
  return static_cast<long>(new_brk);
}

// 网络通信系统调用 - 框架实现
long sys_socket(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                long /*unused*/) noexcept {
  log::klog::warn("syscall: socket() not implemented");
  return -errc::ENOSYS;
}

long sys_bind(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
              long /*unused*/) noexcept {
  log::klog::warn("syscall: bind() not implemented");
  return -errc::ENOSYS;
}

long sys_listen(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                long /*unused*/) noexcept {
  log::klog::warn("syscall: listen() not implemented");
  return -errc::ENOSYS;
}

long sys_accept(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                long /*unused*/) noexcept {
  log::klog::warn("syscall: accept() not implemented");
  return -errc::ENOSYS;
}

// ── Scheduling syscalls ───────────────────────────────────────────

// nice(increment) — adjust calling thread's nice value
// Returns the new nice value on success, or -errno on failure.
long sys_nice(long increment, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
              long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur || !g_scheduler) {
    return -errc::ESRCH;
  }

  const i32 current = cur->se.nice.load();
  i32 new_nice;
  if (increment < static_cast<long>(priority::MIN_NICE - current))
    new_nice = priority::MIN_NICE;
  else if (increment > static_cast<long>(priority::MAX_NICE - current))
    new_nice = priority::MAX_NICE;
  else
    new_nice = current + static_cast<i32>(increment);

  g_scheduler->set_base_nice(cur, new_nice);

  log::klog::info("sys_nice: TID={} nice={} weight={}", static_cast<u32>(cur->tid), new_nice, cur->se.weight.load());
  return static_cast<long>(new_nice);
}

// getpriority(which, who) — get scheduling priority (nice value)
// which: 0=PRIO_PROCESS, who: PID (0 = calling process)
// Returns 20 - nice_value (to avoid negative return indicating error)
long sys_getpriority(long which, long who, long /*unused*/, long /*unused*/, long /*unused*/,
                     long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  // Only support PRIO_PROCESS (which == 0) for now
  if (which != 0) {
    return -errc::EINVAL;
  }

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return -errc::ESRCH;
  }

  if (who == 0 || static_cast<ProcessId>(who) == cur->owner_pid) {
    // Return 20 - nice (Linux convention: avoids ambiguity with -errno)
    return 20 - static_cast<long>(cur->se.nice);
  }

  // Look up the target process
  if (!g_process_manager) {
    return -errc::ESRCH;
  }
  auto proc = g_process_manager->find_process(static_cast<ProcessId>(who));
  if (!proc) {
    return -errc::ESRCH;
  }

  Thread *main_thread = proc->get_main_thread();
  if (!main_thread) {
    return -errc::ESRCH;
  }

  return 20 - static_cast<long>(main_thread->se.nice);
}

// sched_getscheduler(pid) — get scheduling policy
// Returns the policy (SCHED_NORMAL=0, SCHED_FIFO=1, SCHED_RR=2, etc.)
long sys_sched_getscheduler(long pid_arg, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                            long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return -errc::ESRCH;
  }

  shared_ptr<Process> proc; // Own target threads throughout this syscall.
  Thread *target = cur;
  if (pid_arg != 0) {
    if (!g_process_manager) {
      return -errc::ESRCH;
    }
    proc = g_process_manager->find_process(static_cast<ProcessId>(pid_arg));
    if (!proc) {
      return -errc::ESRCH;
    }
    target = proc->get_main_thread();
    if (!target) {
      return -errc::ESRCH;
    }
  }

  return static_cast<long>(static_cast<u8>(target->sched_policy));
}

// sched_get_priority_max(policy) — get maximum RT priority for a policy
long sys_sched_get_priority_max(long policy_arg, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                                long /*unused*/) noexcept {
  auto policy = static_cast<process::SchedPolicy>(static_cast<u8>(policy_arg));
  switch (policy) {
  case process::SchedPolicy::Fifo:
  case process::SchedPolicy::RR:
    return static_cast<long>(process::priority::MAX_RT_PRIORITY);
  case process::SchedPolicy::Normal:
  case process::SchedPolicy::Batch:
  case process::SchedPolicy::Idle:
    return 0;
  default:
    return -errc::EINVAL;
  }
}

// sched_get_priority_min(policy) — get minimum RT priority for a policy
long sys_sched_get_priority_min(long policy_arg, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                                long /*unused*/) noexcept {
  auto policy = static_cast<process::SchedPolicy>(static_cast<u8>(policy_arg));
  switch (policy) {
  case process::SchedPolicy::Fifo:
  case process::SchedPolicy::RR:
    return static_cast<long>(process::priority::MIN_RT_PRIORITY);
  case process::SchedPolicy::Normal:
  case process::SchedPolicy::Batch:
  case process::SchedPolicy::Idle:
    return 0;
  default:
    return -errc::EINVAL;
  }
}

// sched_yield() — voluntarily give up the CPU
// Sets current task's vruntime to min_vruntime + SCHED_LATENCY_NS,
// re-enqueues, then context-switches away.
long sys_sched_yield(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                     long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur || !g_scheduler) {
    return -errc::ESRCH;
  }

  // Publishing Ready must not be interrupted before saving this continuation.
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  u32 cpu = arch::get_current_cpu_id();

  // Penalize vruntime so other tasks get priority
  u64 min_vrt = g_scheduler->get_cpu_min_vruntime(cpu);
  cur->se.vruntime = min_vrt + cfs_params::SCHED_LATENCY_NS;

  // Re-enqueue and trigger reschedule
  g_scheduler->enqueue_task(cur, cpu);

  CfsScheduler::switch_to_bootstrap(cur->context);
  if (restore_irqs) {
    arch::enable_interrupts();
  }

  return 0;
}

// sched_getaffinity(pid, cpusetsize, mask_addr) — get CPU affinity mask
// pid: 0 = calling thread
// Returns 0 on success, -errno on failure
long sys_sched_getaffinity(long pid_arg, long /*unused*/, long mask_addr, long /*unused*/, long /*unused*/,
                           long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  shared_ptr<Process> proc; // Own target threads throughout this syscall.
  Thread *target = nullptr;

  if (pid_arg == 0) {
    target = CfsScheduler::get_current_task();
  } else {
    if (!g_process_manager) {
      return -errc::ESRCH;
    }
    proc = g_process_manager->find_process(static_cast<ProcessId>(pid_arg));
    if (!proc) {
      return -errc::ESRCH;
    }
    target = proc->get_main_thread();
  }

  if (!target) {
    return -errc::ESRCH;
  }

  if (mask_addr != 0) {
    u32 mask_val;
    {
      containers::LockGuard<containers::IrqSpinLock> guard(target->sleep_lock);
      mask_val = target->cpu_affinity_mask.low_word();
    }
    if (copy_to_user(static_cast<u64>(mask_addr), &mask_val, sizeof(mask_val)) < 0) {
      return -errc::EFAULT;
    }
  }

  return 0;
}

// sched_setaffinity(pid, cpusetsize, mask_addr) — set CPU affinity mask
// pid: 0 = calling thread
// Returns 0 on success, -errno on failure
long sys_sched_setaffinity(long pid_arg, long /*unused*/, long mask_addr, long /*unused*/, long /*unused*/,
                           long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  if (mask_addr == 0) {
    return -errc::EFAULT;
  }

  u32 new_mask = 0;
  if (copy_from_user(&new_mask, static_cast<u64>(mask_addr), sizeof(new_mask)) < 0) {
    return -errc::EFAULT;
  }

  // Must allow at least one CPU
  if (new_mask == 0) {
    return -errc::EINVAL;
  }

  // The native ABI currently transfers one u32 mask, so CPUs beyond its 32
  // representable bits cannot be selected until the ABI grows a larger set.
  constexpr u32 kNativeAffinityBits = 32;
  // Mask out CPUs beyond available CPU count.
  u32 num_cpus = g_num_cpus;
  u32 valid_mask = (num_cpus >= kNativeAffinityBits) ? ~0U : ((1U << num_cpus) - 1);
  new_mask &= valid_mask;
  if (new_mask == 0) {
    return -errc::EINVAL;
  }

  shared_ptr<Process> proc; // Own target threads throughout this syscall.
  Thread *target = nullptr;
  Thread *current = CfsScheduler::get_current_task();
  if (!current) {
    return -errc::ESRCH;
  }

  if (pid_arg == 0) {
    target = current;
  } else {
    if (!g_process_manager) {
      return -errc::ESRCH;
    }
    proc = g_process_manager->find_process(static_cast<ProcessId>(pid_arg));
    if (!proc) {
      return -errc::ESRCH;
    }
    // POSIX child-to-parent signals do not confer scheduling authority over
    // a parentless native domain.
    if (proc->parent_pid() == INVALID_PROCESS_ID && proc->pid() != current->owner_pid) {
      return -errc::EPERM;
    }
    target = proc->get_main_thread();
  }

  if (!target) {
    return -errc::ESRCH;
  }

  const bool changes_current = target == current;
  const bool restore_irqs = changes_current && arch::interrupts_enabled();
  if (changes_current) {
    // Keep a timer preemption from redispatching this thread on the old CPU
    // between publishing the restrictive mask and starting its migration.
    arch::disable_interrupts();
  }

  const u32 current_cpu = arch::get_current_cpu_id();
  u32 old_mask = 0;
  u32 destination = current_cpu;
  bool needs_migration = false;
  {
    containers::LockGuard<containers::IrqSpinLock> guard(target->sleep_lock);
    old_mask = target->cpu_affinity_mask.low_word();
    target->cpu_affinity_mask.set_from_u32(new_mask);
    if (changes_current && !target->cpu_affinity_mask.test(current_cpu)) {
      needs_migration = true;
      destination = 0;
      while (destination < num_cpus && !target->cpu_affinity_mask.test(destination)) {
        ++destination;
      }
    }
  }

  if (needs_migration) {
    if (!g_scheduler || destination == num_cpus || !g_scheduler->migrate_current(destination)) {
      // Do not report success with a mask that the running thread could not
      // obey. Restoring the prior mask keeps the syscall failure atomic.
      {
        containers::LockGuard<containers::IrqSpinLock> guard(target->sleep_lock);
        target->cpu_affinity_mask.set_from_u32(old_mask);
      }
      if (restore_irqs) {
        arch::enable_interrupts();
      }
      return -errc::EAGAIN;
    }
  }

  if (restore_irqs) {
    arch::enable_interrupts();
  }

  log::klog::info("sys_sched_setaffinity: TID={} mask={:#x}", static_cast<u32>(target->tid), new_mask);
  return 0;
}

// ── Time syscalls ─────────────────────────────────────────────

// sys_clock_gettime(clock_id, time_ns_ptr)
// Returns monotonic nanoseconds since boot via timer subsystem.
long sys_clock_gettime(long /* clock_id */, long time_ns_addr, long /*unused*/, long /*unused*/, long /*unused*/,
                       long /*unused*/) noexcept {
  if (time_ns_addr == 0) {
    return -errc::EFAULT;
  }
  u64 ns = timer::TimerSubsystem::instance().now_ns();
  if (copy_to_user(static_cast<u64>(time_ns_addr), &ns, sizeof(ns)) < 0) {
    return -errc::EFAULT;
  }
  return 0;
}

// sys_clock_getres(clock_id, resolution_ns_ptr)
// The native ABI returns one u64 nanosecond count, matching clock_gettime
// rather than exposing a POSIX timespec. A counter tick cannot be represented
// by less than ceil(10^9 / frequency_hz) ns, so round up to avoid advertising
// precision that the hardware clocksource cannot provide.
long sys_clock_getres(long clock_id, long resolution_ns_addr, long /*unused*/, long /*unused*/, long /*unused*/,
                      long /*unused*/) noexcept {
  constexpr long kClockRealtime = 0;
  constexpr long kClockMonotonic = 1;
  constexpr u64 kNanosecondsPerSecond = 1000000000ULL;

  if (clock_id != kClockRealtime && clock_id != kClockMonotonic) {
    return -errc::EINVAL;
  }
  if (resolution_ns_addr == 0) {
    return -errc::EFAULT;
  }

  const u64 frequency_hz = timer::TimerSubsystem::instance().clocksource().frequency_hz();
  if (frequency_hz == 0) {
    // A time syscall before timer initialization has no meaningful resolution.
    return -errc::EINVAL;
  }
  const u64 resolution_ns =
      kNanosecondsPerSecond / frequency_hz + static_cast<u64>(kNanosecondsPerSecond % frequency_hz != 0);
  if (copy_to_user(static_cast<u64>(resolution_ns_addr), &resolution_ns, sizeof(resolution_ns)) < 0) {
    return -errc::EFAULT;
  }
  return 0;
}

// Wake callback for nanosleep: called from timer ISR when sleep expires.
// Sets the blocked thread back to Ready and enqueues it for scheduling.
static void nanosleep_wake_callback(void *data) noexcept {
  using namespace moss::kernel::process;
  auto *thread = static_cast<Thread *>(data);
  if (thread && is_blocked_state(thread->state)) {
    if (g_scheduler) {
      g_scheduler->task_wakeup(thread, thread->cpu);
    }
  }
}

// Both sleep syscalls use the same architecture-independent context switch.
static long sleep_until(u64 deadline, u64 remaining_addr = 0) noexcept {
  using namespace moss::kernel::process;
  while (true) {
    const u64 now = timer::TimerSubsystem::instance().now_ns();
    if (deadline <= now) {
      return 0;
    }
    Thread *cur = CfsScheduler::get_current_task();
    if (!cur || !g_scheduler) {
      return -errc::ESRCH;
    }
    if (moss::abi::bridge::moss_io_wait_interrupted()) {
      if (remaining_addr != 0) {
        const u64 remaining_ns = deadline - now;
        if (copy_to_user(remaining_addr, &remaining_ns, sizeof(remaining_ns)) < 0) {
          return -errc::EFAULT;
        }
      }
      return -errc::EINTR;
    }

    const bool restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();
    // Dequeue before publishing the timer, and defer any remote wakeup until
    // bootstrap owns our saved context. The helper also catches signals that
    // arrive between the pending check and publishing Sleeping.
    (void)moss::abi::bridge::moss_prepare_io_wait();
    timer::HrTimer sleep_timer;
    sleep_timer.init(timer::TimerMode::OneShot, nanosleep_wake_callback, cur);
    auto armed = sleep_timer.start(deadline);
    if (!armed) {
      // Roll an unsuccessful arm back through the same handoff; the prepared
      // thread must not return to userspace while still marked/dequeued asleep.
      g_scheduler->task_wakeup(cur, cur->wake_cpu);
    } else {
      moss_validation_sleep_armed(&sleep_timer);
    }
    g_scheduler->commit_sleep();
    // A remote callback must release its reference before this stack disappears.
    sleep_timer.cancel_sync();
    if (restore_irqs) {
      arch::enable_interrupts();
    }
    if (!armed) {
      return armed.error() == ErrorCode::ResourceExhausted ? -errc::ENOMEM : -errc::EINVAL;
    }
    // Signals, timer expiry and unrelated wakeups share the same scheduler path.
    // Recheck the deadline and pending signal before reporting completion.
  }
}

long sys_nanosleep(long ns_addr, long remaining_addr, long /*unused*/, long /*unused*/, long /*unused*/,
                   long /*unused*/) noexcept {
  u64 duration = 0;
  if (copy_from_user(&duration, static_cast<u64>(ns_addr), sizeof(duration)) < 0) {
    return -errc::EFAULT;
  }
  u64 now = timer::TimerSubsystem::instance().now_ns();
  if (duration > ~u64{0} - now) {
    return -errc::EINVAL;
  }
  return sleep_until(now + duration, static_cast<u64>(remaining_addr));
}

// clock_nanosleep(clockid, flags, ns_addr, remaining)
// clockid: 0 = CLOCK_REALTIME, 1 = CLOCK_MONOTONIC (we treat both the same)
// flags:   0 = relative sleep,  1 = TIMER_ABSTIME (absolute deadline)
// ns_addr: pointer to u64 nanoseconds (relative duration or absolute timestamp)
long sys_clock_nanosleep(long clockid, long flags, long ns_addr, long remaining_addr, long /*unused*/,
                         long /*unused*/) noexcept {
  // Only support CLOCK_REALTIME (0) and CLOCK_MONOTONIC (1)
  if (clockid < 0 || clockid > 1 || (flags != 0 && flags != 1)) {
    return -errc::EINVAL;
  }

  if (ns_addr == 0) {
    return -errc::EFAULT;
  }
  u64 target_ns = 0;
  if (copy_from_user(&target_ns, static_cast<u64>(ns_addr), sizeof(target_ns)) < 0) {
    return -errc::EFAULT;
  }

  if (flags == 0) {
    u64 now = timer::TimerSubsystem::instance().now_ns();
    if (target_ns > ~u64{0} - now) {
      return -errc::EINVAL;
    }
    target_ns += now;
  }
  return sleep_until(target_ns, flags == 0 ? static_cast<u64>(remaining_addr) : 0);
}

// ── System monitoring: topinfo ──────────────────────────────

// Kernel-side mirror of userspace TopProcessInfo / TopInfo structs.
// Layout must match exactly (all fields are u64/long on 64-bit).
namespace topinfo_layout {
// Fixed ABI capacities: 64 records and 16-byte NUL-terminated names must
// match userspace/syscall.h. Changing either changes every following offset.
inline constexpr u64 MAX_PROCS = 64;
// Fixed ABI constant — matches userspace TOP_MAX_CPUS in syscall.h.
// Independent of kernel BOOT_MAX_CPUS to maintain ABI stability.
inline constexpr u64 MAX_CPUS_TOP = 32;

struct ProcEntry {
  long pid;
  long ppid;
  u64 state;
  u64 cpu;
  long nice;
  u64 vruntime;
  u64 sum_exec_runtime;
  u64 load_avg;
  u64 util_avg;
  char name[16];
};

struct Info {
  u64 uptime_ns;
  u64 total_processes;
  u64 total_context_switches;
  u64 total_preemptions;
  u64 total_forks;
  u64 total_exits;
  u64 nr_cpus;
  u64 cpu_load[MAX_CPUS_TOP];
  u64 cpu_nr_running[MAX_CPUS_TOP];
  u64 cpu_idle_time_ns[MAX_CPUS_TOP];
  u64 mem_total_pages;
  u64 mem_used_pages;
  u64 mem_free_pages;
  u64 page_size;
  u64 nr_processes;
  ProcEntry procs[MAX_PROCS];
};
} // namespace topinfo_layout

// sys_topinfo(info_ptr) — fill TopInfo struct for userspace `top`
//
// Collect into resident kernel storage before using the shared, fault-contained
// user-copy policy. Filesystem/scheduler inspection must not dereference user
// addresses directly; a failed copy returns EFAULT and may leave a user prefix.
long sys_topinfo(long info_addr, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                 long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  if (info_addr == 0) {
    return -errc::EFAULT;
  }

  // 864 header/CPU-array bytes + 64 * 88-byte process records = 6496 bytes.
  // Growing the fixed ABI consumes more of the 16 KiB syscall kernel stack.
  topinfo_layout::Info kbuf;

  // Zero-initialize on kernel stack (no page fault issues)
  {
    auto *p = reinterpret_cast<u8 *>(&kbuf);
    for (usize i = 0; i < sizeof(kbuf); ++i) {
      p[i] = 0;
    }
  }

  // System summary
  kbuf.uptime_ns = timer::TimerSubsystem::instance().now_ns();
  kbuf.nr_cpus = g_num_cpus < topinfo_layout::MAX_CPUS_TOP ? g_num_cpus : topinfo_layout::MAX_CPUS_TOP;

  if (g_process_manager) {
    kbuf.total_processes = g_process_manager->total_processes();
    kbuf.total_forks = g_process_manager->total_forks();
    kbuf.total_exits = g_process_manager->total_exits();
    kbuf.total_context_switches = g_process_manager->total_context_switches();
  }

  if (g_scheduler) {
    kbuf.total_preemptions = g_scheduler->total_preemptions();
    if (kbuf.total_context_switches == 0) {
      kbuf.total_context_switches = g_scheduler->total_context_switches();
    }

    // Cap to struct array size to avoid out-of-bounds writes
    u32 nr_cpus = g_num_cpus;
    if (nr_cpus > topinfo_layout::MAX_CPUS_TOP) {
      nr_cpus = static_cast<u32>(topinfo_layout::MAX_CPUS_TOP);
    }

    for (u32 cpu = 0; cpu < nr_cpus; ++cpu) {
      kbuf.cpu_load[cpu] = g_scheduler->get_cpu_load(cpu);
      kbuf.cpu_nr_running[cpu] = g_scheduler->get_cpu_nr_running(cpu);

      // Idle time: snapshot includes in-progress idle periods
      auto *idle = get_idle_task(cpu);
      kbuf.cpu_idle_time_ns[cpu] = idle ? idle->snapshot_idle_time_ns(kbuf.uptime_ns) : 0;

      // CFS dequeues running tasks; compensate nr_running
      Thread *running = CfsScheduler::get_current_task_on_cpu(cpu);
      if (running != nullptr) {
        kbuf.cpu_nr_running[cpu] += 1;
      }
    }
  }

  // Memory stats
  auto mem_stats = mm::PageFrameAllocator::get_memory_stats();
  kbuf.mem_total_pages = mem_stats.total_pages;
  kbuf.mem_used_pages = mem_stats.used_pages;
  kbuf.mem_free_pages = mem_stats.free_pages;
  kbuf.page_size = PAGE_SIZE;

  // Process table
  u64 proc_idx = 0;
  if (g_process_manager) {
    g_process_manager->for_each_process([&](ProcessId pid, Process *proc) {
      if (proc_idx >= topinfo_layout::MAX_PROCS || !proc) {
        return;
      }

      auto &pe = kbuf.procs[proc_idx];
      pe.pid = static_cast<long>(pid);
      pe.ppid = static_cast<long>(proc->parent_pid());
      pe.state = static_cast<u64>(static_cast<u8>(proc->state()));

      // Copy at most 15 characters into the zeroed 16-byte ABI name field,
      // preserving its final NUL for userspace formatters.
      const char *n = proc->name();
      for (usize i = 0; i < 15 && n[i]; ++i) {
        pe.name[i] = n[i];
      }

      // Main thread scheduling info
      Thread *main = proc->get_main_thread();
      if (main) {
        pe.cpu = main->cpu;
        pe.nice = main->se.nice;
        pe.vruntime = main->se.vruntime;
        pe.sum_exec_runtime = main->se.sum_exec_runtime;
        pe.load_avg = main->se.load_avg;
        pe.util_avg = main->se.util_avg;
      }

      proc_idx++;
    });
  }
  kbuf.nr_processes = proc_idx;

  // Single bulk copy from kernel stack to user space (with VMA validation).
  if (copy_to_user(static_cast<u64>(info_addr), &kbuf, sizeof(kbuf)) < 0) {
    return -errc::EFAULT;
  }

  return 0;
}

// 未实现系统调用的默认处理器
long sys_not_implemented(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                         long /*unused*/) noexcept {
  log::klog::warn("syscall: unknown/unimplemented");
  return -errc::ENOSYS;
}
} // namespace handlers

// ============================================================================
// Console RX: UART interrupt-driven input with ring buffer
// ============================================================================
//
// Architecture: IRQ handler drains PL011 RX FIFO into a lock-free SPSC ring
// buffer and wakes the single blocked reader thread.  The reader blocks via
// the same Blocked + dequeue + context_switch pattern used by sys_nanosleep.
//
// Exported as extern "C" for use by the VFS module (vfs_init.cpp) which
// cannot directly import moss.interrupts / moss.process.
// ============================================================================

// 全局系统调用表定义
const SyscallDescriptor SYSCALL_TABLE[static_cast<int>(SyscallNumber::MAX_SYSCALL)] = {
    // === 基础系统调用 (0-9) ===
    {"debug_print", handlers::sys_debug_print, 1, true, "调试输出"},
    {"exit", handlers::sys_exit, 1, true, "进程退出"},
    {"getpid", handlers::sys_getpid, 0, true, "获取进程ID"},
    {"getppid", handlers::sys_getppid, 0, true, "获取父进程ID"},
    {"getuid", handlers::sys_getuid, 0, true, "获取用户ID"},
    {"getgid", handlers::sys_getgid, 0, true, "获取组ID"},
    {"geteuid", handlers::sys_geteuid, 0, true, "获取有效用户ID"},
    {"getegid", handlers::sys_getegid, 0, true, "获取有效组ID"},
    {"setsid", handlers::sys_setsid, 0, true, "创建新会话"},
    {"getpgid", handlers::sys_getpgid, 1, true, "获取进程组ID"},

    // === 进程管理 (10-29) ===
    {"fork", handlers::sys_fork, 0, true, "创建子进程"},
    {"execve", handlers::sys_execve, 3, true, "执行程序"},
    {"wait4", handlers::sys_wait4, 4, true, "等待子进程"},
    {"waitpid", handlers::sys_waitpid, 3, true, "等待指定进程"},
    {"kill", handlers::sys_kill, 2, true, "发送信号"},
    {"sigaction", handlers::sys_sigaction, 3, true, "信号处理设置"},
    {"sigprocmask", handlers::sys_sigprocmask, 3, true, "信号掩码操作"},
    {"sigreturn", handlers::sys_sigreturn, 0, true, "信号返回"},
    {"sched_yield", handlers::sys_sched_yield, 0, true, "Yield CPU"},
    {"sched_getaffinity", handlers::sys_sched_getaffinity, 3, true, "Get CPU affinity"},
    {"sched_setaffinity", handlers::sys_sched_setaffinity, 3, true, "Set CPU affinity"},
    {"sigaltstack", handlers::sys_sigaltstack, 2, true, "设置信号备用栈"},
    {"sched_getscheduler", handlers::sys_sched_getscheduler, 1, true, "Get scheduling policy"},
    {"sched_get_priority_max", handlers::sys_sched_get_priority_max, 1, true, "Get max RT priority"},
    {"sched_get_priority_min", handlers::sys_sched_get_priority_min, 1, true, "Get min RT priority"},
    {"getpgrp", handlers::sys_getpgrp, 0, true, "获取进程组"},
    {"setpgrp", handlers::sys_setpgrp, 0, true, "设置进程组"},
    {"getsid", handlers::sys_getsid, 1, true, "获取会话ID"},
    {"nice", handlers::sys_nice, 1, true, "Set process nice value"},
    {"getpriority", handlers::sys_getpriority, 2, true, "Get process priority"},

    // === 文件系统操作 (30-59) ===
    {"open", handlers::sys_open, 3, true, "打开文件"},
    {"close", handlers::sys_close, 1, true, "关闭文件"},
    {"read", handlers::sys_read, 3, true, "读取文件"},
    {"write", handlers::sys_write, 3, true, "写入文件"},
    {"lseek", handlers::sys_lseek, 3, true, "文件定位"},
    {"stat", handlers::sys_stat, 2, true, "获取文件状态"},
    {"fstat", handlers::sys_fstat, 2, true, "获取文件描述符状态"},
    {"lstat", handlers::sys_stat, 2, true, "获取链接文件状态"},
    {"access", handlers::sys_access, 2, true, "Check access using real credentials"},
    {"chmod", handlers::sys_not_implemented, 2, false, "修改文件权限"},
    {"chown", handlers::sys_not_implemented, 3, false, "修改文件所有者"},
    {"umask", handlers::sys_not_implemented, 1, false, "设置文件创建掩码"},
    {"dup", handlers::sys_dup, 1, true, "复制文件描述符"},
    {"dup2", handlers::sys_dup2, 2, true, "复制文件描述符到指定位置"},
    {"pipe", handlers::sys_pipe, 1, true, "创建管道"},
    {"mkdir", handlers::sys_mkdir, 2, true, "创建目录"},
    {"rmdir", handlers::sys_rmdir, 1, true, "删除目录"},
    {"link", handlers::sys_not_implemented, 2, false, "创建硬链接"},
    {"unlink", handlers::sys_unlink, 1, true, "删除文件"},
    {"symlink", handlers::sys_not_implemented, 2, false, "创建符号链接"},
    {"readlink", handlers::sys_not_implemented, 3, false, "读取符号链接"},
    {"chdir", handlers::sys_chdir, 1, true, "改变工作目录"},
    {"getcwd", handlers::sys_getcwd, 2, true, "获取当前目录"},
    {"rename", handlers::sys_rename, 2, true, "重命名文件"},
    {"truncate", handlers::sys_not_implemented, 2, false, "截断文件"},
    {"ftruncate", handlers::sys_not_implemented, 2, false, "截断文件(通过fd)"},
    {"fsync", handlers::sys_not_implemented, 1, false, "同步文件"},
    {"fdatasync", handlers::sys_not_implemented, 1, false, "同步文件数据"},
    {"sync", handlers::sys_not_implemented, 0, false, "同步所有文件"},
    {"mount", handlers::sys_not_implemented, 5, false, "挂载文件系统"},

    // === 内存管理 (60-79) ===
    {"mmap", handlers::sys_mmap, 6, true, "内存映射"},
    {"munmap", handlers::sys_munmap, 2, true, "取消内存映射"},
    {"mprotect", handlers::sys_mprotect, 3, false, "修改内存保护"},
    {"mlock", handlers::sys_not_implemented, 2, false, "锁定内存页"},
    {"munlock", handlers::sys_not_implemented, 2, false, "解锁内存页"},
    {"mlockall", handlers::sys_not_implemented, 1, false, "锁定所有内存页"},
    {"munlockall", handlers::sys_not_implemented, 0, false, "解锁所有内存页"},
    {"madvise", handlers::sys_not_implemented, 3, false, "内存使用建议"},
    {"msync", handlers::sys_not_implemented, 3, false, "同步内存映射"},
    {"brk", handlers::sys_brk, 1, true, "设置数据段大小"},
    {"sbrk", handlers::sys_not_implemented, 1, false, "调整数据段大小"},
    {"mremap", handlers::sys_not_implemented, 5, false, "重新映射内存"},
    {"mincore", handlers::sys_not_implemented, 3, false, "检查页面是否在内存中"},
    {"mmap2", handlers::sys_not_implemented, 6, false, "内存映射(扩展版)"},
    {"remap_file_pages", handlers::sys_not_implemented, 5, false, "重新映射文件页"},
    {"mbind", handlers::sys_not_implemented, 6, false, "NUMA内存绑定"},
    {"get_mempolicy", handlers::sys_not_implemented, 5, false, "获取内存策略"},
    {"set_mempolicy", handlers::sys_not_implemented, 3, false, "设置内存策略"},
    {"migrate_pages", handlers::sys_not_implemented, 4, false, "迁移内存页"},
    {"move_pages", handlers::sys_not_implemented, 6, false, "移动内存页"},

    // === 时间和定时器 (80-89) ===
    {"time", handlers::sys_not_implemented, 1, false, "获取时间"},
    {"gettimeofday", handlers::sys_not_implemented, 2, false, "获取时间(微秒精度)"},
    {"settimeofday", handlers::sys_not_implemented, 2, false, "设置时间"},
    {"clock_gettime", handlers::sys_clock_gettime, 2, true, "Get monotonic time (ns)"},
    {"clock_settime", handlers::sys_not_implemented, 2, false, "设置时钟时间"},
    {"clock_getres", handlers::sys_clock_getres, 2, true, "Get clock resolution (ns)"},
    {"nanosleep", handlers::sys_nanosleep, 2, true, "Yield-loop nanosleep"},
    {"clock_nanosleep", handlers::sys_clock_nanosleep, 4, true, "Clock-based nanosleep"},
    {"timer_settime", handlers::sys_not_implemented, 4, false, "设置定时器"},
    {"timer_gettime", handlers::sys_not_implemented, 2, false, "获取定时器状态"},

    // === 网络通信 (90-109) ===
    {"socket", handlers::sys_socket, 3, false, "创建socket"},
    {"bind", handlers::sys_bind, 3, false, "绑定地址"},
    {"listen", handlers::sys_listen, 2, false, "监听连接"},
    {"accept", handlers::sys_accept, 3, false, "接受连接"},
    {"connect", handlers::sys_not_implemented, 3, false, "建立连接"},
    {"send", handlers::sys_not_implemented, 4, false, "发送数据"},
    {"recv", handlers::sys_not_implemented, 4, false, "接收数据"},
    {"sendto", handlers::sys_not_implemented, 6, false, "发送数据到指定地址"},
    {"recvfrom", handlers::sys_not_implemented, 6, false, "从指定地址接收数据"},
    {"shutdown", handlers::sys_not_implemented, 2, false, "关闭socket"},
    {"setsockopt", handlers::sys_not_implemented, 5, false, "设置socket选项"},
    {"getsockopt", handlers::sys_not_implemented, 5, false, "获取socket选项"},
    {"getsockname", handlers::sys_not_implemented, 3, false, "获取socket名称"},
    {"getpeername", handlers::sys_not_implemented, 3, false, "获取对端名称"},
    {"socketpair", handlers::sys_not_implemented, 4, false, "创建socket对"},
    {"sendmsg", handlers::sys_not_implemented, 3, false, "发送消息"},
    {"recvmsg", handlers::sys_not_implemented, 3, false, "接收消息"},
    {"select", handlers::sys_not_implemented, 5, false, "I/O多路复用"},
    {"poll", handlers::sys_not_implemented, 3, false, "轮询I/O事件"},
    {"epoll_create", handlers::sys_not_implemented, 1, false, "创建epoll实例"},

    // === 系统信息和控制 (110-129) ===
    {"uname", handlers::sys_uname, 1, true, "Read native system identity"},
    {"topinfo", handlers::sys_topinfo, 1, true, "Get system/process info for top"},
    {"getrlimit", handlers::sys_not_implemented, 2, false, "获取资源限制"},
    {"setrlimit", handlers::sys_not_implemented, 2, false, "设置资源限制"},
    {"getrusage", handlers::sys_not_implemented, 2, false, "获取资源使用情况"},
    {"times", handlers::sys_not_implemented, 1, false, "获取进程时间"},
    {"ptrace", handlers::sys_not_implemented, 4, false, "进程跟踪"},
    {"syslog", handlers::sys_not_implemented, 3, false, "系统日志"},
    {"reboot", handlers::sys_not_implemented, 4, false, "系统重启"},
    {"sethostname", handlers::sys_not_implemented, 2, false, "设置主机名"},
    {"gethostname", handlers::sys_not_implemented, 2, false, "获取主机名"},
    {"setdomainname", handlers::sys_not_implemented, 2, false, "设置域名"},
    {"getdomainname", handlers::sys_not_implemented, 2, false, "获取域名"},
    {"iopl", handlers::sys_not_implemented, 1, false, "I/O权限级别"},
    {"ioperm", handlers::sys_not_implemented, 3, false, "I/O端口权限"},
    {"sysctl", handlers::sys_not_implemented, 1, false, "系统控制"},
    {"arch_prctl", handlers::sys_arch_prctl, 2, arch::is_x64, "架构特定控制"},
    {"prctl", handlers::sys_not_implemented, 5, false, "进程控制"},
    {"capget", handlers::sys_not_implemented, 2, false, "获取能力"},
    {"capset", handlers::sys_not_implemented, 2, false, "设置能力"},
    {"fcntl", handlers::sys_fcntl, 3, true, "Query file status flags"},
    {"getdents", handlers::sys_getdents, 3, true, "Read a native directory entry"},
    {"ioctl", handlers::sys_ioctl, 3, true, "Native device control"},
    {"cap_close", handlers::sys_cap_close, 1, true, "Close a process-local capability"},
    {"cap_duplicate", handlers::sys_cap_duplicate, 2, true, "Duplicate with reduced rights"},
    {"cap_set_inherit", handlers::sys_cap_set_inherit, 2, true, "Select fork/exec inheritance"},
    {"ipc_create", handlers::sys_ipc_create, 1, true, "Create a bounded control endpoint"},
    {"ipc_call", handlers::sys_ipc_call, 4, true, "Call a control endpoint"},
    {"ipc_receive", handlers::sys_ipc_receive, 3, true, "Receive a control request"},
    {"ipc_reply", handlers::sys_ipc_reply, 2, true, "Complete a pending call"},
    {"mem_create", handlers::sys_mem_create, 1, true, "Create a capability-backed memory page"},
    {"mem_map", handlers::sys_mem_map, 2, true, "Map a memory capability"},
    {"ipc_mint_badge", handlers::sys_ipc_mint_badge, 2, true, "Mint a sender with a receiver-visible badge"},
    {"fork_domain", handlers::sys_fork_domain, 1, true, "Fork with a child execution-domain capability"},
    {"domain_id", handlers::sys_domain_id, 1, true, "Inspect a domain's diagnostic process ID"},
    {"domain_terminate", handlers::sys_domain_terminate, 1, true, "Request capability-authorized termination"},
    {"domain_wait", handlers::sys_domain_wait, 1, true, "Wait for a capability-addressed domain to exit"},
    {"fork_domain_select", handlers::sys_fork_domain_select, 3, true, "Fork with selected inherited capabilities"},
    {"domain_wait_any", handlers::sys_domain_wait_any, 2, true, "Wait for one of several capability-addressed domains"},
    {"domain_status", handlers::sys_domain_status, 2, true, "Read a capability-addressed domain's exit cause"},
    {"domain_self", handlers::sys_domain_self, 0, true, "Acquire a capability for the calling domain"},
    {"cap_set_exec", handlers::sys_cap_set_exec, 2, true, "Select capability retention after exec"},
    {"domain_same", handlers::sys_domain_same, 2, true, "Compare two inspected domain incarnations"},
    {"fork_domain_inherit", handlers::sys_fork_domain_inherit, 1, true,
     "Fork a native domain with capabilities marked for inheritance"},
    {"domain_spawn", handlers::sys_domain_spawn, 2, true, "Build and start an authorized native domain"},
    {"domain_layout", handlers::sys_domain_layout, 1, true, "Read the active native-domain virtual layout"},
    {"domain_factory", handlers::sys_domain_factory, 0, true, "Mint boot-owned domain construction authority"},
    {"execve_cap", handlers::sys_execve_cap, 4, true, "Exec with an explicit retained startup capability"},
    {"domain_signal", handlers::sys_domain_signal, 2, true, "Signal a capability-addressed domain"},
    {"domain_scope_create", handlers::sys_domain_scope_create, 0, true, "Create a fork-inherited domain scope"},
    {"domain_scope_terminate", handlers::sys_domain_scope_terminate, 1, true,
     "Close a domain scope and terminate its members"},
    {"domain_scope_status", handlers::sys_domain_scope_status, 1, true, "Count live domain scope members"},
    {"fork_domain_scoped", handlers::sys_fork_domain_scoped, 4, true, "Fork a native domain into a specified scope"},
    {"domain_scope_contains", handlers::sys_domain_scope_contains, 2, true,
     "Check whether an inspected domain belongs to a scope"}};

// 系统调用分发器实现
long SyscallDispatcher::dispatch(long syscall_number, long arg0, long arg1, long arg2, long arg3, long arg4,
                                 long arg5) noexcept {
  // 更新统计信息
  ++g_syscall_stats.total_syscalls;

  // 检查系统调用号有效性
  if (!is_valid_syscall(syscall_number)) {
    ++g_syscall_stats.invalid_syscalls;
    return -errc::EINVAL;
  }

  const SyscallDescriptor *desc = &SYSCALL_TABLE[syscall_number];

  // 检查是否已实现
  if (!desc->implemented) {
    ++g_syscall_stats.unimplemented_syscalls;
    return desc->handler(arg0, arg1, arg2, arg3, arg4, arg5);
  }

  // 调用系统调用处理函数
  long result = desc->handler(arg0, arg1, arg2, arg3, arg4, arg5);

  // 更新统计信息
  if (result >= 0) {
    ++g_syscall_stats.successful_syscalls;
  } else {
    ++g_syscall_stats.failed_syscalls;
  }

  return result;
}

const SyscallDescriptor *SyscallDispatcher::get_syscall_info(long syscall_number) noexcept {
  if (!is_valid_syscall(syscall_number)) {
    return nullptr;
  }
  return &SYSCALL_TABLE[syscall_number];
}

bool SyscallDispatcher::is_implemented(long syscall_number) noexcept {
  if (!is_valid_syscall(syscall_number)) {
    return false;
  }
  return SYSCALL_TABLE[syscall_number].implemented;
}

bool SyscallDispatcher::is_valid_syscall(long syscall_number) noexcept {
  return syscall_number >= 0 && syscall_number < static_cast<long>(SyscallNumber::MAX_SYSCALL);
}

void SyscallDispatcher::get_syscall_stats(u64 *total_calls, u64 *implemented_calls) noexcept {
  if (total_calls) {
    *total_calls = g_syscall_stats.total_syscalls;
  }

  if (implemented_calls) {
    u64 count = 0;
    for (int i = 0; i < static_cast<int>(SyscallNumber::MAX_SYSCALL); ++i) {
      if (SYSCALL_TABLE[i].implemented) {
        ++count;
      }
    }
    *implemented_calls = count;
  }
}

void SyscallDispatcher::print_implemented_syscalls() noexcept {
  namespace log = moss::kernel::logging;

  log::klog::info("=== implemented syscalls ===");
  for (int i = 0; i < static_cast<int>(SyscallNumber::MAX_SYSCALL); ++i) {
    const auto &desc = SYSCALL_TABLE[i];
    if (desc.implemented) {
      log::klog::info("  [{}] {}", i, desc.name);
    }
  }
  log::klog::info("============================");
}

} // namespace moss::kernel::syscall
