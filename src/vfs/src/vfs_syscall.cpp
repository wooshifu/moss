// MOSS VFS syscall handler — implementation
// Bridges kernel syscall dispatch to VFS operations.

module;

module moss.vfs;

namespace moss::kernel::vfs::syscall {

long do_open(void *fd_table_ptr, const char *path, u32 flags, [[maybe_unused]] u32 mode) noexcept {
  if (fd_table_ptr == nullptr || path == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }

  auto *fdt = static_cast<FdTable *>(fd_table_ptr);

  // Resolve path to dentry
  Dentry *dentry = resolve_path(path);
  if (dentry == nullptr) {
    return -static_cast<long>(VfsError::NoEntry);
  }

  Inode *inode = dentry->inode;
  if (inode == nullptr) {
    return -static_cast<long>(VfsError::NoEntry);
  }

  // Allocate a File object
  File *file = alloc_file();
  if (file == nullptr) {
    return -static_cast<long>(VfsError::NoMemory);
  }

  file->inode = inode;
  file->dentry = dentry;
  file->f_ops = inode->file_ops;
  file->flags = flags;
  file->pos = 0;
  file->private_data = nullptr;

  inode->ref();

  // Call file_ops->open if provided
  if (file->f_ops != nullptr && file->f_ops->open != nullptr) {
    long ret = file->f_ops->open(file, inode, flags);
    if (ret < 0) {
      inode->unref();
      free_file(file);
      return ret;
    }
  }

  // Install into fd table
  long fd = fdt->alloc_fd(file);
  if (fd < 0) {
    if (file->f_ops != nullptr && file->f_ops->release != nullptr) {
      file->f_ops->release(file);
    }
    inode->unref();
    free_file(file);
  }
  return fd;
}

long do_close(void *fd_table_ptr, long fd) noexcept {
  if (fd_table_ptr == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  return fdt->close_fd(fd);
}

long do_read(void *fd_table_ptr, long fd, u8 *buf, usize count) noexcept {
  if (fd_table_ptr == nullptr || buf == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  File *file = fdt->get_file(fd);
  if (file == nullptr) {
    return -static_cast<long>(VfsError::BadFd);
  }
  if (file->f_ops == nullptr || file->f_ops->read == nullptr) {
    return -static_cast<long>(VfsError::NotSupported);
  }
  return file->f_ops->read(file, buf, count);
}

long do_write(void *fd_table_ptr, long fd, const u8 *buf, usize count) noexcept {
  if (fd_table_ptr == nullptr || buf == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  File *file = fdt->get_file(fd);
  if (file == nullptr) {
    return -static_cast<long>(VfsError::BadFd);
  }
  if (file->f_ops == nullptr || file->f_ops->write == nullptr) {
    return -static_cast<long>(VfsError::NotSupported);
  }
  return file->f_ops->write(file, buf, count);
}

long do_lseek(void *fd_table_ptr, long fd, i64 offset, u32 whence) noexcept {
  if (fd_table_ptr == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  File *file = fdt->get_file(fd);
  if (file == nullptr) {
    return -static_cast<long>(VfsError::BadFd);
  }
  if (file->f_ops == nullptr || file->f_ops->lseek == nullptr) {
    return -static_cast<long>(VfsError::NotSupported);
  }
  return file->f_ops->lseek(file, offset, static_cast<SeekWhence>(whence));
}

long do_fstat(void *fd_table_ptr, long fd, void *stat_buf) noexcept {
  if (fd_table_ptr == nullptr || stat_buf == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  File *file = fdt->get_file(fd);
  if (file == nullptr) {
    return -static_cast<long>(VfsError::BadFd);
  }

  Inode *inode = file->inode;
  if (inode == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }

  auto *st = static_cast<Stat *>(stat_buf);
  st->st_ino = inode->ino;
  st->st_mode = inode->mode;
  st->st_nlink = inode->nlink;
  st->st_size = inode->size;
  st->st_rdev = inode->rdev;
  return 0;
}

long do_dup(void *fd_table_ptr, long oldfd) noexcept {
  if (fd_table_ptr == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  File *file = fdt->get_file(oldfd);
  if (file == nullptr) {
    return -static_cast<long>(VfsError::BadFd);
  }
  return fdt->alloc_fd(file);
}

long do_dup2(void *fd_table_ptr, long oldfd, long newfd) noexcept {
  if (fd_table_ptr == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  File *file = fdt->get_file(oldfd);
  if (file == nullptr) {
    return -static_cast<long>(VfsError::BadFd);
  }
  if (oldfd == newfd)
    return newfd;
  return fdt->install_fd(newfd, file);
}

long do_pipe(void *fd_table_ptr, long *pipefd) noexcept {
  if (fd_table_ptr == nullptr || pipefd == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }

  File *read_file = nullptr;
  File *write_file = nullptr;
  long ret = pipefs::create_pipe(read_file, write_file);
  if (ret < 0)
    return ret;

  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  long rfd = fdt->alloc_fd(read_file);
  if (rfd < 0) {
    free_file(read_file);
    free_file(write_file);
    return rfd;
  }

  long wfd = fdt->alloc_fd(write_file);
  if (wfd < 0) {
    fdt->close_fd(rfd);
    free_file(write_file);
    return wfd;
  }

  pipefd[0] = rfd;
  pipefd[1] = wfd;
  return 0;
}

} // namespace moss::kernel::vfs::syscall
