import moss.std;
import moss.types;
import moss.kernel;
import moss.process;
import moss.mm;
import moss.abi;
import moss.arch;
import moss.vfs;
import moss.smart_ptr;
import moss.capability;
import moss.timer;
import moss.boot;
import moss.hal.mmu;
import moss.initramfs;

#include "framework/ut_kernel.hpp"
#include "validation/core_cases.hpp"
#include "moss/domain_spawn.h"
#include "validation/memory_internal.hpp"
#include "validation_internal.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;

namespace moss::test::validation {

namespace {

struct SyscallFixture {
  process::ProcessManager manager;
  shared_ptr<process::Process> proc;
  shared_ptr<process::AddressSpace> address_space;
  process::ProcessManager *saved_manager = nullptr;
  process::Thread *saved_current = nullptr;
  bool restore_irqs = false;
  bool ready = false;

  SyscallFixture() noexcept {
    saved_manager = process::g_process_manager;
    saved_current = process::CfsScheduler::get_current_task();
    restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();
    auto created = manager.create_process();
    auto space = process::user_space::create_user_address_space();
    if (!created || !space) {
      return;
    }
    proc = *created;
    address_space = *space;
    (void)proc->set_address_space(address_space);
    address_space->brk_base = process::user_layout::HEAP_START;
    address_space->brk_current = process::user_layout::HEAP_START;
    address_space->mmap_next = process::user_layout::MMAP_BASE;
    // The Process owns and deletes its fd table; the fixture only publishes it
    // so syscall handlers observe the same VFS state as a real process.
    auto *table = new vfs::FdTable();
    if (!table) {
      return;
    }
    proc->set_fd_table(table);
    address_space->add_vma(process::user_layout::CODE_BASE,
                           process::user_layout::CODE_BASE + 4 * moss::kernel::PAGE_SIZE,
                           process::vma_flags::READ | process::vma_flags::WRITE | process::vma_flags::DEMAND_ZERO);
    address_space->add_vma(process::user_layout::HEAP_START, process::user_layout::HEAP_START,
                           process::vma_flags::READ | process::vma_flags::WRITE | process::vma_flags::DEMAND_ZERO,
                           process::VmaType::HEAP);
    auto *thread = new process::Thread(100, proc->pid());
    if (!proc->register_thread(thread)) {
      delete thread;
      return;
    }
    thread->state = process::ProcessState::Running;
    thread->is_user_task = true;
    process::CfsScheduler::set_current_task(thread);
    process::g_process_manager = &manager;
    ready = true;
  }

  ~SyscallFixture() noexcept {
    process::CfsScheduler::set_current_task(saved_current);
    process::g_process_manager = saved_manager;
    if (restore_irqs) {
      arch::enable_interrupts();
    }
  }

