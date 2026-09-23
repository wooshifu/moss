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

void rename_lifecycle() {
  // Octal modes exercise owner-only versus public access; UID/GID 42/43 are
  // arbitrary non-root identities. Distinct byte fixtures include '\0'/0xff
  // so replacement checks exercise binary content rather than C strings.
  // One thousand cycles is a bounded reuse/leak workload, not a proof of stability.
  const auto baseline = vfs::pool_usage();
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  vfs::FdTable table;
  ut::expect(vfs::syscall::do_mkdir("/rename-a", 0777, 0, 0) == 0);
  ut::expect(vfs::syscall::do_mkdir("/rename-b", 0700, 0, 0) == 0);
  const auto directories = vfs::pool_usage();
  constexpr u32 create = vfs::O_RDWR | vfs::O_CREAT | vfs::O_EXCL;
  constexpr u8 source_bytes[] = {0, 37, 0xff}, target_bytes[] = {91, 0, 17};
  for (unsigned cycle = 0; cycle < 1000; ++cycle) {
    long source = vfs::syscall::do_open(&table, "/rename-a/source", create, 0600, 42, 43);
    long target = vfs::syscall::do_open(&table, "/rename-b/target", create, 0644);
    ut::expect(source == 0 && target == 1);
    ut::expect(vfs::syscall::do_write(&table, source, vfs::InputBuffer::kernel(source_bytes, 3)) == 3);
    ut::expect(vfs::syscall::do_write(&table, target, vfs::InputBuffer::kernel(target_bytes, 3)) == 3);
    vfs::Stat before{}, replaced{}, current{};
    ut::expect(vfs::syscall::do_stat("/rename-a/source", &before) == 0 &&
               vfs::syscall::do_stat("/rename-b/target", &replaced) == 0);
    const auto populated = vfs::pool_usage();
    const auto populated_heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    ut::expect(vfs::syscall::do_rename("/rename-a/source", "/rename-b/target") == 0);
    ut::expect(vfs::pool_usage() == populated &&
               mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated_heap);
    ut::expect(vfs::syscall::do_stat("/rename-a/source", &current) == -static_cast<long>(vfs::VfsError::NoEntry));
    ut::expect(vfs::syscall::do_stat("/rename-b/target", &current) == 0 && current.st_ino == before.st_ino &&
               current.st_nlink == 1 && current.st_size == 3 && current.st_uid == 42 && current.st_gid == 43);
    ut::expect(vfs::syscall::do_fstat(&table, target, &current) == 0 && current.st_ino == replaced.st_ino &&
               current.st_nlink == 0 && current.st_size == 3);
    u8 bytes[3]{};
    ut::expect(vfs::syscall::do_lseek(&table, source, 0, 0) == 0 &&
               vfs::syscall::do_read(&table, source, vfs::OutputBuffer::kernel(bytes, 3)) == 3 &&
               __builtin_memcmp(bytes, source_bytes, 3) == 0);
    ut::expect(vfs::syscall::do_lseek(&table, target, 0, 0) == 0 &&
               vfs::syscall::do_read(&table, target, vfs::OutputBuffer::kernel(bytes, 3)) == 3 &&
               __builtin_memcmp(bytes, target_bytes, 3) == 0);
    ut::expect(vfs::syscall::do_rename("/rename-b/target", "/rename-b/target") == 0);
    ut::expect(vfs::syscall::do_unlink("/rename-b/target") == 0);
    table.close_all();
    if (!ut::expect(vfs::pool_usage() == directories &&
                    mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap)) {
      break;
    }
  }
  ut::expect(vfs::syscall::do_rmdir("/rename-a") == 0 && vfs::syscall::do_rmdir("/rename-b") == 0);
  ut::expect(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
}

void rename_boundaries() {
  const auto baseline = vfs::pool_usage();
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  vfs::FdTable table;
  constexpr const char *paths[] = {"/rename-a", "/rename-b", "/rename-a/source", "/rename-a/source/leaf",
                                   "/rename-b/target"};
  for (const auto *path : paths) {
    ut::expect(vfs::syscall::do_mkdir(path, 0700, 0, 0) == 0);
  }
  long held = vfs::syscall::do_open(&table, "/rename-b/target", vfs::O_RDONLY, 0);
  long file = vfs::syscall::do_open(&table, "/rename-a/file", vfs::O_RDWR | vfs::O_CREAT, 0600);
  vfs::Stat source{}, target{}, current{}, parent{};
  ut::expect(held >= 0 && file >= 0 && vfs::syscall::do_stat("/rename-a/source", &source) == 0 &&
             vfs::syscall::do_stat("/rename-b/target", &target) == 0);
  ut::expect(vfs::syscall::do_rename("/rename-a/source", "/rename-b/target") == 0);
  ut::expect(vfs::syscall::do_stat("/rename-b/target", &current) == 0 && current.st_ino == source.st_ino &&
             current.st_nlink == 3 && vfs::syscall::do_stat("/rename-b/target/leaf", &current) == 0);
  ut::expect(vfs::syscall::do_fstat(&table, held, &current) == 0 && current.st_ino == target.st_ino &&
             current.st_nlink == 0);
  ut::expect(vfs::syscall::do_stat("/rename-a", &parent) == 0 && parent.st_nlink == 2);
  ut::expect(vfs::syscall::do_stat("/rename-b", &parent) == 0 && parent.st_nlink == 3);
  struct Rejection {
    const char *old_path;
    const char *new_path;
    vfs::VfsError error;
  };
  const Rejection rejections[] = {
      {.old_path = "/rename-b/target", .new_path = "/rename-b/target/leaf/cycle", .error = vfs::VfsError::InvalidArg},
      {.old_path = "/rename-a/file", .new_path = "/rename-b/target", .error = vfs::VfsError::IsDirectory},
      {.old_path = "/rename-b/target", .new_path = "/rename-a/file", .error = vfs::VfsError::NotDirectory},
      {.old_path = "/rename-a", .new_path = "/rename-b", .error = vfs::VfsError::NotEmpty},
      {.old_path = "/rename-a/file", .new_path = "/dev/null", .error = vfs::VfsError::CrossDevice},
      {.old_path = "/", .new_path = "/rename-a/root", .error = vfs::VfsError::Busy},
      {.old_path = "/rename-a/file", .new_path = "/", .error = vfs::VfsError::Busy},
      {.old_path = "/dev", .new_path = "/rename-a/device", .error = vfs::VfsError::Busy},
      {.old_path = "/rename-a/file", .new_path = "/dev", .error = vfs::VfsError::Busy},
      {.old_path = "/rename-a/file/", .new_path = "/rename-a/new", .error = vfs::VfsError::NotDirectory},
      {.old_path = "/rename-a/file", .new_path = "/rename-a/new/", .error = vfs::VfsError::NotDirectory},
      {.old_path = "/rename-a/file", .new_path = "/rename-a/file/child/new", .error = vfs::VfsError::NotDirectory},
      {.old_path = "/rename-a/.", .new_path = "/rename-a/new", .error = vfs::VfsError::InvalidArg},
      {.old_path = "/rename-a/file", .new_path = "/rename-a/..", .error = vfs::VfsError::InvalidArg},
      {.old_path = "/rename-a/missing", .new_path = "/rename-a/new", .error = vfs::VfsError::NoEntry},
      {.old_path = "/rename-a/file", .new_path = "", .error = vfs::VfsError::NoEntry},
  };
  const auto populated = vfs::pool_usage();
  for (const auto &rejection : rejections) {
    ut::expect(vfs::syscall::do_rename(rejection.old_path, rejection.new_path) == -static_cast<long>(rejection.error));
    ut::expect(vfs::pool_usage() == populated && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    ut::expect(vfs::syscall::do_stat("/rename-b/target", &current) == 0 && current.st_ino == source.st_ino &&
               current.st_nlink == 3 && vfs::syscall::do_stat("/rename-a/file", &current) == 0);
  }
  // A full destination must reject a cross-directory insertion without losing
  // the source. Same-directory renaming and replacement need no extra slot.
  ut::expect(vfs::syscall::do_mkdir("/rename-full", 0700, 0, 0) == 0);
  char path[] = "/rename-full/00";
  for (u32 i = 0; i < vfs::Inode::MAX_CHILDREN; ++i) {
    path[13] = static_cast<char>('0' + i / 10);
    path[14] = static_cast<char>('0' + i % 10);
    ut::expect(vfs::syscall::do_mkdir(path, 0700, 0, 0) == 0);
  }
  const auto full = vfs::pool_usage();
  for (unsigned attempt = 0; attempt < 300; ++attempt) {
    ut::expect(vfs::syscall::do_rename("/rename-b/target", "/rename-full/overflow") ==
               -static_cast<long>(vfs::VfsError::NoMemory));
    ut::expect(vfs::pool_usage() == full && vfs::syscall::do_stat("/rename-b/target/leaf", &current) == 0);
  }
  ut::expect(vfs::syscall::do_rename("/rename-full/00", "/rename-full/renamed") == 0);
  ut::expect(vfs::syscall::do_rename("/rename-full/renamed", "/rename-full/00") == 0);
  ut::expect(vfs::syscall::do_rename("/rename-b/target", "/rename-full/00") == 0);
  ut::expect(vfs::syscall::do_stat("/rename-full/00", &current) == 0 && current.st_ino == source.st_ino &&
             vfs::syscall::do_stat("/rename-full/00/leaf", &current) == 0);
  ut::expect(vfs::syscall::do_rmdir("/rename-full/00/leaf") == 0);
  for (u32 i = 0; i < vfs::Inode::MAX_CHILDREN; ++i) {
    path[13] = static_cast<char>('0' + i / 10);
    path[14] = static_cast<char>('0' + i % 10);
    ut::expect(vfs::syscall::do_rmdir(path) == 0);
  }
  table.close_all();
  ut::expect(vfs::syscall::do_unlink("/rename-a/file") == 0);
  constexpr const char *directories[] = {"/rename-a", "/rename-b", "/rename-full"};
  for (const auto *directory : directories) {
    ut::expect(vfs::syscall::do_rmdir(directory) == 0);
  }
  ut::expect(vfs::pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
}

void access_permissions() {
  // Identity 42 owns files, group 43 matches them, and 99 is an unrelated user
  // or group. Octal modes isolate owner/group/other and execute permissions;
  // access masks use R_OK=4, W_OK=2, X_OK=1, F_OK=0 (combinations are bitwise OR).
  using namespace vfs;
  const auto baseline = pool_usage();
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  constexpr long denied = -static_cast<long>(VfsError::PermDenied);
  for (unsigned cycle = 0; cycle < 64; ++cycle) {
    {
      FdTable table;
      // Root provides a writable arena; unprivileged creation must not rely on
      // bypassing the namespace root's 0755 permissions.
      if (!ut::expect(vfs::syscall::do_mkdir("/access-arena", 0777, 0, 0) == 0 &&
                      vfs::syscall::do_chdir(&table, "/access-arena") == 0)) {
        return;
      }
      auto create = [&](const char *path, u32 mode) {
        const long fd = vfs::syscall::do_open(&table, path, O_CREAT | O_EXCL | O_RDWR, mode, 42, 43);
        return fd >= 0 && vfs::syscall::do_close(&table, fd) == 0;
      };
      if (!ut::expect(create("owner", 0640) && create("group", 0040) && create("exec", 0010) &&
                      create("others", 0066) && vfs::syscall::do_mkdir("tree", 0710, 42, 43, &table) == 0 &&
                      create("tree/leaf", 0600) && vfs::syscall::do_mkdir("tree/empty", 0700, 42, 43, &table) == 0)) {
        return;
      }
      ut::expect(vfs::syscall::do_access(&table, "owner", 6, 42, 99) == 0);
      ut::expect(vfs::syscall::do_access(&table, "owner", 4, 99, 43) == 0);
      ut::expect(vfs::syscall::do_access(&table, "owner", 2, 99, 43) == denied);
      ut::expect(vfs::syscall::do_access(&table, "owner", 4, 99, 99) == denied);
      auto open_as = [&](const char *path, u32 flags, u32 uid, u32 gid, bool permitted) {
        const long fd = vfs::syscall::do_open(&table, path, flags, 0, uid, gid);
        ut::expect(permitted ? fd >= 0 : fd == denied);
        if (fd >= 0) {
          ut::expect(vfs::syscall::do_close(&table, fd) == 0);
        }
      };
      const long owner = vfs::syscall::do_open(&table, "owner", O_RDWR, 0, 42, 43);
      constexpr u8 payload[] = {37, 0, 0xff};
      if (!ut::expect(owner >= 0 && vfs::syscall::do_write(&table, owner, InputBuffer::kernel(payload, 3)) == 3)) {
        return;
      }
      Stat original{}, current{}, original_leaf{};
      ut::expect(vfs::syscall::do_fstat(&table, owner, &original) == 0 && original.st_uid == 42 &&
                 original.st_gid == 43 && vfs::syscall::do_stat("tree", &current, &table) == 0 &&
                 current.st_uid == 42 && current.st_gid == 43 &&
                 vfs::syscall::do_stat("tree/leaf", &original_leaf, &table) == 0);
      const auto populated = pool_usage();
      const auto populated_heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
      open_as("owner", O_RDONLY, 99, 99, false);
      open_as("owner", O_RDONLY, 99, 43, true);
      open_as("owner", O_RDWR, 42, 99, true);
      open_as("owner", O_WRONLY | O_TRUNC, 99, 43, false);
      open_as("owner", O_RDWR, 99, 43, false);
      open_as("group", O_RDONLY, 42, 43, false); // Owner bits take precedence over group.
      open_as("group", O_RDONLY, 99, 43, true);
      open_as("others", O_RDWR, 99, 99, true);
      open_as("others", O_RDONLY, 42, 99, false);
      open_as("group", O_RDWR, 0, 99, true);
      open_as("tree/../others", O_RDONLY, 99, 99, false); // Do not normalize away a denied search.
      open_as("tree/../others", O_RDONLY, 99, 43, true);
      open_as("/access-uncreated", O_CREAT | O_WRONLY, 99, 99, false);
      if (vfs::syscall::do_stat("/access-uncreated", &current) == 0) {
        ut::expect(vfs::syscall::do_unlink("/access-uncreated") == 0);
      }
      // Group search alone cannot authorize namespace changes. Rejections must
      // preserve both rename endpoints and the populated resource baseline.
      open_as("tree/uncreated", O_CREAT | O_WRONLY, 99, 43, false);
      ut::expect(vfs::syscall::do_mkdir("tree/uncreated", 0700, 99, 43, &table) == denied);
      ut::expect(vfs::syscall::do_unlink("tree/leaf", &table, 99, 43) == denied);
      ut::expect(vfs::syscall::do_rmdir("tree/empty", &table, 99, 43) == denied);
      ut::expect(vfs::syscall::do_rename("tree/leaf", "owner", &table, 99, 43) == denied);
      ut::expect(vfs::syscall::do_rename("owner", "tree/leaf", &table, 99, 43) == denied);
      ut::expect(vfs::syscall::do_stat("tree/../owner", &current, &table, 99, 99) == denied);
      ut::expect(vfs::syscall::do_stat("tree/leaf", &current, &table, 99, 43) == 0 &&
                 current.st_ino == original_leaf.st_ino && current.st_size == original_leaf.st_size);
      u8 bytes[3]{};
      ut::expect(vfs::syscall::do_fstat(&table, owner, &current) == 0 && current.st_ino == original.st_ino &&
                 current.st_size == 3 && vfs::syscall::do_lseek(&table, owner, 0, 0) == 0 &&
                 vfs::syscall::do_read(&table, owner, OutputBuffer::kernel(bytes, 3)) == 3 &&
                 __builtin_memcmp(bytes, payload, 3) == 0);
      ut::expect(pool_usage() == populated &&
                 mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated_heap);
      ut::expect(vfs::syscall::do_close(&table, owner) == 0);
      ut::expect(vfs::syscall::do_access(&table, "group", 4, 42, 43) == denied);
      ut::expect(vfs::syscall::do_access(&table, "group", 4, 99, 43) == 0);
      ut::expect(vfs::syscall::do_access(&table, "owner", 6, 0, 0) == 0);
      ut::expect(vfs::syscall::do_access(&table, "owner", 1, 0, 0) == denied);
      ut::expect(vfs::syscall::do_access(&table, "exec", 1, 0, 0) == 0);
      ut::expect(vfs::syscall::do_access(&table, "/fixture.bin", 2, 0, 0) == denied);
      ut::expect(vfs::syscall::do_access(&table, "tree", 0, 99, 99) == 0);
      ut::expect(vfs::syscall::do_access(&table, "tree", 1, 99, 99) == denied);
      ut::expect(vfs::syscall::do_access(&table, "tree/leaf", 0, 99, 99) == denied);
      ut::expect(vfs::syscall::do_access(&table, "tree/leaf", 0, 99, 43) == 0);
      ut::expect(vfs::syscall::do_access(&table, "tree/leaf", 4, 99, 43) == denied);
      ut::expect(vfs::syscall::do_access(&table, "owner", 8, 42, 43) == -static_cast<long>(VfsError::InvalidArg));
      ut::expect(vfs::syscall::do_chdir(&table, "tree", 42, 43) == 0 &&
                 vfs::syscall::do_access(&table, "leaf", 6, 42, 43) == 0 && vfs::syscall::do_chdir(&table, "..") == 0);
      ut::expect(vfs::syscall::do_unlink("owner", &table) == 0 && vfs::syscall::do_unlink("group", &table) == 0 &&
                 vfs::syscall::do_unlink("exec", &table) == 0 && vfs::syscall::do_unlink("others", &table) == 0 &&
                 vfs::syscall::do_unlink("tree/leaf", &table) == 0 &&
                 vfs::syscall::do_rmdir("tree/empty", &table) == 0 && vfs::syscall::do_rmdir("tree", &table) == 0 &&
                 vfs::syscall::do_chdir(&table, "/") == 0 && vfs::syscall::do_rmdir("/access-arena") == 0);
    }
    if (!ut::expect(pool_usage() == baseline && mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap)) {
      return;
    }
  }
}

void working_directory_lifecycle() {
  const auto baseline = vfs::pool_usage();
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  auto root_references = [] {
    containers::LockGuard<containers::IrqSpinLock> guard(vfs::namespace_lock);
    auto *root = vfs::resolve_path_locked("/");
    return root ? root->ref_count : 0;
  };
  const auto root_refs = root_references();
  for (unsigned cycle = 0; cycle < 1000; ++cycle) {
    {
      vfs::FdTable table;
      if (!ut::expect(vfs::syscall::do_mkdir("/cwd-tree", 0710, 0, 43) == 0 &&
                      vfs::syscall::do_mkdir("/cwd-tree/leaf", 0755, 0, 43) == 0)) {
        return;
      }
      // Both the final directory and each searched ancestor require access.
      ut::expect(vfs::syscall::do_chdir(&table, "/cwd-tree", 99, 44) == -static_cast<long>(vfs::VfsError::PermDenied));
      ut::expect(vfs::syscall::do_chdir(&table, "/cwd-tree/leaf", 99, 44) ==
                 -static_cast<long>(vfs::VfsError::PermDenied));
      if (!ut::expect(vfs::syscall::do_chdir(&table, "/cwd-tree/leaf", 99, 43) == 0)) {
        return;
      }
      auto *child = table.clone();
      if (!ut::expect(child != nullptr)) {
        return;
      }
      char path[vfs::MAX_PATH_LEN];
      auto output = vfs::OutputBuffer::kernel(path, sizeof(path));
      ut::expect(vfs::syscall::do_getcwd(child, output) == 0 && ut::same_id(path, "/cwd-tree/leaf"));
      ut::expect(vfs::syscall::do_rename("/cwd-tree", "/cwd-moved") == 0 &&
                 vfs::syscall::do_getcwd(child, output) == 0 && ut::same_id(path, "/cwd-moved/leaf"));
      ut::expect(vfs::syscall::do_rmdir("/cwd-moved/leaf") == 0 && vfs::syscall::do_rmdir("/cwd-moved") == 0);
      vfs::Stat status{};
      ut::expect(vfs::syscall::do_getcwd(child, output) == -static_cast<long>(vfs::VfsError::NoEntry) &&
                 vfs::syscall::do_stat(".", &status, child) == 0 && status.st_nlink == 0);
      ut::expect(vfs::syscall::do_open(child, "ghost", vfs::O_CREAT | vfs::O_WRONLY, 0600) ==
                 -static_cast<long>(vfs::VfsError::NoEntry));
      ut::expect(vfs::syscall::do_chdir(&table, "/") == 0 && vfs::pool_usage().dentries == baseline.dentries + 2);
      // The child still owns both detached directories, even after its parent
      // leaves. Moving upward must not read a freed/reused parent pointer.
      ut::expect(vfs::syscall::do_chdir(child, "..") == 0 &&
                 vfs::syscall::do_getcwd(child, output) == -static_cast<long>(vfs::VfsError::NoEntry));
      delete child;
    }
    if (!ut::expect(root_references() == root_refs && vfs::pool_usage() == baseline &&
                    mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap)) {
      return;
    }
  }
}

void directory_capacity() {
  const auto baseline = vfs::pool_usage();
  vfs::FdTable table;
  ut::expect(vfs::syscall::do_open(&table, "/", vfs::O_WRONLY, 0) == -static_cast<long>(vfs::VfsError::IsDirectory));
  ut::expect(vfs::syscall::do_open(&table, "/", vfs::O_RDWR, 0) == -static_cast<long>(vfs::VfsError::IsDirectory));
  ut::expect(vfs::syscall::do_open(&table, "/", 3, 0) == -static_cast<long>(vfs::VfsError::InvalidArg));
  table.close_all();
  ut::expect(vfs::pool_usage() == baseline);
  vfs::Stat root{}, full{}, current{};
  ut::expect(vfs::syscall::do_stat("/", &root) == 0);
  if (!ut::expect(vfs::syscall::do_mkdir("/capacity", 0700, 0, 43) == 0)) {
    return;
  }
  char path[] = "/capacity/00";
  u32 created = 0;
  for (; created < vfs::Inode::MAX_CHILDREN; ++created) {
    path[10] = static_cast<char>('0' + created / 10);
    path[11] = static_cast<char>('0' + created % 10);
    if (!ut::expect(vfs::syscall::do_mkdir(path, 0700, 0, 43) == 0)) {
      break;
    }
  }
  ut::expect(vfs::syscall::do_stat("/capacity", &full) == 0);
  ut::expect(full.st_nlink == created + 2 && full.st_uid == 0 && full.st_gid == 43);
  if (created == vfs::Inode::MAX_CHILDREN) {
    const auto capacity = vfs::pool_usage();
    for (unsigned attempt = 0; attempt < 300; ++attempt) {
      ut::expect(vfs::syscall::do_mkdir("/capacity/overflow", 0700, 0, 0) ==
                 -static_cast<long>(vfs::VfsError::NoMemory));
      ut::expect(vfs::pool_usage() == capacity);
    }
    ut::expect(vfs::syscall::do_mkdir(path, 0700, 0, 0) == -static_cast<long>(vfs::VfsError::FileExists));
    ut::expect(vfs::syscall::do_rmdir("/capacity") == -static_cast<long>(vfs::VfsError::NotEmpty));
    ut::expect(vfs::syscall::do_stat("/capacity", &current) == 0 && current.st_nlink == full.st_nlink);
    ut::expect(vfs::pool_usage() == capacity);
  }
  while (created) {
    --created;
    path[10] = static_cast<char>('0' + created / 10);
    path[11] = static_cast<char>('0' + created % 10);
    ut::expect(vfs::syscall::do_rmdir(path) == 0);
  }
  long held = vfs::syscall::do_open(&table, "/capacity", 0, 0);
  if (ut::expect(held == 0)) {
    auto *file = table.get_file(held);
    for (u32 fd = 1; fd < vfs::MAX_FDS; ++fd) {
      ut::expect(vfs::syscall::do_dup(&table, held) == static_cast<long>(fd));
    }
    const auto installed = vfs::pool_usage();
    const auto inode_refs = file->inode->ref_count, dentry_refs = file->dentry->ref_count;
    for (unsigned attempt = 0; attempt < 300; ++attempt) {
      ut::expect(vfs::syscall::do_open(&table, "/capacity", 0, 0) == -static_cast<long>(vfs::VfsError::TooManyFiles));
      ut::expect(vfs::pool_usage() == installed);
      ut::expect(file->inode->ref_count == inode_refs && file->dentry->ref_count == dentry_refs);
    }
    ut::expect(vfs::syscall::do_rmdir("/capacity") == 0);
    ut::expect(vfs::syscall::do_fstat(&table, held, &current) == 0 && current.st_ino == full.st_ino &&
               current.st_nlink == 0);
    ut::expect(vfs::syscall::do_mkdir("/capacity", 0700, 0, 0) == 0);
    ut::expect(vfs::syscall::do_stat("/capacity", &current) == 0 && current.st_ino != full.st_ino);
    ut::expect(vfs::pool_usage().inodes == baseline.inodes + 2 && vfs::pool_usage().dentries == baseline.dentries + 2);
  }
  table.close_all();
  ut::expect(vfs::syscall::do_rmdir("/capacity") == 0);
  ut::expect(vfs::pool_usage() == baseline);
  ut::expect(vfs::syscall::do_stat("/", &current) == 0 && current.st_nlink == root.st_nlink);
}

} // namespace

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
