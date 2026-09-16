// MOSS VFS syscall handler — implementation
// Bridges kernel syscall dispatch to VFS operations.

module;

module moss.vfs;

// Validation can pause the caller after descriptor publication, with no table
// lock held. The production image keeps this observation point empty.
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_pipe_published(void * /*unused*/) noexcept {}
// Observe copyout while both slots remain reserved, without the FD lock held.
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_pipe_copyout(void * /*unused*/, long /*unused*/,
                                                                          long /*unused*/) noexcept {}
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_dup_selected(void * /*unused*/) noexcept {}

namespace moss::kernel::vfs::syscall {

static long create_node_locked(const char *path, FileType type, u32 mode, u32 uid, u32 gid, Dentry *&created,
                               Dentry *base) noexcept;

static Dentry *working_directory(void *table) noexcept {
  return table ? static_cast<FdTable *>(table)->working_directory() : nullptr;
}

long do_access(void *fd_table_ptr, const char *path, long mode, u32 uid, u32 gid) noexcept {
  if (mode < 0 || mode > 7) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
  VfsError error = VfsError::NoEntry;
  auto *entry = resolve_path_locked(path, &error, working_directory(fd_table_ptr), uid, gid);
  if (!entry || !entry->inode) {
    return -static_cast<long>(error);
  }
  const auto &inode = *entry->inode;
  // Immutable initramfs regular files cannot be modified, even by root.
  if ((mode & 2) && inode.is_regular() && !inode.ramfs_mutable) {
    return -static_cast<long>(VfsError::PermDenied);
  }
  return can_access(inode, uid, gid, static_cast<u32>(mode)) ? 0 : -static_cast<long>(VfsError::PermDenied);
}

long do_chdir(void *fd_table_ptr, const char *path, u32 uid, u32 gid) noexcept {
  if (!fd_table_ptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
  VfsError error = VfsError::NoEntry;
  auto *directory = resolve_path_locked(path, &error, working_directory(fd_table_ptr), uid, gid);
  if (!directory) {
    return -static_cast<long>(error);
  }
  if (!directory->inode || !directory->inode->is_directory()) {
    return -static_cast<long>(VfsError::NotDirectory);
  }
  if (!can_search(*directory->inode, uid, gid)) {
    return -static_cast<long>(VfsError::PermDenied);
  }
  static_cast<FdTable *>(fd_table_ptr)->set_working_directory(directory);
  return 0;
}

long do_getcwd(void *fd_table_ptr, OutputBuffer buffer) noexcept {
  if (!fd_table_ptr || !buffer.size()) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  char path[MAX_PATH_LEN];
  usize begin = sizeof(path) - 1;
  path[begin] = 0;
  {
    containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
    auto *cwd = static_cast<FdTable *>(fd_table_ptr)->working_directory();
    if (!cwd) {
      cwd = resolve_path_locked("/");
    }
    while (cwd && cwd->parent) {
      if (!cwd->inode || !cwd->inode->nlink) {
        return -static_cast<long>(VfsError::NoEntry);
      }
      if (cwd->name_len + 1 > begin) {
        return -static_cast<long>(VfsError::NameTooLong);
      }
      begin -= cwd->name_len;
      __builtin_memcpy(path + begin, cwd->name, cwd->name_len);
      path[--begin] = '/';
      cwd = cwd->parent;
    }
    const char *prefix = g_mount_table.root_path(cwd);
    if (!prefix) {
      return -static_cast<long>(VfsError::NoEntry);
    }
    usize length = __builtin_strlen(prefix);
    if (length == 1 && begin < sizeof(path) - 1) {
      length = 0; // The first component already has the root slash.
    }
    if (length > begin) {
      return -static_cast<long>(VfsError::NameTooLong);
    }
    begin -= length;
    __builtin_memcpy(path + begin, prefix, length);
  }
  const usize size = sizeof(path) - begin;
  if (buffer.size() < size) {
    return -static_cast<long>(VfsError::Range); // Includes the terminating NUL.
  }
  return buffer.copy_from(0, path + begin, size) != size ? -static_cast<long>(VfsError::BadAddress) : 0;
}

long do_open(void *fd_table_ptr, const char *path, u32 flags, u32 mode, u32 uid, u32 gid) noexcept {
  if (fd_table_ptr == nullptr || path == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  if ((flags & O_ACCMODE) == O_ACCMODE) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  if (flags & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_CLOEXEC)) {
    return -static_cast<long>(VfsError::NotSupported);
  }
  if ((flags & O_TRUNC) && (flags & O_ACCMODE) == O_RDONLY) {
    return -static_cast<long>(VfsError::InvalidArg);
  }

  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  // Reserve before creation/truncation; a concurrent allocator cannot turn a
  // completed filesystem mutation into an EMFILE failure at publication.
  FdTable::Reservation slot(*fdt);
  if (slot.fd() < 0) {
    return slot.fd();
  }

  // Allocate a File object
  File *file = alloc_file();
  if (file == nullptr) {
    return -static_cast<long>(VfsError::NoMemory);
  }

  {
    containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
    VfsError error = VfsError::NoEntry;
    Dentry *dentry = resolve_path_locked(path, &error, fdt->working_directory(), uid, gid);
    if (!dentry && error != VfsError::NoEntry) {
      free_file(file);
      return -static_cast<long>(error);
    }
    if (dentry && (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
      free_file(file);
      return -static_cast<long>(VfsError::FileExists);
    }
    // Existing files require the requested access before a driver can truncate
    // them. A newly created file's mode does not restrict its initial open.
    // Native access masks: read=4, write=2, read+write=6, independent of O_* bits.
    u32 requested = 6;
    if ((flags & O_ACCMODE) == O_RDONLY) {
      requested = 4;
    } else if ((flags & O_ACCMODE) == O_WRONLY) {
      requested = 2;
    }
    if (dentry && dentry->inode && !can_access(*dentry->inode, uid, gid, requested)) {
      free_file(file);
      return -static_cast<long>(VfsError::PermDenied);
    }
    if (!dentry && (flags & O_CREAT)) {
      const long result = create_node_locked(path, FileType::Regular, mode, uid, gid, dentry, fdt->working_directory());
      if (result < 0) {
        free_file(file);
        return result;
      }
    }
    if (!dentry || !dentry->inode) {
      free_file(file);
      return -static_cast<long>(VfsError::NoEntry);
    }
    if (dentry->inode->is_directory() && (flags & O_ACCMODE) != O_RDONLY) {
      free_file(file);
      return -static_cast<long>(VfsError::IsDirectory);
    }
    file->inode = dentry->inode;
    file->dentry = dentry;
    file->f_ops = dentry->inode->file_ops;
    file->inode->ref();
    dentry->ref();
  }
  file->flags = flags;
  file->pos = 0;
  file->private_data = nullptr;

  // Call file_ops->open if provided
  if (file->f_ops != nullptr && file->f_ops->open != nullptr) {
    long ret = file->f_ops->open(file, file->inode, flags);
    if (ret < 0) {
      file->f_ops = nullptr; // Failed open did not establish a driver endpoint.
      release_file(file);
      return ret;
    }
  }

  // Install into fd table
  long fd = slot.install(file, (flags & O_CLOEXEC) != 0);
  release_file(file); // Drop the opener's reference, whether installed or not.
  return fd;
}

long do_close(void *fd_table_ptr, long fd) noexcept {
  if (fd_table_ptr == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  return fdt->close_fd(fd);
}

long do_read(void *fd_table_ptr, long fd, OutputBuffer buffer) noexcept {
  if (fd_table_ptr == nullptr || !buffer.valid()) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  auto file = fdt->acquire_file(fd);
  if (!file) {
    return -static_cast<long>(VfsError::BadFd);
  }
  if ((file->flags & O_ACCMODE) == O_WRONLY) {
    return -static_cast<long>(VfsError::BadFd);
  }
  if (file->f_ops == nullptr || file->f_ops->read == nullptr) {
    return -static_cast<long>(VfsError::NotSupported);
  }
  return file->f_ops->read(file.get(), buffer);
}

long do_write(void *fd_table_ptr, long fd, InputBuffer buffer) noexcept {
  if (fd_table_ptr == nullptr || !buffer.valid()) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  auto file = fdt->acquire_file(fd);
  if (!file) {
    return -static_cast<long>(VfsError::BadFd);
  }
  if ((file->flags & O_ACCMODE) == O_RDONLY) {
    return -static_cast<long>(VfsError::BadFd);
  }
  if (file->f_ops == nullptr || file->f_ops->write == nullptr) {
    return -static_cast<long>(VfsError::NotSupported);
  }
  return file->f_ops->write(file.get(), buffer);
}

long do_lseek(void *fd_table_ptr, long fd, i64 offset, u32 whence) noexcept {
  if (fd_table_ptr == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  auto file = fdt->acquire_file(fd);
  if (!file) {
    return -static_cast<long>(VfsError::BadFd);
  }
  if (file->f_ops == nullptr || file->f_ops->lseek == nullptr) {
    return -static_cast<long>(VfsError::NotSupported);
  }
  return file->f_ops->lseek(file.get(), offset, static_cast<SeekWhence>(whence));
}

static void inode_stat(const Inode &inode, Stat &st) noexcept {
  st = {};
  st.st_ino = inode.ino;
  st.st_mode = inode.mode;
  st.st_nlink = inode.nlink;
  st.st_size = inode.size;
  st.st_rdev = inode.rdev;
  st.st_uid = inode.uid;
  st.st_gid = inode.gid;
  st.st_dev = inode.sb ? inode.sb->device : NO_DEVICE;
}

long do_stat(const char *path, void *stat_buf, void *fd_table_ptr, u32 uid, u32 gid) noexcept {
  if (!path || !stat_buf) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
  VfsError error = VfsError::NoEntry;
  auto *dentry = resolve_path_locked(path, &error, working_directory(fd_table_ptr), uid, gid);
  if (!dentry || !dentry->inode) {
    return -static_cast<long>(error);
  }
  inode_stat(*dentry->inode, *static_cast<Stat *>(stat_buf));
  return 0;
}

static long resolve_parent_locked(const char *path, Dentry *&parent, char *name, u32 &name_length, bool &trailing_slash,
                                  Dentry *base, u32 uid, u32 gid) noexcept {
  if (!path || !*path) {
    return -static_cast<long>(VfsError::NoEntry);
  }
  char parent_path[MAX_PATH_LEN];
  usize length = 0;
  while (path[length]) {
    if (length == sizeof(parent_path) - 1) {
      return -static_cast<long>(VfsError::NameTooLong);
    }
    parent_path[length] = path[length];
    ++length;
  }
  trailing_slash = parent_path[length - 1] == '/';
  while (length > 1 && parent_path[length - 1] == '/') {
    --length;
  }
  parent_path[length] = 0;
  usize name_start = length;
  while (name_start && parent_path[name_start - 1] != '/') {
    --name_start;
  }
  name_length = static_cast<u32>(length - name_start);
  if (!name_length) {
    return -static_cast<long>(VfsError::Busy);
  }
  if (name_length > MAX_NAME_LEN) {
    return -static_cast<long>(VfsError::NameTooLong);
  }
  __builtin_memcpy(name, parent_path + name_start, name_length + 1);
  parent_path[name_start] = 0;
  VfsError error = VfsError::NoEntry;
  parent = resolve_path_locked(name_start ? parent_path : ".", &error, base, uid, gid);
  if (!parent || !parent->inode) {
    return -static_cast<long>(error);
  }
  auto *dir = parent->inode;
  if (!dir->nlink) {
    return -static_cast<long>(VfsError::NoEntry);
  }
  if (!dir->is_directory()) {
    return -static_cast<long>(VfsError::NotDirectory);
  }
  // Both callers modify a child entry: creation and rename require W+X here.
  if (!can_access(*dir, uid, gid, 3)) {
    return -static_cast<long>(VfsError::PermDenied);
  }
  if ((name_length == 1 && name[0] == '.') || (name_length == 2 && name[0] == '.' && name[1] == '.')) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  return 0;
}

static long create_node_locked(const char *path, FileType type, u32 mode, u32 uid, u32 gid, Dentry *&created,
                               Dentry *base) noexcept {
  Dentry *parent = nullptr;
  char name[MAX_NAME_LEN + 1];
  u32 name_length = 0;
  bool trailing_slash = false;
  long resolved = resolve_parent_locked(path, parent, name, name_length, trailing_slash, base, uid, gid);
  if (resolved == -static_cast<long>(VfsError::Busy) || resolved == -static_cast<long>(VfsError::InvalidArg)) {
    return -static_cast<long>(VfsError::FileExists);
  }
  if (resolved < 0) {
    return resolved;
  }
  if (trailing_slash && type != FileType::Directory) {
    return -static_cast<long>(VfsError::IsDirectory);
  }
  auto *dir = parent->inode;
  if (!dir->inode_ops || !dir->inode_ops->create) {
    return -static_cast<long>(VfsError::NotSupported);
  }
  long result = dir->inode_ops->create(dir, name, name_length, type, mode);
  if (!result) {
    created = dir->inode_ops->lookup(dir, name, name_length);
    created->inode->uid = uid;
    created->inode->gid = gid;
  }
  return result;
}

long do_rename(const char *old_path, const char *new_path, void *fd_table_ptr, u32 uid, u32 gid) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
  Dentry *old_parent = nullptr, *new_parent = nullptr;
  char old_name[MAX_NAME_LEN + 1], new_name[MAX_NAME_LEN + 1];
  u32 old_length = 0, new_length = 0;
  bool old_slash = false, new_slash = false;
  auto *base = working_directory(fd_table_ptr);
  long result = resolve_parent_locked(old_path, old_parent, old_name, old_length, old_slash, base, uid, gid);
  if (result < 0) {
    return result;
  }
  result = resolve_parent_locked(new_path, new_parent, new_name, new_length, new_slash, base, uid, gid);
  if (result < 0) {
    return result;
  }
  VfsError error = VfsError::NoEntry;
  auto *source = resolve_path_locked(old_path, &error, base, uid, gid);
  if (!source || !source->inode) {
    return -static_cast<long>(error);
  }
  auto *target = resolve_path_locked(new_path, &error, base, uid, gid);
  if (!target && error != VfsError::NoEntry) {
    return -static_cast<long>(error);
  }
  // Mount roots are not directory entries owned by the apparent path parent.
  if (source->parent != old_parent || (target && target->parent != new_parent)) {
    return -static_cast<long>(VfsError::Busy);
  }
  if ((old_slash && !source->inode->is_directory()) || (new_slash && (!target || !target->inode->is_directory()))) {
    return -static_cast<long>(VfsError::NotDirectory);
  }
  if (source->inode->sb != new_parent->inode->sb) {
    return -static_cast<long>(VfsError::CrossDevice);
  }
  const auto *ops = old_parent->inode->inode_ops;
  if (!ops || !ops->rename) {
    return -static_cast<long>(VfsError::NotSupported);
  }
  return ops->rename(source, new_parent, new_name, new_length);
}

long do_mkdir(const char *path, u32 mode, u32 uid, u32 gid, void *fd_table_ptr) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
  Dentry *created = nullptr;
  return create_node_locked(path, FileType::Directory, mode, uid, gid, created, working_directory(fd_table_ptr));
}

static long remove_node(const char *path, bool directory, void *fd_table_ptr, u32 uid, u32 gid) noexcept {
  if (!path || !*path) {
    return -static_cast<long>(VfsError::NoEntry);
  }
  if (directory) {
    const char *end = path + __builtin_strlen(path);
    while (end > path && end[-1] == '/') {
      --end;
    }
    const char *last = end;
    while (last > path && last[-1] != '/') {
      --last;
    }
    if ((end - last == 1 && *last == '.') || (end - last == 2 && last[0] == '.' && last[1] == '.')) {
      return -static_cast<long>(VfsError::InvalidArg);
    }
  }
  containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
  VfsError error = VfsError::NoEntry;
  auto *dentry = resolve_path_locked(path, &error, working_directory(fd_table_ptr), uid, gid);
  if (!dentry || !dentry->inode) {
    return -static_cast<long>(error);
  }
  if (dentry->inode->is_directory() != directory) {
    return -static_cast<long>(directory ? VfsError::NotDirectory : VfsError::IsDirectory);
  }
  if (!dentry->parent) {
    return -static_cast<long>(VfsError::Busy);
  }
  auto *dir = dentry->parent->inode;
  if (!dir || !dir->inode_ops || !dir->inode_ops->remove) {
    return -static_cast<long>(VfsError::NotSupported);
  }
  if (!can_access(*dir, uid, gid, 3)) {
    return -static_cast<long>(VfsError::PermDenied);
  }
  return dir->inode_ops->remove(dir, dentry);
}

long do_rmdir(const char *path, void *fd_table_ptr, u32 uid, u32 gid) noexcept {
  return remove_node(path, true, fd_table_ptr, uid, gid);
}
long do_unlink(const char *path, void *fd_table_ptr, u32 uid, u32 gid) noexcept {
  return remove_node(path, false, fd_table_ptr, uid, gid);
}

long do_getdents(void *fd_table_ptr, long fd, OutputBuffer buffer) noexcept {
  auto *table = static_cast<FdTable *>(fd_table_ptr);
  auto file = table ? table->acquire_file(fd) : FileRef{};
  if (!file) {
    return -static_cast<long>(VfsError::BadFd);
  }
  if (!buffer.valid()) {
    return -static_cast<long>(VfsError::BadAddress);
  }
  if (buffer.size() < sizeof(DirEntry)) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
  auto *dir = file->inode;
  if (!dir || !dir->is_directory() || !file->dentry) {
    return -static_cast<long>(VfsError::NotDirectory);
  }
  if (file->pos < 0) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  Dentry *child = nullptr;
  // Cookies 0/1 emit "."/"..". Real children use inode + 2, and the next
  // cookie adds 3 so the next search starts at inode + 1 without revisiting it.
  if (file->pos < 2) {
    child = file->pos == 1 && file->dentry->parent ? file->dentry->parent : file->dentry;
  } else {
    // ponytail: inode cookies suit this bounded tree without hard links; use
    // per-dentry cookies if hard links are added. Array indices skip siblings
    // when recursive removal compacts the children during enumeration.
    const auto first_ino = static_cast<u64>(file->pos - 2);
    for (u32 index = 0; index < dir->child_count; ++index) {
      auto *candidate = dir->children[index];
      if (candidate->inode->ino >= first_ino && (!child || candidate->inode->ino < child->inode->ino)) {
        child = candidate;
      }
    }
    if (!child) {
      return 0;
    }
    // 2^63 - 4 leaves room for the +3 cookie in a signed 64-bit file position.
    if (child->inode->ino > 0x7ffffffffffffffcULL) {
      return -static_cast<long>(VfsError::Overflow);
    }
  }
  DirEntry entry{};
  entry.ino = child->inode->ino;
  entry.offset = file->pos < 2 ? file->pos + 1 : static_cast<i64>(child->inode->ino + 3);
  entry.record_size = sizeof(entry);
  // DT_* wire values match the pinned mlibc dirent prefix; the internal
  // FileType enum has different ordinals and must be translated explicitly.
  switch (child->inode->type) {
  case FileType::Regular:
    entry.type = 8;
    break;
  case FileType::Directory:
    entry.type = 4;
    break;
  case FileType::CharDev:
    entry.type = 2;
    break;
  case FileType::BlockDev:
    entry.type = 6;
    break;
  case FileType::Fifo:
    entry.type = 1;
    break;
  case FileType::Socket:
    entry.type = 12;
    break;
  case FileType::Symlink:
    entry.type = 10;
    break;
  default:
    return -static_cast<long>(VfsError::IoError);
  }
  if (file->pos < 2) {
    entry.name[0] = '.';
    if (file->pos == 1) {
      entry.name[1] = '.';
    }
  } else {
    __builtin_memcpy(entry.name, child->name, child->name_len + 1);
  }
  if (buffer.copy_from(0, &entry, sizeof(entry)) != sizeof(entry)) {
    return -static_cast<long>(VfsError::BadAddress);
  }
  file->pos = entry.offset; // A failed copyout leaves the entry available for retry.
  return sizeof(entry);
}

long do_fstat(void *fd_table_ptr, long fd, void *stat_buf) noexcept {
  if (fd_table_ptr == nullptr || stat_buf == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  auto file = fdt->acquire_file(fd);
  if (!file) {
    return -static_cast<long>(VfsError::BadFd);
  }

  Inode *inode = file->inode;
  if (inode == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }

  containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
  inode_stat(*inode, *static_cast<Stat *>(stat_buf));
  return 0;
}

long do_ioctl(void *fd_table_ptr, long fd, long command, u64 argument) noexcept {
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  auto file = fdt ? fdt->acquire_file(fd) : FileRef{};
  if (!file) {
    return -static_cast<long>(VfsError::BadFd);
  }
  // Device operations accept a u32 command; reject high bits instead of silently
  // truncating a different userspace request into a supported 32-bit command.
  if (command < 0 || static_cast<u64>(command) > 0xffffffffULL) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  if (!file->f_ops || !file->f_ops->ioctl) {
    return -static_cast<long>(VfsError::NotTerminal);
  }
  return file->f_ops->ioctl(file.get(), static_cast<u32>(command), argument);
}

long do_fcntl(void *fd_table_ptr, long fd, long command, long argument) noexcept {
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  // F_* command numbers use the Linux-style native ABI shared with mlibc;
  // 1030 requests the duplicate with FD_CLOEXEC already installed atomically.
  if (command == 0 || command == 1030) { // F_DUPFD / F_DUPFD_CLOEXEC
    const long result = fdt ? fdt->duplicate(fd, argument, command == 1030) : -static_cast<long>(VfsError::BadFd);
    if (result >= 0) {
      moss_validation_dup_selected(fdt);
    }
    return result;
  }
  auto file = fdt ? fdt->acquire_file(fd) : FileRef{};
  if (!file) {
    return -static_cast<long>(VfsError::BadFd);
  }
  switch (command) {
  case 1: // F_GETFD
    return fdt->descriptor_flags(fd);
  case 2: // F_SETFD
    return fdt->set_descriptor_flags(fd, argument);
  case 3: // F_GETFL
    return file->flags & (O_ACCMODE | O_APPEND | O_NONBLOCK);
  default:
    return -static_cast<long>(VfsError::NotImplemented);
  }
}

long do_dup(void *fd_table_ptr, long oldfd) noexcept {
  if (fd_table_ptr == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  const long result = fdt->duplicate(oldfd);
  if (result >= 0) {
    moss_validation_dup_selected(fdt);
  }
  return result;
}

long do_dup2(void *fd_table_ptr, long oldfd, long newfd) noexcept {
  if (fd_table_ptr == nullptr) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  const long result = fdt->duplicate_to(oldfd, newfd);
  if (result >= 0) {
    moss_validation_dup_selected(fdt);
  }
  return result;
}

long do_pipe(void *fd_table_ptr, long *pipefd) noexcept {
  if (!pipefd) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  return do_pipe(fd_table_ptr, OutputBuffer::kernel(pipefd, 2 * sizeof(long)));
}

long do_pipe(void *fd_table_ptr, OutputBuffer buffer) noexcept {
  if (!fd_table_ptr || buffer.size() < 2 * sizeof(long)) {
    return -static_cast<long>(VfsError::InvalidArg);
  }
  if (!buffer.valid()) {
    return -static_cast<long>(VfsError::BadAddress);
  }

  auto *fdt = static_cast<FdTable *>(fd_table_ptr);
  FdTable::Reservation first(*fdt);
  if (first.fd() < 0) {
    return first.fd();
  }
  FdTable::Reservation second(*fdt);
  if (second.fd() < 0) {
    return second.fd();
  }

  File *read_file = nullptr;
  File *write_file = nullptr;
  long ret = pipefs::create_pipe(read_file, write_file);
  if (ret < 0) {
    return ret;
  }

  // The native pipe ABI copies two 64-bit longs (read end, write end), not
  // libc's two ints. Reservations remain invisible until the full copy succeeds;
  // a partial copy may expose numbers in user memory but installs neither fd.
  const long ends[2] = {first.fd(), second.fd()};
  moss_validation_pipe_copyout(fdt, ends[0], ends[1]);
  ret = buffer.copy_from(0, ends, sizeof(ends)) == sizeof(ends) ? first.install_pair(second, read_file, write_file)
                                                                : -static_cast<long>(VfsError::BadAddress);
  release_file(read_file);
  release_file(write_file);
  if (ret == 0) {
    moss_validation_pipe_published(fdt);
  }
  return ret;
}

} // namespace moss::kernel::vfs::syscall
