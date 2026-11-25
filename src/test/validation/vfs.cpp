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

namespace {
bool console_copy_irqs = false;
usize console_copy_remaining = 0;

usize console_irq_copy(void *target, u64 source, usize count) noexcept {
  console_copy_irqs = console_copy_irqs && arch::interrupts_enabled();
  const usize copied = count < console_copy_remaining ? count : console_copy_remaining;
  console_copy_remaining -= copied;
  __builtin_memcpy(target, reinterpret_cast<const void *>(source), copied);
  return count - copied;
}
} // namespace

void console_write_irqs() {
  vfs::FdTable table;
  const long fd = vfs::syscall::do_open(&table, "/dev/console", vfs::O_WRONLY, 0);
  if (!ut::expect(fd >= 0)) {
    return;
  }
  // Two bytes exercise no progress, partial output and full success. Newlines
  // keep actual UART output separate from the validation protocol records.
  constexpr char bytes[] = {'\n', '\n'};
  constexpr bool entry_states[] = {false, true};
  const bool original_irqs = arch::interrupts_enabled();
  for (const bool enabled : entry_states) {
    for (usize limit = 0; limit <= sizeof(bytes); ++limit) {
      console_copy_irqs = true;
      console_copy_remaining = limit;
      if (enabled) {
        arch::enable_interrupts();
      } else {
        arch::disable_interrupts();
      }
      const long result = vfs::syscall::do_write(
          &table, fd, vfs::InputBuffer::user(reinterpret_cast<u64>(bytes), sizeof(bytes), console_irq_copy));
      const bool restored = arch::interrupts_enabled() == enabled;
      if (original_irqs) {
        arch::enable_interrupts();
      } else {
        arch::disable_interrupts();
      }
      ut::expect(console_copy_irqs);
      ut::expect(restored);
      ut::expect(result == (limit ? static_cast<long>(limit) : -static_cast<long>(vfs::VfsError::BadAddress)));
    }
  }
  ut::expect(vfs::syscall::do_close(&table, fd) == 0);
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

  vfs::VfsError path_error = vfs::VfsError::NoEntry;
  ut::expect(vfs::resolve_path_locked(nullptr, &path_error, nullptr, 0, 0) == nullptr);
  ut::expect(vfs::resolve_path_locked("", &path_error, nullptr, 0, 0) == nullptr);
  auto *root = vfs::resolve_path("/");
  auto *file_dentry = vfs::resolve_path("/fixture.bin");
  if (ut::expect(root != nullptr && file_dentry != nullptr)) {
    ut::expect(vfs::resolve_path_locked(".", &path_error, root, 0, 0) == root);
    ut::expect(vfs::resolve_path_locked("..", &path_error, root, 0, 0) != nullptr);
    ut::expect(vfs::resolve_path_locked("fixture.bin/", &path_error, nullptr, 0, 0) == nullptr);
    ut::expect(path_error == vfs::VfsError::NotDirectory);
    ut::expect(vfs::resolve_path_locked("child", &path_error, file_dentry, 0, 0) == nullptr);
    ut::expect(path_error == vfs::VfsError::NotDirectory);
    vfs::Dentry no_inode_dentry{};
    path_error = vfs::VfsError::NoEntry;
    ut::expect(vfs::resolve_path_locked("child", &path_error, &no_inode_dentry, 0, 0) == nullptr);
    ut::expect(path_error == vfs::VfsError::NotDirectory);
    vfs::Inode denied_inode{};
    denied_inode.type = vfs::FileType::Directory;
    denied_inode.mode = vfs::S_IFDIR | 0700;
    denied_inode.uid = 1000;
    denied_inode.gid = 1000;
    denied_inode.nlink = 1;
    vfs::Dentry denied{};
    denied.inode = &denied_inode;
    denied.ref_count = 1;
    path_error = vfs::VfsError::NoEntry;
    ut::expect(vfs::resolve_path_locked("child", &path_error, &denied, 1001, 1001) == nullptr);
    ut::expect(path_error == vfs::VfsError::PermDenied);
    char long_component[vfs::MAX_NAME_LEN + 2]{};
    for (usize i = 0; i < vfs::MAX_NAME_LEN + 1; ++i) {
      long_component[i] = 'x';
    }
    path_error = vfs::VfsError::NoEntry;
    ut::expect(vfs::resolve_path_locked(long_component, &path_error, root, 0, 0) == nullptr);
    ut::expect(path_error == vfs::VfsError::NameTooLong);
  }
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

void pipe_edges() {
  vfs::FdTable table;
  long ends[2] = {-1, -1};
  if (!ut::expect(vfs::syscall::do_pipe(&table, ends) == 0)) {
    return;
  }
  auto *reader = table.get_file(ends[0]);
  auto *writer = table.get_file(ends[1]);
  if (!ut::expect(reader != nullptr && writer != nullptr)) {
    table.close_all();
    return;
  }
  u8 payload[vfs::PIPE_BUF_SIZE];
  for (usize i = 0; i < sizeof(payload); ++i) {
    payload[i] = static_cast<u8>(i);
  }
  ut::expect(vfs::syscall::do_read(&table, ends[0], vfs::OutputBuffer::kernel(payload, 0)) == 0);
  ut::expect(vfs::syscall::do_write(&table, ends[1], vfs::InputBuffer::kernel(payload, 0)) == 0);
  ut::expect(vfs::syscall::do_lseek(&table, ends[0], 0, 0) == -static_cast<long>(vfs::VfsError::IsPipe));
  ut::expect(vfs::syscall::do_write(&table, ends[0], vfs::InputBuffer::kernel(reader, 1)) ==
             -static_cast<long>(vfs::VfsError::BadFd));
  ut::expect(vfs::syscall::do_read(&table, ends[1], vfs::OutputBuffer::kernel(writer, 1)) ==
             -static_cast<long>(vfs::VfsError::BadFd));
  reader->flags |= vfs::O_NONBLOCK;
  writer->flags |= vfs::O_NONBLOCK;
  ut::expect(vfs::syscall::do_write(&table, ends[1], vfs::InputBuffer::kernel(payload, sizeof(payload))) ==
             static_cast<long>(sizeof(payload)));
  ut::expect(vfs::syscall::do_write(&table, ends[1], vfs::InputBuffer::kernel(payload, 1)) ==
             -static_cast<long>(vfs::VfsError::WouldBlock));
  u8 received[32]{};
  ut::expect(vfs::syscall::do_read(&table, ends[0], vfs::OutputBuffer::kernel(received, sizeof(received))) ==
             static_cast<long>(sizeof(received)));
  ut::expect(vfs::syscall::do_read(&table, ends[0], vfs::OutputBuffer::kernel(payload, sizeof(payload))) ==
             static_cast<long>(sizeof(payload) - sizeof(received)));
  ut::expect(vfs::syscall::do_write(&table, ends[1], vfs::InputBuffer::kernel(payload, sizeof(payload))) ==
             static_cast<long>(sizeof(payload)));
  ut::expect(vfs::syscall::do_close(&table, ends[0]) == 0);
  ut::expect(vfs::syscall::do_write(&table, ends[1], vfs::InputBuffer::kernel(payload, 1)) ==
             -static_cast<long>(vfs::VfsError::BrokenPipe));
  ut::expect(vfs::syscall::do_close(&table, ends[1]) == 0);

  ut::expect(vfs::syscall::do_pipe(nullptr, ends) == -static_cast<long>(vfs::VfsError::InvalidArg));
  ut::expect(vfs::syscall::do_pipe(&table, vfs::OutputBuffer::kernel(nullptr, 0)) ==
             -static_cast<long>(vfs::VfsError::InvalidArg));
  table.close_all();
}

void devfs_edges() {
  vfs::FdTable table;
  const long null_fd = vfs::syscall::do_open(&table, "/dev/null", vfs::O_RDWR, 0);
  const long zero_fd = vfs::syscall::do_open(&table, "/dev/zero", vfs::O_RDWR, 0);
  const long console_ro = vfs::syscall::do_open(&table, "/dev/console", vfs::O_RDONLY, 0);
  const long console_wo = vfs::syscall::do_open(&table, "/dev/console", vfs::O_WRONLY, 0);
  if (!ut::expect(null_fd >= 0 && zero_fd >= 0 && console_ro >= 0 && console_wo >= 0)) {
    table.close_all();
    return;
  }
  u8 bytes[512]{};
  ut::expect(vfs::syscall::do_read(&table, null_fd, vfs::OutputBuffer::kernel(bytes, sizeof(bytes))) == 0);
  ut::expect(vfs::syscall::do_write(&table, null_fd, vfs::InputBuffer::kernel(bytes, sizeof(bytes))) ==
             static_cast<long>(sizeof(bytes)));
  ut::expect(vfs::syscall::do_lseek(&table, null_fd, 0, 0) == -static_cast<long>(vfs::VfsError::IsPipe));
  ut::expect(vfs::syscall::do_read(&table, zero_fd, vfs::OutputBuffer::kernel(bytes, sizeof(bytes))) ==
             static_cast<long>(sizeof(bytes)));
  bool zeroed = true;
  for (u8 byte : bytes) {
    zeroed = zeroed && byte == 0;
  }
  ut::expect(zeroed);
  ut::expect(vfs::syscall::do_read(&table, zero_fd, vfs::OutputBuffer::kernel(bytes, 0)) == 0);
  ut::expect(vfs::syscall::do_write(&table, zero_fd, vfs::InputBuffer::kernel(bytes, sizeof(bytes))) ==
             static_cast<long>(sizeof(bytes)));
  ut::expect(vfs::syscall::do_lseek(&table, zero_fd, 0, 0) == -static_cast<long>(vfs::VfsError::IsPipe));
  ut::expect(vfs::syscall::do_ioctl(&table, console_ro, vfs::MOSS_IOCTL_TRY_GETC, 0) < 0);
  ut::expect(vfs::syscall::do_ioctl(&table, console_ro, vfs::MOSS_IOCTL_TRY_GETC, 1) ==
             -static_cast<long>(vfs::VfsError::InvalidArg));
  ut::expect(vfs::syscall::do_ioctl(&table, console_wo, vfs::MOSS_IOCTL_TRY_GETC, 0) ==
             -static_cast<long>(vfs::VfsError::BadFd));
  ut::expect(vfs::syscall::do_ioctl(&table, console_ro, vfs::MOSS_IOCTL_ISATTY, 0) == 0);
  ut::expect(vfs::syscall::do_ioctl(&table, console_ro, vfs::MOSS_IOCTL_ISATTY, 1) ==
             -static_cast<long>(vfs::VfsError::InvalidArg));
  ut::expect(vfs::syscall::do_ioctl(&table, console_ro, 0x1234, 0) ==
             -static_cast<long>(vfs::VfsError::NotTerminal));
  ut::expect(vfs::syscall::do_lseek(&table, console_ro, 0, 0) == -static_cast<long>(vfs::VfsError::IsPipe));
  vfs::Stat status{};
  ut::expect(vfs::syscall::do_fstat(&table, null_fd, &status) == 0 && (status.st_mode & vfs::S_IFMT) == vfs::S_IFCHR);
  ut::expect(vfs::syscall::do_fstat(&table, zero_fd, &status) == 0 && (status.st_mode & vfs::S_IFMT) == vfs::S_IFCHR);

  const long dev = vfs::syscall::do_open(&table, "/dev", vfs::O_RDONLY, 0);
  if (ut::expect(dev >= 0)) {
    vfs::DirEntry entry{};
    u32 visited = 0;
    for (;;) {
      const long result = vfs::syscall::do_getdents(&table, dev, vfs::OutputBuffer::kernel(&entry, sizeof(entry)));
      if (result <= 0) {
        break;
      }
      ut::expect(result == static_cast<long>(sizeof(entry)));
      ++visited;
    }
    ut::expect(visited >= 3);
    ut::expect(vfs::syscall::do_close(&table, dev) == 0);
  }
  table.close_all();
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

void syscall_edges() {
  vfs::FdTable table;
  constexpr auto err = [](vfs::VfsError value) { return -static_cast<long>(value); };
  ut::expect(vfs::syscall::do_chdir(nullptr, "/", 0, 0) == err(vfs::VfsError::InvalidArg));
  ut::expect(vfs::syscall::do_chdir(&table, "/fixture.bin", 0, 0) == err(vfs::VfsError::NotDirectory));
  ut::expect(vfs::syscall::do_access(&table, "/fixture.bin", 2, 0, 0) == err(vfs::VfsError::PermDenied));
  ut::expect(vfs::syscall::do_access(&table, "/fixture.bin", 8, 0, 0) == err(vfs::VfsError::InvalidArg));
  ut::expect(vfs::syscall::do_access(&table, "/missing", 1, 0, 0) == err(vfs::VfsError::NoEntry));

  ut::expect(vfs::syscall::do_open(nullptr, "/fixture.bin", 0, 0, 0, 0) == err(vfs::VfsError::InvalidArg));
  ut::expect(vfs::syscall::do_open(&table, nullptr, 0, 0, 0, 0) == err(vfs::VfsError::InvalidArg));
  ut::expect(vfs::syscall::do_open(&table, "/fixture.bin", vfs::O_ACCMODE, 0, 0, 0) ==
             err(vfs::VfsError::InvalidArg));
  ut::expect(vfs::syscall::do_open(&table, "/fixture.bin", 0x100, 0, 0, 0) == err(vfs::VfsError::NotSupported));
  ut::expect(vfs::syscall::do_open(&table, "/fixture.bin", vfs::O_TRUNC | vfs::O_RDONLY, 0, 0, 0) ==
             err(vfs::VfsError::InvalidArg));
  ut::expect(vfs::syscall::do_open(&table, "/fixture.bin", vfs::O_CREAT | vfs::O_EXCL, 0600, 0, 0) ==
             err(vfs::VfsError::FileExists));
  ut::expect(vfs::syscall::do_open(&table, "/newfile/", vfs::O_CREAT | vfs::O_RDWR, 0600, 0, 0) ==
             err(vfs::VfsError::IsDirectory));

  vfs::Stat status{};
  ut::expect(vfs::syscall::do_stat(nullptr, nullptr, &table, 0, 0) == err(vfs::VfsError::InvalidArg));
  ut::expect(vfs::syscall::do_stat("/missing", &status, &table, 0, 0) == err(vfs::VfsError::NoEntry));
  ut::expect(vfs::syscall::do_stat("/fixture.bin", &status, &table, 0, 0) == 0 && status.st_dev != 0);
  ut::expect(vfs::syscall::do_fstat(nullptr, 0, &status) == err(vfs::VfsError::InvalidArg));
  ut::expect(vfs::syscall::do_fstat(&table, 8, &status) == err(vfs::VfsError::BadFd));

  ut::expect(vfs::syscall::do_getcwd(nullptr, vfs::OutputBuffer::kernel(&status, sizeof(status))) ==
             err(vfs::VfsError::InvalidArg));
  ut::expect(vfs::syscall::do_getcwd(&table, vfs::OutputBuffer::kernel(&status, 0)) == err(vfs::VfsError::InvalidArg));
  if (ut::expect(vfs::syscall::do_chdir(&table, "/dev", 0, 0) == 0)) {
    ut::expect(vfs::syscall::do_getcwd(&table, vfs::OutputBuffer::kernel(&status, 4)) == err(vfs::VfsError::Range));
    auto fail_copy = [](u64, const void *, usize count) noexcept -> usize { return count; };
    ut::expect(vfs::syscall::do_getcwd(&table, vfs::OutputBuffer::user(0x1000, 8, fail_copy)) ==
               err(vfs::VfsError::BadAddress));
    ut::expect(vfs::syscall::do_chdir(&table, "/", 0, 0) == 0);
  }

  const long file_fd = vfs::syscall::do_open(&table, "/fixture.bin", vfs::O_RDONLY, 0, 0, 0);
  const long dev_fd = vfs::syscall::do_open(&table, "/dev", vfs::O_RDONLY, 0, 0, 0);
  if (!ut::expect(file_fd >= 0 && dev_fd >= 0)) {
    table.close_all();
    return;
  }
  alignas(8) u8 directory_entry[sizeof(vfs::DirEntry)]{};
  ut::expect(vfs::syscall::do_getdents(&table, file_fd,
                                       vfs::OutputBuffer::kernel(directory_entry, sizeof(directory_entry))) ==
             err(vfs::VfsError::NotDirectory));
  ut::expect(vfs::syscall::do_getdents(&table, dev_fd, vfs::OutputBuffer::kernel(nullptr, 1)) ==
             err(vfs::VfsError::BadAddress));
  ut::expect(vfs::syscall::do_getdents(&table, dev_fd, vfs::OutputBuffer::kernel(&status, sizeof(status) - 1)) ==
             err(vfs::VfsError::InvalidArg));
  auto fail_copy = [](u64, const void *, usize count) noexcept -> usize { return count; };
  ut::expect(vfs::syscall::do_getdents(&table, dev_fd, vfs::OutputBuffer::user(0x1000, sizeof(vfs::DirEntry), fail_copy)) ==
             err(vfs::VfsError::BadAddress));
  table.get_file(dev_fd)->pos = -1;
  ut::expect(vfs::syscall::do_getdents(&table, dev_fd, vfs::OutputBuffer::kernel(&status, sizeof(vfs::DirEntry))) ==
             err(vfs::VfsError::InvalidArg));
  ut::expect(vfs::syscall::do_lseek(&table, file_fd, 0, 99) == err(vfs::VfsError::InvalidArg));
  long pipe_fds[2] = {-1, -1};
  ut::expect(vfs::syscall::do_pipe(&table, vfs::OutputBuffer::kernel(pipe_fds, sizeof(pipe_fds) - 1)) ==
             err(vfs::VfsError::InvalidArg));
  ut::expect(vfs::syscall::do_pipe(&table, vfs::OutputBuffer::kernel(nullptr, sizeof(pipe_fds))) ==
             err(vfs::VfsError::BadAddress));

  ut::expect(vfs::syscall::do_mkdir("/", 0755, 0, 0, &table) == err(vfs::VfsError::FileExists));
  ut::expect(vfs::syscall::do_mkdir(".", 0755, 0, 0, &table) == err(vfs::VfsError::FileExists));
  char long_name[vfs::MAX_NAME_LEN + 3]{};
  long_name[0] = '/';
  for (u32 i = 1; i <= vfs::MAX_NAME_LEN + 1; ++i) {
    long_name[i] = 'a';
  }
  ut::expect(vfs::syscall::do_mkdir(long_name, 0755, 0, 0, &table) == err(vfs::VfsError::NameTooLong));
  ut::expect(vfs::syscall::do_rmdir("/fixture.bin", &table, 0, 0) == err(vfs::VfsError::NotDirectory));
  ut::expect(vfs::syscall::do_unlink("/", &table, 0, 0) == err(vfs::VfsError::IsDirectory));
  ut::expect(vfs::syscall::do_rmdir("/", &table, 0, 0) == err(vfs::VfsError::Busy));
  ut::expect(vfs::syscall::do_unlink("/missing", &table, 0, 0) == err(vfs::VfsError::NoEntry));
  ut::expect(vfs::syscall::do_rename("/missing", "/other", &table, 0, 0) == err(vfs::VfsError::NoEntry));
  ut::expect(vfs::syscall::do_rename("/fixture.bin", "/missing/x", &table, 0, 0) == err(vfs::VfsError::NoEntry));

  vfs::MountLookupResult mount{.mount = nullptr, .residual = nullptr, .residual_len = 0};
  ut::expect(vfs::g_mount_table.lookup("/dev/null", mount) && mount.residual_len > 0);
  ut::expect(vfs::g_mount_table.lookup("/device", mount));
  ut::expect(!vfs::g_mount_table.lookup("", mount));
  ut::expect(vfs::g_mount_table.root_path(nullptr) == nullptr);
  ut::expect(vfs::g_mount_table.parent_of_root(nullptr) == nullptr);
  ut::expect(vfs::g_mount_table.child_mount(nullptr, "x", 1) == nullptr);

  ut::expect(vfs::devfs::devfs_init() != nullptr);
  ut::expect(vfs::ramfs::ramfs_init() != nullptr);

  table.close_all();

  // A bounded directory rejects children past its inline capacity; reclaim
  // them so later suites see the same namespace.
  ut::expect(vfs::syscall::do_mkdir("/pool", 0755, 0, 0, &table) == 0);
  u32 made = 0;
  char path[32]{};
  for (u32 i = 0; i < vfs::Inode::MAX_CHILDREN + 2; ++i) {
    for (u32 j = 0; j < sizeof(path); ++j) {
      path[j] = 0;
    }
    path[0] = '/';
    path[1] = 'p';
    path[2] = 'o';
    path[3] = 'o';
    path[4] = 'l';
    path[5] = '/';
    u32 digits = 0;
    u32 value = i;
    char reversed[8]{};
    do {
      reversed[digits++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value);
    for (u32 d = 0; d < digits; ++d) {
      path[6 + d] = reversed[digits - 1 - d];
    }
    if (vfs::syscall::do_mkdir(path, 0755, 0, 0, &table) != 0) {
      break;
    }
    ++made;
  }
  ut::expect(made > 0 && made <= vfs::Inode::MAX_CHILDREN);
  for (u32 i = 0; i < made; ++i) {
    for (u32 j = 0; j < sizeof(path); ++j) {
      path[j] = 0;
    }
    path[0] = '/';
    path[1] = 'p';
    path[2] = 'o';
    path[3] = 'o';
    path[4] = 'l';
    path[5] = '/';
    u32 digits = 0;
    u32 value = i;
    char reversed[8]{};
    do {
      reversed[digits++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value);
    for (u32 d = 0; d < digits; ++d) {
      path[6 + d] = reversed[digits - 1 - d];
    }
    ut::expect(vfs::syscall::do_rmdir(path, &table, 0, 0) == 0);
  }
  ut::expect(vfs::syscall::do_rmdir("/pool", &table, 0, 0) == 0);
}

void syscall_error_paths() {
  auto *table = new vfs::FdTable();
  if (!ut::expect(table != nullptr)) {
    return;
  }
  vfs::Stat status{};
  vfs::DirEntry entry{};
  u8 byte = 0;
  const auto output = vfs::OutputBuffer::kernel(&byte, sizeof(byte));
  const auto input = vfs::InputBuffer::kernel(&byte, sizeof(byte));
  const long invalid = -static_cast<long>(vfs::VfsError::InvalidArg);
  const long bad_fd = -static_cast<long>(vfs::VfsError::BadFd);
  ut::expect(!vfs::InputBuffer::user(0, 1, nullptr).valid());
  ut::expect(!vfs::InputBuffer::user(~u64{0}, 1, nullptr).valid());
  ut::expect(vfs::InputBuffer::kernel(&byte, 1).valid());
  ut::expect(vfs::InputBuffer::kernel(&byte, 1).copy_to(2, &byte, 1) == 0);
  ut::expect(vfs::InputBuffer::kernel(&byte, 1).copy_to(0, &byte, 2) == 0);
  ut::expect(vfs::InputBuffer::kernel(&byte, 1).copy_to(0, &byte, 0) == 0);
  ut::expect(!vfs::OutputBuffer::user(0, 1, nullptr).valid());
  ut::expect(!vfs::OutputBuffer::user(~u64{0}, 1, nullptr).valid());
  ut::expect(vfs::OutputBuffer::kernel(&byte, 1).valid());
  ut::expect(vfs::OutputBuffer::kernel(&byte, 1).copy_from(2, &byte, 1) == 0);
  ut::expect(vfs::OutputBuffer::kernel(&byte, 1).copy_from(0, &byte, 2) == 0);
  ut::expect(vfs::OutputBuffer::kernel(&byte, 1).copy_from(0, &byte, 0) == 0);

  auto *local = new vfs::FdTable();
  if (!ut::expect(local != nullptr)) {
    return;
  }
  local->init();
  ut::expect(local->duplicate(-1) == bad_fd);
  ut::expect(local->duplicate_to(0, -1) == bad_fd);
  ut::expect(local->duplicate_to(0, vfs::MAX_FDS) == bad_fd);
  ut::expect(local->acquire_file(-1).get() == nullptr);
  ut::expect(local->acquire_file(vfs::MAX_FDS).get() == nullptr);
  ut::expect(local->get_file(-1) == nullptr && local->get_file(vfs::MAX_FDS) == nullptr);
  ut::expect(local->descriptor_flags(-1) == bad_fd);
  ut::expect(local->set_descriptor_flags(-1, 0) == bad_fd);
  ut::expect(local->close_fd(-1) == bad_fd);
  local->close_on_exec();
  ut::expect(vfs::syscall::do_read(nullptr, 0, output) == invalid);
  ut::expect(vfs::syscall::do_read(table, 999, output) == bad_fd);
  ut::expect(vfs::syscall::do_read(table, 0, vfs::OutputBuffer::kernel(nullptr, 1)) == invalid);
  ut::expect(vfs::syscall::do_write(nullptr, 0, input) == invalid);
  ut::expect(vfs::syscall::do_write(table, 999, input) == bad_fd);
  ut::expect(vfs::syscall::do_write(table, 0, vfs::InputBuffer::kernel(nullptr, 1)) == invalid);
  ut::expect(vfs::syscall::do_lseek(nullptr, 0, 0, 0) == invalid);
  ut::expect(vfs::syscall::do_lseek(table, 999, 0, 0) == bad_fd);
  ut::expect(vfs::syscall::do_close(nullptr, 0) == invalid);
  ut::expect(vfs::syscall::do_close(table, 999) == bad_fd);
  ut::expect(vfs::syscall::do_fstat(nullptr, 0, &status) == invalid);
  ut::expect(vfs::syscall::do_fstat(table, 999, &status) == bad_fd);
  ut::expect(vfs::syscall::do_fcntl(nullptr, 0, 1, 0) == bad_fd);
  ut::expect(vfs::syscall::do_dup(nullptr, 0) == invalid);
  ut::expect(vfs::syscall::do_dup2(nullptr, 0, 1) == invalid);
  ut::expect(vfs::syscall::do_getdents(nullptr, 0, vfs::OutputBuffer::kernel(&entry, sizeof(entry))) == bad_fd);
  ut::expect(vfs::syscall::do_getdents(table, 999, vfs::OutputBuffer::kernel(&entry, sizeof(entry))) == bad_fd);
  ut::expect(vfs::syscall::do_stat(nullptr, &status, table, 0, 0) == invalid);
  ut::expect(vfs::syscall::do_stat("/missing-coverage", &status, table, 0, 0) ==
             -static_cast<long>(vfs::VfsError::NoEntry));
  ut::expect(vfs::syscall::do_rename(nullptr, "/other", table, 0, 0) < 0);
  ut::expect(vfs::syscall::do_rename("/fixture.bin", nullptr, table, 0, 0) < 0);
  ut::expect(vfs::syscall::do_rename("/fixture.bin", "/missing/x", table, 0, 0) ==
             -static_cast<long>(vfs::VfsError::NoEntry));
  ut::expect(vfs::syscall::do_mkdir(nullptr, 0755, 0, 0, table) < 0);
  ut::expect(vfs::syscall::do_rmdir(".", table, 0, 0) == invalid);
  ut::expect(vfs::syscall::do_rmdir("..", table, 0, 0) == invalid);
  ut::expect(vfs::syscall::do_unlink("/", table, 0, 0) == -static_cast<long>(vfs::VfsError::IsDirectory));
  ut::expect(vfs::syscall::do_rmdir("/", table, 0, 0) == -static_cast<long>(vfs::VfsError::Busy));
  ut::expect(vfs::syscall::do_open(nullptr, "/fixture.bin", 0, 0) == invalid);
  ut::expect(vfs::syscall::do_open(table, nullptr, 0, 0) == invalid);

  long pipefd[2] = {-1, -1};
  ut::expect(vfs::syscall::do_pipe(table, nullptr) == invalid);
  ut::expect(vfs::syscall::do_pipe(table, pipefd) == 0);
  auto *read_end = table->get_file(pipefd[0]);
  auto *write_end = table->get_file(pipefd[1]);
  if (ut::expect(read_end != nullptr && write_end != nullptr)) {
    ut::expect(read_end->f_ops->release(nullptr) == 0);
    ut::expect(write_end->f_ops->release(nullptr) == 0);
    ut::expect(read_end->f_ops->read(nullptr, output) == -static_cast<long>(vfs::VfsError::InvalidArg));
    ut::expect(write_end->f_ops->write(nullptr, input) == -static_cast<long>(vfs::VfsError::InvalidArg));
  }
  (void)vfs::syscall::do_close(table, pipefd[0]);
  (void)vfs::syscall::do_close(table, pipefd[1]);

  // File operation table gaps are reported as unsupported, not as bad fds.
  // An empty, blocking-capable pipe must not wait without a scheduler.
  {
    long empty_pipe[2] = {-1, -1};
    if (ut::expect(vfs::syscall::do_pipe(table, empty_pipe) == 0)) {
      auto *reader = table->get_file(empty_pipe[0]);
      auto *writer = table->get_file(empty_pipe[1]);
      if (ut::expect(reader != nullptr && writer != nullptr)) {
        ut::expect(reader->f_ops->write(reader, input) == -static_cast<long>(vfs::VfsError::BadFd));
        ut::expect(writer->f_ops->read(writer, output) == -static_cast<long>(vfs::VfsError::BadFd));
        reader->flags |= vfs::O_NONBLOCK;
        ut::expect(reader->f_ops->read(reader, output) == -static_cast<long>(vfs::VfsError::WouldBlock));
        auto *saved_scheduler = process::g_scheduler;
        process::g_scheduler = nullptr;
        reader->flags &= ~vfs::O_NONBLOCK;
        ut::expect(reader->f_ops->read(reader, output) == -static_cast<long>(vfs::VfsError::NotSupported));
        process::g_scheduler = saved_scheduler;
        reader->flags &= ~vfs::O_NONBLOCK;
      }
      (void)vfs::syscall::do_close(table, empty_pipe[0]);
      (void)vfs::syscall::do_close(table, empty_pipe[1]);
    }
  }

  const long file_fd = vfs::syscall::do_open(table, "/fixture.bin", vfs::O_RDWR, 0);
  if (ut::expect(file_fd >= 0)) {
    ut::expect(table->duplicate(file_fd, -1) == invalid);
    ut::expect(table->duplicate(file_fd, vfs::MAX_FDS) == invalid);
    auto *file = table->get_file(file_fd);
    if (ut::expect(file != nullptr)) {
      const auto *saved_ops = file->f_ops;
      auto *saved_inode = file->inode;
      const i64 saved_pos = file->pos;
      file->f_ops = nullptr;
      ut::expect(vfs::syscall::do_read(table, file_fd, output) == -static_cast<long>(vfs::VfsError::NotSupported));
      ut::expect(vfs::syscall::do_write(table, file_fd, input) == -static_cast<long>(vfs::VfsError::NotSupported));
      file->f_ops = saved_ops;
      file->inode = nullptr;
      ut::expect(vfs::syscall::do_fstat(table, file_fd, &status) == -static_cast<long>(vfs::VfsError::InvalidArg));
      ut::expect(saved_ops->read(file, output) == -static_cast<long>(vfs::VfsError::InvalidArg));
      ut::expect(saved_ops->lseek(file, 0, vfs::SeekWhence::Set) == -static_cast<long>(vfs::VfsError::InvalidArg));
      file->inode = saved_inode;
      file->pos = saved_pos;
    }
    ut::expect(vfs::syscall::do_close(table, file_fd) == 0);
  }
  const long mutable_fd =
      vfs::syscall::do_open(table, "/mutable-probe", vfs::O_CREAT | vfs::O_RDWR | vfs::O_TRUNC, 0600);
  if (ut::expect(mutable_fd >= 0)) {
    auto *mutable_file = table->get_file(mutable_fd);
    if (ut::expect(mutable_file != nullptr && mutable_file->inode != nullptr)) {
      const auto *ops = mutable_file->f_ops;
      mutable_file->pos = -1;
      ut::expect(ops->write(mutable_file, input) == -static_cast<long>(vfs::VfsError::InvalidArg));
      mutable_file->pos = 0;
      ut::expect(ops->write(mutable_file, vfs::InputBuffer::kernel(&byte, 0)) == 0);
      const u64 saved_size = mutable_file->inode->size;
      mutable_file->inode->size = 0x8000000000000000ULL;
      ut::expect(ops->lseek(mutable_file, 0, vfs::SeekWhence::End) == -static_cast<long>(vfs::VfsError::Overflow));
      mutable_file->inode->size = saved_size;
    }
    ut::expect(vfs::syscall::do_close(table, mutable_fd) == 0);
    ut::expect(vfs::syscall::do_unlink("/mutable-probe", table, 0, 0) == 0);
  }
  (void)vfs::devfs::console_file_ops();
  vfs::free_file(nullptr);
  vfs::g_dcache.insert(nullptr);

  // A locally owned mount table can reach its fixed capacity without touching
  // the live namespace.
  {
    vfs::MountTable mounts;
    mounts.init();
    vfs::MountLookupResult lookup{};
    ut::expect(!mounts.lookup("/", lookup));
    vfs::SuperBlock super{};
    vfs::Dentry root{};
    root.ref_count = 1;
    ut::expect(mounts.mount("/missing/child", &super, &root) == -static_cast<long>(vfs::VfsError::NotDirectory));
    for (u32 index = 0; index < vfs::MAX_MOUNTS; ++index) {
      char path[8] = "/m0";
      path[2] = static_cast<char>('0' + static_cast<char>(index));
      ut::expect(mounts.mount(path, &super, &root) == 0);
    }
    ut::expect(mounts.mount("/overflow", &super, &root) == -static_cast<long>(vfs::VfsError::NoMemory));
  }

  // A deep relative cwd overflows getcwd's fixed path buffer and fails closed.
  char long_name[vfs::MAX_NAME_LEN + 1];
  for (usize i = 0; i < vfs::MAX_NAME_LEN; ++i) {
    long_name[i] = 'x';
  }
  long_name[vfs::MAX_NAME_LEN] = 0;
  ut::expect(vfs::syscall::do_chdir(table, "/") == 0);
  for (unsigned depth = 0; depth < 6; ++depth) {
    ut::expect(vfs::syscall::do_mkdir(long_name, 0700, 0, 0, table) == 0);
    ut::expect(vfs::syscall::do_chdir(table, long_name) == 0);
  }
  u8 cwd_buffer[vfs::MAX_PATH_LEN]{};
  ut::expect(vfs::syscall::do_getcwd(table, vfs::OutputBuffer::kernel(cwd_buffer, sizeof(cwd_buffer))) ==
             -static_cast<long>(vfs::VfsError::NameTooLong));
  ut::expect(vfs::syscall::do_chdir(table, "/") == 0);

  char oversized_path[vfs::MAX_PATH_LEN];
  for (usize i = 0; i < sizeof(oversized_path) - 1; ++i) {
    oversized_path[i] = 'y';
  }
  oversized_path[sizeof(oversized_path) - 1] = 0;
  ut::expect(vfs::syscall::do_mkdir(oversized_path, 0700, 0, 0, table) ==
             -static_cast<long>(vfs::VfsError::NameTooLong));
  ut::expect(vfs::syscall::do_mkdir("/fixture.bin/child", 0700, 0, 0, table) ==
             -static_cast<long>(vfs::VfsError::NotDirectory));
  ut::expect(vfs::syscall::do_mkdir("/dev/child", 0700, 0, 0, table) ==
             -static_cast<long>(vfs::VfsError::NotSupported));
  ut::expect(vfs::syscall::do_rename("/dev/null", "/dev/null2", table, 0, 0) ==
             -static_cast<long>(vfs::VfsError::NotSupported));
  ut::expect(vfs::syscall::do_unlink("/dev/null", table, 0, 0) ==
             -static_cast<long>(vfs::VfsError::NotSupported));

  // Synthetic children exercise the wire type table and inode overflow guard.
  auto *root_dentry = vfs::resolve_path("/");
  if (ut::expect(root_dentry != nullptr && root_dentry->inode != nullptr)) {
    auto *root_inode = root_dentry->inode;
    const u32 saved_count = root_inode->child_count;
    auto *saved_first = root_inode->children[0];
    vfs::Dentry fake{};
    vfs::Inode fake_inode{};
    fake.name[0] = 'z';
    fake.name[1] = 0;
    fake.name_len = 1;
    fake.inode = &fake_inode;
    fake.parent = root_dentry;
    fake.ref_count = 1;
    fake_inode.type = vfs::FileType::Regular;
    fake_inode.mode = vfs::S_IFREG | 0644;
    fake_inode.nlink = 1;
    fake_inode.ino = 5;
    fake_inode.ref_count = 1;
    fake_inode.file_ops = nullptr;
    root_inode->children[0] = &fake;
    root_inode->child_count = 1;

    constexpr vfs::FileType types[] = {vfs::FileType::Regular, vfs::FileType::Directory, vfs::FileType::CharDev,
                                       vfs::FileType::BlockDev, vfs::FileType::Fifo, vfs::FileType::Socket,
                                       vfs::FileType::Symlink};
    constexpr u8 wire_types[] = {8, 4, 2, 6, 1, 12, 10};
    for (usize index = 0; index < sizeof(types) / sizeof(types[0]); ++index) {
      fake_inode.type = types[index];
      const long dir_fd = vfs::syscall::do_open(table, "/", vfs::O_RDONLY, 0);
      if (!ut::expect(dir_fd >= 0)) {
        break;
      }
      ut::expect(vfs::syscall::do_getdents(table, dir_fd, vfs::OutputBuffer::kernel(&entry, sizeof(entry))) ==
                 sizeof(entry));
      ut::expect(vfs::syscall::do_getdents(table, dir_fd, vfs::OutputBuffer::kernel(&entry, sizeof(entry))) ==
                 sizeof(entry));
      ut::expect(vfs::syscall::do_getdents(table, dir_fd, vfs::OutputBuffer::kernel(&entry, sizeof(entry))) ==
                 sizeof(entry));
      ut::expect(entry.type == wire_types[index] && entry.name[0] == 'z');
      (void)vfs::syscall::do_close(table, dir_fd);
    }
    fake_inode.type = static_cast<vfs::FileType>(7);
    {
      const long dir_fd = vfs::syscall::do_open(table, "/", vfs::O_RDONLY, 0);
      if (ut::expect(dir_fd >= 0)) {
        ut::expect(vfs::syscall::do_getdents(table, dir_fd, vfs::OutputBuffer::kernel(&entry, sizeof(entry))) ==
                   sizeof(entry));
        ut::expect(vfs::syscall::do_getdents(table, dir_fd, vfs::OutputBuffer::kernel(&entry, sizeof(entry))) ==
                   sizeof(entry));
        ut::expect(vfs::syscall::do_getdents(table, dir_fd, vfs::OutputBuffer::kernel(&entry, sizeof(entry))) ==
                   -static_cast<long>(vfs::VfsError::IoError));
        (void)vfs::syscall::do_close(table, dir_fd);
      }
    }
    fake_inode.type = vfs::FileType::Regular;
    fake_inode.ino = 0x7ffffffffffffffdULL;
    {
      const long dir_fd = vfs::syscall::do_open(table, "/", vfs::O_RDONLY, 0);
      if (ut::expect(dir_fd >= 0)) {
        ut::expect(vfs::syscall::do_getdents(table, dir_fd, vfs::OutputBuffer::kernel(&entry, sizeof(entry))) ==
                   sizeof(entry));
        ut::expect(vfs::syscall::do_getdents(table, dir_fd, vfs::OutputBuffer::kernel(&entry, sizeof(entry))) ==
                   sizeof(entry));
        ut::expect(vfs::syscall::do_getdents(table, dir_fd, vfs::OutputBuffer::kernel(&entry, sizeof(entry))) ==
                   -static_cast<long>(vfs::VfsError::Overflow));
        (void)vfs::syscall::do_close(table, dir_fd);
      }
    }

    static const vfs::FileOps failing_ops = {
        .open = +[](vfs::File *, vfs::Inode *, u32) noexcept -> long { return -static_cast<long>(vfs::VfsError::IoError); },
        .release = nullptr,
        .read = nullptr,
        .write = nullptr,
        .lseek = nullptr,
        .ioctl = nullptr};
    fake.ref_count = 1;
    fake_inode.ref_count = 1;
    fake_inode.ino = 9;
    fake_inode.file_ops = &failing_ops;
    ut::expect(vfs::syscall::do_open(table, "/z", vfs::O_RDONLY, 0) ==
               -static_cast<long>(vfs::VfsError::IoError));

    root_inode->children[0] = saved_first;
    root_inode->child_count = saved_count;
  }
  // Exhaust the fixed pipe-state pool, which is smaller than the fd table->
  {
    long pipe_ends[2] = {-1, -1};
    u32 created = 0;
    for (; created < 128; ++created) {
      if (vfs::syscall::do_pipe(table, pipe_ends) != 0) {
        break;
      }
    }
    ut::expect(created == 64);
    ut::expect(vfs::syscall::do_pipe(table, pipe_ends) == -static_cast<long>(vfs::VfsError::NoMemory));
    table->close_all();
    ut::expect(vfs::syscall::do_pipe(table, pipe_ends) == 0);
    (void)vfs::syscall::do_close(table, pipe_ends[0]);
    (void)vfs::syscall::do_close(table, pipe_ends[1]);
  }
  table->close_all();
}

void rename_lifecycle();
void rename_boundaries();
void access_permissions();
void working_directory_lifecycle();
void directory_capacity();

void ramfs_errors() {
  vfs::FdTable table;
  ut::expect(vfs::syscall::do_open(&table, "/fixture.bin", vfs::O_WRONLY | vfs::O_TRUNC, 0) ==
             -static_cast<long>(vfs::VfsError::PermDenied));
  const long file = vfs::syscall::do_open(&table, "/mutable", vfs::O_CREAT | vfs::O_RDWR, 0644);
  if (ut::expect(file >= 0)) {
    auto *entry = table.get_file(file);
    if (ut::expect(entry != nullptr)) {
      entry->pos = 0x7fffffffffffffffLL;
      ut::expect(vfs::syscall::do_lseek(&table, file, 1, 1) == -static_cast<long>(vfs::VfsError::Overflow));
      entry->pos = 0;
      entry->inode->size = 0x7fffffffffffffffULL;
      ut::expect(vfs::syscall::do_lseek(&table, file, 1, 2) == -static_cast<long>(vfs::VfsError::Overflow));
      entry->inode->size = 0;
    }
    ut::expect(vfs::syscall::do_close(&table, file) == 0);
  }
  ut::expect(vfs::syscall::do_open(&table, "/dev/missing", vfs::O_RDONLY, 0) ==
             -static_cast<long>(vfs::VfsError::NoEntry));
  vfs::Stat status{};
  ut::expect(vfs::syscall::do_stat("/dev/missing", &status, nullptr, 0, 0) ==
             -static_cast<long>(vfs::VfsError::NoEntry));
  ut::expect(vfs::syscall::do_access(&table, "/dev/zero", 8, 0, 0) ==
             -static_cast<long>(vfs::VfsError::InvalidArg));
  table.close_all();
}

void pool_exhaustion() {
  auto *owner = vfs::alloc_inode();
  if (!ut::expect(owner != nullptr)) {
    return;
  }
  char long_name[vfs::MAX_NAME_LEN + 2]{};
  for (usize i = 0; i < vfs::MAX_NAME_LEN + 1; ++i) {
    long_name[i] = 'x';
  }
  ut::expect(vfs::alloc_dentry(long_name, vfs::MAX_NAME_LEN + 1, owner, nullptr) == nullptr);

  char name[2] = {'d', '\0'};
  u32 dentries = 0;
  for (u32 i = 0; i < 4096; ++i) {
    if (vfs::alloc_dentry(name, 1, owner, nullptr) == nullptr) {
      break;
    }
    ++dentries;
  }
  ut::expect(dentries > 0);
  vfs::FdTable table;
  ut::expect(vfs::syscall::do_mkdir("/pool-full", 0755, 0, 0, &table) == -static_cast<long>(vfs::VfsError::NoMemory));

  u32 inodes = 0;
  for (u32 i = 0; i < 4096; ++i) {
    if (vfs::alloc_inode() == nullptr) {
      break;
    }
    ++inodes;
  }
  ut::expect(inodes > 0);
  vfs::File *read_file = nullptr;
  vfs::File *write_file = nullptr;
  ut::expect(vfs::pipefs::create_pipe(read_file, write_file) == -static_cast<long>(vfs::VfsError::NoMemory));
  table.close_all();
}

void register_vfs_cases() {
  ut::register_suite("vfs", [] {
    ut::register_test("read_position_eof", file_read);
    ut::register_test("errors_readonly", file_errors);
    ut::register_test("console_write_irqs", console_write_irqs);
    ut::register_test("fd_boundaries", fd_boundaries);
    ut::register_test("pipe_reuse", pipe_reuse);
    ut::register_test("pipe_fd_rollback", pipe_fd_rollback);
    ut::register_test("pipe_edges", pipe_edges);
    ut::register_test("devfs_edges", devfs_edges);
    ut::register_test("directory_capacity", directory_capacity);
    ut::register_test("writable_lifecycle", writable_lifecycle);
    ut::register_test("rename_lifecycle", rename_lifecycle);
    ut::register_test("rename_boundaries", rename_boundaries);
    ut::register_test("access_permissions", access_permissions);
    ut::register_test("working_directory_lifecycle", working_directory_lifecycle);
    ut::register_test("syscall_edges", syscall_edges);
    ut::register_test("syscall_error_paths", syscall_error_paths);
    ut::register_test("ramfs_errors", ramfs_errors);
    ut::register_test("pool_exhaustion", pool_exhaustion);
  });
}
} // namespace moss::test::validation