  SyscallFixture(const SyscallFixture &) = delete;
  SyscallFixture &operator=(const SyscallFixture &) = delete;
};

using namespace syscall::handlers;

constexpr long invalid = -1;

void set_syscall_number(moss::abi::TrapFrame &frame, long number) noexcept {
#if defined(MOSS_ARCH_ARM64)
  frame.x8 = static_cast<u64>(number);
#elif defined(MOSS_ARCH_X64)
  frame.rax = static_cast<u64>(number);
#elif defined(MOSS_ARCH_RISCV64)
  frame.a7 = static_cast<u64>(number);
#endif
}

constexpr u64 kPrivilegedTrapStatus =
#if defined(MOSS_ARCH_ARM64)
    1ULL;
#else
    1ULL << 8;
#endif

// x64 reports the privilege level through CS; ARM64/RISC-V use status bits.
void mark_user_frame(moss::abi::TrapFrame &frame) noexcept {
#if defined(MOSS_ARCH_X64)
  frame.cs = 3;
#else
  frame.status = 0;
#endif
}

void basic_contracts() {
  ut::expect(sys_getpid(0, 0, 0, 0, 0, 0) > 0);
  (void)sys_getppid(0, 0, 0, 0, 0, 0);
  (void)sys_getuid(0, 0, 0, 0, 0, 0);
  (void)sys_getgid(0, 0, 0, 0, 0, 0);
  (void)sys_geteuid(0, 0, 0, 0, 0, 0);
  (void)sys_getegid(0, 0, 0, 0, 0, 0);
  ut::expect(sys_debug_print(0, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_debug_print(0x1000, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_arch_prctl(invalid, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_not_implemented(0, 0, 0, 0, 0, 0) < 0);
}

void handle_contracts() {
  constexpr long handles[] = {invalid, 0L, 1024L};
  for (const long handle : handles) {
    (void)sys_domain_id(handle, 0, 0, 0, 0, 0);
    (void)sys_domain_same(handle, handle, 0, 0, 0, 0);
    (void)sys_domain_terminate(handle, 0, 0, 0, 0, 0);
    (void)sys_domain_signal(handle, invalid, 0, 0, 0, 0);
    (void)sys_domain_wait(handle, 0, 0, 0, 0, 0);
    (void)sys_domain_status(handle, 0, 0, 0, 0, 0);
    (void)sys_domain_scope_terminate(handle, 0, 0, 0, 0, 0);
    (void)sys_domain_scope_status(handle, 0, 0, 0, 0, 0);
    (void)sys_domain_scope_contains(handle, handle, 0, 0, 0, 0);
    (void)sys_profile_create(handle, 0, 0, 0, 0, 0);
    (void)sys_profile_apply(handle, 0, 0, 0, 0, 0);
    (void)sys_profile_status(handle, 0, 0, 0, 0, 0);
    (void)sys_code_page_count(handle, 0, 0, 0, 0, 0);
    (void)sys_code_approve(handle, handle, 0, 0, 0, 0);
    (void)sys_code_revoke(handle, handle, 0, 0, 0, 0);
    (void)sys_cap_close(handle, 0, 0, 0, 0, 0);
    (void)sys_cap_duplicate(handle, 0, 0, 0, 0, 0);
    (void)sys_cap_set_inherit(handle, 0, 0, 0, 0, 0);
    (void)sys_cap_set_exec(handle, 0, 0, 0, 0, 0);
  }
  (void)sys_domain_self(0, 0, 0, 0, 0, 0);
  (void)sys_domain_scope_create(0, 0, 0, 0, 0, 0);
  (void)sys_domain_factory(0, 0, 0, 0, 0, 0);
  (void)sys_domain_spawn(invalid, 0, 0, 0, 0, 0);
  (void)sys_domain_layout(0, 0, 0, 0, 0, 0);
  (void)sys_domain_wait_any(0, 0, 0, 0, 0, 0);
  (void)sys_object_wait(0, 0, 0, 0, 0, 0);
  (void)sys_code_snapshot(0, 0, 0, 0, 0, 0);
  (void)sys_code_snapshot_range(0, 0, 0, 0, 0, 0);
  (void)sys_code_read(invalid, 0, 0, 0, 0, 0);
  (void)sys_code_read_range(invalid, 0, 0, 0, 0, 0);
  (void)sys_code_authority(0, 0, 0, 0, 0, 0);
  (void)sys_domain_info(invalid, 0, 0, 0, 0, 0);
  (void)sys_system_info(0, 0, 0, 0, 0, 0);
  (void)sys_topinfo(0, 0, 0, 0, 0, 0);
  ut::expect(sys_domain_id(invalid, 0, 0, 0, 0, 0) < 0);
}

void filesystem_contracts() {
  (void)sys_open(0, 0, 0, 0, 0, 0);
  (void)sys_open(0x1000, 0, 0, 0, 0, 0);
  (void)sys_close(invalid, 0, 0, 0, 0, 0);
  (void)sys_read(invalid, 0, 1, 0, 0, 0);
  (void)sys_write(invalid, 0, 1, 0, 0, 0);
  (void)sys_lseek(invalid, 0, 0, 0, 0, 0);
  (void)sys_fstat(invalid, 0, 0, 0, 0, 0);
  (void)sys_stat(0, 0, 0, 0, 0, 0);
  (void)sys_mkdir(0, 0, 0, 0, 0, 0);
  (void)sys_rmdir(0, 0, 0, 0, 0, 0);
  (void)sys_unlink(0, 0, 0, 0, 0, 0);
  (void)sys_rename(0, 0, 0, 0, 0, 0);
  (void)sys_getdents(invalid, 0, 0, 0, 0, 0);
  (void)sys_dup(invalid, 0, 0, 0, 0, 0);
  (void)sys_dup2(invalid, invalid, 0, 0, 0, 0);
  (void)sys_pipe(0, 0, 0, 0, 0, 0);
  (void)sys_fcntl(invalid, 0, 0, 0, 0, 0);
  ut::expect(sys_close(invalid, 0, 0, 0, 0, 0) < 0);
}

void memory_contracts() {
  (void)sys_mmap(0, 0, 0, 0, invalid, 0);
  (void)sys_mmap(0, 0x1000, 0, 0, invalid, 1);
  (void)sys_munmap(0, 0, 0, 0, 0, 0);
  (void)sys_mprotect(0, 0, 0, 0, 0, 0);
  (void)sys_brk(0, 0, 0, 0, 0, 0);
  (void)sys_brk(invalid, 0, 0, 0, 0, 0);
  ut::expect(sys_mmap(0, 0x1000, 0, 0, invalid, 1) < 0);
}

void scheduling_contracts() {
  (void)sys_nice(100, 0, 0, 0, 0, 0);
  (void)sys_nice(-100, 0, 0, 0, 0, 0);
  (void)sys_getpriority(invalid, invalid, 0, 0, 0, 0);
  (void)sys_getpriority(0, invalid, 0, 0, 0, 0);
  (void)sys_sched_yield(0, 0, 0, 0, 0, 0);
  (void)sys_sched_getaffinity(invalid, 0, 0, 0, 0, 0);
  (void)sys_sched_getaffinity(0, 0, 0, 0, 0, 0);
  (void)sys_sched_setaffinity(invalid, 0, 0, 0, 0, 0);
  (void)sys_sched_setaffinity(0, 0, 0, 0, 0, 0);
  ut::expect(sys_getpriority(invalid, invalid, 0, 0, 0, 0) < 0);
}

void time_and_signal_contracts() {
  (void)sys_clock_gettime(invalid, 0, 0, 0, 0, 0);
  (void)sys_clock_gettime(0, 0, 0, 0, 0, 0);
  (void)sys_clock_getres(invalid, 0, 0, 0, 0, 0);
  (void)sys_clock_getres(0, 0, 0, 0, 0, 0);
  (void)sys_nanosleep(0, 0, 0, 0, 0, 0);
  (void)sys_clock_nanosleep(invalid, 0, 0, 0, 0, 0);
  (void)sys_socket(0, 0, 0, 0, 0, 0);
  (void)sys_bind(0, 0, 0, 0, 0, 0);
  (void)sys_listen(0, 0, 0, 0, 0, 0);
  (void)sys_accept(0, 0, 0, 0, 0, 0);
  ut::expect(sys_clock_gettime(invalid, 0, 0, 0, 0, 0) < 0);
}

void ipc_contracts() {
  (void)sys_ipc_create(0, 0, 0, 0, 0, 0);
  (void)sys_ipc_mint_badge(invalid, 0, 0, 0, 0, 0);
  (void)sys_ipc_call(invalid, 0, 0, 0, 0, 0);
  (void)sys_ipc_receive(invalid, 0, 0, 0, 0, 0);
  (void)sys_ipc_try_receive(invalid, 0, 0, 0, 0, 0);
  (void)sys_ipc_reply(invalid, 0, 0, 0, 0, 0);
  (void)sys_mem_create(0, 0, 0, 0, 0, 0);
  (void)sys_mem_map(invalid, 0, 0, 0, 0, 0);
  (void)sys_boot_archive(0, 0, 0, 0, 0, 0);
  ut::expect(sys_ipc_call(invalid, 0, 0, 0, 0, 0) < 0);
}

void dispatcher_contracts() {
  ut::expect(syscall::SyscallDispatcher::dispatch(invalid, 0, 0, 0, 0, 0, 0) < 0);
  ut::expect(syscall::SyscallDispatcher::dispatch(100000, 0, 0, 0, 0, 0, 0) < 0);
  ut::expect(syscall::SyscallDispatcher::get_syscall_info(invalid) == nullptr);
  ut::expect(!syscall::SyscallDispatcher::is_implemented(invalid));
  u64 total = 0, implemented = 0;
  syscall::SyscallDispatcher::get_syscall_stats(&total, &implemented);
  ut::expect(total > 0 && implemented > 0);
  syscall::SyscallDispatcher::get_syscall_stats(nullptr, nullptr);
}

void null_context_contracts() {
  auto *saved_current = process::CfsScheduler::get_current_task();
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();
  auto invoke = [](const char *name, long a0, long a1, long a2, long a3, long a4, long a5) {
    for (long number = 0; number < static_cast<long>(syscall::SyscallNumber::MAX_SYSCALL); ++number) {
      const auto *descriptor = syscall::SyscallDispatcher::get_syscall_info(number);
      if (!descriptor || !descriptor->name) {
        continue;
      }
      const char *left = descriptor->name;
      const char *right = name;
      while (*left && *right && *left == *right) {
        ++left;
        ++right;
      }
      if (*left == '\0' && *right == '\0') {
        return descriptor->handler(a0, a1, a2, a3, a4, a5);
      }
    }
    return -1L;
  };
  auto sweep = [&] {
    (void)sys_debug_print(0, 0, 0, 0, 0, 0);
    (void)sys_getpid(0, 0, 0, 0, 0, 0);
    (void)sys_getppid(0, 0, 0, 0, 0, 0);
    (void)sys_getuid(0, 0, 0, 0, 0, 0);
    (void)sys_getgid(0, 0, 0, 0, 0, 0);
    (void)sys_geteuid(0, 0, 0, 0, 0, 0);
    (void)sys_getegid(0, 0, 0, 0, 0, 0);
    (void)sys_arch_prctl(0, 0, 0, 0, 0, 0);
    (void)sys_fork(0, 0, 0, 0, 0, 0);
    (void)sys_fork_domain(0, 0, 0, 0, 0, 0);
    (void)sys_fork_domain_inherit(0, 0, 0, 0, 0, 0);
    (void)sys_fork_domain_select(0, 0, 0, 0, 0, 0);
    (void)sys_fork_domain_scoped(0, 0, 0, 0, 0, 0);
    (void)sys_domain_id(0, 0, 0, 0, 0, 0);
    (void)sys_domain_same(0, 0, 0, 0, 0, 0);
    (void)sys_domain_self(0, 0, 0, 0, 0, 0);
    (void)sys_domain_scope_create(0, 0, 0, 0, 0, 0);
    (void)sys_domain_scope_terminate(0, 0, 0, 0, 0, 0);
    (void)sys_domain_scope_status(0, 0, 0, 0, 0, 0);
    (void)sys_domain_scope_contains(0, 0, 0, 0, 0, 0);
    (void)sys_domain_factory(0, 0, 0, 0, 0, 0);
    (void)sys_profile_create(0, 0, 0, 0, 0, 0);
    (void)sys_profile_apply(0, 0, 0, 0, 0, 0);
    (void)sys_profile_status(0, 0, 0, 0, 0, 0);
    (void)sys_code_snapshot(0, 0, 0, 0, 0, 0);
    (void)sys_code_snapshot_range(0, 0, 0, 0, 0, 0);
    (void)sys_code_read(0, 0, 0, 0, 0, 0);
    (void)sys_code_read_range(0, 0, 0, 0, 0, 0);
    (void)sys_code_page_count(0, 0, 0, 0, 0, 0);
    (void)sys_code_authority(0, 0, 0, 0, 0, 0);
    (void)sys_code_approve(0, 0, 0, 0, 0, 0);
    (void)sys_code_revoke(0, 0, 0, 0, 0, 0);
    (void)sys_domain_spawn(0, 0, 0, 0, 0, 0);
    (void)sys_domain_layout(0, 0, 0, 0, 0, 0);
    (void)sys_domain_terminate(0, 0, 0, 0, 0, 0);
    (void)sys_domain_signal(0, 0, 0, 0, 0, 0);
    (void)sys_domain_wait(0, 0, 0, 0, 0, 0);
    (void)sys_domain_wait_any(0, 0, 0, 0, 0, 0);
    (void)sys_object_wait(0, 0, 0, 0, 0, 0);
    (void)sys_domain_status(0, 0, 0, 0, 0, 0);
    (void)sys_execve(0, 0, 0, 0, 0, 0);
    (void)sys_execve_cap(0, 0, 0, 0, 0, 0);
    (void)sys_wait4(-1, 0, 0, 0, 0, 0);
    (void)sys_waitpid(-1, 0, 0, 0, 0, 0);
    (void)sys_kill(0, 0, 0, 0, 0, 0);
    (void)invoke("getpgid", 0, 0, 0, 0, 0, 0);
    (void)invoke("getpgrp", 0, 0, 0, 0, 0, 0);
    (void)invoke("getsid", 0, 0, 0, 0, 0, 0);
    (void)invoke("setpgid", 0, 0, 0, 0, 0, 0);
    (void)invoke("setpgrp", 0, 0, 0, 0, 0, 0);
    (void)invoke("setsid", 0, 0, 0, 0, 0, 0);
    (void)invoke("sigaction", 0, 0, 0, 0, 0, 0);
    (void)invoke("sigprocmask", 0, 0, 0, 0, 0, 0);
    (void)invoke("sigaltstack", 0, 0, 0, 0, 0, 0);
    (void)sys_open(0, 0, 0, 0, 0, 0);
    (void)sys_close(0, 0, 0, 0, 0, 0);
    (void)sys_read(0, 0, 0, 0, 0, 0);
    (void)sys_write(0, 0, 0, 0, 0, 0);
    (void)sys_lseek(0, 0, 0, 0, 0, 0);
    (void)sys_getdents(0, 0, 0, 0, 0, 0);
    (void)sys_stat(0, 0, 0, 0, 0, 0);
    (void)invoke("getcwd", 0, 0, 0, 0, 0, 0);
    (void)invoke("access", 0, 0, 0, 0, 0, 0);
    (void)invoke("chdir", 0, 0, 0, 0, 0, 0);
    (void)sys_mkdir(0, 0, 0, 0, 0, 0);
    (void)sys_rmdir(0, 0, 0, 0, 0, 0);
    (void)sys_unlink(0, 0, 0, 0, 0, 0);
    (void)sys_rename(0, 0, 0, 0, 0, 0);
    (void)sys_fstat(0, 0, 0, 0, 0, 0);
    (void)invoke("ioctl", 0, 0, 0, 0, 0, 0);
    (void)invoke("uname", 0, 0, 0, 0, 0, 0);
    (void)sys_fcntl(0, 0, 0, 0, 0, 0);
    (void)sys_dup(0, 0, 0, 0, 0, 0);
    (void)sys_dup2(0, 0, 0, 0, 0, 0);
    (void)sys_pipe(0, 0, 0, 0, 0, 0);
    (void)sys_mmap(0, 0, 0, 0, 0, 0);
    (void)sys_munmap(0, 0, 0, 0, 0, 0);
    (void)sys_mprotect(0, 0, 0, 0, 0, 0);
    (void)sys_brk(0, 0, 0, 0, 0, 0);
    (void)sys_nice(0, 0, 0, 0, 0, 0);
    (void)sys_getpriority(0, 0, 0, 0, 0, 0);
    (void)invoke("sched_getscheduler", 0, 0, 0, 0, 0, 0);
    (void)invoke("sched_get_priority_max", 0, 0, 0, 0, 0, 0);
    (void)invoke("sched_get_priority_min", 0, 0, 0, 0, 0, 0);
    (void)sys_sched_getaffinity(0, 0, 0, 0, 0, 0);
    (void)sys_sched_setaffinity(0, 0, 0, 0, 0, 0);
    (void)sys_clock_gettime(0, 0, 0, 0, 0, 0);
    (void)sys_clock_getres(0, 0, 0, 0, 0, 0);
    (void)sys_domain_info(0, 0, 0, 0, 0, 0);
    (void)sys_system_info(0, 0, 0, 0, 0, 0);
    (void)sys_topinfo(0, 0, 0, 0, 0, 0);
    (void)sys_not_implemented(0, 0, 0, 0, 0, 0);
  };
  process::CfsScheduler::set_current_task(nullptr);
  sweep();
  process::Thread unknown(9999, 9999);
  unknown.state = process::ProcessState::Running;
  process::CfsScheduler::set_current_task(&unknown);
  sweep();
  process::CfsScheduler::set_current_task(saved_current);
  if (restore_irqs) {
    arch::enable_interrupts();
  }
  ut::expect(true);
}

void missing_managers() {
  using namespace syscall::handlers;
  auto *saved_current = process::CfsScheduler::get_current_task();
  auto *saved_manager = process::g_process_manager;
  auto *saved_scheduler = process::g_scheduler;
  const bool restore_irqs = arch::interrupts_enabled();
  arch::disable_interrupts();

  process::Thread thread(9001, 9001);
  thread.state = process::ProcessState::Running;
  process::CfsScheduler::set_current_task(&thread);
  process::g_process_manager = nullptr;
  process::g_scheduler = nullptr;

  ut::expect(sys_getppid(0, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_getuid(0, 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_getgid(0, 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_geteuid(0, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_getegid(0, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_nice(0, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_wait4(-1, 0, 1, 0, 0, 0) == -syscall::errc::EINVAL);

  (void)sys_domain_id(0, 0, 0, 0, 0, 0);
  (void)sys_domain_self(0, 0, 0, 0, 0, 0);
  (void)sys_domain_same(0, 0, 0, 0, 0, 0);
  (void)sys_domain_factory(0, 0, 0, 0, 0, 0);
  (void)sys_domain_scope_create(0, 0, 0, 0, 0, 0);
  (void)sys_profile_create(0, 0, 0, 0, 0, 0);
  (void)sys_profile_apply(0, 0, 0, 0, 0, 0);
  (void)sys_profile_status(0, 0, 0, 0, 0, 0);
  (void)sys_code_snapshot(0, 0, 0, 0, 0, 0);
  (void)sys_code_snapshot_range(0, 0, 0, 0, 0, 0);
  (void)sys_code_read(0, 0, 0, 0, 0, 0);
  (void)sys_code_read_range(0, 0, 0, 0, 0, 0);
  (void)sys_code_page_count(0, 0, 0, 0, 0, 0);
  (void)sys_code_authority(0, 0, 0, 0, 0, 0);
  (void)sys_code_approve(0, 0, 0, 0, 0, 0);
  (void)sys_code_revoke(0, 0, 0, 0, 0, 0);
  (void)sys_fork(0, 0, 0, 0, 0, 0);
  (void)sys_fork_domain(0, 0, 0, 0, 0, 0);
  (void)sys_domain_spawn(0, 0, 0, 0, 0, 0);
  (void)sys_domain_layout(0, 0, 0, 0, 0, 0);
  (void)sys_domain_terminate(0, 0, 0, 0, 0, 0);
  (void)sys_domain_signal(0, 0, 0, 0, 0, 0);
  (void)sys_domain_wait(0, 0, 0, 0, 0, 0);
  (void)sys_domain_wait_any(0, 0, 0, 0, 0, 0);
  (void)sys_object_wait(0, 0, 0, 0, 0, 0);
  (void)sys_domain_status(0, 0, 0, 0, 0, 0);
  (void)sys_execve(0, 0, 0, 0, 0, 0);
  (void)sys_execve_cap(0, 0, 0, 0, 0, 0);
  (void)sys_kill(0, 0, 0, 0, 0, 0);
  (void)sys_mmap(0, 0, 0, 0, 0, 0);
  (void)sys_munmap(0, 0, 0, 0, 0, 0);
  (void)sys_mprotect(0, 0, 0, 0, 0, 0);
  (void)sys_brk(0, 0, 0, 0, 0, 0);
  (void)sys_getpriority(0, 0, 0, 0, 0, 0);
  (void)sys_sched_yield(0, 0, 0, 0, 0, 0);
  (void)sys_sched_getaffinity(0, 0, 0, 0, 0, 0);
  (void)sys_sched_setaffinity(0, 0, 0, 0, 0, 0);
  (void)sys_clock_gettime(0, 0, 0, 0, 0, 0);
  (void)sys_clock_getres(0, 0, 0, 0, 0, 0);
  (void)sys_domain_info(0, 0, 0, 0, 0, 0);
  (void)sys_system_info(0, 0, 0, 0, 0, 0);
  (void)sys_topinfo(0, 0, 0, 0, 0, 0);

  process::g_process_manager = saved_manager;
  process::g_scheduler = saved_scheduler;
  process::CfsScheduler::set_current_task(saved_current);
  if (restore_irqs) {
    arch::enable_interrupts();
  }
}

void partial_context_matrix() {
  using namespace syscall::handlers;
  auto *saved_current = process::CfsScheduler::get_current_task();
  auto *saved_manager = process::g_process_manager;
  auto *saved_scheduler = process::g_scheduler;

  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  auto *fixture_thread = process::CfsScheduler::get_current_task();
  auto *fixture_manager = process::g_process_manager;
  constexpr long unmapped = 0xdead0000L;
  auto named = [](const char *name, long a0, long a1, long a2, long a3, long a4, long a5) {
    for (long number = 0; number < static_cast<long>(syscall::SyscallNumber::MAX_SYSCALL); ++number) {
      const auto *descriptor = syscall::SyscallDispatcher::get_syscall_info(number);
      if (!descriptor || !descriptor->name) {
        continue;
      }
      const char *left = descriptor->name;
      const char *right = name;
      while (*left && *right && *left == *right) {
        ++left;
        ++right;
      }
      if (*left == '\0' && *right == '\0') {
        return descriptor->handler(a0, a1, a2, a3, a4, a5);
      }
    }
    return -1L;
  };

  auto call_matrix = [&] {
    (void)sys_wait4(-1, 0, 1, 0, 0, 0);
    (void)sys_kill(0, 0, 0, 0, 0, 0);
    (void)named("getpgid", 0, 0, 0, 0, 0, 0);
    (void)named("getpgrp", 0, 0, 0, 0, 0, 0);
    (void)named("getsid", 0, 0, 0, 0, 0, 0);
    (void)named("setpgid", 0, 0, 0, 0, 0, 0);
    (void)named("setpgrp", 0, 0, 0, 0, 0, 0);
    (void)named("setsid", 0, 0, 0, 0, 0, 0);
    (void)named("sigaction", 0, 0, 0, 0, 0, 0);
    (void)named("sigprocmask", 0, 0, 0, 0, 0, 0);
    (void)named("sigaltstack", 0, 0, 0, 0, 0, 0);
    (void)sys_execve(0, 0, 0, 0, 0, 0);
    (void)sys_execve_cap(0, 0, 0, 0, 0, 0);
    (void)sys_mmap(unmapped, 0, 0, 0, 0, 0);
    (void)sys_munmap(unmapped, 0, 0, 0, 0, 0);
    (void)sys_brk(0, 0, 0, 0, 0, 0);
    (void)sys_nanosleep(0, 0, 0, 0, 0, 0);
    (void)sys_clock_nanosleep(0, 0, 0, 0, 0, 0);
    (void)sys_sched_setaffinity(0, 0, 0, 0, 0, 0);
    (void)sys_getpriority(0, 0, 0, 0, 0, 0);
    (void)sys_nice(0, 0, 0, 0, 0, 0);
    (void)sys_topinfo(0, 0, 0, 0, 0, 0);
    (void)sys_system_info(0, 0, 0, 0, 0, 0);
    (void)sys_domain_wait(0, 0, 0, 0, 0, 0);
    (void)sys_object_wait(0, 0, 0, 0, 0, 0);
    (void)sys_ipc_call(0, 0, 0, 0, 0, 0);
    (void)sys_ipc_receive(0, 0, 0, 0, 0, 0);
    (void)sys_ipc_try_receive(0, 0, 0, 0, 0, 0);
    (void)sys_ipc_reply(0, 0, 0, 0, 0, 0);
  };

  // Each partial state reaches a different operand of the compound guards.
  process::CfsScheduler::set_current_task(nullptr);
  process::g_process_manager = fixture_manager;
  process::g_scheduler = saved_scheduler;
  call_matrix();

  process::CfsScheduler::set_current_task(fixture_thread);
  process::g_process_manager = nullptr;
  process::g_scheduler = saved_scheduler;
  call_matrix();

  process::CfsScheduler::set_current_task(fixture_thread);
  process::g_process_manager = fixture_manager;
  process::g_scheduler = nullptr;
  call_matrix();

  process::g_scheduler = saved_scheduler;
  fixture.proc->clear_address_space();
  call_matrix();
  (void)fixture.proc->set_address_space(fixture.address_space);

  process::CfsScheduler::set_current_task(saved_current);
  process::g_process_manager = saved_manager;
  process::g_scheduler = saved_scheduler;
  ut::expect(true);
}

void allocation_failures() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  HeapPressure pressure;
  if (!ut::expect(pressure.acquire(64))) {
    pressure.release();
    return;
  }
  ut::expect(sys_ipc_create(code + 3 * page + 768, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_mem_create(0, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_domain_scope_create(0, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_domain_factory(0, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_profile_create(0, 1000, 10000, 0, 0, 0) < 0);
  ut::expect(sys_mmap(0, page, process::vma_flags::READ | process::vma_flags::WRITE,
                      process::vma_flags::DEMAND_ZERO, -1, 0) < 0);
  // Pipe descriptors come from the preallocated fd slab, so heap pressure
  // does not necessarily fail this call; release whatever it published.
  if (sys_pipe(code + 3 * page + 800, 0, 0, 0, 0, 0) == 0) {
    long fds[2] = {-1, -1};
    if (fixture.address_space->copy_from_user(fds, code + 3 * page + 800, sizeof(fds)) == 0) {
      (void)sys_close(fds[0], 0, 0, 0, 0, 0);
      (void)sys_close(fds[1], 0, 0, 0, 0, 0);
    }
  }
  pressure.release();
}

void argument_matrix() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  using Handler = long (*)(long, long, long, long, long, long) noexcept;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr mapped = code + 3 * page + 768;
  const long pid = static_cast<long>(fixture.proc->pid());

  constexpr Handler entries[] = {
      sys_getpid,          sys_getppid,          sys_getuid,
      sys_getgid,          sys_geteuid,          sys_getegid,
      sys_domain_id,       sys_domain_same,      sys_domain_self,
      sys_domain_scope_create, sys_domain_scope_terminate, sys_domain_scope_status,
      sys_domain_scope_contains, sys_domain_factory, sys_profile_create,
      sys_profile_apply,   sys_profile_status,   sys_code_snapshot,
      sys_code_snapshot_range, sys_code_read,    sys_code_read_range,
      sys_code_page_count, sys_code_authority,   sys_code_approve,
      sys_code_revoke,     sys_cap_close,        sys_cap_duplicate,
      sys_cap_set_inherit, sys_cap_set_exec,     sys_open,
      sys_close,           sys_read,             sys_write,
      sys_lseek,           sys_stat,             sys_fstat,
      sys_getdents,        sys_mkdir,            sys_rmdir,
      sys_unlink,          sys_rename,           sys_fcntl,
      sys_dup,             sys_dup2,
      sys_pipe,            sys_mmap,             sys_munmap,
      sys_mprotect,        sys_brk,              sys_nice,
      sys_getpriority,     sys_sched_yield,      sys_sched_getaffinity,
      sys_sched_setaffinity, sys_clock_gettime,   sys_clock_getres,
      sys_clock_nanosleep, sys_socket,           sys_bind,
      sys_listen,          sys_accept,           sys_ipc_create,
      sys_ipc_mint_badge,  sys_ipc_call,         sys_ipc_try_receive,
      sys_ipc_reply,       sys_mem_create,       sys_mem_map,
      sys_boot_archive,    sys_domain_info,      sys_domain_layout,
      sys_domain_status,   sys_domain_terminate, sys_domain_signal,
      sys_domain_wait,     sys_domain_wait_any,  sys_object_wait,
      sys_system_info,     sys_topinfo,          sys_debug_print,
      sys_not_implemented,
  };
  const long patterns[][6] = {
      {0, 0, 0, 0, 0, 0},
      {-1, -1, -1, -1, -1, -1},
      {static_cast<long>(mapped), static_cast<long>(mapped), static_cast<long>(mapped), static_cast<long>(mapped),
       static_cast<long>(mapped), static_cast<long>(mapped)},
      {pid, 0, 0, 0, 0, 0},
  };
  constexpr usize entry_count = sizeof(entries) / sizeof(entries[0]);
  const usize pattern_count = sizeof(patterns) / sizeof(patterns[0]);
  for (usize i = 0; i < entry_count; ++i) {
    for (usize j = 0; j < pattern_count; ++j) {
      const long *args = patterns[j];
      (void)entries[i](args[0], args[1], args[2], args[3], args[4], args[5]);
    }
  }
  ut::expect(true);
}

void filesystem_syscall_paths() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr path_addr = code + 3 * page + 768;
  constexpr VirtAddr buffer_addr = code + 3 * page + 1024;
  constexpr VirtAddr stat_addr = code + 3 * page + 1280;
  constexpr VirtAddr dirent_addr = code + 3 * page + 1536;
  constexpr VirtAddr pipe_addr = code + 3 * page + 1792;
  constexpr VirtAddr path_addr2 = code + 3 * page + 2048;
  constexpr VirtAddr unmapped = 0xdead0000ULL;
  auto put_path = [&](const char *path) {
    return fixture.address_space->copy_to_user(path_addr, path, __builtin_strlen(path) + 1) == 0;
  };

  ut::expect(sys_open(0, vfs::O_RDONLY, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(put_path("/fixture.bin"));
  ut::expect(sys_open(path_addr, -1, 0, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(sys_open(path_addr, vfs::O_CREAT, -1, 0, 0, 0) == -syscall::errc::EINVAL);
  const long file = sys_open(path_addr, vfs::O_RDONLY, 0, 0, 0, 0);
  ut::expect(file >= 0);
  if (file >= 0) {
    u8 bytes[16]{};
    ut::expect(sys_read(file, 0, sizeof(bytes), 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_read(file, buffer_addr, 0, 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_read(file, unmapped, sizeof(bytes), 0, 0, 0) == -syscall::errc::EFAULT);
    ut::expect(sys_read(file, buffer_addr, sizeof(bytes), 0, 0, 0) == static_cast<long>(sizeof(bytes)));
    ut::expect(fixture.address_space->copy_from_user(bytes, buffer_addr, sizeof(bytes)) == 0);
    bool content = true;
    for (usize i = 0; i < sizeof(bytes); ++i) {
      content = content && bytes[i] == static_cast<u8>(i);
    }
    ut::expect(content);
    ut::expect(sys_lseek(file, 0, 0, 0, 0, 0) == 0);
    ut::expect(sys_lseek(file, 0, 3, 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_fstat(file, 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);
    ut::expect(sys_fstat(file, stat_addr, 0, 0, 0, 0) == 0);
    ut::expect(sys_fcntl(file, 1, 0, 0, 0, 0) >= 0);
    ut::expect(sys_fcntl(file, 2, 1, 0, 0, 0) == 0);
    ut::expect(sys_fcntl(file, 3, 0, 0, 0, 0) >= 0);
    ut::expect(sys_fcntl(file, 99, 0, 0, 0, 0) < 0);
    const long duplicate = sys_dup(file, 0, 0, 0, 0, 0);
    ut::expect(duplicate >= 0);
    if (duplicate >= 0) {
      ut::expect(sys_close(duplicate, 0, 0, 0, 0, 0) == 0);
    }
    ut::expect(sys_dup2(file, 10, 0, 0, 0, 0) == 10);
    ut::expect(sys_close(10, 0, 0, 0, 0, 0) == 0);
    ut::expect(sys_dup(9999, 0, 0, 0, 0, 0) == -syscall::errc::EBADF);
    ut::expect(sys_close(file, 0, 0, 0, 0, 0) == 0);
    ut::expect(sys_close(file, 0, 0, 0, 0, 0) == -syscall::errc::EBADF);
  }

  ut::expect(sys_stat(0, stat_addr, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_stat(path_addr, stat_addr, 0, 0, 0, 0) == 0);

  ut::expect(put_path("/dev/null"));
  const long null_fd = sys_open(path_addr, vfs::O_RDWR, 0, 0, 0, 0);
  ut::expect(null_fd >= 0);
  if (null_fd >= 0) {
    const char bytes[] = {'n', 'u', 'l', 'l'};
    ut::expect(fixture.address_space->copy_to_user(buffer_addr, bytes, sizeof(bytes)) == 0);
    ut::expect(sys_write(null_fd, buffer_addr, sizeof(bytes), 0, 0, 0) == static_cast<long>(sizeof(bytes)));
    ut::expect(sys_read(null_fd, buffer_addr, sizeof(bytes), 0, 0, 0) == 0);
    ut::expect(sys_close(null_fd, 0, 0, 0, 0, 0) == 0);
  }

  long fds[2] = {-1, -1};
  ut::expect(sys_pipe(pipe_addr, 0, 0, 0, 0, 0) == 0);
  if (ut::expect(fixture.address_space->copy_from_user(fds, pipe_addr, sizeof(fds)) == 0) && fds[0] >= 0 &&
      fds[1] >= 0) {
    const char bytes[] = {'p', 'i', 'p', 'e'};
    ut::expect(fixture.address_space->copy_to_user(buffer_addr, bytes, sizeof(bytes)) == 0);
    ut::expect(sys_write(fds[1], buffer_addr, sizeof(bytes), 0, 0, 0) == static_cast<long>(sizeof(bytes)));
    ut::expect(sys_read(fds[0], buffer_addr, sizeof(bytes), 0, 0, 0) == static_cast<long>(sizeof(bytes)));
    ut::expect(sys_close(fds[0], 0, 0, 0, 0, 0) == 0);
    ut::expect(sys_close(fds[1], 0, 0, 0, 0, 0) == 0);
  }

  ut::expect(put_path("/covdir"));
  ut::expect(sys_mkdir(path_addr, 0755, 0, 0, 0, 0) == 0);
  const long dir = sys_open(path_addr, vfs::O_RDONLY, 0, 0, 0, 0);
  ut::expect(dir >= 0);
  if (dir >= 0) {
    ut::expect(sys_getdents(dir, dirent_addr, 0, 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_getdents(dir, dirent_addr, sizeof(vfs::DirEntry), 0, 0, 0) >= 0);
    ut::expect(sys_close(dir, 0, 0, 0, 0, 0) == 0);
  }
  ut::expect(fixture.address_space->copy_to_user(path_addr2, "/covdir2", sizeof("/covdir2")) == 0);
  ut::expect(sys_rename(path_addr, path_addr2, 0, 0, 0, 0) == 0);
  ut::expect(fixture.address_space->copy_to_user(path_addr, "/covdir2", sizeof("/covdir2")) == 0);
  ut::expect(put_path("/covfile"));
  ut::expect(sys_unlink(path_addr, 0, 0, 0, 0, 0) < 0);
  ut::expect(fixture.address_space->copy_to_user(path_addr, "/covdir2", sizeof("/covdir2")) == 0);
  ut::expect(sys_rmdir(path_addr, 0, 0, 0, 0, 0) == 0);
}

void fork_domain_errors() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr cap_out = code + 3 * page + 768;
  constexpr VirtAddr unmapped = 0xdead0000ULL;
  auto *thread = process::CfsScheduler::get_current_task();
  if (!ut::expect(thread != nullptr)) {
    return;
  }
  moss::abi::TrapFrame frame{};
  mark_user_frame(frame);
  frame.pc = static_cast<u64>(code);
  frame.sp = static_cast<u64>(code + 4 * page);
  thread->trap_frame = &frame;

  ut::expect(sys_fork_domain(unmapped, 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_fork_domain_scoped(cap_out, 0, 0, 9999, 0, 0) == -syscall::errc::EBADF);
  ut::expect(sys_fork_domain_scoped(cap_out, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);
  const long scope = sys_domain_scope_create(0, 0, 0, 0, 0, 0);
  ut::expect(scope > 0);
  if (scope > 0) {
    ut::expect(sys_domain_scope_terminate(scope, 0, 0, 0, 0, 0) == 0);
    ut::expect(sys_fork_domain_scoped(cap_out, 0, 0, scope, 0, 0) == -syscall::errc::EACCES);
    ut::expect(sys_cap_close(scope, 0, 0, 0, 0, 0) == 0);
  }

  HeapPressure pressure;
  if (ut::expect(pressure.acquire(sizeof(process::Process)))) {
    ut::expect(sys_fork_domain(cap_out, 0, 0, 0, 0, 0) == -syscall::errc::ENOMEM);
    pressure.release();
  }
  thread->trap_frame = nullptr;
}

void control_ipc_error_paths() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr handles_addr = code + 3 * page + 768;
  constexpr VirtAddr request_addr = code + 3 * page + 832;
  constexpr VirtAddr response_addr = code + 3 * page + 1152;
  constexpr VirtAddr unmapped = 0xdead0000ULL;
  struct ControlMessageCompat {
    u64 size;
    long capability;
    u64 rights;
    u64 badge;
    u8 payload[256];
  };
  auto put = [&](VirtAddr destination, const void *source, usize size) {
    return fixture.address_space->copy_to_user(destination, source, size) == 0;
  };
  ut::expect(sys_mem_create(1L << 48, 0, 0, 0, 0, 0) == -syscall::errc::ENOMEM);
  const long memory = sys_mem_create(page, 0, 0, 0, 0, 0);
  ut::expect(memory > 0);
  if (memory > 0) {
    ut::expect(sys_mem_map(memory, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);
    fixture.address_space->mmap_next = moss::kernel::USER_MAX;
    ut::expect(sys_mem_map(memory, capability::rights::MAP_READ, 0, 0, 0, 0) < 0);
    fixture.address_space->mmap_next = process::user_layout::MMAP_BASE;
    ut::expect(sys_mem_map(9999, capability::rights::MAP_READ, 0, 0, 0, 0) == -syscall::errc::EBADF);
    ut::expect(sys_cap_close(memory, 0, 0, 0, 0, 0) == 0);
  }

  HeapPressure pressure;
  if (ut::expect(pressure.acquire(page))) {
    const long pressured_memory = sys_mem_create(page, 0, 0, 0, 0, 0);
    if (pressured_memory > 0) {
      ut::expect(sys_cap_close(pressured_memory, 0, 0, 0, 0, 0) == 0);
    }
    if (sys_ipc_create(handles_addr, 0, 0, 0, 0, 0) == 0) {
      long pair[2] = {-1, -1};
      if (fixture.address_space->copy_from_user(pair, handles_addr, sizeof(pair)) == 0) {
        (void)sys_cap_close(pair[0], 0, 0, 0, 0, 0);
        (void)sys_cap_close(pair[1], 0, 0, 0, 0, 0);
      }
    }
    pressure.release();
  }
  ut::expect(sys_ipc_create(unmapped, 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);

  long endpoints[2] = {-1, -1};
  ut::expect(sys_ipc_create(handles_addr, 0, 0, 0, 0, 0) == 0);
  if (ut::expect(fixture.address_space->copy_from_user(endpoints, handles_addr, sizeof(endpoints)) == 0) &&
      endpoints[0] >= 0 && endpoints[1] >= 0) {
    ControlMessageCompat message{};
    message.size = 1;
    message.capability = 9999;
    message.rights = capability::rights::MAP_READ | capability::rights::TRANSFER;
    message.payload[0] = 1;
    ut::expect(put(request_addr, &message, sizeof(message)));
    ut::expect(sys_ipc_call(endpoints[0], request_addr, response_addr, 0, 0, 0) == -syscall::errc::EBADF);
    if (ut::expect(pressure.acquire(page))) {
      ut::expect(sys_ipc_call(endpoints[0], request_addr, response_addr, 1, 0, 0) < 0);
      pressure.release();
    }
    message.capability = INVALID_HANDLE;
    message.rights = 0;
    ut::expect(put(request_addr, &message, sizeof(message)));
    auto *saved_scheduler = process::g_scheduler;
    process::g_scheduler = nullptr;
    ut::expect(sys_ipc_call(endpoints[0], request_addr, response_addr, 0, 0, 0) == -syscall::errc::ESRCH);
    ut::expect(sys_ipc_try_receive(endpoints[1], request_addr, response_addr, 0, 0, 0) == -syscall::errc::ESRCH);
    process::g_scheduler = saved_scheduler;

    const long duplicate = sys_cap_duplicate(endpoints[1], capability::rights::RECEIVE | capability::rights::TRANSFER |
                                                 capability::rights::DUPLICATE, 0, 0, 0, 0);
    ut::expect(duplicate > 0);
    if (duplicate > 0) {
      ut::expect(sys_cap_close(endpoints[1], 0, 0, 0, 0, 0) == 0);
      ut::expect(sys_ipc_try_receive(duplicate, request_addr, response_addr, 0, 0, 0) == -syscall::errc::EAGAIN);
      ut::expect(sys_cap_close(duplicate, 0, 0, 0, 0, 0) == 0);
    }
    ut::expect(sys_cap_close(endpoints[0], 0, 0, 0, 0, 0) == 0);
  }

  auto *saved_manager = process::g_process_manager;
  process::g_process_manager = nullptr;
  ut::expect(sys_ipc_reply(1, response_addr, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  process::g_process_manager = saved_manager;

  auto second = fixture.manager.create_process();
  ut::expect(static_cast<bool>(second));
  if (ut::expect(static_cast<bool>(second))) {
    auto *thread = new process::Thread(8001, (*second)->pid());
    if (ut::expect((*second)->register_thread(thread))) {
      thread->state = process::ProcessState::Running;
      process::CfsScheduler::set_current_task(thread);
      const long handle = sys_mem_create(page, 0, 0, 0, 0, 0);
      ut::expect(handle > 0);
      if (handle > 0) {
        ut::expect(sys_mem_map(handle, capability::rights::MAP_READ, 0, 0, 0, 0) == -syscall::errc::ESRCH);
        (*second)->designate_initial_supervisor();
        ut::expect(sys_boot_archive(unmapped, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
        ut::expect(sys_cap_close(handle, 0, 0, 0, 0, 0) == 0);
      }
    } else {
      delete thread;
    }
  }
  process::CfsScheduler::set_current_task(fixture.saved_current);
}

void ipc_branch_paths() {
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr handles_addr = code + 3 * page + 768;
  constexpr VirtAddr request_addr = code + 3 * page + 832;
  constexpr VirtAddr response_addr = code + 3 * page + 1408;

  // No current process, thread or scheduler: every control-IPC entry has to
  // reject before touching capability or timer state.
  auto *saved_current = process::CfsScheduler::get_current_task();
  auto *saved_manager = process::g_process_manager;
  auto *saved_scheduler = process::g_scheduler;
  process::CfsScheduler::set_current_task(nullptr);
  process::g_process_manager = nullptr;
  process::g_scheduler = nullptr;
  (void)sys_mem_create(page, 0, 0, 0, 0, 0);
  (void)sys_mem_map(1, capability::rights::MAP_READ, 0, 0, 0, 0);
  (void)sys_boot_archive(0, 0, 0, 0, 0, 0);
  (void)sys_ipc_create(0, 0, 0, 0, 0, 0);
  (void)sys_ipc_mint_badge(1, 0, 0, 0, 0, 0);
  (void)sys_ipc_call(1, 0, 0, 0, 0, 0);
  (void)sys_ipc_receive(1, 0, 0, 0, 0, 0);
  (void)sys_ipc_try_receive(1, 0, 0, 0, 0, 0);
  (void)sys_ipc_reply(1, 0, 0, 0, 0, 0);
  process::CfsScheduler::set_current_task(saved_current);
  process::g_process_manager = saved_manager;
  process::g_scheduler = saved_scheduler;
  ut::expect(true);

  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  auto put = [&](VirtAddr destination, const void *source, usize size) {
    return fixture.address_space->copy_to_user(destination, source, size) == 0;
  };

  struct ControlMessageCompat {
    u64 size;
    long capability;
    u64 rights;
    u64 badge;
    u8 payload[256];
  };
  ControlMessageCompat message{};
  ut::expect(put(request_addr, &message, sizeof(message)));

  const long memory = sys_mem_create(page, 0, 0, 0, 0, 0);
  ut::expect(memory > 0);
  if (memory > 0) {
    fixture.address_space->mmap_next = moss::kernel::USER_MAX;
    (void)sys_mem_map(memory, capability::rights::MAP_READ, 0, 0, 0, 0);
    fixture.address_space->mmap_next = process::user_layout::MMAP_BASE;
    const auto conflict = fixture.address_space->add_vma(
        process::user_layout::MMAP_BASE, process::user_layout::MMAP_BASE + page, process::vma_flags::READ,
        process::VmaType::MMAP);
    (void)sys_mem_map(memory, capability::rights::MAP_READ, 0, 0, 0, 0);
    if (conflict) {
      (void)fixture.address_space->remove_vma(process::user_layout::MMAP_BASE,
                                              process::user_layout::MMAP_BASE + page);
    }
    fixture.proc->clear_address_space();
    (void)sys_mem_map(memory, capability::rights::MAP_READ, 0, 0, 0, 0);
    (void)fixture.proc->set_address_space(fixture.address_space);
    (void)sys_cap_close(memory, 0, 0, 0, 0, 0);
  }

  // The archive path has two independent early exits: an empty initramfs and
  // a supervisor without an address space.
  fixture.proc->designate_initial_supervisor();
  auto saved_archive = initramfs::g_initramfs;
  initramfs::g_initramfs = {};
  (void)sys_boot_archive(handles_addr, 0, 0, 0, 0, 0);
  initramfs::g_initramfs = saved_archive;
  fixture.proc->clear_address_space();
  (void)sys_boot_archive(handles_addr, 0, 0, 0, 0, 0);
  (void)fixture.proc->set_address_space(fixture.address_space);

  const long handle = sys_mem_create(page, 0, 0, 0, 0, 0);
  if (handle > 0) {
    (void)sys_cap_close(handle, 0, 0, 0, 0, 0);
  }
  {
    PagePressure pressure;
    if (pressure.acquire(0)) {
      (void)sys_mem_create(page, 0, 0, 0, 0, 0);
    }
  }

  ut::expect(sys_ipc_create(0, 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  long endpoints[2] = {-1, -1};
  if (!ut::expect(sys_ipc_create(handles_addr, 0, 0, 0, 0, 0) == 0) ||
      !ut::expect(fixture.address_space->copy_from_user(endpoints, handles_addr, sizeof(endpoints)) == 0) ||
      endpoints[0] < 0 || endpoints[1] < 0) {
    return;
  }

  const long minted = sys_ipc_mint_badge(endpoints[0], 0, 0, 0, 0, 0);
  if (minted > 0) {
    (void)sys_cap_close(minted, 0, 0, 0, 0, 0);
  }

  auto *current = process::CfsScheduler::get_current_task();
  if (ut::expect(current != nullptr)) {
    process::PriorityDonation held{};
    current->ipc_wait = &held;
    (void)sys_ipc_call(endpoints[0], request_addr, response_addr, 0, 0, 0);
    current->ipc_wait = nullptr;
  }

  (void)sys_ipc_call(endpoints[0], request_addr, response_addr, 1, 0, 0);

  auto &timers = timer::TimerSubsystem::instance();
  timers.shutdown();
  (void)sys_ipc_call(endpoints[0], request_addr, response_addr, 0, 0, 0);
  ut::expect(timers.initialize().has_value());

  ut::expect(sys_ipc_try_receive(endpoints[1], request_addr, response_addr, 0, 0, 0) == -syscall::errc::EAGAIN);
  if (current != nullptr) {
    current->pending_signals.fetch_or(process::sig::sigmask(process::sig::SIGKILL));
    (void)sys_ipc_receive(endpoints[1], request_addr, response_addr, 0, 0, 0);
    current->pending_signals.store(0);
  }

  (void)sys_ipc_reply(endpoints[0], 0xdead0000ULL, 0, 0, 0, 0);
  (void)sys_ipc_reply(endpoints[0], request_addr, 0, 0, 0, 0);
  (void)sys_cap_close(endpoints[1], 0, 0, 0, 0, 0);
  (void)sys_ipc_try_receive(endpoints[1], request_addr, response_addr, 0, 0, 0);
  (void)sys_cap_close(endpoints[0], 0, 0, 0, 0, 0);
}

void capability_exhaustion_paths() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr handles_addr = code + 3 * page + 768;
  long handles[128]{};
  usize count = 0;

  for (; count < sizeof(handles) / sizeof(handles[0]); ++count) {
    handles[count] = sys_mem_create(page, 0, 0, 0, 0, 0);
    if (handles[count] < 0) {
      break;
    }
  }
  ut::expect(count > 0);
  ut::expect(sys_mem_create(page, 0, 0, 0, 0, 0) < 0);
  (void)sys_ipc_create(handles_addr, 0, 0, 0, 0, 0);
  fixture.proc->designate_domain_factory_source();
  (void)sys_domain_self(0, 0, 0, 0, 0, 0);
  (void)sys_domain_scope_create(0, 0, 0, 0, 0, 0);
  (void)sys_domain_factory(0, 0, 0, 0, 0, 0);
  (void)sys_profile_create(0, 1000, 10000, 0, 0, 0);
  (void)sys_code_snapshot(code, 0, 0, 0, 0, 0);
  (void)sys_code_snapshot_range(code, 1, 0, 0, 0, 0);
  (void)sys_code_authority(0, 0, 0, 0, 0, 0);

  // One free slot makes the first install succeed and the paired receiver
  // install fail, exercising the rollback of the sender handle.
  if (count > 0) {
    (void)sys_cap_close(handles[--count], 0, 0, 0, 0, 0);
  }
  (void)sys_ipc_create(handles_addr, 0, 0, 0, 0, 0);

  // Two free slots create a usable endpoint pair; refill the table before
  // minting so only the minted sender installation can fail.
  for (usize i = 0; i < 2 && count > 0; ++i) {
    (void)sys_cap_close(handles[--count], 0, 0, 0, 0, 0);
  }
  long endpoints[2] = {-1, -1};
  if (sys_ipc_create(handles_addr, 0, 0, 0, 0, 0) == 0 &&
      fixture.address_space->copy_from_user(endpoints, handles_addr, sizeof(endpoints)) == 0) {
    for (; count < sizeof(handles) / sizeof(handles[0]); ++count) {
      handles[count] = sys_mem_create(page, 0, 0, 0, 0, 0);
      if (handles[count] < 0) {
        break;
      }
    }
    (void)sys_ipc_mint_badge(endpoints[0], 1, 0, 0, 0, 0);
    (void)sys_cap_close(endpoints[0], 0, 0, 0, 0, 0);
    (void)sys_cap_close(endpoints[1], 0, 0, 0, 0, 0);
  }

  for (usize i = 0; i < count; ++i) {
    (void)sys_cap_close(handles[i], 0, 0, 0, 0, 0);
  }
}

void syscall_state_edges() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr handles_addr = code + 3 * page + 768;
  constexpr VirtAddr status_addr = code + 3 * page + 896;
  constexpr long unmapped = 0x1000;
  auto *thread = process::CfsScheduler::get_current_task();
  const auto saved_state = fixture.proc->state();
  const u32 saved_cpus = g_num_cpus;

  const process::ProcessState states[] = {process::ProcessState::Sleeping, process::ProcessState::Stopped,
                                          process::ProcessState::Zombie, process::ProcessState::Terminated};
  for (const auto state : states) {
    fixture.proc->set_state(state);
    (void)sys_wait4(static_cast<long>(fixture.proc->pid()), status_addr, 1, 0, 0, 0);
    (void)sys_kill(static_cast<long>(fixture.proc->pid()), 0, 0, 0, 0, 0);
    (void)sys_getpriority(0, static_cast<long>(fixture.proc->pid()), 0, 0, 0, 0);
    (void)sys_sched_getaffinity(static_cast<long>(fixture.proc->pid()), sizeof(u32), status_addr, 0, 0, 0);
    (void)sys_sched_setaffinity(static_cast<long>(fixture.proc->pid()), sizeof(u32), status_addr, 0, 0, 0);
    (void)sys_domain_status(0, status_addr, 0, 0, 0, 0);
    (void)sys_domain_terminate(0, 0, 0, 0, 0, 0);
    (void)sys_domain_signal(0, 0, 0, 0, 0, 0);
    (void)sys_domain_wait(0, 0, 0, 0, 0, 0);
    (void)sys_domain_wait_any(handles_addr, 0, 0, 0, 0, 0);
    (void)sys_object_wait(handles_addr, 0, 0, 0, 0, 0);
  }
  fixture.proc->set_state(saved_state);

  fixture.proc->clear_address_space();
  (void)sys_mmap(unmapped, page, 0, 0, 0, 0);
  (void)sys_munmap(unmapped, page, 0, 0, 0, 0);
  (void)sys_brk(static_cast<long>(process::user_layout::HEAP_START), 0, 0, 0, 0, 0);
  (void)sys_open(unmapped, 0, 0, 0, 0, 0);
  (void)sys_read(0, unmapped, 1, 0, 0, 0);
  (void)sys_write(0, unmapped, 1, 0, 0, 0);
  (void)sys_stat(unmapped, status_addr, 0, 0, 0, 0);
  (void)sys_pipe(handles_addr, 0, 0, 0, 0, 0);
  (void)sys_code_snapshot(code, 0, 0, 0, 0, 0);
  (void)sys_boot_archive(status_addr, 0, 0, 0, 0, 0);
  (void)sys_mem_map(0, capability::rights::MAP_READ, 0, 0, 0, 0);
  (void)fixture.proc->set_address_space(fixture.address_space);

  if (thread) {
    const auto saved_thread_state = thread->state.load(moss::memory_order_relaxed);
    const process::ProcessState thread_states[] = {process::ProcessState::Sleeping, process::ProcessState::Zombie,
                                                   process::ProcessState::Terminated};
    for (const auto state : thread_states) {
      thread->state.store(state, moss::memory_order_relaxed);
      (void)sys_nanosleep(0, 0, 0, 0, 0, 0);
      (void)sys_sched_yield(0, 0, 0, 0, 0, 0);
      (void)sys_ipc_call(0, 0, 0, 0, 0, 0);
    }
    thread->state.store(saved_thread_state, moss::memory_order_relaxed);
  }

  g_num_cpus = 0;
  (void)sys_sched_setaffinity(0, sizeof(u32), status_addr, 0, 0, 0);
  (void)sys_topinfo(status_addr, 0, 0, 0, 0, 0);
  (void)sys_system_info(status_addr, 0, 0, 0, 0, 0);
  g_num_cpus = saved_cpus;
  ut::expect(true);
}

void environment_matrix() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr mapped = process::user_layout::CODE_BASE + 3 * page;
  constexpr VirtAddr mapped2 = mapped + 512;
  constexpr VirtAddr unmapped = 0xdead0000ULL;
  auto put = [&](VirtAddr destination, const void *source, usize size) {
    return fixture.address_space->copy_to_user(destination, source, size) == 0;
  };
  u32 mask = 1;
  Handle handle = 0;
  (void)put(mapped, &mask, sizeof(mask));
  (void)put(mapped2, &handle, sizeof(handle));

  auto sweep = [&] {
    (void)sys_getpid(0, 0, 0, 0, 0, 0);
    (void)sys_getppid(0, 0, 0, 0, 0, 0);
    (void)sys_getuid(0, 0, 0, 0, 0, 0);
    (void)sys_getgid(0, 0, 0, 0, 0, 0);
    (void)sys_geteuid(0, 0, 0, 0, 0, 0);
    (void)sys_getegid(0, 0, 0, 0, 0, 0);
    (void)sys_domain_id(0, 0, 0, 0, 0, 0);
    (void)sys_domain_same(0, 0, 0, 0, 0, 0);
    (void)sys_domain_self(0, 0, 0, 0, 0, 0);
    (void)sys_domain_factory(0, 0, 0, 0, 0, 0);
    (void)sys_domain_scope_create(0, 0, 0, 0, 0, 0);
    (void)sys_domain_scope_terminate(0, 0, 0, 0, 0, 0);
    (void)sys_domain_scope_status(0, static_cast<long>(mapped), 0, 0, 0, 0);
    (void)sys_domain_scope_contains(0, 0, 0, 0, 0, 0);
    (void)sys_profile_create(0, 1000, 10000, 0, 0, 0);
    (void)sys_profile_apply(0, 1000, 0, 0, 0, 0);
    (void)sys_profile_status(0, static_cast<long>(mapped), 0, 0, 0, 0);
    (void)sys_code_snapshot(0, 0, 0, 0, 0, 0);
    (void)sys_code_snapshot_range(0, 0, 0, static_cast<long>(mapped), 0, 0);
    (void)sys_code_read(0, 0, static_cast<long>(mapped), 1, 0, 0);
    (void)sys_code_read_range(0, 0, 1, static_cast<long>(mapped), 0, 0);
    (void)sys_code_page_count(0, 0, 0, 0, 0, 0);
    (void)sys_code_authority(0, 0, 0, 0, 0, 0);
    (void)sys_code_approve(0, 0, 0, 0, 0, 0);
    (void)sys_code_revoke(0, 0, 0, 0, 0, 0);
    (void)sys_cap_close(0, 0, 0, 0, 0, 0);
    (void)sys_cap_duplicate(0, capability::rights::MAP_READ, 0, 0, 0, 0);
    (void)sys_cap_set_inherit(0, 0, 0, 0, 0, 0);
    (void)sys_cap_set_exec(0, 0, 0, 0, 0, 0);
    (void)sys_open(static_cast<long>(mapped2), 0, 0, 0, 0, 0);
    (void)sys_close(0, 0, 0, 0, 0, 0);
    (void)sys_read(0, static_cast<long>(mapped), 1, 0, 0, 0);
    (void)sys_write(0, static_cast<long>(mapped), 1, 0, 0, 0);
    (void)sys_lseek(0, 0, 0, 0, 0, 0);
    (void)sys_stat(static_cast<long>(mapped2), static_cast<long>(mapped), 0, 0, 0, 0);
    (void)sys_fstat(0, static_cast<long>(mapped), 0, 0, 0, 0);
    (void)sys_getdents(0, static_cast<long>(mapped), sizeof(vfs::DirEntry), 0, 0, 0);
    (void)sys_mkdir(static_cast<long>(mapped2), 0755, 0, 0, 0, 0);
    (void)sys_rmdir(static_cast<long>(mapped2), 0, 0, 0, 0, 0);
    (void)sys_unlink(static_cast<long>(mapped2), 0, 0, 0, 0, 0);
    (void)sys_rename(static_cast<long>(mapped2), static_cast<long>(mapped2), 0, 0, 0, 0);
    (void)sys_mmap(0, page, process::vma_flags::READ | process::vma_flags::WRITE,
                   process::vma_flags::DEMAND_ZERO, -1, 0);
    (void)sys_mprotect(static_cast<long>(mapped), page, 0, 0, 0, 0);
    (void)sys_brk(static_cast<long>(process::user_layout::HEAP_START), 0, 0, 0, 0, 0);
    (void)sys_nice(0, 0, 0, 0, 0, 0);
    (void)sys_getpriority(0, 0, 0, 0, 0, 0);
    (void)sys_sched_yield(0, 0, 0, 0, 0, 0);
    (void)sys_sched_getaffinity(0, sizeof(mask), static_cast<long>(mapped), 0, 0, 0);
    (void)sys_sched_setaffinity(0, sizeof(mask), static_cast<long>(mapped), 0, 0, 0);
    (void)sys_clock_gettime(0, static_cast<long>(mapped), 0, 0, 0, 0);
    (void)sys_clock_getres(0, static_cast<long>(mapped), 0, 0, 0, 0);
    (void)sys_ipc_create(static_cast<long>(mapped), 0, 0, 0, 0, 0);
    (void)sys_ipc_mint_badge(0, 0, 0, 0, 0, 0);
    (void)sys_ipc_call(0, static_cast<long>(mapped2), static_cast<long>(mapped), 1, 0, 0);
    (void)sys_ipc_receive(0, static_cast<long>(mapped2), static_cast<long>(mapped), 0, 0, 0);
    (void)sys_ipc_try_receive(0, static_cast<long>(mapped2), static_cast<long>(mapped), 0, 0, 0);
    (void)sys_ipc_reply(0, static_cast<long>(mapped2), 0, 0, 0, 0);
    (void)sys_mem_create(page, 0, 0, 0, 0, 0);
    (void)sys_mem_map(0, capability::rights::MAP_READ, 0, 0, 0, 0);
    (void)sys_boot_archive(static_cast<long>(mapped), 0, 0, 0, 0, 0);
    (void)sys_domain_info(0, static_cast<long>(mapped), sizeof(moss::abi::DomainInfo), 0, 0, 0);
    (void)sys_domain_layout(0, static_cast<long>(mapped), 0, 0, 0, 0);
    (void)sys_domain_status(0, static_cast<long>(mapped), 0, 0, 0, 0);
    (void)sys_domain_terminate(0, 0, 0, 0, 0, 0);
    (void)sys_domain_signal(0, 0, 0, 0, 0, 0);
    (void)sys_domain_wait(0, 0, 0, 0, 0, 0);
    (void)sys_domain_wait_any(static_cast<long>(mapped2), 1, 0, 0, 0, 0);
    (void)sys_object_wait(static_cast<long>(mapped2), 1, 1, 0, 0, 0);
    (void)sys_system_info(static_cast<long>(mapped), 0, 0, 0, 0, 0);
    (void)sys_topinfo(static_cast<long>(mapped), 0, 0, 0, 0, 0);
    (void)sys_wait4(-1, static_cast<long>(mapped), 1, 0, 0, 0);
  };

  auto *saved_current = process::CfsScheduler::get_current_task();
  auto *saved_manager = process::g_process_manager;
  auto *saved_scheduler = process::g_scheduler;
  process::Thread unknown(9002, 9002);
  unknown.state = process::ProcessState::Running;
  process::CfsScheduler::set_current_task(nullptr);
  sweep();
  process::CfsScheduler::set_current_task(&unknown);
  sweep();
  process::CfsScheduler::set_current_task(saved_current);
  process::g_process_manager = nullptr;
  process::g_scheduler = nullptr;
  sweep();
  process::g_process_manager = saved_manager;
  sweep();
  process::g_scheduler = saved_scheduler;
  process::CfsScheduler::set_current_task(saved_current);
  (void)sys_munmap(static_cast<long>(mapped), page, 0, 0, 0, 0);
  (void)unmapped;
  ut::expect(true);
}

void fork_allocation_matrix() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr cap_out = code + 3 * page + 768;
  auto *thread = process::CfsScheduler::get_current_task();
  if (!ut::expect(thread != nullptr)) {
    return;
  }
  moss::abi::TrapFrame frame{};
  mark_user_frame(frame);
  frame.pc = static_cast<u64>(code);
  frame.sp = static_cast<u64>(code + 4 * page);
  thread->trap_frame = &frame;

  const long result = sys_fork_domain(cap_out, 0, 0, 0, 0, 0);
  if (result >= 0) {
    Handle domain = INVALID_HANDLE;
    if (fixture.address_space->copy_from_user(&domain, cap_out, sizeof(domain)) != 0 ||
        domain == INVALID_HANDLE) {
      ut::expect(false);
    }
    (void)sys_domain_terminate(static_cast<long>(domain), 0, 0, 0, 0, 0);
    (void)sys_domain_wait(static_cast<long>(domain), 0, 0, 0, 0, 0);
    (void)sys_domain_status(static_cast<long>(domain), cap_out, 0, 0, 0, 0);
    (void)sys_cap_close(static_cast<long>(domain), 0, 0, 0, 0, 0);
  } else {
    ut::expect(result == -syscall::errc::ENOMEM || result == -syscall::errc::EAGAIN ||
               result == -syscall::errc::EFAULT);
  }
  thread->trap_frame = nullptr;
  ut::expect(fixture.manager.total_processes() >= 1);
}

void memory_allocation_matrix() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr handles_addr = code + 3 * page + 768;
  constexpr VirtAddr archive_addr = code + 3 * page + 832;
  fixture.proc->designate_initial_supervisor();

  {
    const long result = sys_mem_create(page, 0, 0, 0, 0, 0);
    if (result > 0) {
      (void)sys_cap_close(result, 0, 0, 0, 0, 0);
    }
  }
  {
    const long result = sys_ipc_create(handles_addr, 0, 0, 0, 0, 0);
    if (result == 0) {
      long pair[2] = {-1, -1};
      if (fixture.address_space->copy_from_user(pair, handles_addr, sizeof(pair)) == 0) {
        (void)sys_cap_close(pair[0], 0, 0, 0, 0, 0);
        (void)sys_cap_close(pair[1], 0, 0, 0, 0, 0);
      }
    }
  }
  {
    const long result = sys_boot_archive(archive_addr, 0, 0, 0, 0, 0);
    if (result > 0) {
      (void)sys_cap_close(result, 0, 0, 0, 0, 0);
    }
  }
  ut::expect(true);
}

void process_syscall_paths() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  auto put = [&](VirtAddr destination, const void *source, usize size) {
    return fixture.address_space->copy_to_user(destination, source, size) == 0;
  };
  auto get = [&](void *destination, VirtAddr source, usize size) {
    return fixture.address_space->copy_from_user(destination, source, size) == 0;
  };
  auto call = [](long number, long a0, long a1, long a2, long a3, long a4, long a5) {
    const auto *descriptor = syscall::SyscallDispatcher::get_syscall_info(number);
    return descriptor ? descriptor->handler(a0, a1, a2, a3, a4, a5) : -1;
  };
  auto number_for = [](const char *name) {
    for (long number = 0; number < static_cast<long>(syscall::SyscallNumber::MAX_SYSCALL); ++number) {
      const auto *descriptor = syscall::SyscallDispatcher::get_syscall_info(number);
      if (!descriptor || !descriptor->name) {
        continue;
      }
      const char *left = descriptor->name;
      const char *right = name;
      while (*left && *right && *left == *right) {
        ++left;
        ++right;
      }
      if (*left == '\0' && *right == '\0') {
        return number;
      }
    }
    return -1L;
  };

  auto *fdt = new vfs::FdTable();
  fdt->init();
  fixture.proc->set_fd_table(fdt);
  fixture.proc->set_name("coverage");

  ut::expect(sys_getpid(0, 0, 0, 0, 0, 0) > 0);
  ut::expect(sys_getppid(0, 0, 0, 0, 0, 0) >= 0);
  ut::expect(sys_getuid(0, 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_getgid(0, 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_geteuid(0, 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_getegid(0, 0, 0, 0, 0, 0) == 0);

  ut::expect(sys_brk(0, 0, 0, 0, 0, 0) == static_cast<long>(process::user_layout::HEAP_START));
  ut::expect(sys_brk(static_cast<long>(process::user_layout::HEAP_START - page), 0, 0, 0, 0, 0) ==
             static_cast<long>(process::user_layout::HEAP_START));
  ut::expect(sys_brk(static_cast<long>(process::user_layout::HEAP_START + 16 * 1024 * 1024 + page), 0, 0, 0, 0, 0) ==
             static_cast<long>(process::user_layout::HEAP_START));
  ut::expect(sys_brk(static_cast<long>(process::user_layout::HEAP_START + page), 0, 0, 0, 0, 0) ==
             static_cast<long>(process::user_layout::HEAP_START + page));
  ut::expect(sys_brk(static_cast<long>(process::user_layout::HEAP_START), 0, 0, 0, 0, 0) ==
             static_cast<long>(process::user_layout::HEAP_START));

  constexpr long prot_rw = 0x1 | 0x2;
  constexpr long map_private_anon = 0x02 | 0x20;
  const auto map1 = sys_mmap(static_cast<long>(process::user_layout::MMAP_BASE), page, prot_rw, map_private_anon, -1, 0);
  ut::expect(map1 == static_cast<long>(process::user_layout::MMAP_BASE));
  const auto map2 = sys_mmap(static_cast<long>(process::user_layout::MMAP_BASE), page, prot_rw, map_private_anon, -1, 0);
  ut::expect(map2 == static_cast<long>(process::user_layout::MMAP_BASE + page));
  ut::expect(sys_mmap(0, 0, prot_rw, map_private_anon, -1, 0) < 0);
  ut::expect(sys_mmap(0, page, prot_rw, 0x02, -1, 0) < 0);
  ut::expect(sys_mmap(0, page, prot_rw | 0x4, map_private_anon, -1, 0) < 0);
  ut::expect(sys_munmap(map1, page, 0, 0, 0, 0) == 0);
  ut::expect(sys_munmap(map2, page, 0, 0, 0, 0) == 0);
  ut::expect(sys_munmap(map1 + 1, page, 0, 0, 0, 0) < 0);
  ut::expect(sys_munmap(map1, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_mprotect(0, 0, 0, 0, 0, 0) < 0);

  u32 affinity = 1U << arch::get_current_cpu_id();
  ut::expect(put(code + 512, &affinity, sizeof(affinity)));
  ut::expect(sys_sched_getaffinity(0, sizeof(affinity), code + 640, 0, 0, 0) == 0);
  ut::expect(sys_sched_setaffinity(0, sizeof(affinity), code + 512, 0, 0, 0) == 0);
  ut::expect(sys_sched_setaffinity(0, 0, code + 512, 0, 0, 0) < 0);
  ut::expect(sys_sched_setaffinity(0, sizeof(affinity), 0, 0, 0, 0) < 0);
  ut::expect(call(22, 0, 0, 0, 0, 0, 0) == static_cast<long>(process::SchedPolicy::Normal));
  ut::expect(call(22, static_cast<long>(fixture.proc->pid()), 0, 0, 0, 0, 0) >= 0);
  ut::expect(call(22, 9999, 0, 0, 0, 0, 0) < 0);
  ut::expect(call(23, static_cast<long>(process::SchedPolicy::RR), 0, 0, 0, 0, 0) ==
             static_cast<long>(process::priority::MAX_RT_PRIORITY));
  ut::expect(call(23, static_cast<long>(process::SchedPolicy::Idle), 0, 0, 0, 0, 0) == 0);
  ut::expect(call(23, static_cast<long>(process::SchedPolicy::Batch), 0, 0, 0, 0, 0) == 0);
  ut::expect(call(24, static_cast<long>(process::SchedPolicy::Fifo), 0, 0, 0, 0, 0) ==
             static_cast<long>(process::priority::MIN_RT_PRIORITY));
  ut::expect(call(24, static_cast<long>(process::SchedPolicy::Idle), 0, 0, 0, 0, 0) == 0);
  ut::expect(call(24, 99, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_nice(0, 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_getpriority(0, 0, 0, 0, 0, 0) == 20);
  ut::expect(sys_getpriority(0, static_cast<long>(fixture.proc->pid()), 0, 0, 0, 0) == 20);
  ut::expect(sys_getpriority(0, 9999, 0, 0, 0, 0) < 0);
  ut::expect(call(9, 0, 0, 0, 0, 0, 0) >= 0);
  ut::expect(call(9, 9999, 0, 0, 0, 0, 0) < 0);
  ut::expect(call(27, 0, 0, 0, 0, 0, 0) >= 0);
  ut::expect(call(27, 9999, 0, 0, 0, 0, 0) < 0);
  ut::expect(call(25, 0, 0, 0, 0, 0, 0) >= 0);
  fixture.proc->set_pgid(9);
  ut::expect(call(8, 0, 0, 0, 0, 0, 0) == static_cast<long>(fixture.proc->pid()));
  ut::expect(call(26, 0, 0, 0, 0, 0, 0) == 0);

  ut::expect(sys_kill(static_cast<long>(fixture.proc->pid()), 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_kill(-1, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_kill(-9999, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_kill(0x100000000LL, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_kill(0, 999, 0, 0, 0, 0) < 0);

  u64 signal_mask = 0;
  ut::expect(put(code + 768, &signal_mask, sizeof(signal_mask)));
  ut::expect(call(16, 2, code + 768, code + 704, 0, 0, 0) == 0);
  ut::expect(get(&signal_mask, code + 704, sizeof(signal_mask)));
  unsigned long signal_action[3] = {0, 0, 0};
  ut::expect(put(code + 832, signal_action, sizeof(signal_action)));
  ut::expect(call(15, 10, code + 832, 0, 0, 0, 0) == 0);
  ut::expect(call(15, 10, 0, code + 896, 0, 0, 0) == 0);
  ut::expect(call(15, 9, 0, 0, 0, 0, 0) < 0);
  ut::expect(call(15, 64, 0, 0, 0, 0, 0) < 0);
  ut::expect(call(15, 10, code + 832, 0, 0, 0, 0) == 0);
  ut::expect(call(15, 10, 0, 0, 0, 0, 0) == 0);

  u64 time_ns = 0;
  ut::expect(sys_clock_gettime(0, code + 960, 0, 0, 0, 0) == 0);
  ut::expect(get(&time_ns, code + 960, sizeof(time_ns)) && time_ns > 0);
  ut::expect(sys_clock_getres(0, code + 960, 0, 0, 0, 0) == 0);
  ut::expect(sys_clock_getres(1, code + 960, 0, 0, 0, 0) == 0);
  ut::expect(sys_clock_getres(2, code + 960, 0, 0, 0, 0) < 0);

  ut::expect(call(number_for("uname"), code + 1024, 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_domain_layout(code + 1280, 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_system_info(code + 1536, sizeof(moss::abi::SystemInfo), 0, 0, 0, 0) == 0);
  ut::expect(sys_system_info(code + 1536, 0, 0, 0, 0, 0) < 0);

  const auto code_handle = sys_code_snapshot(code + 2 * page, 0, 0, 0, 0, 0);
  ut::expect(code_handle >= 0);
  if (code_handle >= 0) {
    ut::expect(sys_code_page_count(code_handle, 0, 0, 0, 0, 0) == 1);
    ut::expect(sys_code_read(code_handle, code + 3 * page, 0, 0, 0, 0) == 0);
    ut::expect(sys_code_read_range(code_handle, 0, code + 3 * page, 1, 0, 0) == 0);
    ut::expect(sys_code_read_range(code_handle, 0, code + 3 * page, 2, 0, 0) < 0);
    ut::expect(sys_code_read_range(code_handle, 1, code + 3 * page, 1, 0, 0) < 0);
    ut::expect(sys_cap_close(code_handle, 0, 0, 0, 0, 0) == 0);
  }

  const auto archive_handle = sys_boot_archive(code + 1792, 0, 0, 0, 0, 0);
  if (archive_handle >= 0) {
    ut::expect(sys_cap_close(archive_handle, 0, 0, 0, 0, 0) == 0);
  }

  ut::expect(sys_domain_status(9999, code + 1856, 0, 0, 0, 0) < 0);
  ut::expect(sys_domain_info(9999, code + 1920, sizeof(moss::abi::DomainInfo), 0, 0, 0) < 0);
  ut::expect(sys_object_wait(0, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_object_wait(code + 2048, 1, 8, 0, 0, 0) < 0);
  ut::expect(sys_wait4(-1, 0, 1, 0, 0, 0) < 0);

  const VirtAddr paths = code + 2 * page;
  ut::expect(put(paths, "/\0", 2));
  ut::expect(put(paths + 64, "/coverage-dir\0", 14));
  ut::expect(put(paths + 128, "/coverage-dir2\0", 15));
  ut::expect(put(paths + 192, "/coverage-file\0", 15));
  ut::expect(put(paths + 256, "\\x01\\x02coverage\\n", 11));
  ut::expect(sys_stat(paths, code + 3 * page, 0, 0, 0, 0) == 0);
  ut::expect(call(38, paths, 0, 0, 0, 0, 0) == 0);
  ut::expect(call(38, paths, 8, 0, 0, 0, 0) < 0);
  ut::expect(call(51, paths, 0, 0, 0, 0, 0) == 0);
  ut::expect(call(52, paths + 320, 64, 0, 0, 0, 0) == 0);

  const long dir_fd = sys_open(paths, vfs::O_RDONLY, 0, 0, 0, 0);
  ut::expect(dir_fd >= 0);
  if (dir_fd >= 0) {
    ut::expect(sys_getdents(dir_fd, code + 3 * page + 128, sizeof(vfs::DirEntry), 0, 0, 0) >= 0);
    ut::expect(sys_fstat(dir_fd, code + 3 * page + 32, 0, 0, 0, 0) == 0);
    ut::expect(sys_lseek(dir_fd, 0, 0, 0, 0, 0) < 0);
    ut::expect(sys_close(dir_fd, 0, 0, 0, 0, 0) == 0);
  }

  ut::expect(sys_mkdir(paths + 64, 0755, 0, 0, 0, 0) == 0);
  ut::expect(sys_rename(paths + 64, paths + 128, 0, 0, 0, 0) == 0);
  ut::expect(sys_rmdir(paths + 128, 0, 0, 0, 0, 0) == 0);

  const long file_fd = sys_open(paths + 192, vfs::O_CREAT | vfs::O_RDWR | vfs::O_TRUNC, 0644, 0, 0, 0);
  ut::expect(file_fd >= 0);
  if (file_fd >= 0) {
    ut::expect(sys_write(file_fd, paths + 256, 11, 0, 0, 0) == 11);
    ut::expect(sys_lseek(file_fd, 0, 0, 0, 0, 0) == 0);
    ut::expect(sys_read(file_fd, code + 3 * page + 192, 11, 0, 0, 0) == 11);
    ut::expect(sys_dup(file_fd, 0, 0, 0, 0, 0) >= 0);
    ut::expect(sys_dup2(file_fd, 20, 0, 0, 0, 0) == 20);
    ut::expect(sys_fcntl(20, 1, 0, 0, 0, 0) >= 0);
    ut::expect(sys_fcntl(20, 2, 1, 0, 0, 0) == 0);
    ut::expect(sys_fcntl(20, 3, 0, 0, 0, 0) >= 0);
    ut::expect(sys_fcntl(20, 99, 0, 0, 0, 0) < 0);
    ut::expect(sys_close(20, 0, 0, 0, 0, 0) == 0);
    ut::expect(sys_close(file_fd, 0, 0, 0, 0, 0) == 0);
    ut::expect(sys_unlink(paths + 192, 0, 0, 0, 0, 0) == 0);
  }

  long pipe_fds[2] = {-1, -1};
  ut::expect(sys_pipe(code + 3 * page + 512, 0, 0, 0, 0, 0) == 0);
  ut::expect(get(pipe_fds, code + 3 * page + 512, sizeof(pipe_fds)));
  if (pipe_fds[0] >= 0 && pipe_fds[1] >= 0) {
    ut::expect(sys_write(pipe_fds[1], paths + 256, 11, 0, 0, 0) == 11);
    ut::expect(sys_read(pipe_fds[0], code + 3 * page + 576, 11, 0, 0, 0) == 11);
    ut::expect(sys_close(pipe_fds[0], 0, 0, 0, 0, 0) == 0);
    ut::expect(sys_close(pipe_fds[1], 0, 0, 0, 0, 0) == 0);
  }

  const long memory_handle = sys_mem_create(page, 0, 0, 0, 0, 0);
  ut::expect(memory_handle > 0);
  if (memory_handle > 0) {
    ut::expect(sys_mem_map(memory_handle, capability::rights::MAP_READ | capability::rights::MAP_WRITE, 0, 0, 0, 0) >
               0);
    ut::expect(sys_mem_map(memory_handle, 0, 0, 0, 0, 0) < 0);
    ut::expect(sys_mem_map(memory_handle, capability::rights::MAP_READ | capability::rights::SEND, 0, 0, 0, 0) < 0);
    const long duplicate =
        sys_cap_duplicate(memory_handle, capability::rights::MAP_READ | capability::rights::DUPLICATE, 0, 0, 0, 0);
    ut::expect(duplicate > 0);
    if (duplicate > 0) {
      ut::expect(sys_cap_set_inherit(duplicate, 1, 0, 0, 0, 0) == 0);
      ut::expect(sys_cap_set_exec(duplicate, 1, 0, 0, 0, 0) == 0);
      ut::expect(sys_cap_set_inherit(duplicate, 2, 0, 0, 0, 0) < 0);
      ut::expect(sys_cap_set_exec(duplicate, 2, 0, 0, 0, 0) < 0);
      ut::expect(sys_cap_close(duplicate, 0, 0, 0, 0, 0) == 0);
    }
    ut::expect(sys_cap_close(memory_handle, 0, 0, 0, 0, 0) == 0);
    ut::expect(sys_cap_close(memory_handle, 0, 0, 0, 0, 0) < 0);
  }

  long endpoints[2] = {-1, -1};
  ut::expect(sys_ipc_create(0, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_ipc_create(code + 3 * page + 640, 0, 0, 0, 0, 0) == 0);
  ut::expect(get(endpoints, code + 3 * page + 640, sizeof(endpoints)));
  if (endpoints[0] >= 0 && endpoints[1] >= 0) {
    const long minted = sys_ipc_mint_badge(endpoints[0], 7, 0, 0, 0, 0);
    ut::expect(minted > 0);
    ut::expect(sys_ipc_mint_badge(endpoints[1], 7, 0, 0, 0, 0) < 0);
    ut::expect(sys_ipc_call(endpoints[1], code, code + 64, -1, 0, 0) < 0);
    ut::expect(sys_ipc_try_receive(endpoints[1], code, code + 64, 0, 0, 0) < 0);
    ut::expect(sys_ipc_reply(invalid, 0, 0, 0, 0, 0) < 0);
    if (minted > 0) {
      ut::expect(sys_cap_close(minted, 0, 0, 0, 0, 0) == 0);
    }
    ut::expect(sys_cap_close(endpoints[0], 0, 0, 0, 0, 0) == 0);
    ut::expect(sys_cap_close(endpoints[1], 0, 0, 0, 0, 0) == 0);
  }

  ut::expect(sys_topinfo(code, 0, 0, 0, 0, 0) == 0);
}

void syscall_edge_paths() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  using namespace moss::kernel::process;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr unmapped = 0xdead0000ULL;
  auto named = [](const char *name, long a0, long a1, long a2, long a3, long a4, long a5) {
    for (long number = 0; number < static_cast<long>(syscall::SyscallNumber::MAX_SYSCALL); ++number) {
      const auto *descriptor = syscall::SyscallDispatcher::get_syscall_info(number);
      if (!descriptor || !descriptor->name) {
        continue;
      }
      const char *left = descriptor->name;
      const char *right = name;
      while (*left && *right && *left == *right) {
        ++left;
        ++right;
      }
      if (*left == '\0' && *right == '\0') {
        return descriptor->handler(a0, a1, a2, a3, a4, a5);
      }
    }
    return -1L;
  };

  // Affinity validation: size, pointer, online mask, missing target, and copyout.
  u32 mask = 0;
  ut::expect(sys_sched_getaffinity(0, 0, code + 512, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(sys_sched_getaffinity(0, sizeof(mask), 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_sched_getaffinity(0, sizeof(mask), unmapped, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_sched_setaffinity(0, 0, code + 512, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(sys_sched_setaffinity(0, sizeof(mask), 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_sched_setaffinity(0, sizeof(mask), unmapped, 0, 0, 0) == -syscall::errc::EFAULT);
  mask = 0;
  ut::expect(fixture.address_space->copy_to_user(code + 512, &mask, sizeof(mask)) == 0);
  ut::expect(sys_sched_setaffinity(0, sizeof(mask), code + 512, 0, 0, 0) == -syscall::errc::EINVAL);
  mask = 0x80000000U;
  ut::expect(fixture.address_space->copy_to_user(code + 512, &mask, sizeof(mask)) == 0);
  ut::expect(sys_sched_setaffinity(0, sizeof(mask), code + 512, 0, 0, 0) == -syscall::errc::EINVAL);
  mask = 1U << arch::get_current_cpu_id();
  ut::expect(fixture.address_space->copy_to_user(code + 512, &mask, sizeof(mask)) == 0);
  ut::expect(sys_sched_setaffinity(0, sizeof(mask), code + 512, 0, 0, 0) == 0);
  ut::expect(sys_sched_getaffinity(0, sizeof(mask), code + 640, 0, 0, 0) == 0);

  auto child_result = fixture.manager.create_process(fixture.proc->pid());
  if (!ut::expect(static_cast<bool>(child_result))) {
    return;
  }
  auto child = *child_result;
  fixture.proc->add_child(child->pid());
  // Registered process without a main thread is not a schedulable target.
  ut::expect(sys_sched_getaffinity(static_cast<long>(child->pid()), sizeof(mask), code + 640, 0, 0, 0) ==
             -syscall::errc::ESRCH);
  ut::expect(sys_sched_setaffinity(static_cast<long>(child->pid()), sizeof(mask), code + 512, 0, 0, 0) ==
             -syscall::errc::ESRCH);
  ut::expect(sys_getpriority(0, static_cast<long>(child->pid()), 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_getpriority(5, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(named("sched_getscheduler", static_cast<long>(child->pid()), 0, 0, 0, 0, 0) ==
             -syscall::errc::ESRCH);

  auto orphan_result = fixture.manager.create_process(INVALID_PROCESS_ID);
  if (!ut::expect(static_cast<bool>(orphan_result))) {
    return;
  }
  auto orphan = *orphan_result;
  if (!ut::expect(orphan->pid() > 1)) {
    return;
  }
  ut::expect(sys_sched_setaffinity(static_cast<long>(orphan->pid()), sizeof(mask), code + 512, 0, 0, 0) ==
             -syscall::errc::EPERM);
  ut::expect(sys_kill(static_cast<long>(orphan->pid()), 0, 0, 0, 0, 0) == -syscall::errc::EPERM);

  // Degraded scheduler/manager observations that the normal path never sees.
  auto *saved_scheduler = process::g_scheduler;
  process::g_scheduler = nullptr;
  mask = 1U << arch::get_current_cpu_id();
  ut::expect(fixture.address_space->copy_to_user(code + 512, &mask, sizeof(mask)) == 0);
  ut::expect(sys_sched_setaffinity(0, sizeof(mask), code + 512, 0, 0, 0) == 0);
  if (g_num_cpus > 1) {
    mask = 1U << ((arch::get_current_cpu_id() + 1) % g_num_cpus);
    ut::expect(fixture.address_space->copy_to_user(code + 512, &mask, sizeof(mask)) == 0);
    ut::expect(sys_sched_setaffinity(0, sizeof(mask), code + 512, 0, 0, 0) == -syscall::errc::EAGAIN);
  }
  process::g_scheduler = saved_scheduler;
  auto *saved_manager = process::g_process_manager;
  process::g_process_manager = nullptr;
  ut::expect(sys_sched_getaffinity(9999, sizeof(mask), code + 640, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_sched_setaffinity(9999, sizeof(mask), code + 512, 0, 0, 0) == -syscall::errc::EFAULT);
  process::g_process_manager = saved_manager;

  const char debug_text[] = "moss-debug\n";
  ut::expect(fixture.address_space->copy_to_user(code + 3 * page + 768, debug_text, sizeof(debug_text)) == 0);
  ut::expect(sys_debug_print(code + 3 * page + 768, 0, 0, 0, 0, 0) == 0);

  // Process-group admission: foreign session, non-child, missing target, and
  // a repeated assignment that must be a no-op.
  fixture.proc->set_sid(0);
  fixture.proc->set_pgid(9);
  ut::expect(named("setpgrp", 0, 0, 0, 0, 0, 0) == 0);
  ut::expect(named("getpgrp", 0, 0, 0, 0, 0, 0) == static_cast<long>(fixture.proc->pid()));
  fixture.proc->set_pgid(fixture.proc->pid());
  ut::expect(named("setsid", 0, 0, 0, 0, 0, 0) == -syscall::errc::EPERM);
  fixture.proc->set_pgid(9);
  ut::expect(named("setsid", 0, 0, 0, 0, 0, 0) == static_cast<long>(fixture.proc->pid()));
  ut::expect(named("getpgrp", 0, 0, 0, 0, 0, 0) == static_cast<long>(fixture.proc->pid()));
  ut::expect(named("getsid", 0, 0, 0, 0, 0, 0) == static_cast<long>(fixture.proc->pid()));
  ut::expect(named("getpgid", 9999, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(named("getsid", 9999, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);

  // Group and broadcast signals use existence probes only.
  child->set_pgid(fixture.proc->pgid());
  auto *child_thread = new Thread(200, child->pid());
  if (ut::expect(child_thread != nullptr && child->register_thread(child_thread).has_value())) {
    child_thread->state = ProcessState::Ready;
    ut::expect(sys_kill(0, 0, 0, 0, 0, 0) == 0);
    ut::expect(sys_kill(-static_cast<long>(fixture.proc->pgid()), 0, 0, 0, 0, 0) == 0);
    ut::expect(sys_kill(-9999, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
    ut::expect(sys_kill(-1, 0, 0, 0, 0, 0) == 0);
    child_thread->state = ProcessState::Running;
    ut::expect(sys_kill(static_cast<long>(fixture.proc->pid()), sig::SIGCONT, 0, 0, 0, 0) == 0);
  }

 // Signal and alternate-stack validation.
  u64 signal_mask = ~0ULL;
  ut::expect(named("sigprocmask", 0, code + 512, unmapped, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(named("sigprocmask", 0, unmapped, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(fixture.address_space->copy_to_user(code + 512, &signal_mask, sizeof(signal_mask)) == 0);
  ut::expect(named("sigprocmask", 9, code + 512, 0, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(named("sigprocmask", 2, code + 512, code + 640, 0, 0, 0) == 0);

  struct UserStackCompat {
    unsigned long sp;
    unsigned long size;
    unsigned long flags;
  };
  UserStackCompat stack{};
  stack.sp = code + 1024;
  stack.size = page;
  stack.flags = 4;
  ut::expect(fixture.address_space->copy_to_user(code + 704, &stack, sizeof(stack)) == 0);
  ut::expect(named("sigaltstack", code + 704, unmapped, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(named("sigaltstack", code + 704, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);
  stack.flags = 0;
  stack.size = 1024;
  ut::expect(fixture.address_space->copy_to_user(code + 704, &stack, sizeof(stack)) == 0);
  ut::expect(named("sigaltstack", code + 704, 0, 0, 0, 0, 0) == -syscall::errc::ENOMEM);
  stack.sp = unmapped;
  stack.size = page;
  ut::expect(fixture.address_space->copy_to_user(code + 704, &stack, sizeof(stack)) == 0);
  ut::expect(named("sigaltstack", code + 704, 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  stack.flags = 2;
  ut::expect(fixture.address_space->copy_to_user(code + 704, &stack, sizeof(stack)) == 0);
  ut::expect(named("sigaltstack", code + 704, 0, 0, 0, 0, 0) == 0);
  fixture.proc->get_main_thread()->on_alt_stack = true;
  ut::expect(named("sigaltstack", code + 704, 0, 0, 0, 0, 0) == -syscall::errc::EPERM);
  fixture.proc->get_main_thread()->on_alt_stack = false;

  unsigned long action[3] = {0, 0, 0x100};
  ut::expect(fixture.address_space->copy_to_user(code + 832, action, sizeof(action)) == 0);
  ut::expect(named("sigaction", 10, code + 832, 0, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(named("sigaction", 0, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(named("sigaction", 10, unmapped, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(named("sigaction", 10, 0, unmapped, 0, 0, 0) == -syscall::errc::EFAULT);

  // User-copy failures on file operations and memory syscalls.
  ut::expect(sys_open(1, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_read(0, unmapped, 1, 0, 0, 0) < 0);
  const long dir_fd = sys_open(code + 64, vfs::O_RDONLY, 0, 0, 0, 0);
  ut::expect(dir_fd < 0);
  ut::expect(fixture.address_space->copy_from_user(nullptr, code + 64, 0) == 0);
  ut::expect(sys_stat(unmapped, code + 3 * page, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_mkdir(unmapped, 0755, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_mkdir(code + 64, 512L, 0, 0, 0, 0) == -syscall::errc::EINVAL);

  // Memory range validation and a complete VMA removal.
  ut::expect(sys_mmap(0, 0x7fffffffffffffffL, 0x3, 0x22, -1, 0) < 0);
  ut::expect(sys_mmap(0, page, 0x7, 0x22, -1, 0) == -syscall::errc::EACCES);
  const auto hint_address = fixture.address_space->mmap_next;
  ut::expect(sys_mmap(static_cast<long>(code), page, 0x3, 0x22, -1, 0) == static_cast<long>(hint_address));
  ut::expect(sys_munmap(static_cast<long>(code + page), page, 0, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(sys_munmap(static_cast<long>(unmapped), page, 0, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(sys_munmap(static_cast<long>(code), static_cast<long>(1ULL << 40), 0, 0, 0, 0) ==
             -syscall::errc::EINVAL);
  // Conflicting heap range must not move brk.
  auto conflict = fixture.address_space->add_vma(user_layout::HEAP_START + page, user_layout::HEAP_START + 2 * page,
                                                  vma_flags::READ | vma_flags::WRITE, VmaType::MMAP);
  ut::expect(conflict);
  const auto before_brk = fixture.address_space->brk_current;
  ut::expect(sys_brk(static_cast<long>(user_layout::HEAP_START + 2 * page), 0, 0, 0, 0, 0) ==
             static_cast<long>(before_brk));
  fixture.address_space->remove_vma(user_layout::HEAP_START + page, user_layout::HEAP_START + 2 * page);

  // Object-wait validation without sleeping: receiver readiness, aliased
  // queues, non-waitable objects, nonblocking and expired deadlines.
  long endpoints[2] = {-1, -1};
  ut::expect(sys_ipc_create(code + 3 * page + 640, 0, 0, 0, 0, 0) == 0);
  if (ut::expect(fixture.address_space->copy_from_user(endpoints, code + 3 * page + 640, sizeof(endpoints)) == 0)) {
    long handles[2] = {endpoints[1], endpoints[1]};
    ut::expect(fixture.address_space->copy_to_user(code + 3 * page + 704, handles, sizeof(handles)) == 0);
    ut::expect(sys_object_wait(code + 3 * page + 704, 1, 1, 0, 0, 0) == -syscall::errc::EAGAIN);
    ut::expect(sys_object_wait(code + 3 * page + 704, 1, 0, 1, 0, 0) == -syscall::errc::ETIMEDOUT);
    ut::expect(sys_object_wait(code + 3 * page + 704, 2, 1, 0, 0, 0) == -syscall::errc::EINVAL);
    handles[0] = endpoints[0];
    ut::expect(fixture.address_space->copy_to_user(code + 3 * page + 704, handles, sizeof(handles)) == 0);
    ut::expect(sys_object_wait(code + 3 * page + 704, 1, 1, 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_object_wait(0, 1, 0, 0, 0, 0) == -syscall::errc::EFAULT);
    ut::expect(sys_object_wait(code + 3 * page + 704, 1, 2, 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_object_wait(code + 3 * page + 704, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_object_wait(code + 3 * page + 704, 1, 0, -1, 0, 0) == -syscall::errc::EINVAL);
    handles[0] = 9999;
    ut::expect(fixture.address_space->copy_to_user(code + 3 * page + 704, handles, sizeof(handles)) == 0);
    ut::expect(sys_object_wait(code + 3 * page + 704, 1, 1, 0, 0, 0) < 0);
    ut::expect(sys_cap_close(endpoints[0], 0, 0, 0, 0, 0) == 0);
    ut::expect(sys_cap_close(endpoints[1], 0, 0, 0, 0, 0) == 0);
  }

  // Domain self/status paths reachable without starting another domain.
  const long self = sys_domain_self(0, 0, 0, 0, 0, 0);
  ut::expect(self > 0);
  if (self > 0) {
    ut::expect(sys_domain_status(self, 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);
    ut::expect(sys_domain_status(self, code + 3 * page + 768, 0, 0, 0, 0) == -syscall::errc::EAGAIN);
    ut::expect(sys_domain_info(self, unmapped, sizeof(moss::abi::DomainInfo), 0, 0, 0) == -syscall::errc::EFAULT);
    ut::expect(sys_domain_wait(self, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);
    const long duplicate = sys_cap_duplicate(self, capability::rights::DOMAIN_OBSERVE, 0, 0, 0, 0);
    if (duplicate > 0) {
      Handle pair[2] = {static_cast<Handle>(self), static_cast<Handle>(duplicate)};
      ut::expect(fixture.address_space->copy_to_user(code + 3 * page + 832, pair, sizeof(pair)) == 0);
      ut::expect(sys_domain_wait_any(code + 3 * page + 832, 1, 0, 0, 0, 0) == -syscall::errc::EINVAL);
      ut::expect(sys_domain_wait_any(code + 3 * page + 832, 2, 0, 0, 0, 0) == -syscall::errc::EINVAL);
      ut::expect(sys_cap_close(duplicate, 0, 0, 0, 0, 0) == 0);
    }
    ut::expect(sys_domain_wait_any(0, 1, 0, 0, 0, 0) == -syscall::errc::EFAULT);
    ut::expect(sys_domain_wait_any(code + 3 * page + 832, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_domain_wait_any(code + 3 * page + 832, static_cast<long>(capability::Table::capacity() + 1), 0,
                                   0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_domain_info(self, code + 3 * page + 896, sizeof(moss::abi::DomainInfo), 0, 0, 0) == 0);
    ut::expect(sys_cap_close(self, 0, 0, 0, 0, 0) == 0);
  }

  // Remove the fixture code VMA last; earlier user-copy probes need it.
  ut::expect(sys_munmap(static_cast<long>(code), 4 * page, 0, 0, 0, 0) == 0);
}

void ipc_edge_paths() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr unmapped = 0xdead0000ULL;
  auto put = [&](VirtAddr destination, const void *source, usize size) {
    return fixture.address_space->copy_to_user(destination, source, size) == 0;
  };

  // Capability and memory admission before any IPC transfer.
  ut::expect(sys_mem_create(0, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(sys_mem_create(page + 1, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);
  const long memory = sys_mem_create(page, 0, 0, 0, 0, 0);
  ut::expect(memory > 0);
  if (memory > 0) {
    ut::expect(sys_mem_map(memory, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_mem_map(memory, ~0L, 0, 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_mem_map(memory, capability::rights::MAP_READ, 0, 0, 0, 0) > 0);
    ut::expect(sys_mem_map(memory, capability::rights::MAP_WRITE, 0, 0, 0, 0) > 0);
    ut::expect(sys_mem_map(9999, capability::rights::MAP_READ, 0, 0, 0, 0) == -syscall::errc::EBADF);
    ut::expect(sys_cap_duplicate(memory, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_cap_duplicate(memory, ~0L, 0, 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_cap_set_inherit(memory, 2, 0, 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_cap_set_exec(memory, 2, 0, 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_cap_duplicate(9999, capability::rights::MAP_READ, 0, 0, 0, 0) == -syscall::errc::EBADF);
    ut::expect(sys_cap_close(9999, 0, 0, 0, 0, 0) == -syscall::errc::EBADF);
  }

  long endpoints[2] = {-1, -1};
  constexpr VirtAddr handles_addr = code + 3 * page + 768;
  ut::expect(sys_ipc_create(0, 0, 0, 0, 0, 0) < 0);
  ut::expect(sys_ipc_create(handles_addr, 0, 0, 0, 0, 0) == 0);
  if (!ut::expect(fixture.address_space->copy_from_user(endpoints, handles_addr, sizeof(endpoints)) == 0)) {
    return;
  }
  if (endpoints[0] < 0 || endpoints[1] < 0) {
    return;
  }
  ut::expect(sys_ipc_mint_badge(endpoints[1], 5, 0, 0, 0, 0) < 0);
  const long minted = sys_ipc_mint_badge(endpoints[0], 5, 0, 0, 0, 0);
  ut::expect(minted > 0);
  const long attenuated =
      sys_cap_duplicate(endpoints[0], capability::rights::SEND | capability::rights::TRANSFER |
                                       capability::rights::DUPLICATE, 0, 0, 0, 0);
  ut::expect(attenuated > 0);
  if (attenuated > 0) {
    ut::expect(sys_ipc_mint_badge(attenuated, 6, 0, 0, 0, 0) == -syscall::errc::EACCES);
    ut::expect(sys_cap_close(attenuated, 0, 0, 0, 0, 0) == 0);
  }

  struct ControlMessageCompat {
    u64 size;
    long capability;
    u64 rights;
    u64 badge;
    u8 payload[256];
  };
  constexpr VirtAddr request_addr = code + 3 * page + 832;
  constexpr VirtAddr response_addr = code + 3 * page + 1152;
  ControlMessageCompat message{};
  message.size = 257;
  ut::expect(put(request_addr, &message, sizeof(message)));
  ut::expect(sys_ipc_call(endpoints[0], request_addr, response_addr, 0, 0, 0) == -syscall::errc::EINVAL);
  message.size = 0;
  message.badge = 1;
  ut::expect(put(request_addr, &message, sizeof(message)));
  ut::expect(sys_ipc_call(endpoints[0], request_addr, response_addr, 0, 0, 0) == -syscall::errc::EINVAL);
  message.badge = 0;
  message.capability = memory;
  message.rights = 0;
  ut::expect(put(request_addr, &message, sizeof(message)));
  ut::expect(sys_ipc_call(endpoints[0], request_addr, response_addr, 0, 0, 0) == -syscall::errc::EINVAL);
  message.rights = 1ULL << 40;
  ut::expect(put(request_addr, &message, sizeof(message)));
  ut::expect(sys_ipc_call(endpoints[0], request_addr, response_addr, 0, 0, 0) == -syscall::errc::EINVAL);
  message.rights = capability::rights::MAP_READ | capability::rights::TRANSFER;
  message.size = 1;
  ut::expect(put(request_addr, &message, sizeof(message)));
  ut::expect(sys_ipc_call(endpoints[0], request_addr, response_addr, 1, 0, 0) == -syscall::errc::ETIMEDOUT);
  ut::expect(sys_ipc_call(endpoints[0], unmapped, response_addr, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_ipc_call(endpoints[0], request_addr, response_addr, -1, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(sys_ipc_call(endpoints[1], request_addr, response_addr, 0, 0, 0) < 0);
  ut::expect(sys_ipc_call(9999, request_addr, response_addr, 0, 0, 0) == -syscall::errc::EBADF);

  // Receive and reply type/admission checks.
  ut::expect(sys_ipc_receive(endpoints[0], code + 128, code + 192, 0, 0, 0) < 0);
  ut::expect(sys_ipc_try_receive(endpoints[1], code + 128, code + 192, 0, 0, 0) == -syscall::errc::EAGAIN);
  ut::expect(sys_ipc_receive(9999, code + 128, code + 192, 0, 0, 0) == -syscall::errc::EBADF);
  ut::expect(sys_ipc_try_receive(9999, code + 128, code + 192, 0, 0, 0) == -syscall::errc::EBADF);
  ut::expect(sys_ipc_reply(9999, code + 128, 0, 0, 0, 0) == -syscall::errc::EBADF);
  ut::expect(sys_ipc_reply(minted, code + 128, 0, 0, 0, 0) < 0);

  if (minted > 0) {
    ut::expect(sys_cap_close(minted, 0, 0, 0, 0, 0) == 0);
  }
  if (memory > 0) {
    ut::expect(sys_cap_close(memory, 0, 0, 0, 0, 0) == 0);
  }
  ut::expect(sys_cap_close(endpoints[0], 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_cap_close(endpoints[1], 0, 0, 0, 0, 0) == 0);

  // The archive path is gated on initial-supervisor authority; exercise the
  // full copy after the generic capability checks above.
  fixture.proc->designate_initial_supervisor();
  ut::expect(sys_boot_archive(unmapped, 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  const long archive = sys_boot_archive(code + 3 * page + 1216, 0, 0, 0, 0, 0);
  ut::expect(archive > 0);
  if (archive > 0) {
    ut::expect(sys_cap_close(archive, 0, 0, 0, 0, 0) == 0);
  }
}

void domain_spawn_edges() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr image_addr = code + 3 * page + 128;
  constexpr VirtAddr page_addr = code + 3 * page + 256;
  auto put = [&](VirtAddr destination, const void *source, usize size) {
    return fixture.address_space->copy_to_user(destination, source, size) == 0;
  };

  fixture.proc->designate_domain_factory_source();
  const long factory = sys_domain_factory(0, 0, 0, 0, 0, 0);
  if (!ut::expect(factory > 0)) {
    return;
  }
  moss_domain_spawn image{};
  moss_domain_page program_page{};
  program_page.address = code;
  program_page.source = code;
  program_page.size = 16;
  program_page.flags = MOSS_DOMAIN_PAGE_READ;
  ut::expect(put(page_addr, &program_page, sizeof(program_page)));
  image.entry = code;
  image.stack_pointer = process::user_layout::STACK_TOP - 16;
  image.pages = page_addr;
  image.page_count = 1;
  ut::expect(put(image_addr, &image, sizeof(image)));

  ut::expect(sys_domain_spawn(0, image_addr, 0, 0, 0, 0) < 0);
  ut::expect(sys_domain_spawn(factory, 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_domain_spawn(factory, 0xdead0000ULL, 0, 0, 0, 0) == -syscall::errc::EFAULT);

  auto try_image = [&](const moss_domain_spawn &candidate) {
    ut::expect(put(image_addr, &candidate, sizeof(candidate)));
    return sys_domain_spawn(factory, image_addr, 0, 0, 0, 0);
  };
  auto with_field = [&](auto change) {
    auto candidate = image;
    change(candidate);
    return try_image(candidate);
  };

  ut::expect(with_field([](moss_domain_spawn &value) { value.pages = 0; }) == -syscall::errc::EINVAL);
  ut::expect(with_field([](moss_domain_spawn &value) { value.page_count = 0; }) == -syscall::errc::EINVAL);
  ut::expect(with_field([](moss_domain_spawn &value) { value.page_count = MOSS_DOMAIN_MAX_IMAGE_PAGES + 1; }) ==
             -syscall::errc::EINVAL);
  ut::expect(with_field([](moss_domain_spawn &value) { value.capability_count = 65; }) == -syscall::errc::EINVAL);
  ut::expect(with_field([](moss_domain_spawn &value) {
    value.capability_count = 1;
    value.capabilities = 0;
  }) == -syscall::errc::EINVAL);
  ut::expect(with_field([](moss_domain_spawn &value) { value.stack_size = process::user_layout::STACK_SIZE + 1; }) ==
             -syscall::errc::EINVAL);
  ut::expect(with_field([](moss_domain_spawn &value) { value.stack_size = 1; }) == -syscall::errc::EINVAL);
  ut::expect(with_field([](moss_domain_spawn &value) { value.stack_pointer = process::user_layout::STACK_TOP; }) ==
             -syscall::errc::EINVAL);
  ut::expect(with_field([](moss_domain_spawn &value) {
    value.stack_pointer = process::user_layout::STACK_TOP - 15;
  }) == -syscall::errc::EINVAL);
  ut::expect(with_field([](moss_domain_spawn &value) {
    value.stack_pointer = process::user_layout::STACK_TOP - 16;
    value.stack_size = 17;
  }) == -syscall::errc::EINVAL);
  ut::expect(with_field([](moss_domain_spawn &value) {
    value.pages = ~u64{0};
    value.page_count = 2;
  }) == -syscall::errc::EFAULT);
  ut::expect(with_field([](moss_domain_spawn &value) {
    value.capabilities = ~u64{0};
    value.capability_count = 2;
  }) == -syscall::errc::EFAULT);
  ut::expect(with_field([](moss_domain_spawn &value) {
    value.stack_source = ~u64{0};
    value.stack_size = 2;
  }) == -syscall::errc::EFAULT);
  ut::expect(with_field([](moss_domain_spawn &value) { value.pages = 0xdead0000ULL; }) ==
             -syscall::errc::EFAULT);
  ut::expect(with_field([](moss_domain_spawn &value) {
    value.capability_count = 1;
    value.capabilities = 0xdead0000ULL;
  }) == -syscall::errc::EFAULT);
  ut::expect(with_field([](moss_domain_spawn &value) { value.entry = 0xdead0000ULL; }) ==
             -syscall::errc::EINVAL);

  auto try_page = [&](const moss_domain_page &candidate) {
    ut::expect(put(page_addr, &candidate, sizeof(candidate)));
    return try_image(image);
  };
  auto with_page = [&](auto change) {
    auto candidate = program_page;
    change(candidate);
    return try_page(candidate);
  };
  ut::expect(with_page([](moss_domain_page &value) { value.address = code + 1; }) == -syscall::errc::EINVAL);
  ut::expect(with_page([](moss_domain_page &value) { value.address = 0; }) == -syscall::errc::EINVAL);
  ut::expect(with_page([](moss_domain_page &value) { value.flags = 0; }) == -syscall::errc::EINVAL);
  ut::expect(with_page([](moss_domain_page &value) {
    value.flags = MOSS_DOMAIN_PAGE_READ | MOSS_DOMAIN_PAGE_WRITE | MOSS_DOMAIN_PAGE_EXEC;
  }) == -syscall::errc::EINVAL);
  ut::expect(with_page([](moss_domain_page &value) { value.flags = 0x8; }) == -syscall::errc::EINVAL);
  ut::expect(with_page([](moss_domain_page &value) { value.size = page + 1; }) == -syscall::errc::EINVAL);
  ut::expect(with_page([](moss_domain_page &value) {
    value.source = ~u64{0};
    value.size = 2;
  }) == -syscall::errc::EINVAL);
  ut::expect(with_page([](moss_domain_page &value) { value.code = 5; }) == -syscall::errc::EINVAL);
  ut::expect(with_page([](moss_domain_page &value) {
    value.flags = MOSS_DOMAIN_PAGE_READ | MOSS_DOMAIN_PAGE_EXEC;
  }) == -syscall::errc::EINVAL);
  ut::expect(with_page([](moss_domain_page &value) {
    value.flags = MOSS_DOMAIN_PAGE_READ | MOSS_DOMAIN_PAGE_EXEC;
    value.source = 0;
    value.size = 0;
  }) == -syscall::errc::EINVAL);
  ut::expect(with_page([](moss_domain_page &value) { value.address = process::user_layout::SIGRETURN_PAGE; }) ==
             -syscall::errc::EINVAL);
  ut::expect(with_page([](moss_domain_page &value) { value.address = process::user_layout::HEAP_START; }) ==
             -syscall::errc::EINVAL);
  program_page.source = 0;
  program_page.size = 0;

  // An allocation failure while constructing the new address space must fail
  // closed before any child process or handle is published.
  {
    PagePressure pressure;
    if (ut::expect(pressure.acquire(0))) {
      ut::expect(with_field([](moss_domain_spawn &) {}) == -syscall::errc::ENOMEM);
    }
  }
  ut::expect(sys_cap_close(factory, 0, 0, 0, 0, 0) == 0);
}
void ipc_cancel_paths() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr handles_addr = code + 3 * page + 768;
  constexpr VirtAddr request_addr = code + 3 * page + 832;
  constexpr VirtAddr response_addr = code + 3 * page + 1152;
  auto put = [&](VirtAddr destination, const void *source, usize size) {
    return fixture.address_space->copy_to_user(destination, source, size) == 0;
  };

  long endpoints[2] = {-1, -1};
  if (!ut::expect(sys_ipc_create(handles_addr, 0, 0, 0, 0, 0) == 0)) {
    return;
  }
  if (!ut::expect(fixture.address_space->copy_from_user(endpoints, handles_addr, sizeof(endpoints)) == 0)) {
    return;
  }
  if (endpoints[0] < 0 || endpoints[1] < 0) {
    return;
  }
  struct ControlMessageCompat {
    u64 size;
    long capability;
    u64 rights;
    u64 badge;
    u8 payload[256];
  };
  ControlMessageCompat message{};
  message.size = 1;
  message.payload[0] = 7;
  ut::expect(put(request_addr, &message, sizeof(message)));

  auto *current = process::CfsScheduler::get_current_task();
  if (!ut::expect(current != nullptr)) {
    return;
  }
  current->pending_signals.fetch_or(process::sig::sigmask(process::sig::SIGKILL));
  ut::expect(sys_ipc_call(endpoints[0], request_addr, response_addr, 0, 0, 0) == -syscall::errc::EINTR);
  current->pending_signals.store(0);
  ut::expect(sys_ipc_call(endpoints[0], request_addr, response_addr, 1, 0, 0) == -syscall::errc::ETIMEDOUT);
  ut::expect(sys_ipc_try_receive(endpoints[1], request_addr, response_addr, 0, 0, 0) == -syscall::errc::EAGAIN);

  // Closing the last receiver closes the channel; a later send must fail
  // before publication and leave no descriptor or escrow behind.
  ut::expect(sys_cap_close(endpoints[1], 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_ipc_call(endpoints[0], request_addr, response_addr, 0, 0, 0) == -syscall::errc::EPIPE);
  ut::expect(sys_cap_close(endpoints[0], 0, 0, 0, 0, 0) == 0);
}
void code_capability_paths() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  fixture.proc->designate_domain_factory_source();

  ut::expect(sys_code_snapshot(0, 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_code_snapshot_range(code, -1, 0, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(sys_code_snapshot_range(code, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(sys_code_snapshot_range(code, MOSS_DOMAIN_MAX_IMAGE_PAGES + 1, 0, 0, 0, 0) == -syscall::errc::EINVAL);

  const long handle = sys_code_snapshot(code, 0, 0, 0, 0, 0);
  if (handle > 0) {
    ut::expect(sys_cap_close(handle, 0, 0, 0, 0, 0) == 0);
  }

  const long version = sys_code_snapshot(code, 0, 0, 0, 0, 0);
  if (!ut::expect(version > 0)) {
    return;
  }
  ut::expect(sys_code_page_count(version, 0, 0, 0, 0, 0) == 1);
  ut::expect(sys_code_read_range(version, 0, 0, 1, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_code_read_range(version, 0, 0xdead0000ULL, 1, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_code_read_range(version, 2, code + page, 1, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(sys_code_read_range(9999, 0, code + page, 1, 0, 0) == -syscall::errc::EBADF);
  ut::expect(sys_code_read(version, code + page, 0, 0, 0, 0) == 0);

  const long authority = sys_code_authority(0, 0, 0, 0, 0, 0);
  if (!ut::expect(authority > 0)) {
    (void)sys_cap_close(version, 0, 0, 0, 0, 0);
    return;
  }
  const long extra = sys_code_authority(0, 0, 0, 0, 0, 0);
  if (extra > 0) {
    ut::expect(sys_cap_close(extra, 0, 0, 0, 0, 0) == 0);
  } else {
    ut::expect(extra == -syscall::errc::ENOMEM);
  }
  ut::expect(sys_code_approve(version, authority, 0, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(sys_code_approve(9999, version, 0, 0, 0, 0) == -syscall::errc::EBADF);
  ut::expect(sys_code_approve(authority, 9999, 0, 0, 0, 0) == -syscall::errc::EBADF);
  const long approval = sys_code_approve(authority, version, 0, 0, 0, 0);
  if (ut::expect(approval > 0)) {
    const long second = sys_code_authority(0, 0, 0, 0, 0, 0);
    if (ut::expect(second > 0)) {
      ut::expect(sys_code_revoke(second, approval, 0, 0, 0, 0) == -syscall::errc::EACCES);
      ut::expect(sys_code_revoke(authority, second, 0, 0, 0, 0) == -syscall::errc::EINVAL);
      ut::expect(sys_cap_close(second, 0, 0, 0, 0, 0) == 0);
    }
    ut::expect(sys_code_revoke(9999, approval, 0, 0, 0, 0) == -syscall::errc::EBADF);
    ut::expect(sys_code_revoke(authority, 9999, 0, 0, 0, 0) == -syscall::errc::EBADF);
    ut::expect(sys_code_revoke(authority, approval, 0, 0, 0, 0) == 0);
    ut::expect(sys_cap_close(approval, 0, 0, 0, 0, 0) == 0);
  }
  const long self = sys_domain_self(0, 0, 0, 0, 0, 0);
  if (ut::expect(self > 0)) {
    ut::expect(sys_code_page_count(self, 0, 0, 0, 0, 0) == -syscall::errc::EACCES);
    ut::expect(sys_code_read(self, code + page, 0, 0, 0, 0) == -syscall::errc::EACCES);
    ut::expect(sys_cap_close(self, 0, 0, 0, 0, 0) == 0);
  }
  ut::expect(sys_cap_close(authority, 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_cap_close(version, 0, 0, 0, 0, 0) == 0);
}

void domain_spawn_allocation_matrix() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr image_addr = code + 4 * page - 256;
  constexpr VirtAddr page_addr = code + 4 * page - 512;
  fixture.proc->designate_domain_factory_source();
#if defined(MOSS_ARCH_ARM64)
  const u32 park = 0x14000000U; // b .
#elif defined(MOSS_ARCH_RISCV64)
  const u32 park = 0x0000006FU; // jal x0, 0
#elif defined(MOSS_ARCH_X64)
  const u16 park = 0xFEEBU; // jmp $
#endif
  if (!ut::expect(fixture.address_space->copy_to_user(code, &park, sizeof(park)) == 0)) {
    return;
  }

  const long version = sys_code_snapshot(code, 0, 0, 0, 0, 0);
  const long authority = version > 0 ? sys_code_authority(0, 0, 0, 0, 0, 0) : -1;
  const long approval = authority > 0 ? sys_code_approve(authority, version, 0, 0, 0, 0) : -1;
  const long factory = sys_domain_factory(0, 0, 0, 0, 0, 0);
  if (!ut::expect(version > 0 && authority > 0 && approval > 0 && factory > 0)) {
    return;
  }

  moss_domain_spawn image{};
  moss_domain_page program{};
  program.address = code;
  program.flags = MOSS_DOMAIN_PAGE_READ | MOSS_DOMAIN_PAGE_EXEC;
  program.code = static_cast<u64>(approval);
  program.code_page_index = 0;
  image.entry = code;
  image.stack_pointer = process::user_layout::STACK_TOP - 16;
  image.pages = page_addr;
  image.page_count = 1;
  if (!ut::expect(fixture.address_space->copy_to_user(page_addr, &program, sizeof(program)) == 0)) {
    return;
  }
  if (!ut::expect(fixture.address_space->copy_to_user(image_addr, &image, sizeof(image)) == 0)) {
    return;
  }

  auto attempt = [&]() { return sys_domain_spawn(factory, image_addr, 0, 0, 0, 0); };
  auto collect = [&](long handle) {
    if (handle > 0) {
      (void)sys_domain_terminate(handle, 0, 0, 0, 0, 0);
      (void)sys_domain_wait(handle, 0, 0, 0, 0, 0);
      ut::expect(sys_cap_close(handle, 0, 0, 0, 0, 0) == 0);
    } else {
      ut::expect(handle == -syscall::errc::ENOMEM || handle == -syscall::errc::EAGAIN ||
                 handle == -syscall::errc::EACCES || handle == -syscall::errc::EFAULT ||
                 handle == -syscall::errc::EMFILE || handle == -syscall::errc::EINVAL);
    }
  };
  const long handle = attempt();
  collect(handle);

  long held[capability::Table::capacity()]{};
  usize held_count = 0;
  for (; held_count < capability::Table::capacity(); ++held_count) {
    const long minted = sys_mem_create(page, 0, 0, 0, 0, 0);
    if (minted <= 0) {
      break;
    }
    held[held_count] = minted;
  }
  ut::expect(attempt() == -syscall::errc::EMFILE);
  while (held_count) {
    --held_count;
    ut::expect(sys_cap_close(held[held_count], 0, 0, 0, 0, 0) == 0);
  }
  ut::expect(sys_cap_close(factory, 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_cap_close(approval, 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_cap_close(authority, 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_cap_close(version, 0, 0, 0, 0, 0) == 0);
}

void failure_paths() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr unmapped = 0xdead0000ULL;
  auto *saved_current = process::CfsScheduler::get_current_task();
  auto *saved_manager = process::g_process_manager;
  auto *saved_scheduler = process::g_scheduler;
  auto call = [](const char *name, long a0, long a1, long a2, long a3, long a4, long a5) {
    for (long number = 0; number < static_cast<long>(syscall::SyscallNumber::MAX_SYSCALL); ++number) {
      const auto *descriptor = syscall::SyscallDispatcher::get_syscall_info(number);
      if (!descriptor || !descriptor->name) {
        continue;
      }
      const char *left = descriptor->name;
      const char *right = name;
      while (*left && *right && *left == *right) {
        ++left;
        ++right;
      }
      if (*left == '\0' && *right == '\0') {
        return descriptor->handler(a0, a1, a2, a3, a4, a5);
      }
    }
    return -1L;
  };

  process::CfsScheduler::set_current_task(nullptr);
  ut::expect(sys_mmap(0, page, 1, 0x22, -1, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_munmap(static_cast<long>(code), page, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_brk(0, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_getpriority(0, 9999, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(call("sched_getscheduler", 9999, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);

  process::Thread stale(9100, 9100);
  stale.state = process::ProcessState::Running;
  process::CfsScheduler::set_current_task(&stale);
  ut::expect(sys_mmap(0, page, 1, 0x22, -1, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_munmap(static_cast<long>(code), page, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_brk(0, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);

  auto bare_result = fixture.manager.create_process();
  if (ut::expect(static_cast<bool>(bare_result))) {
    auto bare = *bare_result;
    process::Thread bare_thread(9101, bare->pid());
    bare_thread.state = process::ProcessState::Running;
    process::CfsScheduler::set_current_task(&bare_thread);
    ut::expect(sys_mmap(0, page, 1, 0x22, -1, 0) == -syscall::errc::ENOMEM);
    ut::expect(sys_munmap(static_cast<long>(code), page, 0, 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_brk(0, 0, 0, 0, 0, 0) == -syscall::errc::ENOMEM);
    process::CfsScheduler::set_current_task(saved_current);
  }

  process::g_process_manager = nullptr;
  ut::expect(sys_getpriority(0, 9999, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(call("sched_getscheduler", 9999, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  process::g_process_manager = saved_manager;

  ut::expect(call("sched_get_priority_max", static_cast<long>(process::SchedPolicy::Fifo), 0, 0, 0, 0, 0) ==
             static_cast<long>(process::priority::MAX_RT_PRIORITY));
  ut::expect(call("sched_get_priority_max", 99, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(call("sched_get_priority_min", static_cast<long>(process::SchedPolicy::Fifo), 0, 0, 0, 0, 0) ==
             static_cast<long>(process::priority::MIN_RT_PRIORITY));
  ut::expect(call("sched_get_priority_min", 99, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);

  auto child_result = fixture.manager.create_process(fixture.proc->pid());
  if (ut::expect(static_cast<bool>(child_result))) {
    auto child = *child_result;
    auto *child_thread = new process::Thread(9102, child->pid());
    if (ut::expect(child->register_thread(child_thread).has_value())) {
      child_thread->state = process::ProcessState::Running;
      ut::expect(sys_getpriority(0, static_cast<long>(child->pid()), 0, 0, 0, 0) == 20);
      ut::expect(call("sched_getscheduler", static_cast<long>(child->pid()), 0, 0, 0, 0, 0) ==
                 static_cast<long>(process::SchedPolicy::Normal));
    }
  }

  constexpr VirtAddr cap_out = code + 3 * page + 768;
  ut::expect(sys_fork_domain(cap_out, 0, 0, 0, 0, 0) == -syscall::errc::EAGAIN);
  ut::expect(sys_fork_domain_select(cap_out, 0, -1, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(sys_fork_domain_select(cap_out, 0, static_cast<long>(capability::Table::capacity() + 1), 0, 0, 0) ==
             -syscall::errc::EINVAL);

  long handles[capability::Table::capacity()]{};
  usize handle_count = 0;
  for (; handle_count < capability::Table::capacity(); ++handle_count) {
    const long handle = sys_mem_create(page, 0, 0, 0, 0, 0);
    if (handle <= 0) {
      break;
    }
    handles[handle_count] = handle;
  }
  moss::abi::TrapFrame frame{};
  mark_user_frame(frame);
  frame.pc = code;
  frame.sp = code + 4 * page;
  if (auto *thread = process::CfsScheduler::get_current_task()) {
    thread->trap_frame = &frame;
  }
  ut::expect(sys_fork_domain(cap_out, 0, 0, 0, 0, 0) == -syscall::errc::EMFILE);
  if (auto *thread = process::CfsScheduler::get_current_task()) {
    thread->trap_frame = nullptr;
  }
  while (handle_count) {
    --handle_count;
    ut::expect(sys_cap_close(handles[handle_count], 0, 0, 0, 0, 0) == 0);
  }

  long pairs[2][2] = {{-1, -1}, {-1, -1}};
  constexpr VirtAddr handles_addr = code + 3 * page + 832;
  bool paired = true;
  for (usize index = 0; index < 2; ++index) {
    paired = ut::expect(sys_ipc_create(static_cast<long>(handles_addr + index * 0x40), 0, 0, 0, 0, 0) == 0) && paired;
    paired = ut::expect(fixture.address_space->copy_from_user(pairs[index], handles_addr + index * 0x40,
                                                              sizeof(pairs[index])) == 0) && paired;
  }
  if (paired) {
    Handle wait_handles[2] = {static_cast<Handle>(pairs[0][1]), static_cast<Handle>(pairs[1][1])};
    ut::expect(fixture.address_space->copy_to_user(handles_addr, wait_handles, sizeof(wait_handles)) == 0);
  }
  for (usize index = 0; index < 2; ++index) {
    if (pairs[index][0] >= 0) {
      ut::expect(sys_cap_close(pairs[index][0], 0, 0, 0, 0, 0) == 0);
      ut::expect(sys_cap_close(pairs[index][1], 0, 0, 0, 0, 0) == 0);
    }
  }

  ut::expect(sys_clock_nanosleep(0, 1, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  u64 instant = 0;
  ut::expect(fixture.address_space->copy_to_user(handles_addr, &instant, sizeof(instant)) == 0);
  ut::expect(sys_clock_nanosleep(0, 1, handles_addr, 0, 0, 0) == 0);
  ut::expect(sys_clock_nanosleep(2, 0, handles_addr, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(sys_clock_nanosleep(0, 2, handles_addr, 0, 0, 0) == -syscall::errc::EINVAL);
  instant = ~u64{0};
  ut::expect(fixture.address_space->copy_to_user(handles_addr, &instant, sizeof(instant)) == 0);
  ut::expect(sys_clock_nanosleep(0, 0, handles_addr, 0, 0, 0) == -syscall::errc::EINVAL);
  ut::expect(sys_nanosleep(unmapped, 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);

  process::g_scheduler = saved_scheduler;
  ut::expect(true);
}

void syscall_control_edges() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr scratch = code + 3 * page + 768;
  auto named = [](const char *name, long a0, long a1, long a2, long a3, long a4, long a5) {
    for (long number = 0; number < static_cast<long>(syscall::SyscallNumber::MAX_SYSCALL); ++number) {
      const auto *descriptor = syscall::SyscallDispatcher::get_syscall_info(number);
      if (!descriptor || !descriptor->name) {
        continue;
      }
      const char *left = descriptor->name;
      const char *right = name;
      while (*left && *right && *left == *right) {
        ++left;
        ++right;
      }
      if (*left == '\0' && *right == '\0') {
        return descriptor->handler(a0, a1, a2, a3, a4, a5);
      }
    }
    return -1L;
  };
  fixture.proc->designate_domain_factory_source();

  const long handle = sys_domain_self(0, 0, 0, 0, 0, 0);
  if (handle > 0) {
    ut::expect(sys_cap_close(handle, 0, 0, 0, 0, 0) == 0);
  }

  const long self = sys_domain_self(0, 0, 0, 0, 0, 0);
  if (!ut::expect(self > 0)) {
    return;
  }
  const long profile = sys_profile_create(self, 1000, 10000, 0, 0, 0);
  if (ut::expect(profile > 0)) {
    auto *saved_scheduler = process::g_scheduler;
    ut::expect(sys_profile_status(profile, 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);
    ut::expect(sys_profile_status(profile, static_cast<long>(scratch), 0, 0, 0, 0) == 0);
    ut::expect(sys_profile_apply(profile, 0, 0, 0, 0, 0) == -syscall::errc::EINVAL);
    ut::expect(sys_profile_apply(profile, 2000, 0, 0, 0, 0) == -syscall::errc::EINVAL);
    process::g_scheduler = nullptr;
    ut::expect(sys_profile_apply(profile, 500, 0, 0, 0, 0) == -syscall::errc::EAGAIN);
    ut::expect(sys_profile_status(profile, static_cast<long>(scratch), 0, 0, 0, 0) == -syscall::errc::EAGAIN);
    process::g_scheduler = saved_scheduler;
    ut::expect(sys_profile_apply(profile, 500, 0, 0, 0, 0) == 0);
    ut::expect(sys_profile_apply(profile, 500, 0, 0, 0, 0) == -syscall::errc::EBUSY);
    ut::expect(sys_cap_close(profile, 0, 0, 0, 0, 0) == 0);
  }

  auto child_result = fixture.manager.create_process(fixture.proc->pid());
  if (!ut::expect(static_cast<bool>(child_result))) {
    (void)sys_cap_close(self, 0, 0, 0, 0, 0);
    return;
  }
  auto child = *child_result;
  fixture.proc->add_child(child->pid());
  auto *child_thread = new process::Thread(process::Process::allocate_thread_id(), child->pid());
  if (!ut::expect(child->register_thread(child_thread).has_value())) {
    (void)sys_cap_close(self, 0, 0, 0, 0, 0);
    return;
  }
  child_thread->state = process::ProcessState::Sleeping;

  ut::expect(sys_kill(static_cast<long>(child->pid()), 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_kill(static_cast<long>(child->pid()), process::sig::SIGCONT, 0, 0, 0, 0) == 0);
  child->set_pgid(fixture.proc->pgid());
  ut::expect(sys_kill(-static_cast<long>(fixture.proc->pgid()), 0, 0, 0, 0, 0) == 0);
  child_thread->state = process::ProcessState::Running;

  auto no_thread_result = fixture.manager.create_process(fixture.proc->pid());
  if (ut::expect(static_cast<bool>(no_thread_result))) {
    auto no_thread = *no_thread_result;
    ut::expect(sys_kill(static_cast<long>(no_thread->pid()), 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
    (void)fixture.manager.terminate_process(no_thread->pid(), 0);
  }

  auto stranger_result = fixture.manager.create_process(INVALID_PROCESS_ID);
  if (ut::expect(static_cast<bool>(stranger_result))) {
    auto stranger = *stranger_result;
    (void)fixture.manager.terminate_process(stranger->pid(), 0);
  }
  const u32 saved_sid = fixture.proc->sid();
  child->set_sid(saved_sid + 1);
  child->set_sid(saved_sid);

  auto *saved_scheduler = process::g_scheduler;
  process::g_scheduler = nullptr;
  ut::expect(sys_wait4(-1, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  process::g_scheduler = saved_scheduler;

  auto *saved_manager = process::g_process_manager;
  process::g_process_manager = nullptr;
  ut::expect(named("sigaction", 10, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  process::g_process_manager = saved_manager;

  auto *saved_current = process::CfsScheduler::get_current_task();
  process::CfsScheduler::set_current_task(nullptr);
  ut::expect(named("sigreturn", 0, 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  process::CfsScheduler::set_current_task(saved_current);

  ut::expect(named("sigaltstack", 0xdead0000ULL, 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(named("sigaltstack", scratch, 0, 0, 0, 0, 0) == -syscall::errc::ENOMEM);
  ut::expect(sys_cap_close(self, 0, 0, 0, 0, 0) == 0);
}

void missing_process_edges() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr path_addr = code + 3 * page + 768;
  constexpr VirtAddr path2_addr = code + 3 * page + 1024;
  constexpr VirtAddr stat_addr = code + 3 * page + 1280;
  auto put = [&](VirtAddr destination, const char *text) {
    return fixture.address_space->copy_to_user(destination, text, __builtin_strlen(text) + 1) == 0;
  };
  auto named = [](const char *name, long a0, long a1, long a2, long a3, long a4, long a5) {
    for (long number = 0; number < static_cast<long>(syscall::SyscallNumber::MAX_SYSCALL); ++number) {
      const auto *descriptor = syscall::SyscallDispatcher::get_syscall_info(number);
      if (!descriptor || !descriptor->name) {
        continue;
      }
      const char *left = descriptor->name;
      const char *right = name;
      while (*left && *right && *left == *right) {
        ++left;
        ++right;
      }
      if (*left == '\0' && *right == '\0') {
        return descriptor->handler(a0, a1, a2, a3, a4, a5);
      }
    }
    return -1L;
  };

  ut::expect(put(path_addr, "/fixture.bin"));
  ut::expect(put(path2_addr, "/other"));
  ut::expect(fixture.manager.terminate_process(fixture.proc->pid(), 0).has_value());

  ut::expect(sys_stat(path_addr, stat_addr, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(named("access", static_cast<long>(path_addr), 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(named("chdir", static_cast<long>(path_addr), 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_mkdir(path_addr, 0755, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_rmdir(path_addr, 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_unlink(path_addr, 0, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(sys_rename(path_addr, path2_addr, 0, 0, 0, 0) == -syscall::errc::EFAULT);
  ut::expect(named("getpgid", 0, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(named("getsid", 0, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(named("setpgrp", 0, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(named("setsid", 0, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_fork(0, 0, 0, 0, 0, 0) == -syscall::errc::EAGAIN);
  ut::expect(sys_execve(0, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_kill(0, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_mem_create(page, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_domain_self(0, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_profile_apply(0, 1, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_profile_status(0, static_cast<long>(stat_addr), 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_cap_close(0, 0, 0, 0, 0, 0) == -syscall::errc::ESRCH);
  ut::expect(sys_open(path_addr, vfs::O_RDONLY, 0, 0, 0, 0) == -syscall::errc::EBADF);
  process::CfsScheduler::set_current_task(fixture.saved_current);
}

void kernel_entry_edges() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  auto *thread = process::CfsScheduler::get_current_task();
  if (!ut::expect(thread != nullptr)) {
    return;
  }
  moss::abi::TrapFrame frame{};
  mark_user_frame(frame);
  set_syscall_number(frame, static_cast<long>(syscall::SyscallNumber::SYS_GETPID));
  moss::abi::entry::system_call_handler(&frame);
  ut::expect(frame.result() == fixture.proc->pid());
  ut::expect(thread->trap_frame == nullptr);

  frame.status = kPrivilegedTrapStatus;
  moss::abi::entry::user_return_handler(&frame);
  ut::expect(true);

  process::CfsScheduler::set_current_task(nullptr);
  moss::abi::entry::user_return_handler(&frame);
  ut::expect(true);
  process::CfsScheduler::set_current_task(thread);

  (void)moss::abi::bridge::get_current_pgd_phys();
  ut::expect(moss::abi::bridge::resolve_current_user_fault(process::user_layout::CODE_BASE, 99, false) == 0);
}

void fork_page_failure_matrix() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr cap_out = code + 3 * page + 768;
  auto *thread = process::CfsScheduler::get_current_task();
  if (!ut::expect(thread != nullptr)) {
    return;
  }
  moss::abi::TrapFrame frame{};
  mark_user_frame(frame);
  frame.pc = static_cast<u64>(code);
  frame.sp = static_cast<u64>(code + 4 * page);
  thread->trap_frame = &frame;

  const long result = sys_fork_domain(cap_out, 0, 0, 0, 0, 0);
  if (result >= 0) {
    Handle domain = INVALID_HANDLE;
    if (fixture.address_space->copy_from_user(&domain, cap_out, sizeof(domain)) != 0 ||
        domain == INVALID_HANDLE) {
      ut::expect(false);
    }
    (void)sys_domain_terminate(static_cast<long>(domain), 0, 0, 0, 0, 0);
    (void)sys_domain_wait(static_cast<long>(domain), 0, 0, 0, 0, 0);
    (void)sys_cap_close(static_cast<long>(domain), 0, 0, 0, 0, 0);
  } else {
    ut::expect(result == -syscall::errc::ENOMEM || result == -syscall::errc::EAGAIN ||
               result == -syscall::errc::EFAULT);
  }
  thread->trap_frame = nullptr;
  ut::expect(fixture.manager.total_processes() >= 1);
}

void profile_budget_edges() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr status_addr = code + 3 * page + 768;
  constexpr VirtAddr cap_out = code + 3 * page + 1280;
  fixture.proc->designate_domain_factory_source();
  auto *thread = process::CfsScheduler::get_current_task();
  if (!ut::expect(thread != nullptr)) {
    return;
  }
  thread->state = process::ProcessState::Running;

  const long self = sys_domain_self(0, 0, 0, 0, 0, 0);
  if (!ut::expect(self > 0)) {
    return;
  }
  const long first = sys_profile_create(self, 1000, 10000, 0, 0, 0);
  const long second = sys_profile_create(self, 10000, 10000, 0, 0, 0);
  ut::expect(first > 0 && second > 0);
  auto &timers = timer::TimerSubsystem::instance();
  timers.shutdown();
  if (first > 0) {
    ut::expect(sys_profile_apply(first, 1000, 0, 0, 0, 0) == -syscall::errc::EAGAIN);
  }
  ut::expect(timers.initialize().has_value());
  if (first > 0) {
    ut::expect(sys_profile_apply(first, 1000, 0, 0, 0, 0) == 0);
    ut::expect(sys_profile_status(first, static_cast<long>(status_addr), 0, 0, 0, 0) == 0);
  }
  // Keep the scheduler's admitted total while clearing the per-thread profile
  // so the next profile reaches the admission Capacity branch.
  thread->budget_profile_reserved = false;
  thread->budget_profile_pending.store(false, moss::memory_order_relaxed);
  thread->se.cpu_budget = process::CpuBudget{};
  if (second > 0) {
    ut::expect(sys_profile_apply(second, 10000, 0, 0, 0, 0) == -syscall::errc::EAGAIN);
  }
  if (first > 0) {
    ut::expect(sys_cap_close(first, 0, 0, 0, 0, 0) == 0);
  }
  if (second > 0) {
    ut::expect(sys_cap_close(second, 0, 0, 0, 0, 0) == 0);
  }
  ut::expect(sys_cap_close(self, 0, 0, 0, 0, 0) == 0);

  moss::abi::TrapFrame frame{};
  mark_user_frame(frame);
  frame.pc = static_cast<u64>(code);
  frame.sp = static_cast<u64>(code + 4 * page);
  thread->trap_frame = &frame;
  const long child_domain = sys_fork_domain(cap_out, 0, 0, 0, 0, 0);
  if (ut::expect(child_domain >= 0)) {
    Handle domain = INVALID_HANDLE;
    ut::expect(fixture.address_space->copy_from_user(&domain, cap_out, sizeof(domain)) == 0);
    const long profile = domain == INVALID_HANDLE
                             ? -1
                             : sys_profile_create(static_cast<long>(domain), 1000, 10000, 0, 0, 0);
    const long child_pid = sys_domain_id(static_cast<long>(domain), 0, 0, 0, 0, 0);
    ut::expect(sys_domain_terminate(static_cast<long>(domain), 0, 0, 0, 0, 0) == 0);
    ut::expect(sys_domain_wait(static_cast<long>(domain), 0, 0, 0, 0, 0) == 0);
    if (child_pid > 0) {
      auto child_proc = fixture.manager.find_process(static_cast<ProcessId>(child_pid));
      if (child_proc) {
        if (auto *child_thread = child_proc->get_main_thread()) {
          child_thread->state = process::ProcessState::Terminated;
        }
      }
    }
    if (profile > 0) {
      ut::expect(sys_profile_apply(profile, 500, 0, 0, 0, 0) == -syscall::errc::ESRCH);
      (void)sys_profile_status(profile, static_cast<long>(status_addr), 0, 0, 0, 0);
      ut::expect(sys_cap_close(profile, 0, 0, 0, 0, 0) == 0);
    }
    ut::expect(sys_cap_close(static_cast<long>(domain), 0, 0, 0, 0, 0) == 0);
  }
  thread->trap_frame = nullptr;
}

void kill_group_edges() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  auto child_result = fixture.manager.create_process(fixture.proc->pid());
  if (!ut::expect(static_cast<bool>(child_result))) {
    return;
  }
  auto child = *child_result;
  auto *child_thread = new process::Thread(process::Process::allocate_thread_id(), child->pid());
  if (!ut::expect(child->register_thread(child_thread).has_value())) {
    delete child_thread;
    return;
  }
  fixture.proc->add_child(child->pid());
  child->set_pgid(fixture.proc->pgid());
  child_thread->state = process::ProcessState::Sleeping;
  const u32 signo = process::sig::SIGCONT;
  ut::expect(sys_kill(0, 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_kill(0, static_cast<long>(signo), 0, 0, 0, 0) == 0);
  ut::expect(sys_kill(-1, 0, 0, 0, 0, 0) == 0);
  ut::expect(sys_kill(static_cast<long>(child->pid()), 0, 0, 0, 0, 0) == 0);
  (void)fixture.manager.terminate_process(child->pid(), 0);
}

void dispatcher_edges() {
  using namespace syscall;
  ut::expect(SyscallDispatcher::is_implemented(static_cast<long>(SyscallNumber::SYS_GETPID)));
  ut::expect(!SyscallDispatcher::is_implemented(static_cast<long>(SyscallNumber::MAX_SYSCALL)));
  u64 total = 0;
  SyscallDispatcher::get_syscall_stats(&total, nullptr);
  ut::expect(total > 0);
  u64 implemented = 0;
  SyscallDispatcher::get_syscall_stats(nullptr, &implemented);
  ut::expect(implemented > 0);
  SyscallDispatcher::print_implemented_syscalls();
  ut::expect(SyscallDispatcher::get_syscall_info(static_cast<long>(SyscallNumber::SYS_GETPID)) != nullptr);
}

void syscall_state_paths() {
  SyscallFixture fixture;
  if (!ut::expect(fixture.ready)) {
    return;
  }
  using namespace syscall::handlers;
  constexpr usize page = moss::kernel::PAGE_SIZE;
  constexpr VirtAddr code = process::user_layout::CODE_BASE;
  constexpr VirtAddr scratch = code + 3 * page + 768;
  auto put = [&](VirtAddr destination, const void *source, usize size) {
    return fixture.address_space->copy_to_user(destination, source, size) == 0;
  };
  auto *saved_current = process::CfsScheduler::get_current_task();
  auto *saved_manager = process::g_process_manager;
  auto *saved_scheduler = process::g_scheduler;

  process::CfsScheduler::set_current_task(nullptr);
  (void)sys_wait4(-1, 0, 1, 0, 0, 0);
  (void)sys_kill(0, 0, 0, 0, 0, 0);
  (void)sys_getpriority(0, 0, 0, 0, 0, 0);
  (void)sys_sched_getaffinity(0, 0, 0, 0, 0, 0);
  (void)sys_sched_setaffinity(0, 0, 0, 0, 0, 0);

  process::CfsScheduler::set_current_task(saved_current);
  process::g_process_manager = nullptr;
  (void)sys_wait4(-1, 0, 1, 0, 0, 0);
  (void)sys_kill(0, 0, 0, 0, 0, 0);
  (void)sys_sched_setaffinity(static_cast<long>(fixture.proc->pid()), 0, 0, 0, 0, 0);
  process::g_process_manager = saved_manager;

  process::g_scheduler = nullptr;
  (void)sys_wait4(-1, 0, 1, 0, 0, 0);
  (void)sys_wait4(0, static_cast<long>(scratch), 1, 0, 0, 0);
  u32 mask = 1U << 1;
  (void)put(scratch, &mask, sizeof(mask));
  (void)sys_sched_setaffinity(0, sizeof(mask), static_cast<long>(scratch), 0, 0, 0);
  process::g_scheduler = saved_scheduler;

  auto child_result = fixture.manager.create_process(fixture.proc->pid());
  if (ut::expect(static_cast<bool>(child_result))) {
    auto child = *child_result;
    fixture.proc->add_child(child->pid());
    auto *child_thread = new process::Thread(process::Process::allocate_thread_id(), child->pid());
    if (ut::expect(child->register_thread(child_thread).has_value())) {
      child_thread->state = process::ProcessState::Running;
      child->set_state(process::ProcessState::Running);
      (void)sys_wait4(static_cast<long>(child->pid()), static_cast<long>(scratch), 1, 0, 0, 0);
      child_thread->state = process::ProcessState::Sleeping;
      child->set_state(process::ProcessState::Sleeping);
      (void)sys_wait4(static_cast<long>(child->pid()), static_cast<long>(scratch), 1, 0, 0, 0);
      child_thread->state = process::ProcessState::Stopped;
      child_thread->job_stopped = true;
      child->set_state(process::ProcessState::Stopped);
      (void)sys_wait4(static_cast<long>(child->pid()), static_cast<long>(scratch), 1, 0, 0, 0);
      child_thread->publish_wait_status((static_cast<u32>(process::sig::SIGSTOP) << 8) | 0x7f);
      (void)sys_wait4(static_cast<long>(child->pid()), static_cast<long>(scratch), 2, 0, 0, 0);
      child_thread->state = process::ProcessState::Terminated;
      child->set_state(process::ProcessState::Zombie);
      (void)sys_wait4(static_cast<long>(child->pid()), 0, 1, 0, 0, 0);
      (void)sys_wait4(static_cast<long>(child->pid()), 0xdead0000ULL, 1, 0, 0, 0);
    }
  }

  (void)sys_wait4(0x100000000L, 0, 1, 0, 0, 0);
  (void)sys_wait4(-0x100000000L, 0, 1, 0, 0, 0);
  (void)sys_kill(0, -1, 0, 0, 0, 0);
  (void)sys_kill(0, process::sig::NSIG, 0, 0, 0, 0);
  (void)sys_kill(-static_cast<long>(fixture.proc->pgid()), 0, 0, 0, 0, 0);
  (void)sys_kill(1, 0, 0, 0, 0, 0);
  (void)sys_kill(999999, 0, 0, 0, 0, 0);

  auto bare_result = fixture.manager.create_process();
  if (ut::expect(static_cast<bool>(bare_result))) {
    auto bare = *bare_result;
    (void)sys_sched_setaffinity(static_cast<long>(bare->pid()), static_cast<long>(scratch), 0, 0, 0, 0);
    (void)sys_kill(static_cast<long>(bare->pid()), 0, 0, 0, 0, 0);
    (void)sys_wait4(static_cast<long>(bare->pid()), 0, 1, 0, 0, 0);
  }

  (void)sys_mmap(0, page, 1, process::vma_flags::DEMAND_ZERO, -1, 0);
  (void)sys_mmap(code, page, process::vma_flags::READ | process::vma_flags::WRITE, 0, 0, 0);
  (void)sys_mmap(code, page, 0x8, process::vma_flags::DEMAND_ZERO, -1, 0);
  (void)sys_mmap(code, page, process::vma_flags::READ, process::vma_flags::DEMAND_ZERO, 0, 0);
  (void)sys_brk(process::user_layout::HEAP_START - 1, 0, 0, 0, 0, 0);
  (void)sys_brk(process::user_layout::HEAP_START + 16 * 1024 * 1024 + 1, 0, 0, 0, 0, 0);
  (void)sys_brk(0xdead0000, 0, 0, 0, 0, 0);
  ut::expect(sys_brk(process::user_layout::HEAP_START + page, 0, 0, 0, 0, 0) ==
             static_cast<long>(process::user_layout::HEAP_START + page));
  (void)sys_brk(process::user_layout::HEAP_START, 0, 0, 0, 0, 0);

  // Affinity migration and rollback paths. Keep the harness's current mask
  // recoverable and restore the CPU topology after the synthetic 32-CPU case.
  process::CfsScheduler::set_current_task(fixture.saved_current);
  auto *current = process::CfsScheduler::get_current_task();
  if (ut::expect(current != nullptr)) {
    const auto saved_mask = current->cpu_affinity_mask;
    const u32 current_cpu = arch::get_current_cpu_id();
    const u32 other_cpu = (current_cpu + 1) % g_num_cpus;
    u32 local_mask = 1U << other_cpu;
    ut::expect(put(scratch, &local_mask, sizeof(local_mask)));
    const bool irq_state = arch::interrupts_enabled();
    arch::enable_interrupts();
    (void)sys_sched_setaffinity(0, sizeof(local_mask), static_cast<long>(scratch), 0, 0, 0);
    (void)sys_sched_setaffinity(0, sizeof(local_mask), static_cast<long>(scratch), 0, 0, 0);
    if (!irq_state) {
      arch::disable_interrupts();
    }
    local_mask = 1U << current_cpu;
    ut::expect(put(scratch, &local_mask, sizeof(local_mask)));
    (void)sys_sched_setaffinity(0, sizeof(local_mask), static_cast<long>(scratch), 0, 0, 0);

    auto *saved_sched = process::g_scheduler;
    process::g_scheduler = nullptr;
    local_mask = 1U << other_cpu;
    ut::expect(put(scratch, &local_mask, sizeof(local_mask)));
    if (irq_state) {
      arch::enable_interrupts();
    } else {
      arch::disable_interrupts();
    }
    (void)sys_sched_setaffinity(0, sizeof(local_mask), static_cast<long>(scratch), 0, 0, 0);
    process::g_scheduler = saved_sched;
    if (!irq_state) {
      arch::disable_interrupts();
    }

    const u32 saved_cpus = g_num_cpus;
    const u64 saved_online = moss::boot::online_cpu_mask;
    g_num_cpus = 32;
    moss::boot::online_cpu_mask = ~0ULL;
    local_mask = 1U << current_cpu;
    ut::expect(put(scratch, &local_mask, sizeof(local_mask)));
    (void)sys_sched_setaffinity(0, sizeof(local_mask), static_cast<long>(scratch), 0, 0, 0);
    g_num_cpus = saved_cpus;
    moss::boot::online_cpu_mask = saved_online;
    current->cpu_affinity_mask = saved_mask;

    auto orphan_result = fixture.manager.create_process();
    if (ut::expect(static_cast<bool>(orphan_result))) {
      auto orphan = *orphan_result;
      auto *orphan_thread = new process::Thread(process::Process::allocate_thread_id(), orphan->pid());
      if (ut::expect(orphan->register_thread(orphan_thread).has_value())) {
        ut::expect(sys_sched_setaffinity(static_cast<long>(orphan->pid()), sizeof(local_mask), static_cast<long>(scratch), 0, 0, 0) ==
                   -syscall::errc::EPERM);
      }
    }
    ut::expect(sys_sched_setaffinity(999999, sizeof(local_mask), static_cast<long>(scratch), 0, 0, 0) == -syscall::errc::ESRCH);
    process::g_process_manager = nullptr;
    (void)sys_sched_setaffinity(static_cast<long>(fixture.proc->pid()), sizeof(local_mask), static_cast<long>(scratch), 0, 0, 0);
    process::g_process_manager = saved_manager;
  }

  // Map a buffer larger than the pre-existing code VMA for top/system info.
  const long info_map = sys_mmap(0, 3 * page, process::vma_flags::READ | process::vma_flags::WRITE, 0x22, -1, 0);
  if (ut::expect(info_map > 0)) {
    const auto info_addr = static_cast<long>(info_map);
    (void)sys_topinfo(info_addr, 0, 0, 0, 0, 0);
    (void)sys_system_info(info_addr, 0, 0, 0, 0, 0);
    (void)sys_domain_info(0, info_addr, static_cast<long>(sizeof(moss::abi::DomainInfo)), 0, 0, 0);
    process::g_process_manager = nullptr;
    process::g_scheduler = nullptr;
    (void)sys_system_info(info_addr, static_cast<long>(sizeof(moss::abi::SystemInfo)), 0, 0, 0, 0);
    (void)sys_topinfo(info_addr, 0, 0, 0, 0, 0);
    process::g_process_manager = saved_manager;
    process::g_scheduler = saved_scheduler;
    (void)sys_munmap(info_addr, 3 * page, 0, 0, 0, 0);
  }
  (void)sys_munmap(0xdead0000, page, 0, 0, 0, 0);
  (void)sys_munmap(process::user_layout::CODE_BASE, page + 1, 0, 0, 0, 0);
  ut::expect(true);
}

} // namespace

void register_syscall_cases() {
  ut::register_suite("syscall_contracts", [] {
    ut::register_test("basic", basic_contracts);
    ut::register_test("handles", handle_contracts);
    ut::register_test("filesystem", filesystem_contracts);
    ut::register_test("memory", memory_contracts);
    ut::register_test("scheduling", scheduling_contracts);
    ut::register_test("time_signal", time_and_signal_contracts);
    ut::register_test("ipc", ipc_contracts);
    ut::register_test("dispatcher", dispatcher_contracts);
    ut::register_test("null_contexts", null_context_contracts);
    ut::register_test("missing_managers", missing_managers);
    ut::register_test("partial_context_matrix", partial_context_matrix);
    ut::register_test("allocation_failures", allocation_failures);
    ut::register_test("argument_matrix", argument_matrix);
    ut::register_test("filesystem_syscall_paths", filesystem_syscall_paths);
    ut::register_test("fork_domain_errors", fork_domain_errors);
    ut::register_test("control_ipc_error_paths", control_ipc_error_paths);
    ut::register_test("environment_matrix", environment_matrix);
    ut::register_test("fork_allocation_matrix", fork_allocation_matrix);
    ut::register_test("memory_allocation_matrix", memory_allocation_matrix);
    ut::register_test("process_paths", process_syscall_paths);
    ut::register_test("edge_paths", syscall_edge_paths);
    ut::register_test("ipc_edge_paths", ipc_edge_paths);
    ut::register_test("domain_spawn_edges", domain_spawn_edges);
    ut::register_test("ipc_cancel_paths", ipc_cancel_paths);
    ut::register_test("code_capability_paths", code_capability_paths);
    ut::register_test("domain_spawn_allocation_matrix", domain_spawn_allocation_matrix);
    ut::register_test("failure_paths", failure_paths);
    ut::register_test("control_edges", syscall_control_edges);
    ut::register_test("missing_process_edges", missing_process_edges);
    ut::register_test("kernel_entry_edges", kernel_entry_edges);
    ut::register_test("fork_page_failure_matrix", fork_page_failure_matrix);
    ut::register_test("profile_budget_edges", profile_budget_edges);
    ut::register_test("kill_group_edges", kill_group_edges);
    ut::register_test("dispatcher_edges", dispatcher_edges);
    ut::register_test("ipc_branch_paths", ipc_branch_paths);
    ut::register_test("capability_exhaustion_paths", capability_exhaustion_paths);
    ut::register_test("syscall_state_edges", syscall_state_edges);
    ut::register_test("syscall_state_paths", syscall_state_paths);
  });
}

} // namespace moss::test::validation
