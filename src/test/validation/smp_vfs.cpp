import moss.std;
import moss.types;
import moss.arch;
import moss.abi;
import moss.hal.timer;
import moss.boot;
import moss.fdt;
import moss.mm;
import moss.vfs;
import moss.process;
import moss.timer;
import moss.containers;
import moss.smart_ptr;
import moss.hal.uart;
import moss.hal.mmu;
import moss.logging;
import moss.ipc;
import moss.drivers;
import moss.result;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "ipc_regression.hpp"
#include "queue_regression.hpp"
#include "scheduler_regression.hpp"
#include "validation/memory_internal.hpp"
#include "validation/smp_cases.hpp"
#include "validation/smp_vfs_fixture.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;

namespace moss::test::validation {
long FileReferences::peer() {
  peer_cpu = arch::get_current_cpu_id();
  __atomic_store_n(&arrived, 1U, __ATOMIC_RELEASE);
  for (u32 cycle = 0; cycle < cycles; ++cycle) {
    const u32 start = 2 * cycle + 1;
    wait_for_phase(phase, start);
    auto *copy = writers[1].clone();
    __atomic_store_n(&arrived, start + 1, __ATOMIC_RELEASE);
    wait_for_phase(phase, start + 1);
    copy->close_all();
    delete copy;
    __atomic_store_n(&arrived, start + 2, __ATOMIC_RELEASE);
  }
  wait_for_phase(phase, 2 * cycles + 1);
  writers[1].close_all();
  __atomic_store_n(&arrived, 2 * cycles + 2, __ATOMIC_RELEASE);
  wait_for_phase(phase, 2 * cycles + 2);
  peer_thread = process::CfsScheduler::get_current_task();
  __atomic_store_n(&arrived, 2 * cycles + 3, __ATOMIC_RELEASE);
  wait_for_phase(phase, 2 * cycles + 3);
  shared_read_result = vfs::syscall::do_read(&reader, shared_read_fd, vfs::OutputBuffer::kernel(&shared_byte, 1));
  __atomic_store_n(&arrived, 2 * cycles + 4, __ATOMIC_RELEASE);
  wait_for_phase(phase, 2 * cycles + 4);
  wait_for_phase(phase, 2 * cycles + 5);
  // Arbitrary one-byte payload, checked at the receiving endpoint; it is
  // independent of the negative -73 descriptor sentinel above.
  const u8 sent = 73;
  shared_write_result = vfs::syscall::do_write(&reader, shared_write_fd, vfs::InputBuffer::kernel(&sent, 1));
  __atomic_store_n(&arrived, 2 * cycles + 6, __ATOMIC_RELEASE);
  // Force the valid interleaving where this peer resumes only after the owner
  // advances past its intermediate release. The earlier milestone is not lost.
  wait_for_phase(phase, 2 * cycles + 7);
  wait_for_phase(phase, 2 * cycles + 6);
  shared_open_result = vfs::syscall::do_open(&reader, "/shared-open", vfs::O_WRONLY | vfs::O_TRUNC, 0);
  __atomic_store_n(&arrived, 2 * cycles + 8, __ATOMIC_RELEASE);
  wait_for_phase(phase, 2 * cycles + 8);
  wait_for_phase(phase, 2 * cycles + 9);
  shared_pipe_result = vfs::syscall::do_pipe(&reader, shared_pipe_fds);
  __atomic_store_n(&arrived, 2 * cycles + 11, __ATOMIC_RELEASE);
  wait_for_phase(phase, 2 * cycles + 11);
  wait_for_phase(phase, 2 * cycles + 12);
  shared_pipe_result = vfs::syscall::do_pipe(&reader, shared_pipe_fds);
  __atomic_store_n(&arrived, 2 * cycles + 14, __ATOMIC_RELEASE);
  wait_for_phase(phase, 2 * cycles + 14);
  auto proc = process::current_process();
  require(proc && proc->fd_table());
  native_table = static_cast<vfs::FdTable *>(proc->fd_table());
  __atomic_store_n(&arrived, 2 * cycles + 15, __ATOMIC_RELEASE);
  wait_for_phase(phase, 2 * cycles + 15);
  return 2; // Continue through the real userspace pipe syscall, not a mock.
}

bool FileReferences::dup_peer() {
  // Four variants share the same fixture: dup, dup2, F_DUPFD (0), and
  // F_DUPFD_CLOEXEC (1030), using the project's Linux fcntl ABI constants.
  // After the earlier 17 milestones, each variant reserves three more:
  // enter the syscall, coordinate its copy, then acknowledge completion.
  for (u32 kind = 0; kind < 4; ++kind) {
    const u32 start = 2 * cycles + 18 + 3 * kind;
    wait_for_phase(phase, start);
    if (kind == 0) {
      dup_result = vfs::syscall::do_dup(&reader, 0);
    } else if (kind == 1) {
      dup_result = vfs::syscall::do_dup2(&reader, 0, 2);
    } else {
      dup_result = vfs::syscall::do_fcntl(&reader, 0, kind == 2 ? 0 : 1030, 0);
    }
    __atomic_store_n(&arrived, start + 2, __ATOMIC_RELEASE);
    wait_for_phase(phase, start + 2);
  }
  return arch::get_current_cpu_id() == 1;
}

FileReferences *file_references = nullptr;

static void empty_case() {}

void register_vfs_smp_cases() {
  ut::register_suite("vfs.smp", [] { ut::register_test("shared_references", empty_case); });
}

long start_vfs_smp_suite() {
  const char *selection = selected_suite();
  if (ut::same_id(selection, "vfs.smp")) {
    start_case("shared_references");
    FileReferences::require(g_num_cpus >= 2);
    file_references = new FileReferences();
    return 3; // Reuse the existing CPU0/CPU1 fork, control and reap protocol.
  }
  return 0;
}

long vfs_smp_control(long op, long arg1, long arg2) {
  const char *selection = selected_suite();
  const char *active_case = running_case();
  if (ut::same_id(selection, "vfs.smp") && active_case && file_references) {
    if (op == 7 && arg1 == 0) {
      arch::enable_interrupts();
      return file_references->peer();
    }
    if (op == 7 && arg1 == 1) {
      file_references->native_result = arg2;
      __atomic_store_n(&file_references->arrived, 2 * FileReferences::cycles + 17, __ATOMIC_RELEASE);
      arch::enable_interrupts();
      wait_for_phase(file_references->phase, 2 * FileReferences::cycles + 17);
      return file_references->dup_peer();
    }
    if (op == 8) {
      FileReferences::require(arg1 && affinity_valid());
      arch::enable_interrupts();
      file_references->owner();
      return 0;
    }
    if (op == 9) {
      ut::expect(arg1 && affinity_valid());
      delete file_references;
      file_references = nullptr;
      end_case();
      finish();
    }
  }
  invalid_control();
}

extern "C" void moss_validation_pipe_published(void *table) noexcept {
  if (!file_references || table != &file_references->reader) {
    return;
  }
  const u32 phase = __atomic_load_n(&file_references->phase, __ATOMIC_ACQUIRE);
  if (phase != 2 * FileReferences::cycles + 9 && phase != 2 * FileReferences::cycles + 12) {
    return;
  }
  __atomic_store_n(&file_references->pipe_published, 1U, __ATOMIC_RELEASE);
  wait_for_phase(file_references->phase, phase + 1);
}

extern "C" void moss_validation_pipe_copyout(void *table, long first, long second) noexcept {
  if (!file_references || table != file_references->native_table ||
      __atomic_load_n(&file_references->phase, __ATOMIC_ACQUIRE) != 2 * FileReferences::cycles + 15) {
    return;
  }
  file_references->native_fds[0] = first;
  file_references->native_fds[1] = second;
  __atomic_store_n(&file_references->copyout_ready, 1U, __ATOMIC_RELEASE);
  wait_for_phase(file_references->phase, 2 * FileReferences::cycles + 16);
}

extern "C" void moss_validation_dup_selected(void *table) noexcept {
  if (!file_references || table != &file_references->reader || arch::get_current_cpu_id() != 1) {
    return;
  }
  const u32 phase = __atomic_load_n(&file_references->phase, __ATOMIC_ACQUIRE);
  const u32 offset = phase - (2 * FileReferences::cycles + 18);
  if (offset >= 12 || offset % 3 != 0) {
    return;
  }
  __atomic_store_n(&file_references->dup_ready, 1U, __ATOMIC_RELEASE);
  wait_for_phase(file_references->phase, phase + 1);
}

} // namespace moss::test::validation
