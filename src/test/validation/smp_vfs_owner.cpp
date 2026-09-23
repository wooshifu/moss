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
void FileReferences::owner() {
  wait_for_phase(arrived, 1);
  require(peer_cpu == 1 && affinity_valid());
  const auto baseline = vfs::pool_usage();
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  long ends[2] = {-1, -1};
  require(vfs::syscall::do_pipe(&reader, ends) == 0);
  auto *file = reader.get_file(ends[1]);
  for (auto &table : writers) {
    for (u32 fd = 0; fd < copies; ++fd) {
      require(table.alloc_fd(file) == static_cast<long>(fd));
    }
  }
  require(reader.close_fd(ends[1]) == 0);
  const auto populated = vfs::pool_usage();
  for (u32 cycle = 0; cycle < cycles; ++cycle) {
    const u32 start = 2 * cycle + 1;
    __atomic_store_n(&phase, start, __ATOMIC_RELEASE);
    auto *copy = writers[0].clone();
    wait_for_phase(arrived, start + 1);
    // Two original tables plus two clones each hold 'copies' writer refs;
    // after both clones close, only the two original tables remain.
    require_refs(file, 4 * copies, cycle);
    __atomic_store_n(&phase, start + 1, __ATOMIC_RELEASE);
    copy->close_all();
    delete copy;
    wait_for_phase(arrived, start + 2);
    require_refs(file, 2 * copies, cycle);
    const u8 sent = static_cast<u8>(cycle);
    u8 received = 0;
    require(vfs::syscall::do_write(&writers[0], 0, vfs::InputBuffer::kernel(&sent, 1)) == 1);
    require(vfs::syscall::do_read(&reader, ends[0], vfs::OutputBuffer::kernel(&received, 1)) == 1 && received == sent);
    require(vfs::pool_usage() == populated && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  }
  __atomic_store_n(&phase, 2 * cycles + 1, __ATOMIC_RELEASE);
  writers[0].close_all();
  wait_for_phase(arrived, 2 * cycles + 2);
  u8 byte = 0;
  require(vfs::syscall::do_read(&reader, ends[0], vfs::OutputBuffer::kernel(&byte, 1)) == 0);
  reader.close_all();
  require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  logging::klog::info("File references: CPU0/CPU{}, {} clone/close cycles, EOF and pools restored", peer_cpu, cycles);

  // A blocked I/O must retain its endpoint after another CPU closes and
  // reuses that descriptor in the very same table.
  require(vfs::syscall::do_pipe(&reader, ends) == 0);
  shared_read_fd = ends[0];
  __atomic_store_n(&phase, 2 * cycles + 2, __ATOMIC_RELEASE);
  wait_for_phase(arrived, 2 * cycles + 3);
  require(peer_thread != nullptr);
  __atomic_store_n(&phase, 2 * cycles + 3, __ATOMIC_RELEASE);
  for (;;) {
    {
      containers::LockGuard<containers::IrqSpinLock> guard(peer_thread->sleep_lock);
      if (peer_thread->state == process::ProcessState::Sleeping && peer_thread->sleep_handoff.load() == 0) {
        break;
      }
    }
    if (__atomic_load_n(&arrived, __ATOMIC_ACQUIRE) == 2 * cycles + 4) {
      require(false); // Returning before data or EOF is available is a failure.
    }
    arch::cpu_yield();
  }
  require(ut::eq(reader.close_fd(ends[0]), 0L));
  require(ut::eq(vfs::syscall::do_open(&reader, "/dev/null", 0, 0), ends[0]));
  const u8 sent = 37;
  require(ut::eq(vfs::syscall::do_write(&reader, ends[1], vfs::InputBuffer::kernel(&sent, 1)), 1L));
  wait_for_phase(arrived, 2 * cycles + 4);
  require(ut::eq(shared_read_result, 1L));
  require(ut::eq(shared_byte, sent));
  require(vfs::syscall::do_read(&reader, ends[0], vfs::OutputBuffer::kernel(&byte, 1)) == 0);
  reader.close_all();
  require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  __atomic_store_n(&phase, 2 * cycles + 4, __ATOMIC_RELEASE);

  require(vfs::syscall::do_pipe(&reader, ends) == 0);
  // Fill the real 4096-byte pipe without a page-sized kernel stack temporary.
  u8 bytes[64] = {};
  for (unsigned i = 0; i < 64; ++i) {
    require(ut::eq(vfs::syscall::do_write(&reader, ends[1], vfs::InputBuffer::kernel(bytes, sizeof(bytes))), 64L));
  }
  shared_write_fd = ends[1];
  __atomic_store_n(&phase, 2 * cycles + 5, __ATOMIC_RELEASE);
  for (;;) {
    {
      containers::LockGuard<containers::IrqSpinLock> guard(peer_thread->sleep_lock);
      if (peer_thread->state == process::ProcessState::Sleeping && peer_thread->sleep_handoff.load() == 0) {
        break;
      }
    }
    if (__atomic_load_n(&arrived, __ATOMIC_ACQUIRE) == 2 * cycles + 6) {
      require(false); // A full pipe cannot accept the peer's byte yet.
    }
    arch::cpu_yield();
  }
  require(ut::eq(reader.close_fd(ends[1]), 0L));
  require(ut::eq(vfs::syscall::do_open(&reader, "/dev/null", vfs::O_WRONLY, 0), ends[1]));
  // Reader, active writer and replacement descriptor each still own a File.
  require(ut::eq(vfs::pool_usage().files, baseline.files + 3U));
  for (unsigned i = 0; i < 64; ++i) {
    require(ut::eq(vfs::syscall::do_read(&reader, ends[0], vfs::OutputBuffer::kernel(bytes, sizeof(bytes))), 64L));
    for (u8 value : bytes) {
      require(ut::eq(value, u8{0}));
    }
  }
  wait_for_phase(arrived, 2 * cycles + 6);
  require(ut::eq(shared_write_result, 1L));
  require(ut::eq(vfs::syscall::do_read(&reader, ends[0], vfs::OutputBuffer::kernel(&byte, 1)), 1L));
  require(ut::eq(byte, u8{73}));
  require(ut::eq(vfs::syscall::do_read(&reader, ends[0], vfs::OutputBuffer::kernel(&byte, 1)), 0L));
  require(ut::eq(vfs::syscall::do_write(&reader, ends[1], vfs::InputBuffer::kernel(&byte, 1)), 1L));
  reader.close_all();
  require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  __atomic_store_n(&phase, 2 * cycles + 6, __ATOMIC_RELEASE);

  require(ut::eq(vfs::syscall::do_open(&reader, "/shared-open", vfs::O_RDWR | vfs::O_CREAT | vfs::O_EXCL, 0600), 0L));
  require(ut::eq(vfs::syscall::do_write(&reader, 0, vfs::InputBuffer::kernel(&sent, 1)), 1L));
  vfs::Stat original{}, after{};
  require(ut::eq(vfs::syscall::do_fstat(&reader, 0, &original), 0L));
  require(ut::eq(original.st_size, u64{1}));
  for (u32 fd = 1; fd + 1 < vfs::MAX_FDS; ++fd) {
    require(ut::eq(vfs::syscall::do_dup(&reader, 0), static_cast<long>(fd)));
  }
  const auto files_before_open = vfs::file_pool_usage();
  long competitor = -1;
  {
    // Hold mutation before launching the real open. File allocation makes
    // its progress observable without borrowing an unpublished object.
    containers::LockGuard<containers::IrqSpinLock> guard(vfs::namespace_lock);
    __atomic_store_n(&phase, 2 * cycles + 7, __ATOMIC_RELEASE);
    while (vfs::file_pool_usage() == files_before_open) {
      if (__atomic_load_n(&arrived, __ATOMIC_ACQUIRE) == 2 * cycles + 8) {
        require(false);
      }
      arch::cpu_yield();
    }
    require(ut::eq(vfs::file_pool_usage(), files_before_open + 1U));
    competitor = vfs::syscall::do_dup(&reader, 0);
  }
  wait_for_phase(arrived, 2 * cycles + 8);
  const long full = -static_cast<long>(vfs::VfsError::TooManyFiles);
  const long last = vfs::MAX_FDS - 1;
  require((competitor == last && shared_open_result == full) || (competitor == full && shared_open_result == last));
  require(ut::eq(vfs::syscall::do_lseek(&reader, 0, 0, 0), 0L));
  const long expected_bytes = shared_open_result == full ? 1 : 0;
  require(ut::eq(vfs::syscall::do_fstat(&reader, 0, &after), 0L));
  require(ut::eq(after.st_ino, original.st_ino));
  require(ut::eq(after.st_size, static_cast<u64>(expected_bytes)));
  require(ut::eq(vfs::syscall::do_read(&reader, 0, vfs::OutputBuffer::kernel(&byte, 1)), expected_bytes));
  if (expected_bytes) {
    require(ut::eq(byte, sent));
  }
  reader.close_all();
  require(ut::eq(vfs::syscall::do_unlink("/shared-open"), 0L));
  require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  __atomic_store_n(&phase, 2 * cycles + 8, __ATOMIC_RELEASE);

  require(ut::eq(vfs::syscall::do_open(&reader, "/dev/null", vfs::O_RDWR, 0), 0L));
  for (u32 fd = 1; fd + 1 < vfs::MAX_FDS; ++fd) {
    require(ut::eq(vfs::syscall::do_dup(&reader, 0), static_cast<long>(fd)));
  }
  __atomic_store_n(&phase, 2 * cycles + 9, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&pipe_published, __ATOMIC_ACQUIRE) &&
         __atomic_load_n(&arrived, __ATOMIC_ACQUIRE) != 2 * cycles + 11) {
    arch::cpu_yield();
  }
  const bool published = __atomic_load_n(&pipe_published, __ATOMIC_ACQUIRE) != 0;
  if (published) {
    require(ut::eq(reader.close_fd(last), 0L));
  }
  require(ut::eq(vfs::syscall::do_dup(&reader, 0), last));
  require(ut::eq(vfs::syscall::do_write(&reader, last, vfs::InputBuffer::kernel(&sent, 1)), 1L));
  __atomic_store_n(&phase, 2 * cycles + 10, __ATOMIC_RELEASE);
  wait_for_phase(arrived, 2 * cycles + 11);
  require(ut::eq(shared_pipe_result, full));
  require(ut::eq(shared_pipe_fds[0], -37L));
  require(ut::eq(shared_pipe_fds[1], -73L));
  require(ut::eq(vfs::syscall::do_write(&reader, 0, vfs::InputBuffer::kernel(&sent, 1)), 1L));
  // A failed pipe must not close a descriptor installed by the other CPU,
  // nor transiently expose just one endpoint of the unsuccessful pair.
  require(ut::eq(vfs::syscall::do_write(&reader, last, vfs::InputBuffer::kernel(&sent, 1)), 1L));
  require(!published);
  reader.close_all();
  require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  __atomic_store_n(&phase, 2 * cycles + 11, __ATOMIC_RELEASE);

  // With two slots available, another CPU must see and clone both endpoints
  // together, even before the publishing syscall returns.
  __atomic_store_n(&pipe_published, 0U, __ATOMIC_RELEASE);
  require(ut::eq(vfs::syscall::do_open(&reader, "/dev/null", vfs::O_RDWR, 0), 0L));
  for (u32 fd = 1; fd + 2 < vfs::MAX_FDS; ++fd) {
    require(ut::eq(vfs::syscall::do_dup(&reader, 0), static_cast<long>(fd)));
  }
  __atomic_store_n(&phase, 2 * cycles + 12, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&pipe_published, __ATOMIC_ACQUIRE) &&
         __atomic_load_n(&arrived, __ATOMIC_ACQUIRE) != 2 * cycles + 14) {
    arch::cpu_yield();
  }
  require(__atomic_load_n(&pipe_published, __ATOMIC_ACQUIRE) != 0);
  require(reader.get_file(last - 1) != nullptr && reader.get_file(last) != nullptr);
  require(ut::eq(vfs::syscall::do_fcntl(&reader, last - 1, 3, 0), static_cast<long>(vfs::O_RDONLY)));
  require(ut::eq(vfs::syscall::do_fcntl(&reader, last, 3, 0), static_cast<long>(vfs::O_WRONLY)));
  require(reader.descriptor_flags(last - 1) == 0 && reader.descriptor_flags(last) == 0);
  auto *copy = reader.clone();
  require(copy->get_file(last - 1) != nullptr && copy->get_file(last) != nullptr);
  require(ut::eq(vfs::syscall::do_write(&reader, last, vfs::InputBuffer::kernel(&sent, 1)), 1L));
  require(ut::eq(vfs::syscall::do_read(copy, last - 1, vfs::OutputBuffer::kernel(&byte, 1)), 1L));
  require(ut::eq(byte, sent));
  copy->close_all();
  delete copy;
  __atomic_store_n(&phase, 2 * cycles + 13, __ATOMIC_RELEASE);
  wait_for_phase(arrived, 2 * cycles + 14);
  require(ut::eq(shared_pipe_result, 0L));
  require(ut::eq(shared_pipe_fds[0], last - 1));
  require(ut::eq(shared_pipe_fds[1], last));
  require(ut::eq(reader.close_fd(last), 0L));
  require(ut::eq(vfs::syscall::do_read(&reader, last - 1, vfs::OutputBuffer::kernel(&byte, 1)), 0L));
  reader.close_all();
  require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  __atomic_store_n(&phase, 2 * cycles + 14, __ATOMIC_RELEASE);

  wait_for_phase(arrived, 2 * cycles + 15);
  require(native_table != nullptr);
  const long anchor = vfs::syscall::do_open(native_table, "/dev/null", vfs::O_RDWR, 0);
  require(ut::eq(anchor, 3L));
  __atomic_store_n(&phase, 2 * cycles + 15, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&copyout_ready, __ATOMIC_ACQUIRE) &&
         __atomic_load_n(&arrived, __ATOMIC_ACQUIRE) != 2 * cycles + 17) {
    arch::cpu_yield();
  }
  require(__atomic_load_n(&copyout_ready, __ATOMIC_ACQUIRE) != 0);
  require(native_fds[0] == 4 && native_fds[1] == 5);
  const bool visible = native_table->get_file(native_fds[0]) != nullptr;
  for (const long fd : native_fds) {
    if (visible) {
      require(ut::eq(native_table->close_fd(fd), 0L));
      require(ut::eq(vfs::syscall::do_dup2(native_table, anchor, fd), fd));
      require(ut::eq(vfs::syscall::do_write(native_table, fd, vfs::InputBuffer::kernel(&sent, 1)), 1L));
    } else {
      require(native_table->get_file(fd) == nullptr);
      require(ut::eq(native_table->close_fd(fd), -static_cast<long>(vfs::VfsError::BadFd)));
      require(ut::eq(vfs::syscall::do_dup2(native_table, anchor, fd), -static_cast<long>(vfs::VfsError::Busy)));
    }
  }
  if (!visible) {
    auto *pending_copy = native_table->clone();
    require(pending_copy->get_file(native_fds[0]) == nullptr && pending_copy->get_file(native_fds[1]) == nullptr);
    require(ut::eq(vfs::syscall::do_dup(pending_copy, anchor), native_fds[0]));
    pending_copy->close_all();
    delete pending_copy;
  }
  __atomic_store_n(&phase, 2 * cycles + 16, __ATOMIC_RELEASE);
  wait_for_phase(arrived, 2 * cycles + 17);
  require(ut::eq(native_result, -14L)); // The native copy into read-only user memory must fail.
  require(ut::eq(vfs::syscall::do_write(native_table, anchor, vfs::InputBuffer::kernel(&sent, 1)), 1L));
  for (const long fd : native_fds) {
    if (!visible) {
      require(ut::eq(vfs::syscall::do_dup(native_table, anchor), fd));
    }
    require(ut::eq(vfs::syscall::do_write(native_table, fd, vfs::InputBuffer::kernel(&sent, 1)), 1L));
  }
  require(!visible);
  for (const long fd : native_fds) {
    require(ut::eq(native_table->close_fd(fd), 0L));
  }
  require(ut::eq(native_table->close_fd(anchor), 0L));
  require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  __atomic_store_n(&phase, 2 * cycles + 17, __ATOMIC_RELEASE);
  for (u32 kind = 0; kind < 4; ++kind) {
    const u32 start = 2 * cycles + 18 + 3 * kind;
    require(ut::eq(vfs::syscall::do_open(&reader, "/fixture.bin", vfs::O_RDONLY, 0), 0L));
    require(ut::eq(vfs::syscall::do_open(&reader, "/dev/null", vfs::O_WRONLY, 0), 1L));
    __atomic_store_n(&dup_ready, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&phase, start, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&dup_ready, __ATOMIC_ACQUIRE) && __atomic_load_n(&arrived, __ATOMIC_ACQUIRE) != start + 2) {
      arch::cpu_yield();
    }
    require(__atomic_load_n(&dup_ready, __ATOMIC_ACQUIRE) != 0);
    require(ut::eq(reader.close_fd(0), 0L));
    require(reader.get_file(0) == nullptr);
    require(ut::eq(vfs::syscall::do_dup2(&reader, 1, 2), 2L));
    require(ut::eq(vfs::syscall::do_write(&reader, 2, vfs::InputBuffer::kernel(&sent, 1)), 1L));
    __atomic_store_n(&phase, start + 1, __ATOMIC_RELEASE);
    wait_for_phase(arrived, start + 2);
    // Atomic duplication either precedes source close, or sees EBADF. It
    // cannot resurrect source FD 0 or overwrite the later target replacement.
    require(dup_result == 2 || dup_result == -static_cast<long>(vfs::VfsError::BadFd));
    require(reader.get_file(0) == nullptr);
    require(ut::eq(vfs::syscall::do_write(&reader, 1, vfs::InputBuffer::kernel(&sent, 1)), 1L));
    require(ut::eq(vfs::syscall::do_write(&reader, 2, vfs::InputBuffer::kernel(&sent, 1)), 1L));
    require(ut::eq(reader.descriptor_flags(2), 0L));
    reader.close_all();
    require(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    __atomic_store_n(&phase, start + 2, __ATOMIC_RELEASE);
  }
}
} // namespace moss::test::validation
