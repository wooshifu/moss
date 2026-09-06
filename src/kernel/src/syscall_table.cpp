// MOSS内核系统调用表实现
// 提供完整的系统调用处理和分发机制

module;

#ifdef MOSS_ARCH_X86_64
// Cross-module interrupt dispatch callback (defined in boot_impl.cpp)
extern "C" void (*g_x86_64_uart_rx_handler)() noexcept;
#endif

module moss.kernel;

import moss.abi;
import moss.vfs;

// Assembly symbols from moss.abi
using moss::abi::context_switch;
using moss::abi::switch_to_user;

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

// ── User pointer validation (copy_from_user / copy_to_user) ────────────
//
// Every syscall that dereferences a user-space pointer MUST first validate
// that the entire range falls inside a legitimate VMA with the correct
// permissions.  This prevents a malicious program from tricking the kernel
// into reading/writing arbitrary physical memory.

/// Return the calling process's AddressSpace, or nullptr.
static process::AddressSpace *get_current_address_space() noexcept {
  using namespace moss::kernel::process;
  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return nullptr;
  }
  Process *proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : nullptr;
  if (!proc) {
    return nullptr;
  }
  return proc->address_space();
}

/// Validate that [user_addr, user_addr+len) lies within a single VMA that
/// has the given permission flags (vma_flags::READ / WRITE).
static bool validate_user_range(u64 user_addr, usize len, u32 required_flags) noexcept {
  using namespace moss::kernel::process;
  if (len == 0) {
    return true;
  }
  if (user_addr == 0) {
    return false;
  }
  // Overflow check
  if (user_addr + len < user_addr) {
    return false;
  }
  auto *as = get_current_address_space();
  if (!as) {
    return false;
  }
  const auto *vma = as->find_vma(static_cast<VirtAddr>(user_addr));
  if (!vma) {
    return false;
  }
  // Entire range must stay inside the same VMA
  if (static_cast<VirtAddr>(user_addr + len) > vma->end_addr) {
    return false;
  }
  // Permission check
  if ((vma->flags & required_flags) != required_flags) {
    return false;
  }
  return true;
}

/// Copy `len` bytes from validated user address to a kernel buffer.
/// Returns 0 on success, -EFAULT if the range is invalid.
static long copy_from_user(void *kernel_dst, u64 user_src, usize len) noexcept {
  using namespace moss::kernel::process;
  if (!validate_user_range(user_src, len, vma_flags::READ)) {
    return -errc::EFAULT;
  }
  const auto *src = reinterpret_cast<const u8 *>(static_cast<usize>(user_src));
  auto *dst = static_cast<u8 *>(kernel_dst);
  for (usize i = 0; i < len; ++i) {
    dst[i] = src[i];
  }
  return 0;
}

/// Copy `len` bytes from a kernel buffer to a validated user address.
/// Returns 0 on success, -EFAULT if the range is invalid.
static long copy_to_user(u64 user_dst, const void *kernel_src, usize len) noexcept {
  using namespace moss::kernel::process;
  if (!validate_user_range(user_dst, len, vma_flags::WRITE)) {
    return -errc::EFAULT;
  }
  auto *dst = reinterpret_cast<volatile u8 *>(static_cast<usize>(user_dst));
  const auto *src = static_cast<const u8 *>(kernel_src);
  for (usize i = 0; i < len; ++i) {
    dst[i] = src[i];
  }
  return 0;
}

/// Copy a NUL-terminated string from user space into a kernel buffer.
/// Validates VMA read permission.  Copies at most `max_len - 1` bytes
/// plus a trailing '\0'.  Returns 0 on success, -EFAULT on bad pointer.
static long copy_string_from_user(char *kernel_dst, u64 user_src, usize max_len) noexcept {
  using namespace moss::kernel::process;
  if (user_src == 0 || max_len == 0) {
    return -errc::EFAULT;
  }
  auto *as = get_current_address_space();
  if (!as) {
    return -errc::EFAULT;
  }
  const auto *vma = as->find_vma(static_cast<VirtAddr>(user_src));
  if (!vma || (vma->flags & vma_flags::READ) == 0) {
    return -errc::EFAULT;
  }

  const auto *src = reinterpret_cast<const char *>(static_cast<usize>(user_src));
  VirtAddr vma_end = vma->end_addr;
  usize i = 0;
  for (; i < max_len - 1; ++i) {
    if (static_cast<VirtAddr>(user_src + i) >= vma_end) {
      return -errc::EFAULT;
    }
    kernel_dst[i] = src[i];
    if (src[i] == '\0') {
      return 0;
    }
  }
  kernel_dst[i] = '\0';
  return 0; // truncated but valid
}

// 基础系统调用处理函数
long sys_debug_print(long arg0, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                     long /*unused*/) noexcept {
  if (arg0 == 0) {
    return -errc::EINVAL;
  }
  char buf[256];
  if (copy_string_from_user(buf, static_cast<u64>(arg0), sizeof(buf)) < 0) {
    return -errc::EFAULT;
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
  Process *proc = g_process_manager ? g_process_manager->find_process(pid) : nullptr;
  if (!proc) {
    log::klog::error("sys_exit: process not found PID={}", pid);
    while (true) {
      ::moss::kernel::arch::cpu_yield();
    }
  }

  // Delegate to shared Zombie transition (never returns)
  do_exit(cur, proc, static_cast<i32>(exit_code));
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
  Process *proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : nullptr;
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
  Process *proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : nullptr;
  return proc ? static_cast<long>(proc->uid()) : 0;
}

long sys_getgid(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                long /*unused*/) noexcept {
  using namespace moss::kernel::process;
  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return 0;
  }
  Process *proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : nullptr;
  return proc ? static_cast<long>(proc->gid()) : 0;
}

