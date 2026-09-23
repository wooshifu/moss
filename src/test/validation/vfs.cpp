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

#include "framework/ut_kernel.hpp"
#include "validation_internal.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;

namespace moss::test::validation {
namespace {
void *fd_table() {
  auto *thread = process::CfsScheduler::get_current_task();
  auto proc = process::g_process_manager->find_process(thread->owner_pid);
  return proc->fd_table();
}
} // namespace

void file_read() {
  void *table = fd_table();
  long fd = vfs::syscall::do_open(table, "/fixture.bin", 0, 0);
  if (!ut::expect(fd >= 0)) {
    return;
  }
  u8 bytes[256]{};
  ut::expect(vfs::syscall::do_read(table, fd, vfs::OutputBuffer::kernel(bytes, sizeof(bytes))) == 256);
  bool content = true;
  for (unsigned i = 0; i < 256; ++i) {
    content = content && bytes[i] == i;
  }
  ut::expect(content);
  ut::expect(vfs::syscall::do_lseek(table, fd, 0, 1) == 256);
  ut::expect(vfs::syscall::do_lseek(table, fd, 65536, 0) == 65536);
  ut::expect(vfs::syscall::do_read(table, fd, vfs::OutputBuffer::kernel(bytes, sizeof(bytes))) == 0);
  ut::expect(vfs::syscall::do_close(table, fd) == 0);
  ut::expect(vfs::syscall::do_close(table, fd) == -static_cast<long>(vfs::VfsError::BadFd));
}

void file_errors() {
  void *table = fd_table();
  ut::expect(vfs::syscall::do_open(table, "/missing.fixture", 0, 0) == -static_cast<long>(vfs::VfsError::NoEntry));
  long fd = vfs::syscall::do_open(table, "/fixture.bin", 2, 0);
  if (!ut::expect(fd >= 0)) {
    return;
  }
  const u8 byte = 7;
  ut::expect(vfs::syscall::do_write(table, fd, vfs::InputBuffer::kernel(&byte, 1)) ==
             -static_cast<long>(vfs::VfsError::PermDenied));
  ut::expect(vfs::syscall::do_close(table, fd) == 0);
}

void fd_boundaries() {
  vfs::FdTable table;
  long fd = vfs::syscall::do_open(&table, "/fixture.bin", 0, 0);
  if (!ut::expect(fd >= 0)) {
    return;
  }
  constexpr long bad_fd = 1L << 32;
  auto *file = table.get_file(fd);
  bool valid = ut::expect(table.get_file(bad_fd + fd) == nullptr);
  valid = ut::expect(table.close_fd(bad_fd + fd) == -static_cast<long>(vfs::VfsError::BadFd)) && valid;
  if (valid) {
    ut::expect(vfs::syscall::do_dup2(&table, fd, bad_fd + fd) == -static_cast<long>(vfs::VfsError::BadFd));
    ut::expect(table.get_file(fd) == file && file->ref_count == 1);
  }
  {
    vfs::FdTable::Reservation pending(table);
    ut::expect(pending.fd() == 1 && table.get_file(1) == nullptr);
    ut::expect(table.close_fd(1) == -static_cast<long>(vfs::VfsError::BadFd));
    ut::expect(table.descriptor_flags(1) == -static_cast<long>(vfs::VfsError::BadFd));
    ut::expect(vfs::syscall::do_dup2(&table, fd, 1) == -static_cast<long>(vfs::VfsError::Busy));
    table.close_on_exec();
    ut::expect(vfs::syscall::do_dup(&table, fd) == 2);
    ut::expect(table.close_fd(2) == 0);
    long ends[2] = {-1, -1};
    if (ut::expect(vfs::syscall::do_pipe(&table, ends) == 0)) {
      ut::expect(ends[0] == 2 && ends[1] == 3 && table.get_file(1) == nullptr);
      ut::expect(table.close_fd(ends[0]) == 0);
      ut::expect(table.close_fd(ends[1]) == 0);
    }
    auto *copy = table.clone();
    ut::expect(copy->get_file(1) == nullptr && vfs::syscall::do_dup(copy, fd) == 1);
    copy->close_all();
    delete copy;
  }
  ut::expect(file->ref_count == 1 && vfs::syscall::do_dup(&table, fd) == 1);
  ut::expect(table.close_fd(1) == 0);
  {
    vfs::FdTable::Reservation pending(table);
    ut::expect(pending.fd() == 1 && pending.install(file, true) == 1);
  }
  ut::expect(table.get_file(1) == file && table.descriptor_flags(1) == 1);
  table.close_on_exec();
  ut::expect(table.get_file(1) == nullptr && file->ref_count == 1);
  for (unsigned attempt = 0; attempt < vfs::MAX_FDS; ++attempt) {
    ut::expect(vfs::syscall::do_open(&table, "/missing-open-reservation", 0, 0) ==
               -static_cast<long>(vfs::VfsError::NoEntry));
  }
  const u32 occupied = vfs::file_pool_usage();
  auto **held = new vfs::File *[vfs::MAX_FILES] {};
  u32 count = 0;
  while (count < vfs::MAX_FILES && (held[count] = vfs::alloc_file()) != nullptr) {
    ++count;
  }
  ut::expect(count == vfs::MAX_FILES - occupied);
  long pipe_ends[2] = {-37, -73};
  ut::expect(vfs::syscall::do_pipe(&table, pipe_ends) == -static_cast<long>(vfs::VfsError::NoMemory));
  ut::expect(pipe_ends[0] == -37 && pipe_ends[1] == -73);
  // One available File cannot build both endpoints; return that temporary
  // File as well as both reserved descriptors on the second allocation error.
  if (count) {
    vfs::release_file(held[--count]);
    ut::expect(vfs::syscall::do_pipe(&table, pipe_ends) == -static_cast<long>(vfs::VfsError::NoMemory));
    ut::expect(pipe_ends[0] == -37 && pipe_ends[1] == -73);
    ut::expect(vfs::file_pool_usage() == vfs::MAX_FILES - 1);
    held[count] = vfs::alloc_file();
    if (ut::expect(held[count] != nullptr)) {
      ++count;
    }
  }
  ut::expect(vfs::syscall::do_open(&table, "/uncreated-open-reservation", vfs::O_RDWR | vfs::O_CREAT, 0600) ==
             -static_cast<long>(vfs::VfsError::NoMemory));
  vfs::Stat missing{};
  ut::expect(vfs::syscall::do_stat("/uncreated-open-reservation", &missing) ==
             -static_cast<long>(vfs::VfsError::NoEntry));
  for (u32 i = 0; i < count; ++i) {
    vfs::release_file(held[i]);
  }
  delete[] held;
  ut::expect(vfs::file_pool_usage() == occupied);
  if (ut::expect(vfs::syscall::do_pipe(&table, pipe_ends) == 0)) {
    ut::expect(pipe_ends[0] == 1 && pipe_ends[1] == 2);
    ut::expect(table.close_fd(pipe_ends[0]) == 0);
    ut::expect(table.close_fd(pipe_ends[1]) == 0);
  }
  ut::expect(vfs::syscall::do_dup(&table, fd) == 1);
  table.close_all();

  // A failed fork-table allocation must not acquire any file/CWD references
  // or modify the source. Once memory is returned, the same clone can succeed.
  const auto pools = vfs::pool_usage();
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  fd = vfs::syscall::do_open(&table, "/fixture.bin", vfs::O_CLOEXEC, 0);
  if (!ut::expect(fd == 0 && vfs::syscall::do_chdir(&table, "/") == 0)) {
    return;
  }
  auto *source = table.get_file(fd);
  const auto populated = vfs::pool_usage();
  const auto populated_heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto cwd_refs = table.working_directory()->ref_count;
  {
    HeapPressure pressure;
    if (ut::expect(pressure.acquire(sizeof(vfs::FdTable)))) {
      auto *copy = table.clone();
      ut::expect(copy == nullptr);
      if (copy) {
        copy->close_all();
      }
      delete copy;
      ut::expect(table.get_file(fd) == source && source->ref_count == 1 && table.descriptor_flags(fd) == 1);
      ut::expect(table.working_directory()->ref_count == cwd_refs && vfs::pool_usage() == populated);
    }
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated_heap);
  auto *copy = table.clone();
  if (ut::expect(copy != nullptr)) {
    ut::expect(copy->get_file(fd) == source && source->ref_count == 2 && copy->descriptor_flags(fd) == 1);
    ut::expect(copy->working_directory() == table.working_directory() &&
               table.working_directory()->ref_count == cwd_refs + 1);
    copy->close_all();
    delete copy;
  }
  ut::expect(source->ref_count == 1 && table.working_directory()->ref_count == cwd_refs);
  u8 byte = 255;
  ut::expect(vfs::syscall::do_read(&table, fd, vfs::OutputBuffer::kernel(&byte, 1)) == 1 && byte == 0);
  table.close_all();
  ut::expect(vfs::pool_usage() == pools && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
}

void pipe_reuse() {
  vfs::FdTable table;
  const u8 sent[] = {0, 1, 2, 255};
  u8 received[sizeof(sent)]{};
  for (unsigned cycle = 0; cycle < 1000; ++cycle) {
    long ends[2] = {-1, -1};
    if (!ut::expect(vfs::syscall::do_pipe(&table, ends) == 0)) {
      break;
    }
    bool valid = ut::expect(vfs::syscall::do_write(&table, ends[1], vfs::InputBuffer::kernel(sent, sizeof(sent))) ==
                            sizeof(sent));
    valid = ut::expect(vfs::syscall::do_close(&table, ends[1]) == 0) && valid;
    valid = ut::expect(vfs::syscall::do_read(&table, ends[0], vfs::OutputBuffer::kernel(received, sizeof(received))) ==
                       sizeof(received)) &&
            valid;
    valid = ut::expect(__builtin_memcmp(sent, received, sizeof(sent)) == 0) && valid;
    valid = ut::expect(vfs::syscall::do_read(&table, ends[0], vfs::OutputBuffer::kernel(received, 1)) == 0) && valid;
    table.close_all();
    if (!valid) {
      break;
    }
  }
  table.close_all();
}

void pipe_fd_rollback() {
  for (unsigned free_slots = 0; free_slots < 2; ++free_slots) {
    vfs::FdTable table;
    long file = vfs::syscall::do_open(&table, "/fixture.bin", 0, 0);
    if (!ut::expect(file == 0)) {
      table.close_all();
      return;
    }
    for (u32 fd = 1; fd < vfs::MAX_FDS - free_slots; ++fd) {
      ut::expect(vfs::syscall::do_dup(&table, file) == static_cast<long>(fd));
    }
    bool valid = true;
    // Exceed the pipe-state capacity while every failed attempt must roll back.
    for (unsigned attempt = 0; valid && attempt < 1000; ++attempt) {
      long ends[2] = {-1, -1};
      valid = ut::expect(vfs::syscall::do_pipe(&table, ends) == -static_cast<long>(vfs::VfsError::TooManyFiles));
      valid = ut::expect(ends[0] == -1 && ends[1] == -1) && valid;
    }
    table.close_all();
    long ends[2] = {-1, -1};
    ut::expect(vfs::syscall::do_pipe(&table, ends) == 0);
    table.close_all();
    if (!valid) {
      return;
    }
  }
}

void writable_lifecycle() {
  const auto baseline = vfs::pool_usage();
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  vfs::FdTable table;
  constexpr u32 create = vfs::O_RDWR | vfs::O_CREAT | vfs::O_EXCL;
  constexpr const char *path = "/writable";
  constexpr u8 payload[] = {0, 1, 2, 0xff};
  long fd = vfs::syscall::do_open(&table, path, create, 0600, 0, 43);
  if (!ut::expect(fd == 0)) {
    return;
  }
  vfs::Stat original{}, status{};
  ut::expect(vfs::syscall::do_fstat(&table, fd, &original) == 0 && original.st_size == 0 && original.st_uid == 0 &&
             original.st_gid == 43 && (original.st_mode & 0777) == 0600);
  ut::expect(vfs::syscall::do_open(&table, path, create, 0600) == -static_cast<long>(vfs::VfsError::FileExists));
  ut::expect(vfs::syscall::do_write(&table, fd, vfs::InputBuffer::kernel(payload, sizeof(payload))) == 4);
  long duplicate = vfs::syscall::do_dup(&table, fd);
  ut::expect(duplicate == 1 && vfs::syscall::do_lseek(&table, duplicate, 0, 0) == 0);
  u8 bytes[5]{};
  ut::expect(vfs::syscall::do_read(&table, fd, vfs::OutputBuffer::kernel(bytes, 4)) == 4 &&
             __builtin_memcmp(bytes, payload, 4) == 0 && vfs::syscall::do_lseek(&table, duplicate, 0, 1) == 4);
  ut::expect(vfs::syscall::do_lseek(&table, fd, 4096, 0) == 4096);
  ut::expect(vfs::syscall::do_write(&table, fd, vfs::InputBuffer::kernel(payload, 4)) == 4);
  ut::expect(vfs::syscall::do_lseek(&table, fd, 4095, 0) == 4095);
  ut::expect(vfs::syscall::do_read(&table, fd, vfs::OutputBuffer::kernel(bytes, 5)) == 5 && !bytes[0] &&
             __builtin_memcmp(bytes + 1, payload, 4) == 0);
  const auto populated_heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto populated_pool = vfs::pool_usage();
  ut::expect(vfs::syscall::do_lseek(&table, fd, 16384, 0) == 16384);
  auto rejected = vfs::InputBuffer::user(1, 4, [](void *, u64, usize size) noexcept { return size; });
  ut::expect(vfs::syscall::do_write(&table, fd, rejected) == -static_cast<long>(vfs::VfsError::BadAddress));
  ut::expect(vfs::syscall::do_lseek(&table, fd, 0, 1) == 16384 && vfs::syscall::do_fstat(&table, fd, &status) == 0 &&
             status.st_size == 4100);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated_heap &&
             vfs::pool_usage() == populated_pool);
  ut::expect(vfs::syscall::do_lseek(&table, fd, 1LL << 40, 0) == (1LL << 40));
  ut::expect(vfs::syscall::do_write(&table, fd, vfs::InputBuffer::kernel(payload, 1)) ==
             -static_cast<long>(vfs::VfsError::NoMemory));
  ut::expect(vfs::syscall::do_lseek(&table, fd, 0, 1) == (1LL << 40) &&
             mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated_heap &&
             vfs::pool_usage() == populated_pool);
  constexpr i64 max_offset = 0x7fffffffffffffffLL;
  ut::expect(vfs::syscall::do_lseek(&table, fd, max_offset, 0) == max_offset);
  ut::expect(vfs::syscall::do_lseek(&table, fd, 1, 1) == -static_cast<long>(vfs::VfsError::Overflow));
  ut::expect(vfs::syscall::do_lseek(&table, fd, max_offset, 2) == -static_cast<long>(vfs::VfsError::Overflow));
  ut::expect(vfs::syscall::do_lseek(&table, fd, 0, 1) == max_offset);
  ut::expect(vfs::syscall::do_write(&table, fd, vfs::InputBuffer::kernel(payload, 1)) ==
             -static_cast<long>(vfs::VfsError::FileTooLarge));
  ut::expect(vfs::syscall::do_lseek(&table, fd, -1, 0) == -static_cast<long>(vfs::VfsError::InvalidArg));
  for (u32 next = 2; next < vfs::MAX_FDS; ++next) {
    ut::expect(vfs::syscall::do_dup(&table, fd) == static_cast<long>(next));
  }
  ut::expect(vfs::syscall::do_open(&table, "/uncreated", create, 0600) ==
             -static_cast<long>(vfs::VfsError::TooManyFiles));
  ut::expect(vfs::syscall::do_stat("/uncreated", &status) == -static_cast<long>(vfs::VfsError::NoEntry));
  ut::expect(vfs::syscall::do_open(&table, path, vfs::O_WRONLY | vfs::O_TRUNC, 0) ==
             -static_cast<long>(vfs::VfsError::TooManyFiles));
  ut::expect(vfs::syscall::do_fstat(&table, fd, &status) == 0 && status.st_size == 4100 &&
             mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated_heap);
  table.close_all();
  fd = vfs::syscall::do_open(&table, path, vfs::O_RDONLY, 0);
  long append = vfs::syscall::do_open(&table, path, vfs::O_WRONLY | vfs::O_APPEND, 0);
  ut::expect(vfs::syscall::do_write(&table, fd, vfs::InputBuffer::kernel(payload, 4)) ==
             -static_cast<long>(vfs::VfsError::BadFd));
  ut::expect(vfs::syscall::do_read(&table, append, vfs::OutputBuffer::kernel(bytes, 4)) ==
             -static_cast<long>(vfs::VfsError::BadFd));
  ut::expect(vfs::syscall::do_lseek(&table, append, 0, 0) == 0 &&
             vfs::syscall::do_write(&table, append, vfs::InputBuffer::kernel(payload, 4)) == 4 &&
             vfs::syscall::do_lseek(&table, append, 0, 1) == 4104);
  ut::expect(vfs::syscall::do_unlink(path) == 0);
  ut::expect(vfs::syscall::do_stat(path, &status) == -static_cast<long>(vfs::VfsError::NoEntry));
  ut::expect(vfs::syscall::do_fstat(&table, fd, &status) == 0 && status.st_ino == original.st_ino &&
             status.st_nlink == 0 && status.st_size == 4104);
  ut::expect(vfs::syscall::do_read(&table, fd, vfs::OutputBuffer::kernel(bytes, 4)) == 4 &&
             __builtin_memcmp(bytes, payload, 4) == 0);
  long replacement = vfs::syscall::do_open(&table, path, create, 0644);
  ut::expect(replacement >= 0 && vfs::syscall::do_fstat(&table, replacement, &status) == 0 &&
             status.st_ino != original.st_ino && status.st_size == 0);
  auto partial =
      vfs::InputBuffer::user(reinterpret_cast<u64>(payload), 4, [](void *target, u64 source, usize count) noexcept {
        __builtin_memcpy(target, reinterpret_cast<const void *>(source), 2);
        return count - 2;
      });
  ut::expect(vfs::syscall::do_write(&table, replacement, partial) == 2 &&
             vfs::syscall::do_fstat(&table, replacement, &status) == 0 && status.st_size == 2);
  ut::expect(vfs::syscall::do_lseek(&table, replacement, 0, 0) == 0 &&
             vfs::syscall::do_read(&table, replacement, vfs::OutputBuffer::kernel(bytes, 4)) == 2 &&
             __builtin_memcmp(bytes, payload, 2) == 0);
  long truncate = vfs::syscall::do_open(&table, path, vfs::O_WRONLY | vfs::O_TRUNC, 0);
  ut::expect(truncate >= 0 && vfs::syscall::do_fstat(&table, replacement, &status) == 0 && status.st_size == 0 &&
             vfs::syscall::do_read(&table, replacement, vfs::OutputBuffer::kernel(bytes, 4)) == 0);
  ut::expect(vfs::syscall::do_unlink(path) == 0);
  table.close_all();
  ut::expect(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  for (unsigned cycle = 0; cycle < 1000; ++cycle) {
    fd = vfs::syscall::do_open(&table, path, create, 0600);
    ut::expect(fd == 0 && vfs::syscall::do_write(&table, fd, vfs::InputBuffer::kernel(payload, 4)) == 4);
    ut::expect(vfs::syscall::do_unlink(path) == 0 && vfs::syscall::do_lseek(&table, fd, 0, 0) == 0);
    ut::expect(vfs::syscall::do_read(&table, fd, vfs::OutputBuffer::kernel(bytes, 4)) == 4 &&
               __builtin_memcmp(bytes, payload, 4) == 0);
    table.close_all();
    if (!ut::expect(vfs::pool_usage() == baseline &&
                    mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap)) {
      break;
    }
  }
}

void rename_lifecycle();
void rename_boundaries();
void access_permissions();
void working_directory_lifecycle();
void directory_capacity();

void register_vfs_cases() {
  ut::register_suite("vfs", [] {
    ut::register_test("read_position_eof", file_read);
    ut::register_test("errors_readonly", file_errors);
    ut::register_test("fd_boundaries", fd_boundaries);
    ut::register_test("pipe_reuse", pipe_reuse);
    ut::register_test("pipe_fd_rollback", pipe_fd_rollback);
    ut::register_test("directory_capacity", directory_capacity);
    ut::register_test("writable_lifecycle", writable_lifecycle);
    ut::register_test("rename_lifecycle", rename_lifecycle);
    ut::register_test("rename_boundaries", rename_boundaries);
    ut::register_test("access_permissions", access_permissions);
    ut::register_test("working_directory_lifecycle", working_directory_lifecycle);
  });
}
} // namespace moss::test::validation