// fork() — create a child process with COW-shared address space.
// Child returns 0, parent returns child PID.
long sys_fork(long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
              long /*unused*/) noexcept {
  using namespace moss::kernel::process;
  namespace log = moss::kernel::logging;

  // 1. Get current thread and process
  Thread *parent_thread = CfsScheduler::get_current_task();
  if (!parent_thread) {
    log::klog::error("sys_fork: no current thread");
    return -errc::EAGAIN;
  }

  Process *parent_proc = g_process_manager ? g_process_manager->find_process(parent_thread->owner_pid) : nullptr;
  if (!parent_proc || !parent_proc->address_space()) {
    log::klog::error("sys_fork: no parent process or address space");
    return -errc::EAGAIN;
  }

  AddressSpace *parent_as = parent_proc->address_space();

  // 2. Capture user-mode PC and SP from the trap frame on the kernel stack.
  //
  //    parent_thread->context contains kernel-mode register state (saved by
  //    context_switch), NOT user-space GP registers.  The actual user state
  //    was saved by the trap entry code into a frame at the top of the
  //    per-thread kernel stack.
  u64 user_pc = 0;
  u64 user_sp = 0;
#if defined(MOSS_ARCH_ARM64)
  asm volatile("mrs %0, elr_el1" : "=r"(user_pc));
  asm volatile("mrs %0, sp_el0" : "=r"(user_sp));
#elif defined(MOSS_ARCH_RISCV)
  // RISC-V trap frame layout (from riscv_syscall.S, FRAME_SIZE = 288):
  //   OFF_SEPC = 0xE0: saved sepc (already +4 to skip ecall instruction)
  //   OFF_USP  = 0xF0: saved user sp (from sscratch swap on trap entry)
  // The final 16 bytes preserve the CPU identity across user-mode traps.
  {
    u64 kstop = parent_thread->kernel_stack_top();
    const auto *trap_frame = reinterpret_cast<const u64 *>(kstop - 16 - 288);
    user_pc = trap_frame[0xE0 / 8]; // sepc (+4, past ecall)
    user_sp = trap_frame[0xF0 / 8]; // user sp
  }
#elif defined(MOSS_ARCH_X86_64)
  const auto *syscall_frame = reinterpret_cast<const u64 *>(parent_thread->kernel_stack_top() - 128);
  user_pc = syscall_frame[1];
  user_sp = syscall_frame[15];
#endif

  // 3. Create child process
  auto child_proc_result = g_process_manager->create_process(parent_proc->pid());
  if (!child_proc_result) {
    log::klog::error("sys_fork: create_process failed");
    return -errc::ENOMEM;
  }
  Process *child_proc = *child_proc_result;

  // Helper: clean up the child process on error (removes from process
  // table and triggers ~Process which frees address space, threads, etc.)
  auto cleanup_child = [&](Process *cp) {
    if (g_process_manager) {
      (void)g_process_manager->terminate_process(cp->pid(), -1);
    }
  };

  // 4. Create child address space (new PGD + ASID)
  auto child_as_result = user_space::create_user_address_space();
  if (!child_as_result) {
    log::klog::error("sys_fork: create_user_address_space failed");
    cleanup_child(child_proc);
    return -errc::ENOMEM;
  }
  auto child_as = moss::move(*child_as_result);

  // 5. Clone page tables with COW
  mm::PageTableManager::clone_user_page_tables(parent_as->pgd_phys, child_as->pgd_phys);

  // 6. Flush parent TLB (PTEs changed to readonly/COW)
#if defined(MOSS_ARCH_ARM64)
  {
    u64 asid_val = static_cast<u64>(parent_as->asid) << 48;
    asm volatile("tlbi aside1is, %0" ::"r"(asid_val));
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb" ::: "memory");
  }
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("sfence.vma" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("mov %0, %%cr3" ::"r"(parent_as->pgd_phys) : "memory");
#endif

  // 7. Copy VMAs from parent to child via RcuList iteration
  parent_as->vmas.for_each([&child_as](const process::VmaRegion &vma) { child_as->vmas.push_front(vma); });

  // 8. Bind address space to child process
  auto set_result = child_proc->set_address_space(moss::move(child_as));
  if (!set_result) {
    log::klog::error("sys_fork: set_address_space failed");
    // child_as was moved — if set failed, unique_ptr may still own it
    // and ~AddressSpace will free the page tables.
    cleanup_child(child_proc);
    return -errc::ENOMEM;
  }

  // 9. Create child thread
  ThreadId child_tid = Process::allocate_thread_id();
  auto *child_thread = new Thread(child_tid, child_proc->pid());
  if (!child_thread) {
    log::klog::error("sys_fork: thread allocation failed");
    cleanup_child(child_proc);
    return -errc::ENOMEM;
  }

  // 10. Copy parent's USER-SPACE registers → child context.
  //
  // parent_thread->context contains KERNEL-mode state (from the last
  // context_switch), NOT user-space GP registers.  The actual user
  // registers were saved by lower_el_sync_dispatch in a 34-slot frame
  // at the top of the kernel stack:
  //   [kstop - 272 + 0*8] = user x0
  //   [kstop - 272 + 1*8] = user x1
  //   ...
  //   [kstop - 272 + 30*8] = user x30
  //   [kstop - 272 + 31*8] = ELR_EL1
  //   [kstop - 272 + 32*8] = SPSR_EL1
  //   [kstop - 272 + 33*8] = SP_EL0
#if defined(MOSS_ARCH_ARM64)
  {
    // Read user GP registers from the syscall entry frame on
    // the parent's kernel stack.
    u64 kstop = parent_thread->kernel_stack_top();
    const auto *trap_frame = reinterpret_cast<const u64 *>(kstop - 34ULL * 8);

    // Copy all 31 GP registers (x0-x30) from trap frame
    for (int i = 0; i < 31; ++i) {
      child_thread->context.x[i] = trap_frame[i];
    }

    // Child fork returns 0
    child_thread->context.x[0] = 0;
  }
#elif defined(MOSS_ARCH_X86_64)
  auto &context = child_thread->context;
  context.rax = 0;
  context.r11 = syscall_frame[0];
  context.rcx = syscall_frame[1];
  context.r9 = syscall_frame[2];
  context.r8 = syscall_frame[3];
  context.r10 = syscall_frame[4];
  context.rdx = syscall_frame[5];
  context.rsi = syscall_frame[6];
  context.rdi = syscall_frame[7];
  context.r15 = syscall_frame[9];
  context.r14 = syscall_frame[10];
  context.r13 = syscall_frame[11];
  context.r12 = syscall_frame[12];
  context.rbx = syscall_frame[13];
  context.rbp = syscall_frame[14];
#elif defined(MOSS_ARCH_RISCV)
  const auto *frame = reinterpret_cast<const u64 *>(parent_thread->kernel_stack_top() - 16 - 288);
  auto &registers = child_thread->context.x;
  registers[1] = frame[0];
  registers[3] = frame[0x108 / 8];
  registers[4] = frame[0x100 / 8];
  for (unsigned i = 0; i < 3; ++i) {
    registers[5 + i] = frame[1 + i];
  }
  for (unsigned i = 0; i < 4; ++i) {
    registers[28 + i] = frame[4 + i];
  }
  for (unsigned i = 1; i < 8; ++i) {
    registers[10 + i] = frame[8 + i];
  }
  registers[8] = frame[16];
  registers[9] = frame[17];
  for (unsigned i = 0; i < 10; ++i) {
    registers[18 + i] = frame[18 + i];
  }
  registers[10] = 0;
#endif
  child_thread->context.pc = user_pc; // return to instruction after SVC
  child_thread->context.sp = user_sp; // same user stack
  child_thread->context.pstate = 0;   // EL0t, all interrupts enabled

  child_thread->stack_base = parent_thread->stack_base;
  child_thread->stack_size = parent_thread->stack_size;
  child_thread->needs_initial_eret = true;
  child_thread->is_user_task = true;
  child_thread->sched_class = SchedClass::Normal;
  child_thread->se.nice = parent_thread->se.nice;
  child_thread->se.weight = parent_thread->se.weight;
  child_thread->cpu_affinity_mask = parent_thread->cpu_affinity_mask;
  child_thread->state = ProcessState::Ready;

  // 11. Allocate per-thread kernel stack (16KB)
  constexpr usize KERNEL_STACK_ORDER = 2; // 4 pages = 16KB
  constexpr usize KERNEL_STACK_SIZE = PAGE_SIZE << KERNEL_STACK_ORDER;
  auto kstack_result = mm::allocate_pages(KERNEL_STACK_ORDER);
  if (!kstack_result) {
    log::klog::error("sys_fork: kernel stack alloc failed");
    delete child_thread;
    cleanup_child(child_proc);
    return -errc::ENOMEM;
  }
  PhysAddr kstack_phys = *kstack_result;
  child_thread->kernel_stack_base = static_cast<VirtAddr>(kstack_phys);
  child_thread->kernel_stack_size = KERNEL_STACK_SIZE;

  // 12. Register child thread in child process's thread list
  child_proc->register_thread(child_thread);

  // 12b. Clone VFS fd table from parent to child
  if (parent_proc->fd_table() != nullptr) {
    auto *parent_fdt = static_cast<moss::kernel::vfs::FdTable *>(parent_proc->fd_table());
    auto *child_fdt = parent_fdt->clone();
    child_proc->set_fd_table(child_fdt);
  }

  // 12c. Inherit process name from parent
  child_proc->set_name(parent_proc->name());

  // 12d. Inherit process group and session from parent (POSIX semantics)
  child_proc->set_pgid(parent_proc->pgid());
  child_proc->set_sid(parent_proc->sid());

  // 13. Register child in parent's children list (for waitpid)
  parent_proc->add_child(child_proc->pid());

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

long sys_execve(long pathname_addr, long argv_addr, long /* envp */, long /*unused*/, long /*unused*/,
                long /*unused*/) noexcept {
  using namespace moss::kernel::process;
  using namespace moss::kernel::elf;
  // 1. Get current thread and process
  Thread *cur = g_scheduler ? CfsScheduler::get_current_task() : nullptr;
  if (!cur) {
    log::klog::error("execve: no current task");
    return -errc::ESRCH;
  }
  Process *proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : nullptr;
  if (!proc || !proc->address_space()) {
    log::klog::error("execve: no process or address space");
    return -errc::ESRCH;
  }

  // 2. Copy pathname from user memory into kernel buffer (with VMA validation).
  //    After step 5 switches TTBR0 to the kernel PGD, user addresses
  //    are no longer accessible, so we must capture the string now.
  constexpr usize PATH_MAX = 256;
  char pathname_buf[PATH_MAX];
  if (copy_string_from_user(pathname_buf, static_cast<u64>(pathname_addr), PATH_MAX) < 0) {
    return -errc::EFAULT;
  }
  const char *pathname = pathname_buf;

  // 2a. Copy argv strings from user memory into kernel buffer (with VMA validation).
  //     Must be done before TTBR0 switch (user addresses become invalid).
  constexpr usize MAX_ARGS = 16;
  constexpr usize ARGV_BUF_SIZE = 512;
  char argv_buf[ARGV_BUF_SIZE]; // flat buffer for all strings
  usize argv_offsets[MAX_ARGS]; // offset of each string in argv_buf
  usize kernel_argc = 0;
  usize argv_buf_pos = 0;

  if (argv_addr != 0) {
    // Validate the argv pointer array itself (up to MAX_ARGS pointers + null terminator)
    // Read each pointer individually via copy_from_user
    for (usize ai = 0; ai < MAX_ARGS; ++ai) {
      usize arg_ptr = 0;
      u64 ptr_addr = static_cast<u64>(argv_addr) + ai * sizeof(usize);
      if (copy_from_user(&arg_ptr, ptr_addr, sizeof(arg_ptr)) < 0) {
        return -errc::EFAULT;
      }
      if (arg_ptr == 0) {
        break; // null terminator
      }
      argv_offsets[kernel_argc] = argv_buf_pos;
      usize remaining = ARGV_BUF_SIZE - argv_buf_pos;
      if (remaining <= 1) {
        break; // buffer exhausted
      }
      if (copy_string_from_user(&argv_buf[argv_buf_pos], static_cast<u64>(arg_ptr), remaining) < 0) {
        return -errc::EFAULT;
      }
      // Advance past the copied string (including null terminator)
      while (argv_buf_pos < ARGV_BUF_SIZE && argv_buf[argv_buf_pos] != '\0') {
        ++argv_buf_pos;
      }
      if (argv_buf_pos < ARGV_BUF_SIZE) {
        ++argv_buf_pos; // skip '\0'
      }
      ++kernel_argc;
    }
  }

  // 2b. Set process name from pathname basename
  {
    const char *basename = pathname;
    for (const char *p = pathname; *p; ++p) {
      if (*p == '/') {
        basename = p + 1;
      }
    }
    proc->set_name(basename);
  }

  // 3. Resolve file via VFS path resolution (replaces direct initramfs access)
  auto *dentry = moss::kernel::vfs::resolve_path(pathname);
  if (!dentry || !dentry->inode) {
    log::klog::error("execve: '{}' not found via VFS", pathname);
    return -errc::ENOENT;
  }
  auto *file_inode = dentry->inode;
  if (file_inode->type != moss::kernel::vfs::FileType::Regular) {
    log::klog::error("execve: '{}' is not a regular file", pathname);
    return -errc::EACCES;
  }
  if (file_inode->data == nullptr || file_inode->size == 0) {
    log::klog::error("execve: '{}' has no data", pathname);
    return -errc::ENOEXEC;
  }

  // 4. Validate ELF header (inode->data = zero-copy ELF backing)
  const auto *elf_hdr = reinterpret_cast<const ElfHeader *>(file_inode->data);
  if (!validate_elf_header(elf_hdr, file_inode->size)) {
    log::klog::error("execve: '{}' is not a valid ELF", pathname);
    return -errc::ENOEXEC;
  }

  VirtAddr elf_entry = elf_hdr->e_entry;
  const auto *phdrs = get_program_headers(elf_hdr);
  u16 phnum = elf_hdr->e_phnum;

  // ===== Point of no return =====
  // From here, errors terminate the process (old address space is gone).

  // 5. Switch TTBR0 to kernel PGD (safe teardown)
  AddressSpace *old_as = proc->address_space();
  PhysAddr old_pgd = old_as->pgd_phys;

#if defined(MOSS_ARCH_ARM64)
  {
    auto *kpgd = mm::PageTableManager::get_kernel_pgd();
    if (kpgd) {
      u64 kpgd_phys = mm::PageTableManager::get_physical_address(kpgd);
      asm volatile("msr ttbr0_el1, %0" ::"r"(kpgd_phys));
      asm volatile("dsb ish" ::: "memory");
      asm volatile("isb" ::: "memory");
    }
  }
#elif defined(MOSS_ARCH_RISCV)
  // Switch SATP to kernel PGD before freeing old user page tables.
  {
    auto *kpgd = mm::PageTableManager::get_kernel_pgd();
    if (kpgd) {
      u64 kpgd_phys = mm::PageTableManager::get_physical_address(kpgd);
      u64 satp_val = hal::mmu::make_satp_value(kpgd_phys);
      asm volatile("csrw satp, %0" ::"r"(satp_val) : "memory");
      asm volatile("sfence.vma" ::: "memory");
    }
  }
#elif defined(MOSS_ARCH_X86_64)
  // Switch CR3 to kernel PGD before freeing old user page tables.
  {
    auto *kpgd = mm::PageTableManager::get_kernel_pgd();
    if (kpgd) {
      u64 kpgd_phys = mm::PageTableManager::get_physical_address(kpgd);
      asm volatile("mov %0, %%cr3" ::"r"(kpgd_phys) : "memory");
    }
  }
#endif

  // 6. Free old user page tables
  if (old_pgd != 0) {
    mm::PageTableManager::free_user_page_tables(old_pgd);
    old_as->pgd_phys = 0; // prevent double-free
  }

  // 7. Create new address space
  auto new_as_result = user_space::create_user_address_space();
  if (!new_as_result) {
    log::klog::error("execve: failed to create new address space");
    // Unrecoverable — process has no address space
    cur->state = ProcessState::Terminated;
    if (g_scheduler) {
      g_scheduler->dequeue_task(cur);
      g_scheduler->schedule_after_exit();
    }
    while (true) {
      ::moss::kernel::arch::cpu_halt();
    }
  }
  auto new_as = moss::move(*new_as_result);

  // 8. Load PT_LOAD segments as VMAs
  //
  // Multiple PT_LOAD segments may fall within the same page (e.g.
  // .text at 0x400000 and .rodata at 0x400048 both within page
  // 0x400000-0x401000).  The demand-paging handler maps one page
  // per fault using a single VMA's backing data, so overlapping
  // VMAs would cause data loss.
  //
  // Solution: two-pass approach.
  //   Pass 1 — compute the overall VA range and file-offset range
  //            across all PT_LOAD segments.
  //   Pass 2 — create one merged VMA if all segments fit in the
  //            same page range, otherwise fall back to per-segment
  //            VMAs (safe when segments are page-separated).
  {
    constexpr u16 MAX_LOADS = 8;
    u16 load_count = 0;

    // Collect PT_LOAD segments
    VirtAddr overall_start = ~0ULL;
    VirtAddr overall_end = 0;
    u64 file_offset_min = ~0ULL;
    u64 file_offset_max = 0; // offset + filesz
    u32 merged_flags = 0;

    for (u16 i = 0; i < phnum && load_count < MAX_LOADS; ++i) {
      const auto &ph = phdrs[i];
      if (ph.p_type != PT_LOAD || ph.p_memsz == 0) {
        continue;
      }
      ++load_count;

      if (ph.p_vaddr < overall_start) {
        overall_start = ph.p_vaddr;
      }
      VirtAddr seg_end = ph.p_vaddr + ph.p_memsz;
      if (seg_end > overall_end) {
        overall_end = seg_end;
      }

      if (ph.p_filesz > 0) {
        if (ph.p_offset < file_offset_min) {
          file_offset_min = ph.p_offset;
        }
        u64 fo_end = ph.p_offset + ph.p_filesz;
        if (fo_end > file_offset_max) {
          file_offset_max = fo_end;
        }
      }

      if (ph.p_flags & PF_R) {
        merged_flags |= vma_flags::READ;
      }
      if (ph.p_flags & PF_W) {
        merged_flags |= vma_flags::WRITE;
      }
      if (ph.p_flags & PF_X) {
        merged_flags |= vma_flags::EXEC;
      }
    }

    // Page-align the overall range
    VirtAddr page_start = overall_start & ~(static_cast<VirtAddr>(PAGE_SIZE) - 1);
    VirtAddr page_end = (overall_end + PAGE_SIZE - 1) & ~(static_cast<VirtAddr>(PAGE_SIZE) - 1);

    // Check if any segments overlap when page-aligned.
    // This is common: .text ending at 0x36f8 and .rodata starting
    // at 0x36f8 share the same page (0x3000-0x4000).  Overlapping
    // VMAs cause demand-paging data loss, so we must merge.
    bool has_page_overlap = false;
    if (load_count > 1) {
      // Simple O(n²) check — MAX_LOADS ≤ 8
      struct {
        VirtAddr s;
        VirtAddr e;
      } ranges[MAX_LOADS];
      u16 ri = 0;
      for (u16 i = 0; i < phnum && ri < MAX_LOADS; ++i) {
        const auto &ph2 = phdrs[i];
        if (ph2.p_type != PT_LOAD || ph2.p_memsz == 0) {
          continue;
        }
        ranges[ri].s = ph2.p_vaddr & ~(static_cast<VirtAddr>(PAGE_SIZE) - 1);
        ranges[ri].e = (ph2.p_vaddr + ph2.p_memsz + PAGE_SIZE - 1) & ~(static_cast<VirtAddr>(PAGE_SIZE) - 1);
        ++ri;
      }
      for (u16 a = 0; a < ri && !has_page_overlap; ++a) {
        for (u16 b = a + 1; b < ri; ++b) {
          if (ranges[a].s < ranges[b].e && ranges[b].s < ranges[a].e) {
            has_page_overlap = true;
            break;
          }
        }
      }
    }

    bool use_merged = has_page_overlap || load_count <= 1;

    if (use_merged && load_count > 0) {
      // Merged VMA: one VMA covering all PT_LOAD segments.
      // backing_offset accounts for the gap between page_start and
      // the first byte of file data in the ELF.
      VmaType vma_type = (merged_flags & vma_flags::EXEC) ? VmaType::CODE : VmaType::DATA;

      const u8 *backing = nullptr;
      usize backing_size = 0;
      u64 backing_offset = 0;
      if (file_offset_max > file_offset_min) {
        backing = file_inode->data + file_offset_min;
        backing_size = static_cast<usize>(file_offset_max - file_offset_min);
        // backing_offset = how far into the page the data starts
        backing_offset = overall_start - page_start;
      }

      if (backing_size == 0) {
        merged_flags |= vma_flags::DEMAND_ZERO;
      }

      new_as->add_vma(page_start, page_end, merged_flags, vma_type, backing, backing_offset, backing_size);

    } else {
      // Separate VMAs for page-separated segments (general case)
      for (u16 i = 0; i < phnum; ++i) {
        const auto &ph = phdrs[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) {
          continue;
        }

        u32 vma_flags = 0;
        if (ph.p_flags & PF_R) {
          vma_flags |= vma_flags::READ;
        }
        if (ph.p_flags & PF_W) {
          vma_flags |= vma_flags::WRITE;
        }
        if (ph.p_flags & PF_X) {
          vma_flags |= vma_flags::EXEC;
        }

        VmaType vma_type = VmaType::DATA;
        if ((ph.p_flags & PF_X) && !(ph.p_flags & PF_W)) {
          vma_type = VmaType::CODE;
        }

        VirtAddr seg_start = ph.p_vaddr;
        VirtAddr seg_end = (seg_start + ph.p_memsz + PAGE_SIZE - 1) & ~(static_cast<VirtAddr>(PAGE_SIZE) - 1);

        const u8 *backing = (ph.p_filesz > 0) ? (file_inode->data + ph.p_offset) : nullptr;
        usize b_size = static_cast<usize>(ph.p_filesz);

        if (ph.p_filesz == 0) {
          vma_flags |= vma_flags::DEMAND_ZERO;
        }

        new_as->add_vma(seg_start, seg_end, vma_flags, vma_type, backing, 0, b_size);

        log::klog::info("  PT_LOAD: {:#x}-{:#x} filesz={} memsz={}", seg_start, seg_end, static_cast<u64>(ph.p_filesz),
                        static_cast<u64>(ph.p_memsz));
      }
    }
  }

  // 9a. Sigreturn trampoline VMA (read + exec) — signal handler LR points here
  {
    static constexpr u8 sigreturn_stub[] = {
        0x28, 0x02, 0x80, 0xD2, // mov x8, #0x11 (17 = SYS_SIGRETURN)
        0x01, 0x00, 0x00, 0xD4, // svc #0
    };
    new_as->add_vma(user_layout::SIGRETURN_PAGE, user_layout::SIGRETURN_PAGE + PAGE_SIZE,
                    vma_flags::READ | vma_flags::EXEC, VmaType::CODE, sigreturn_stub, 0, sizeof(sigreturn_stub));
  }

  // 9b. Add stack VMA (demand-zero)
  const VirtAddr stack_bottom = user_layout::STACK_TOP - user_layout::STACK_SIZE;
  new_as->add_vma(stack_bottom, user_layout::STACK_TOP, vma_flags::READ | vma_flags::WRITE | vma_flags::DEMAND_ZERO,
                  VmaType::STACK);

  // 10. Add heap VMA (demand-zero) and initialize program break
  new_as->add_vma(user_layout::HEAP_START, user_layout::HEAP_START + user_layout::HEAP_INIT,
                  vma_flags::READ | vma_flags::WRITE | vma_flags::DEMAND_ZERO, VmaType::HEAP);
  new_as->brk_base = user_layout::HEAP_START;
  new_as->brk_current = user_layout::HEAP_START;
  new_as->mmap_next = user_layout::MMAP_BASE;

  // 11. Bind new address space to process
  auto set_result = proc->set_address_space(moss::move(new_as));
  if (!set_result) {
    log::klog::error("execve: set_address_space failed");
    cur->state = ProcessState::Terminated;
    if (g_scheduler) {
      g_scheduler->dequeue_task(cur);
      g_scheduler->schedule_after_exit();
    }
    while (true) {
      ::moss::kernel::arch::cpu_halt();
    }
  }

  // 11b. Switch TTBR0 to the new address space BEFORE writing to user
  //      stack.  Steps 5-6 switched TTBR0 to kernel PGD for safe teardown;
  //      now that the new address space is bound, we need user-space
  //      mappings active so demand-paging works when we write argv data.
#if defined(MOSS_ARCH_ARM64)
  if (proc->address_space() && proc->address_space()->pgd_phys != 0) {
    u64 ttbr0_val = proc->address_space()->pgd_phys | (static_cast<u64>(proc->address_space()->asid) << 48);
    asm volatile("msr ttbr0_el1, %0" ::"r"(ttbr0_val));
    asm volatile("tlbi aside1, %0" ::"r"(static_cast<u64>(proc->address_space()->asid) << 48));
    asm volatile("dsb sy" ::: "memory");
    asm volatile("isb" ::: "memory");
  }
#elif defined(MOSS_ARCH_RISCV)
  if (proc->address_space() && proc->address_space()->pgd_phys != 0) {
    u64 satp_val = hal::mmu::make_satp_value(proc->address_space()->pgd_phys, proc->address_space()->asid);
    asm volatile("csrw satp, %0" ::"r"(satp_val) : "memory");
    asm volatile("sfence.vma" ::: "memory");
  }
#elif defined(MOSS_ARCH_X86_64)
  if (proc->address_space() && proc->address_space()->pgd_phys != 0) {
    u64 cr3_val = proc->address_space()->pgd_phys;
    asm volatile("mov %0, %%cr3" ::"r"(cr3_val) : "memory");
  }
#endif

  // 12. Set up user stack with argc/argv, then reset thread context.
  //
  // Standard C ABI: _start receives argc in x0, argv in x1.
  // We place the argv string data and pointer array on the user stack:
  //
  //   [STACK_TOP - 16]  (alignment padding)
  //   ...strings...     null-terminated argv strings
  //   argv[argc] = NULL
  //   argv[argc-1]      pointers to strings (user VAs)
  //   ...
  //   argv[0]
  //   <--- SP (16-byte aligned)
  //
  VirtAddr user_sp = user_layout::STACK_TOP - 16;
  if (kernel_argc > 0) {
    // Phase 1: calculate where strings will live on user stack.
    // Strings are placed first (high addresses), then argv[] array below.
    VirtAddr strings_base = user_sp - argv_buf_pos;
    strings_base &= ~static_cast<VirtAddr>(0x7); // 8-byte align

    // Phase 2: build argv[] pointer array (points to user VAs)
    // argv[0..argc-1] + argv[argc]=NULL
    usize argv_array_size = (kernel_argc + 1) * sizeof(u64);
    VirtAddr argv_base = strings_base - argv_array_size;
    argv_base &= ~static_cast<VirtAddr>(0xF); // 16-byte align SP

    user_sp = argv_base;

    // Phase 3: write strings and argv[] to user stack.
    // Note: these user addresses are demand-zero pages.  Writing to
    // them triggers kernel page faults that are resolved by the
    // kernel_page_fault_handler (which handles user addresses via
    // demand paging).  The data is written via volatile pointers to
    // prevent the compiler from optimizing away the stores.

    // Write string data
    {
      auto *dst = reinterpret_cast<volatile char *>(strings_base);
      for (usize i = 0; i < argv_buf_pos; ++i) {
        dst[i] = argv_buf[i];
      }
    }

    // Write argv[] pointer array
    {
      auto *argv_ptrs = reinterpret_cast<volatile u64 *>(argv_base);
      for (usize i = 0; i < kernel_argc; ++i) {
        argv_ptrs[i] = strings_base + argv_offsets[i];
      }
      argv_ptrs[kernel_argc] = 0; // NULL terminator
    }
  }

  cur->context = CpuContext{}; // zero all registers
  cur->context.pc = elf_entry;
  cur->context.sp = user_sp;
  cur->context.pstate = 0; // EL0t
#if defined(MOSS_ARCH_ARM64)
  cur->context.x[0] = kernel_argc;      // x0 = argc
  cur->context.x[1] = (kernel_argc > 0) // x1 = argv
                          ? (user_sp)   // argv_base == user_sp
                          : 0;
#elif defined(MOSS_ARCH_RISCV)
  cur->context.x[10] = kernel_argc;      // a0 = argc
  cur->context.x[11] = (kernel_argc > 0) // a1 = argv
                           ? (user_sp)   // argv_base == user_sp
                           : 0;
#elif defined(MOSS_ARCH_X86_64)
  cur->context.rdi = kernel_argc;      // rdi = argc (System V ABI arg0)
  cur->context.rsi = (kernel_argc > 0) // rsi = argv (System V ABI arg1)
                         ? (user_sp)   // argv_base == user_sp
                         : 0;
  cur->context.pstate = 0x202; // RFLAGS: IF=1 (interrupts enabled on iretq)
#endif
  cur->needs_initial_eret = true; // next dispatch does switch_to_user + eret
  cur->stack_base = stack_bottom;
  cur->stack_size = user_layout::STACK_SIZE;

  // 13. Direct eret to new program image.
  //
  // execve is called from a syscall handler (EL1), so we can eret
  // directly to the new ELF entry point.  This is safe because:
  //   - We already rebuilt the address space and page tables
  //   - switch_to_user sets up ELR_EL1/SPSR_EL1/SP_EL0 and does eret
  //   - When the new program is later preempted by timer IRQ,
  //     irq_trampoline saves its state via context_switch, and
  //     bootstrap_contexts_[cpu] is already valid (saved by the
  //     user_eret_trampoline path that initially dispatched this task).
#if defined(MOSS_ARCH_ARM64)
  {
    cur->needs_initial_eret = false;
    cur->state = ProcessState::Running;

    arch::disable_interrupts();

    // TTBR0 already switched to new address space in step 11b.

    // Set TPIDR_EL1 for per-thread kernel stack
    if (cur->kernel_stack_base != 0) {
      u64 kstack_top = cur->kernel_stack_top();
      asm volatile("msr tpidr_el1, %0" ::"r"(kstack_top));
    }

    // eret to new program — never returns
    switch_to_user(&cur->context, cur->context.sp);
  }
#elif defined(MOSS_ARCH_RISCV)
  {
    cur->needs_initial_eret = false;
    cur->state = ProcessState::Running;

    arch::disable_interrupts();

    // SATP already switched to new address space in step 11b.

    // Set sscratch to per-thread kernel stack top so the next U-mode
    // trap entry swaps to the correct kernel stack.  switch_to_user
    // also writes sscratch, but with the *current* sp (which is the
    // C call stack, not kernel_stack_top).  We must override it.
    if (cur->kernel_stack_base != 0) {
      u64 kstack_top = cur->kernel_stack_top();
      arch::set_user_kernel_stack(kstack_top);
    }

    // eret to new program — never returns
    switch_to_user(&cur->context, cur->context.sp);
  }
#elif defined(MOSS_ARCH_X86_64)
  {
    cur->needs_initial_eret = false;
    cur->state = ProcessState::Running;

    arch::disable_interrupts();

    // CR3 already switched to new address space in step 11b.

    // Update TSS RSP0 and SYSCALL kernel stack for the new program.
    if (cur->kernel_stack_base != 0) {
      u64 kstack_top = cur->kernel_stack_top();
      moss::abi::x86_64::set_kernel_stack(kstack_top);
    }

    // iretq to new program — never returns
    switch_to_user(&cur->context, cur->context.sp);
  }
#endif

  // Should not reach here (switch_to_user does eret)
  while (true) {
    ::moss::kernel::arch::cpu_halt();
  }
}

// wait4(pid, wstatus, options, rusage) — wait for child process state change
// pid > 0: wait for specific child
// pid == -1: wait for any child
// options: WNOHANG (1) = return immediately if no child has exited
long sys_wait4(long wait_pid, long wstatus_addr, long options, long /*unused*/, long /*unused*/,
               long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  constexpr long WNOHANG = 1;

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return -errc::EINVAL;
  }

  Process *proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : nullptr;
  if (!proc) {
    return -errc::EINVAL;
  }

  // Must have children
  if (!proc->has_children()) {
    return -errc::ECHILD;
  }

  while (true) {
    // Scan for matching zombie child
    ProcessId zombie_pid = proc->find_zombie_child(wait_pid);

    if (zombie_pid != INVALID_PROCESS_ID) {
      // Found a zombie — reap it
      Process *zombie = g_process_manager->find_process(zombie_pid);
      if (!zombie) {
        // Race: already reaped by another thread, retry
        continue;
      }

      i32 child_exit_code = zombie->exit_code();
      ProcessId result_pid = zombie->pid();

      // Remove from parent's children list
      proc->remove_child(zombie_pid);

      // Remove from process table and free Process object
      // terminate_process sets Terminated + removes from table + release()
      (void)g_process_manager->terminate_process(zombie_pid, child_exit_code);

      // Write status to user space if pointer is non-null
      // Linux WEXITSTATUS encoding: (exit_code & 0xFF) << 8
      if (wstatus_addr != 0) {
        int wstatus = (static_cast<int>(child_exit_code) & 0xFF) << 8;
        if (copy_to_user(static_cast<u64>(wstatus_addr), &wstatus, sizeof(wstatus)) < 0) {
          return -errc::EFAULT;
        }
      }

      return static_cast<long>(result_pid);
    }

    // No zombie found
    // Check if specified PID is actually a child
    if (wait_pid > 0 && !proc->is_child(static_cast<ProcessId>(wait_pid))) {
      return -errc::ECHILD;
    }

    // WNOHANG: non-blocking, return 0
    if (options & WNOHANG) {
      return 0;
    }

    // Block: add self to wait queue, set Sleeping (interruptible), dequeue.
    // When a child calls sys_exit, it wakes all waiters on parent's WQ,
    // setting them back to Ready and re-enqueueing them.  The thread
    // then resumes here (after being re-dispatched by scheduler_tick's
    // context_switch) and loops back to rescan for zombies.
    // Sleeping = TASK_INTERRUPTIBLE: a future signal could wake us early.
    proc->child_exit_wait_queue().add_waiter(static_cast<void *>(cur), /*exclusive=*/true);
    cur->state = ProcessState::Sleeping;
    if (g_scheduler) {
      g_scheduler->dequeue_task(cur);
    }

    // Yield CPU: switch to bootstrap context, let scheduler pick next task.
    // When this thread is woken (state=Ready, re-enqueued), scheduler_tick
    // will context_switch back and we resume after this point.
    {
      u32 cpu = arch::get_current_cpu_id();
      CpuContext *my_ctx = &cur->context;
      CpuContext *bootstrap = &CfsScheduler::bootstrap_context(cpu);

      CfsScheduler::set_current_task(nullptr);
      arch::disable_interrupts();
      context_switch(my_ctx, bootstrap);
      arch::enable_interrupts();
    }
    // Resumed — remove self from wait queue and rescan
    proc->child_exit_wait_queue().remove_waiter(static_cast<void *>(cur));

    // Check we still have children (might have been reaped by another thread)
    if (!proc->has_children()) {
      return -errc::ECHILD;
    }
  }
}

// waitpid(pid, wstatus, options) — thin wrapper over wait4
long sys_waitpid(long pid, long wstatus, long options, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  return sys_wait4(pid, wstatus, options, 0, 0, 0);
}

// kill(pid, sig) — send signal to process.
//   pid > 0:  send to specific process
//   pid == 0: send to all processes in caller's process group
//   pid == -1: send to all processes (except init) — simplified
//   pid < -1: send to process group |pid|
long sys_kill(long pid_arg, long sig_arg, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  auto signo = static_cast<u32>(sig_arg);
  if (signo >= sig::NSIG) {
    return -errc::EINVAL;
  }

  // sig == 0: permission check only (no signal sent)
  if (signo == 0) {
    return 0;
  }

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur || !g_process_manager || !g_scheduler) {
    return -errc::ESRCH;
  }

  auto pid = static_cast<i64>(pid_arg);

  if (pid > 0) {
    // Send to specific process
    Process *target = g_process_manager->find_process(static_cast<ProcessId>(pid));
    if (!target) {
      return -errc::ESRCH;
    }
    Thread *main_thread = target->get_main_thread();
    if (!main_thread) {
      return -errc::ESRCH;
    }
    if (!send_signal(main_thread, signo)) {
      return -errc::EPERM;
    }
    // Wake the thread if it was sleeping (interruptible)
    if (main_thread->state == ProcessState::Sleeping) {
      g_scheduler->task_wakeup(main_thread, main_thread->cpu);
    }
    return 0;
  }

  if (pid == 0) {
    // Send to all processes in caller's process group
    Process *caller = g_process_manager->find_process(cur->owner_pid);
    if (!caller) {
      return -errc::ESRCH;
    }
    ProcessId my_pgid = caller->pgid();
    bool sent = false;
    g_process_manager->for_each_process([&](ProcessId, Process *proc) {
      if (proc->pgid() == my_pgid) {
        Thread *thr = proc->get_main_thread();
        if (thr && send_signal(thr, signo)) {
          if (thr->state == ProcessState::Sleeping) {
            g_scheduler->task_wakeup(thr, thr->cpu);
          }
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
      if (proc->pgid() == target_pgid) {
        Thread *thr = proc->get_main_thread();
        if (thr && send_signal(thr, signo)) {
          if (thr->state == ProcessState::Sleeping) {
            g_scheduler->task_wakeup(thr, thr->cpu);
          }
          sent = true;
        }
      }
    });
    return sent ? 0 : -errc::ESRCH;
  }

  // pid == -1: send to all (simplified — skip PID 0 and PID 1)
  bool sent = false;
  g_process_manager->for_each_process([&](ProcessId proc_pid, Process *proc) {
    if (proc_pid <= 1) {
      return; // skip kernel (0) and init (1)
    }
    Thread *thr = proc->get_main_thread();
    if (thr && send_signal(thr, signo)) {
      if (thr->state == ProcessState::Sleeping) {
        g_scheduler->task_wakeup(thr, thr->cpu);
      }
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
  Process *proc = g_process_manager->find_process(target);
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
  Process *proc = g_process_manager->find_process(target);
  if (!proc) {
    return -errc::ESRCH;
  }
  return static_cast<long>(proc->sid());
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

  Process *caller = g_process_manager->find_process(cur->owner_pid);
  if (!caller) {
    return -errc::ESRCH;
  }

  ProcessId target_pid = (pid_arg == 0) ? cur->owner_pid : static_cast<ProcessId>(pid_arg);
  ProcessId new_pgid = (pgid_arg == 0) ? target_pid : static_cast<ProcessId>(pgid_arg);

  Process *target = g_process_manager->find_process(target_pid);
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

  target->set_pgid(new_pgid);
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

  Process *proc = g_process_manager->find_process(cur->owner_pid);
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
  proc->set_pgid(proc->pid());
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

  auto signo = static_cast<u32>(sig_arg);
  if (signo == 0 || signo >= sig::NSIG) {
    return -errc::EINVAL;
  }

  // Cannot change SIGKILL or SIGSTOP handlers
  if (signo == sig::SIGKILL || signo == sig::SIGSTOP) {
    return -errc::EINVAL;
  }

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur || !g_process_manager) {
    return -errc::ESRCH;
  }

  Process *proc = g_process_manager->find_process(cur->owner_pid);
  if (!proc) {
    return -errc::ESRCH;
  }

  SignalState *sigstate = get_signal_state(proc);
  if (sigstate == nullptr) {
    // Lazily initialize signal state
    init_signal_state(proc);
    sigstate = get_signal_state(proc);
    if (sigstate == nullptr) {
      return -errc::ENOMEM;
    }
  }

  Sigaction &sa = sigstate->actions[signo];

  // Return old action if requested
  if (oldact_addr != 0) {
    UserSigaction kold;
    kold.handler = sa.handler;
    kold.mask = sa.mask;
    kold.flags = sa.flags;
    if (copy_to_user(static_cast<u64>(oldact_addr), &kold, sizeof(kold)) < 0) {
      return -errc::EFAULT;
    }
  }

  // Set new action if provided
  if (act_addr != 0) {
    UserSigaction kact;
    if (copy_from_user(&kact, static_cast<u64>(act_addr), sizeof(kact)) < 0) {
      return -errc::EFAULT;
    }
    sa.handler = static_cast<VirtAddr>(kact.handler);
    sa.mask = kact.mask;
    sa.flags = static_cast<u32>(kact.flags);
  }

  return 0;
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
    if (new_ss.ss_flags & ss_flags::SS_DISABLE) {
      cur->alt_stack_sp = 0;
      cur->alt_stack_size = 0;
      cur->alt_stack_flags = ss_flags::SS_DISABLE;
    } else {
      if (new_ss.ss_size < 2048) { // MINSIGSTKSZ
        return -errc::ENOMEM;
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
  Process *proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : nullptr;
  return proc ? proc->fd_table() : nullptr;
}

long sys_open(long pathname_addr, long flags, long mode, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  void *fdt = get_current_fd_table();
  if (!fdt) {
    return -errc::EBADF;
  }

  char path_buf[256];
  if (copy_string_from_user(path_buf, static_cast<u64>(pathname_addr), sizeof(path_buf)) < 0) {
    return -errc::EFAULT;
  }

  return moss::kernel::vfs::syscall::do_open(fdt, path_buf, static_cast<u32>(flags), static_cast<u32>(mode));
}

long sys_close(long fd, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  void *fdt = get_current_fd_table();
  if (!fdt) {
    return -errc::EBADF;
  }

  return moss::kernel::vfs::syscall::do_close(fdt, static_cast<int>(fd));
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

  auto *buf = reinterpret_cast<u8 *>(static_cast<usize>(buf_addr));
  return moss::kernel::vfs::syscall::do_read(fdt, static_cast<int>(fd), buf, static_cast<usize>(count));
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

  const auto *buf = reinterpret_cast<const u8 *>(static_cast<usize>(buf_addr));
  return moss::kernel::vfs::syscall::do_write(fdt, static_cast<int>(fd), buf, static_cast<usize>(count));
}

// ── Additional VFS syscalls (dup, dup2, pipe, lseek, fstat) ────

long sys_lseek(long fd, long offset, long whence, long /*unused*/, long /*unused*/, long /*unused*/) noexcept {
  void *fdt = get_current_fd_table();
  if (!fdt) {
    return -errc::EBADF;
  }
  return moss::kernel::vfs::syscall::do_lseek(fdt, fd, static_cast<i64>(offset), static_cast<u32>(whence));
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
  // Use kernel-stack buffer, then copy to validated user address
  long kpipefd[2] = {0, 0};
  long ret = moss::kernel::vfs::syscall::do_pipe(fdt, kpipefd);
  if (ret < 0) {
    return ret;
  }
  if (copy_to_user(static_cast<u64>(pipefd_addr), kpipefd, sizeof(kpipefd)) < 0) {
    return -errc::EFAULT;
  }
  return 0;
}

// 内存管理系统调用

// Anonymous mmap constants (AArch64 Linux ABI values)
constexpr long PROT_READ = 0x1;
constexpr long PROT_WRITE = 0x2;
constexpr long PROT_EXEC = 0x4;
constexpr long MAP_PRIVATE = 0x02;
constexpr long MAP_ANONYMOUS = 0x20;

long sys_mmap(long addr, long length, long prot, long flags, long fd, long /*offset*/) noexcept {
  using namespace moss::kernel::process;

  // Only support MAP_ANONYMOUS | MAP_PRIVATE (no file-backed mmap yet)
  if (length <= 0) {
    return -errc::EINVAL;
  }
  if (!(flags & MAP_ANONYMOUS)) {
    return -errc::ENOSYS;
  }
  if (!(flags & MAP_PRIVATE)) {
    return -errc::EINVAL;
  }
  if (fd != -1) {
    return -errc::EINVAL;
  }

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return -errc::ESRCH;
  }
  Process *proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : nullptr;
  if (!proc) {
    return -errc::ESRCH;
  }
  auto *as = proc->address_space();
  if (!as) {
    return -errc::ENOMEM;
  }

  // Page-align length upward
  auto map_len = static_cast<usize>(length);
  map_len = (map_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  // Choose mapping address
  VirtAddr map_addr = 0;
  if (addr == 0) {
    map_addr = as->mmap_next;
  } else {
    // Use hint address (page-aligned), not MAP_FIXED
    map_addr = static_cast<VirtAddr>(static_cast<usize>(addr)) & ~(static_cast<VirtAddr>(PAGE_SIZE) - 1);
  }

  // Convert prot flags to VMA flags
  u32 vflags = vma_flags::DEMAND_ZERO;
  if (prot & PROT_READ) {
    vflags |= vma_flags::READ;
  }
  if (prot & PROT_WRITE) {
    vflags |= vma_flags::WRITE;
  }
  if (prot & PROT_EXEC) {
    vflags |= vma_flags::EXEC;
  }

  // Add VMA (overlap check built in)
  if (!as->add_vma(map_addr, map_addr + map_len, vflags, VmaType::MMAP)) {
    return -errc::ENOMEM;
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
  map_len = (map_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur) {
    return -errc::ESRCH;
  }
  Process *proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : nullptr;
  if (!proc) {
    return -errc::ESRCH;
  }
  auto *as = proc->address_space();
  if (!as) {
    return -errc::EINVAL;
  }

  // Find VMA containing the unmap address
  const auto *vma = as->find_vma(map_addr);
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
  Process *proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : nullptr;
  if (!proc) {
    return -errc::ESRCH;
  }
  auto *as = proc->address_space();
  if (!as) {
    return -errc::ENOMEM;
  }

  // brk(0): query current program break
  if (addr == 0) {
    return static_cast<long>(as->brk_current);
  }

  auto new_brk = static_cast<VirtAddr>(static_cast<usize>(addr));

  // Reject addresses below heap base (Linux returns current brk on failure)
  if (new_brk < as->brk_base) {
    return static_cast<long>(as->brk_current);
  }

  // Reject addresses beyond maximum heap size (16MB)
  constexpr usize MAX_HEAP = 16ULL * 1024 * 1024;
  if (new_brk > as->brk_base + MAX_HEAP) {
    return static_cast<long>(as->brk_current);
  }

  // Expand or shrink the HEAP VMA to cover the new break (page-aligned)
  VirtAddr aligned_end = (new_brk + PAGE_SIZE - 1) & ~(static_cast<VirtAddr>(PAGE_SIZE) - 1);
  (void)as->vmas.find_if([&](const VmaRegion &vma) {
    if (vma.type == VmaType::HEAP) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      const_cast<VmaRegion &>(vma).end_addr = aligned_end;
      return true;
    }
    return false;
  });

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
  if (!cur) {
    return -errc::ESRCH;
  }

  i32 new_nice = cur->se.nice + static_cast<i32>(increment);

  // Clamp to valid range [-20, 19]
  if (new_nice < priority::MIN_NICE) {
    new_nice = priority::MIN_NICE;
  }
  if (new_nice > priority::MAX_NICE) {
    new_nice = priority::MAX_NICE;
  }

  cur->se.nice = new_nice;
  cur->se.weight = cfs_params::nice_to_weight(new_nice);
  cur->se.load_weight = cur->se.weight;

  log::klog::info("sys_nice: TID={} nice={} weight={}", static_cast<u32>(cur->tid), new_nice, cur->se.weight);
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
  Process *proc = g_process_manager->find_process(static_cast<ProcessId>(who));
  if (!proc) {
    return -errc::ESRCH;
  }

  Thread *main_thread = proc->get_main_thread();
  if (!main_thread) {
    return -errc::ESRCH;
  }

  return 20 - static_cast<long>(main_thread->se.nice);
}

// sched_setscheduler(pid, policy, rt_priority) — set scheduling policy and RT priority
// policy: 0=SCHED_NORMAL, 1=SCHED_FIFO, 2=SCHED_RR, 3=SCHED_BATCH, 5=SCHED_IDLE
// rt_priority: 1-99 for SCHED_FIFO/RR, ignored for other policies
long sys_sched_setscheduler(long pid_arg, long policy_arg, long rt_prio_arg, long /*unused*/, long /*unused*/,
                            long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur || !g_scheduler || !g_process_manager) {
    return -errc::ESRCH;
  }

  // Find target thread
  Thread *target = cur;
  if (pid_arg != 0) {
    Process *proc = g_process_manager->find_process(static_cast<ProcessId>(pid_arg));
    if (!proc) {
      return -errc::ESRCH;
    }
    target = proc->get_main_thread();
    if (!target) {
      return -errc::ESRCH;
    }
  }

  // Validate and map policy
  auto policy = static_cast<SchedPolicy>(static_cast<u8>(policy_arg));
  SchedClass new_class = policy_to_class(policy);

  // Validate RT priority
  u32 rt_prio = static_cast<u32>(rt_prio_arg);
  if (new_class == SchedClass::RealTime) {
    if (rt_prio < priority::MIN_RT_PRIORITY || rt_prio > priority::MAX_RT_PRIORITY) {
      return -errc::EINVAL;
    }
  } else {
    // Non-RT policies: rt_priority must be 0
    rt_prio = 0;
  }

  // Validate policy value itself
  switch (policy) {
  case SchedPolicy::Normal:
  case SchedPolicy::Fifo:
  case SchedPolicy::RR:
  case SchedPolicy::Batch:
  case SchedPolicy::Idle:
    break;
  default:
    return -errc::EINVAL;
  }

  // If class is changing, dequeue from old queue and enqueue to new
  SchedClass old_class = target->sched_class;
  bool class_changed = (old_class != new_class);

  if (class_changed && (target->state == ProcessState::Ready || target->state == ProcessState::Running)) {
    g_scheduler->dequeue_task(target);
  }

  target->sched_class = new_class;
  target->sched_policy = policy;
  target->rt.priority = rt_prio;

  // Reset SCHED_RR time slice
  if (policy == SchedPolicy::RR) {
    target->rt.time_slice_remaining = rt_params::RR_TIMESLICE_NS;
  }

  if (class_changed && (target->state == ProcessState::Ready || target->state == ProcessState::Running)) {
    u32 cpu = target->cpu;
    target->state = ProcessState::Ready;
    g_scheduler->enqueue_task(target, cpu);
  }

  log::klog::info("sched_setscheduler: PID={} policy={} class={} rt_prio={}", static_cast<u32>(target->owner_pid),
                  static_cast<u32>(static_cast<u8>(policy)), static_cast<u32>(static_cast<u8>(new_class)), rt_prio);
  return 0;
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

  Thread *target = cur;
  if (pid_arg != 0) {
    if (!g_process_manager) {
      return -errc::ESRCH;
    }
    Process *proc = g_process_manager->find_process(static_cast<ProcessId>(pid_arg));
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

  u32 cpu = arch::get_current_cpu_id();

  // Penalize vruntime so other tasks get priority
  u64 min_vrt = g_scheduler->get_cpu_min_vruntime(cpu);
  cur->se.vruntime = min_vrt + cfs_params::SCHED_LATENCY_NS;

  // Re-enqueue and trigger reschedule
  g_scheduler->enqueue_task(cur, cpu);

#if defined(MOSS_ARCH_ARM64)
  {
    CpuContext *my_ctx = &cur->context;
    CpuContext *bootstrap = &CfsScheduler::bootstrap_context(cpu);
    CfsScheduler::set_current_task(nullptr);
    arch::disable_interrupts();
    context_switch(my_ctx, bootstrap);
    arch::enable_interrupts();
  }
#endif

  return 0;
}

// sched_getaffinity(pid, cpusetsize, mask_addr) — get CPU affinity mask
// pid: 0 = calling thread
// Returns 0 on success, -errno on failure
long sys_sched_getaffinity(long pid_arg, long /*unused*/, long mask_addr, long /*unused*/, long /*unused*/,
                           long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  Thread *target = nullptr;

  if (pid_arg == 0) {
    target = CfsScheduler::get_current_task();
  } else {
    if (!g_process_manager) {
      return -errc::ESRCH;
    }
    Process *proc = g_process_manager->find_process(static_cast<ProcessId>(pid_arg));
    if (!proc) {
      return -errc::ESRCH;
    }
    target = proc->get_main_thread();
  }

  if (!target) {
    return -errc::ESRCH;
  }

  if (mask_addr != 0) {
    u32 mask_val = target->cpu_affinity_mask.low_word();
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

  // Mask out CPUs beyond available CPU count
  u32 num_cpus = g_num_cpus;
  u32 valid_mask = (num_cpus >= 32) ? 0xFFFFFFFFU : ((1U << num_cpus) - 1);
  new_mask &= valid_mask;
  if (new_mask == 0) {
    return -errc::EINVAL;
  }

  Thread *target = nullptr;

  if (pid_arg == 0) {
    target = CfsScheduler::get_current_task();
  } else {
    if (!g_process_manager) {
      return -errc::ESRCH;
    }
    Process *proc = g_process_manager->find_process(static_cast<ProcessId>(pid_arg));
    if (!proc) {
      return -errc::ESRCH;
    }
    target = proc->get_main_thread();
  }

  if (!target) {
    return -errc::ESRCH;
  }

  target->cpu_affinity_mask.set_from_u32(new_mask);

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

// sys_nanosleep(ns_ptr, remaining_ptr)
// Blocking sleep: arms a one-shot HrTimer, blocks the calling thread,
// and lets the CPU idle (WFI).  The timer ISR wakes the thread.
long sys_nanosleep(long ns_addr, long /* remaining */, long /*unused*/, long /*unused*/, long /*unused*/,
                   long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  if (ns_addr == 0) {
    return -errc::EFAULT;
  }
  u64 duration = 0;
  if (copy_from_user(&duration, static_cast<u64>(ns_addr), sizeof(duration)) < 0) {
    return -errc::EFAULT;
  }
  if (duration == 0) {
    return 0;
  }

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur || !g_scheduler) {
    return -errc::ESRCH;
  }

  // 1. Arm one-shot timer to wake us after `duration` ns.
  //    HrTimer lives on kernel stack — safe because the stack
  //    frame is preserved while the thread is blocked
  //    (context_switch only saves/restores registers, not stack).
  timer::HrTimer sleep_timer;
  sleep_timer.init(timer::TimerMode::OneShot, nanosleep_wake_callback, cur);
  sleep_timer.start_relative(duration);

  // 2. Block: set Sleeping (interruptible), dequeue, context-switch.
  //    Same pattern as sys_wait4.  After context_switch, the CPU
  //    enters idle (WFI) if no other tasks are runnable, causing
  //    idle_time_ns to accumulate correctly.
  cur->state = ProcessState::Sleeping;
  g_scheduler->dequeue_task(cur);

#if defined(MOSS_ARCH_ARM64)
  {
    u32 cpu = arch::get_current_cpu_id();
    CpuContext *my_ctx = &cur->context;
    CpuContext *bootstrap = &CfsScheduler::bootstrap_context(cpu);

    log::klog::debug("nanosleep: TID={} pre-switch pc={:#x} sp={:#x} x30={:#x}", static_cast<u32>(cur->tid), my_ctx->pc,
                     my_ctx->sp, my_ctx->x[30]);

    CfsScheduler::set_current_task(nullptr);
    arch::disable_interrupts();
    context_switch(my_ctx, bootstrap);
    arch::enable_interrupts();

    log::klog::debug("nanosleep: TID={} resumed pc={:#x} sp={:#x} x30={:#x}", static_cast<u32>(cur->tid), my_ctx->pc,
                     my_ctx->sp, my_ctx->x[30]);
  }
#endif

  // 3. Resumed: timer fired, ISR called task_wakeup, scheduler
  //    re-dispatched us.  Cancel defensively (already inactive).
  sleep_timer.cancel();

  return 0;
}

// clock_nanosleep(clockid, flags, ns_addr, remaining)
// clockid: 0 = CLOCK_REALTIME, 1 = CLOCK_MONOTONIC (we treat both the same)
// flags:   0 = relative sleep,  1 = TIMER_ABSTIME (absolute deadline)
// ns_addr: pointer to u64 nanoseconds (relative duration or absolute timestamp)
long sys_clock_nanosleep(long clockid, long flags, long ns_addr, long /*remaining*/, long /*unused*/,
                         long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  // Only support CLOCK_REALTIME (0) and CLOCK_MONOTONIC (1)
  if (clockid < 0 || clockid > 1) {
    return -errc::EINVAL;
  }

  if (ns_addr == 0) {
    return -errc::EFAULT;
  }
  u64 target_ns = 0;
  if (copy_from_user(&target_ns, static_cast<u64>(ns_addr), sizeof(target_ns)) < 0) {
    return -errc::EFAULT;
  }

  Thread *cur = CfsScheduler::get_current_task();
  if (!cur || !g_scheduler) {
    return -errc::ESRCH;
  }

  // Compute relative duration for the timer
  constexpr long TIMER_ABSTIME = 1;
  u64 duration = 0;

  if (flags & TIMER_ABSTIME) {
    // Absolute: sleep until target_ns timestamp
    u64 now = timer::TimerSubsystem::instance().now_ns();
    if (target_ns <= now) {
      return 0; // deadline already passed
    }
    duration = target_ns - now;
  } else {
    // Relative: sleep for target_ns nanoseconds (same as nanosleep)
    duration = target_ns;
    if (duration == 0) {
      return 0;
    }
  }

  // Same timer + block pattern as sys_nanosleep
  timer::HrTimer sleep_timer;
  sleep_timer.init(timer::TimerMode::OneShot, nanosleep_wake_callback, cur);
  sleep_timer.start_relative(duration);

  cur->state = ProcessState::Sleeping;
  g_scheduler->dequeue_task(cur);

#if defined(MOSS_ARCH_ARM64)
  {
    u32 cpu = arch::get_current_cpu_id();
    CpuContext *my_ctx = &cur->context;
    CpuContext *bootstrap = &CfsScheduler::bootstrap_context(cpu);

    CfsScheduler::set_current_task(nullptr);
    arch::disable_interrupts();
    context_switch(my_ctx, bootstrap);
    arch::enable_interrupts();
  }
#endif

  sleep_timer.cancel();
  return 0;
}

// ── System monitoring: topinfo ──────────────────────────────

// Kernel-side mirror of userspace TopProcessInfo / TopInfo structs.
// Layout must match exactly (all fields are u64/long on 64-bit).
namespace topinfo_layout {
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
// Strategy: collect ALL data into a kernel-stack local struct first,
// then copy to user space in one shot.  This avoids data loss caused
// by ARM64 demand-paging: when we write directly to user addresses,
// page faults can invalidate TLB entries for previously-written pages,
// causing those stores to be lost.  By buffering on the kernel stack
// (which is always resident), we guarantee no data loss.
long sys_topinfo(long info_addr, long /*unused*/, long /*unused*/, long /*unused*/, long /*unused*/,
                 long /*unused*/) noexcept {
  using namespace moss::kernel::process;

  if (info_addr == 0) {
    return -errc::EFAULT;
  }

  // Kernel-stack buffer (~5920 bytes, kernel stack is 16KB)
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

      // Copy process name
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

namespace console_rx {

// Lock-free SPSC ring buffer (single producer = IRQ, single consumer = reader).
// Power-of-2 size for mask-based wrap-around.
constexpr usize RX_BUF_SIZE = 256;
constexpr usize RX_BUF_MASK = RX_BUF_SIZE - 1;

static u8 rx_buf_[RX_BUF_SIZE];
static volatile usize rx_head_ = 0; // Written by IRQ (producer)
static volatile usize rx_tail_ = 0; // Written by consumer

static bool initialized_ = false;

#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X86_64)
// The thread currently blocked waiting for input (at most one reader).
// Used by uart_rx_irq_handler and console_getc_blocking.
static process::Thread *blocked_reader_ = nullptr;
#endif

static bool buf_empty() noexcept { return rx_head_ == rx_tail_; }

static int buf_get() noexcept {
  if (buf_empty()) {
    return -1;
  }
  u8 ch = rx_buf_[rx_tail_];
  rx_tail_ = (rx_tail_ + 1) & RX_BUF_MASK;
  return ch;
}

#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X86_64)

static bool buf_put(u8 ch) noexcept {
  usize next_head = (rx_head_ + 1) & RX_BUF_MASK;
  if (next_head == rx_tail_) {
    return false; // full — drop char
  }
  rx_buf_[rx_head_] = ch;
  rx_head_ = next_head;
  return true;
}

// Wake the blocked reader (shared by ARM64 and x86_64 IRQ handlers)
static void wake_blocked_reader() noexcept {
  if (blocked_reader_ != nullptr && process::is_blocked_state(blocked_reader_->state)) {
    auto *thr = blocked_reader_;
    blocked_reader_ = nullptr;
    if (process::g_scheduler) {
      process::g_scheduler->task_wakeup(thr, thr->cpu);
    }
  }
}

#if defined(MOSS_ARCH_ARM64)
// UART RX IRQ handler — called from GIC interrupt context (IRQ 33).
// Drains PL011 RX FIFO into ring buffer, then wakes the blocked reader.
static void uart_rx_irq_handler(u32 /*irq*/, void * /*context*/) noexcept {
  for (int ch = hal::uart::getc(); ch >= 0; ch = hal::uart::getc()) {
    buf_put(static_cast<u8>(ch));
  }
  hal::uart::ack_rx_interrupt();

  wake_blocked_reader();
}
#endif // MOSS_ARCH_ARM64

#if defined(MOSS_ARCH_X86_64)
// x86_64 COM1 UART RX handler — called via g_x86_64_uart_rx_handler callback.
// Reads COM1 RBR while Data Ready (LSR bit 0) is set.
static void x86_64_uart_rx_dispatch() noexcept {
  for (int ch = hal::uart::getc(); ch >= 0; ch = hal::uart::getc()) {
    buf_put(static_cast<u8>(ch));
  }

  wake_blocked_reader();
}
#endif // MOSS_ARCH_X86_64

#endif // MOSS_ARCH_ARM64 || MOSS_ARCH_X86_64

} // namespace console_rx

// extern "C" bridge: initialize console RX interrupt subsystem.
// Called once from console_read() on first invocation.
extern "C" void console_rx_init() noexcept {
  using namespace console_rx;
  if (initialized_) {
    return;
  }

#if defined(MOSS_ARCH_ARM64)
  // 1. Enable PL011 RXE bit
  hal::uart::enable_rx();

  hal::uart::enable_rx_interrupt();

  // 3. Register IRQ handler with GIC and enable UART IRQ (SPI 33)
  if (interrupts::g_gic) {
    u32 uart_irq = platform::hardware.uart.irq;
    auto reg = interrupts::g_gic->register_interrupt(uart_irq, uart_rx_irq_handler, nullptr, "uart_rx");
    if (reg) {
      (void)interrupts::g_gic->enable_interrupt(uart_irq);
    }
  }
#elif defined(MOSS_ARCH_X86_64)
  hal::uart::enable_rx_interrupt();

  // 3. Register UART RX callback for interrupt dispatch
  g_x86_64_uart_rx_handler = +[]() noexcept { x86_64_uart_rx_dispatch(); };

  // 4. Unmask COM1 IRQ4 in I/O APIC
  if (interrupts::g_gic) {
    (void)interrupts::g_gic->enable_interrupt(platform::hardware.uart.irq);
  }
#endif

  initialized_ = true;
}

// extern "C" bridge: blocking getc — blocks the calling thread until a
// character is available in the ring buffer.  Returns 0-255.
extern "C" int console_getc_blocking() noexcept {
  using namespace console_rx;
  using namespace process;

  // Fast path: char already in buffer
  int ch = buf_get();
  if (ch >= 0) {
    return ch;
  }

#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X86_64)
  // Slow path: block until UART IRQ delivers a character
  Thread *cur = CfsScheduler::get_current_task();
  if (!cur || !g_scheduler) {
    // Fallback: WFI/HLT polling if scheduler not available yet
    while (buf_empty()) {
#if defined(MOSS_ARCH_ARM64)
      asm volatile("wfi" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
      asm volatile("hlt" ::: "memory");
#endif
    }
    return buf_get();
  }

  while (buf_empty()) {
    // 1. Set Sleeping (interruptible) + dequeue first
    cur->state = ProcessState::Sleeping;
    g_scheduler->dequeue_task(cur);

    // 2. Record as blocked reader — if UART IRQ fires between here
    //    and context_switch, handler calls task_wakeup (safe: thread
    //    is already Blocked, wakeup re-enqueues it, and bootstrap
    //    will pick it back up immediately).
    blocked_reader_ = cur;

    // 3. Context-switch to bootstrap (CPU enters idle → WFI/HLT)
    {
      u32 cpu = arch::get_current_cpu_id();
      CpuContext *my_ctx = &cur->context;
      CpuContext *bootstrap = &CfsScheduler::bootstrap_context(cpu);
      CfsScheduler::set_current_task(nullptr);
      arch::disable_interrupts();
      context_switch(my_ctx, bootstrap);
      arch::enable_interrupts();
    }

    // 4. Resumed after task_wakeup — loop re-checks buf_empty()
  }

  return buf_get();
#else
  // RISC-V: WFI polling with direct UART read (no IRQ handler yet).
  for (;;) {
    int c = hal::uart::getc();
    if (c >= 0) {
      return c;
    }
    asm volatile("wfi" ::: "memory");
  }
#endif
}

// 全局系统调用表定义
const SyscallDescriptor SYSCALL_TABLE[static_cast<int>(SyscallNumber::MAX_SYSCALL)] = {
    // === 基础系统调用 (0-9) ===
    {"debug_print", handlers::sys_debug_print, 1, true, "调试输出"},
    {"exit", handlers::sys_exit, 1, true, "进程退出"},
    {"getpid", handlers::sys_getpid, 0, true, "获取进程ID"},
    {"getppid", handlers::sys_getppid, 0, true, "获取父进程ID"},
    {"getuid", handlers::sys_getuid, 0, true, "获取用户ID"},
    {"getgid", handlers::sys_getgid, 0, true, "获取组ID"},
    {"geteuid", handlers::sys_not_implemented, 0, false, "获取有效用户ID"},
    {"getegid", handlers::sys_not_implemented, 0, false, "获取有效组ID"},
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
    {"stat", handlers::sys_not_implemented, 2, false, "获取文件状态"},
    {"fstat", handlers::sys_fstat, 2, true, "获取文件描述符状态"},
    {"lstat", handlers::sys_not_implemented, 2, false, "获取链接文件状态"},
    {"access", handlers::sys_not_implemented, 2, false, "检查文件权限"},
    {"chmod", handlers::sys_not_implemented, 2, false, "修改文件权限"},
    {"chown", handlers::sys_not_implemented, 3, false, "修改文件所有者"},
    {"umask", handlers::sys_not_implemented, 1, false, "设置文件创建掩码"},
    {"dup", handlers::sys_dup, 1, true, "复制文件描述符"},
    {"dup2", handlers::sys_dup2, 2, true, "复制文件描述符到指定位置"},
    {"pipe", handlers::sys_pipe, 1, true, "创建管道"},
    {"mkdir", handlers::sys_not_implemented, 2, false, "创建目录"},
    {"rmdir", handlers::sys_not_implemented, 1, false, "删除目录"},
    {"link", handlers::sys_not_implemented, 2, false, "创建硬链接"},
    {"unlink", handlers::sys_not_implemented, 1, false, "删除文件"},
    {"symlink", handlers::sys_not_implemented, 2, false, "创建符号链接"},
    {"readlink", handlers::sys_not_implemented, 3, false, "读取符号链接"},
    {"chdir", handlers::sys_not_implemented, 1, false, "改变工作目录"},
    {"getcwd", handlers::sys_not_implemented, 2, false, "获取当前目录"},
    {"rename", handlers::sys_not_implemented, 2, false, "重命名文件"},
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
    {"clock_getres", handlers::sys_not_implemented, 2, false, "获取时钟分辨率"},
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
    {"uname", handlers::sys_not_implemented, 1, false, "获取系统信息"},
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
    {"arch_prctl", handlers::sys_not_implemented, 2, false, "架构特定控制"},
    {"prctl", handlers::sys_not_implemented, 5, false, "进程控制"},
    {"capget", handlers::sys_not_implemented, 2, false, "获取能力"},
    {"capset", handlers::sys_not_implemented, 2, false, "设置能力"}};

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

  // Signal checkpoint: before returning to user-space, check for
  // pending signals and process them.  If a signal's default action
  // is Terminate, do_exit() is called (which does not return).
  {
    using namespace moss::kernel::process;
    Thread *cur = CfsScheduler::get_current_task();
    if (cur != nullptr && signal_pending(cur)) {
      if (do_signal_checkpoint(cur)) {
        // Signal caused termination — call do_exit (noreturn)
        Process *proc = g_process_manager ? g_process_manager->find_process(cur->owner_pid) : nullptr;
        if (proc) {
          do_exit(cur, proc, 128 + static_cast<i32>(cur->pending_signals & 0xFF));
        }
      }
      // If a signal interrupted a sleeping syscall, return -EINTR
      if (result == 0 && is_blocked_state(cur->state)) {
        result = -errc::EINTR;
      }
    }
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
