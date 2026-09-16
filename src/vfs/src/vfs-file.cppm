// MOSS VFS File Layer — File descriptor and FdTable
//
// File represents an open file description (shared across dup/fork).
// FdTable is the per-process file descriptor table mapping int fd → File*.

export module moss.vfs:file;

import moss.std;
import moss.types;
import moss.containers;
import :types;
import :inode;
import :dcache;

export namespace moss::kernel::vfs {

// ============================================================================
// File — open file description (like Linux struct file)
// ============================================================================

struct File {
  Inode *inode;          // backing inode
  Dentry *dentry;        // dentry used to open this file
  const FileOps *f_ops;  // cached from inode->file_ops at open time
  u32 flags;             // O_RDONLY / O_WRONLY / O_RDWR / ...
  i64 pos;               // current file position (lseek)
  void *private_data;    // driver/fs-specific (e.g. PipeState*)
  atomic<u32> ref_count; // owned by the allocator, then descriptors (dup, fork)

  // The caller must already hold a live reference (or serialize its owner).
  void ref() noexcept { ref_count.fetch_add(1, memory_order_relaxed); }
  [[nodiscard]] bool unref() noexcept { return ref_count.fetch_sub(1, memory_order_acq_rel) == 1; }
};

// ============================================================================
// File pool — simple static allocator for File objects
// ============================================================================

inline constexpr u32 MAX_FILES = 512;

/// File occupancy without acquiring namespace_lock (e.g. during namespace exclusion).
[[nodiscard]] u32 file_pool_usage() noexcept;

/// Allocate a File from the global file pool with one caller-owned reference.
[[nodiscard]] File *alloc_file() noexcept;

/// Return an unpublished or fully released File to the global file pool.
void free_file(File *file) noexcept;

/// Drop one owned reference, including the allocator's temporary reference.
/// The final release closes its endpoint and returns File/inode pool ownership.
void release_file(File *file) noexcept;

// A lookup-owned reference. It uses the File's existing counter, not a heap
// control block, and can outlive descriptor removal or reuse during I/O.
class FileRef {
public:
  FileRef() noexcept = default;
  FileRef(const FileRef &) = delete;
  FileRef &operator=(const FileRef &) = delete;
  ~FileRef() noexcept { release_file(file_); }
  [[nodiscard]] File *get() const noexcept { return file_; }
  File *operator->() const noexcept { return file_; }
  explicit operator bool() const noexcept { return file_ != nullptr; }

private:
  friend class FdTable;
  explicit FileRef(File *file) noexcept : file_(file) {
    if (file_)
      file_->ref();
  }
  File *file_ = nullptr;
};

// ============================================================================
// FdTable — per-process file descriptor table
// ============================================================================

/// Maps integer file descriptors [0..MAX_FDS) to File* pointers.
/// Designed to be embedded (via void*) in Process to avoid circular deps.
// ponytail: one lock serializes descriptor transactions; split only if measured
// descriptor contention requires it.
class FdTable {
public:
  FdTable() noexcept = default;
  FdTable(const FdTable &) = delete;
  FdTable &operator=(const FdTable &) = delete;
  // Moss currently owns one table per process. CWD shares its fork/exit
  // lifetime, but is independent of descriptors and survives close_on_exec.
  // The caller holds namespace_lock while borrowing the directory.
  [[nodiscard]] Dentry *working_directory() const noexcept { return cwd_; }
  void set_working_directory(Dentry *directory) noexcept {
    if (directory)
      directory->ref();
    release_dentry(cwd_);
    cwd_ = directory;
  }
  ~FdTable() noexcept;
  /// Initialize an unused table; not a reset of live descriptor ownership.
  void init() noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    for (u32 i = 0; i < MAX_FDS; ++i) {
      fds_[i] = nullptr;
      cloexec_[i] = false;
      reserved_[i] = false;
    }
  }

  // An unpublished slot, held without keeping the table lock across filesystem
  // work. Errors release it automatically; only a fully initialized File is
  // installed. The caller keeps the table alive until this owner is destroyed.
  class Reservation {
  public:
    explicit Reservation(FdTable &table) noexcept : table_(table) {
      containers::LockGuard<containers::IrqSpinLock> guard(table_.lock_);
      fd_ = table_.find_free_fd(0);
      if (fd_ >= 0)
        table_.reserved_[static_cast<u32>(fd_)] = true;
    }
    Reservation(const Reservation &) = delete;
    Reservation &operator=(const Reservation &) = delete;
    ~Reservation() noexcept {
      if (fd_ >= 0) {
        containers::LockGuard<containers::IrqSpinLock> guard(table_.lock_);
        table_.reserved_[static_cast<u32>(fd_)] = false;
      }
    }
    [[nodiscard]] long fd() const noexcept { return fd_; }
    long install(File *file, bool cloexec) noexcept {
      if (fd_ < 0)
        return fd_;
      if (!file)
        return -static_cast<long>(VfsError::InvalidArg);
      containers::LockGuard<containers::IrqSpinLock> guard(table_.lock_);
      const auto index = static_cast<u32>(fd_);
      file->ref();
      table_.fds_[index] = file;
      table_.cloexec_[index] = cloexec;
      table_.reserved_[index] = false;
      const long installed = fd_;
      fd_ = -static_cast<long>(VfsError::BadFd);
      return installed;
    }

    /// Commit two reservations together after fallible work, including copyout.
    long install_pair(Reservation &second, File *first_file, File *second_file) noexcept {
      if (this == &second || &table_ != &second.table_ || !first_file || !second_file)
        return -static_cast<long>(VfsError::InvalidArg);
      if (fd_ < 0)
        return fd_;
      if (second.fd_ < 0)
        return second.fd_;
      containers::LockGuard<containers::IrqSpinLock> guard(table_.lock_);
      const auto a = static_cast<u32>(fd_), b = static_cast<u32>(second.fd_);
      first_file->ref();
      second_file->ref();
      table_.fds_[a] = first_file;
      table_.fds_[b] = second_file;
      table_.cloexec_[a] = table_.cloexec_[b] = false;
      table_.reserved_[a] = table_.reserved_[b] = false;
      fd_ = second.fd_ = -static_cast<long>(VfsError::BadFd);
      return 0;
    }

  private:
    FdTable &table_;
    long fd_ = -static_cast<long>(VfsError::BadFd);
  };

  /// Allocate the lowest available fd and install `file`.
  /// Returns fd on success, or -EMFILE if table is full.
  [[nodiscard]] long alloc_fd(File *file, u32 minimum = 0, bool cloexec = false) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    const long fd = find_free_fd(minimum);
    if (fd >= 0) {
      file->ref();
      fds_[static_cast<u32>(fd)] = file;
      cloexec_[static_cast<u32>(fd)] = cloexec;
    }
    return fd;
  }

  /// Select the source and allocate its duplicate in one table transaction.
  [[nodiscard]] long duplicate(long source, long minimum = 0, bool cloexec = false) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    auto *file = lookup_file(source);
    if (!file)
      return -static_cast<long>(VfsError::BadFd);
    if (minimum < 0 || minimum >= MAX_FDS)
      return -static_cast<long>(VfsError::InvalidArg);
    const long fd = find_free_fd(static_cast<u32>(minimum));
    if (fd >= 0) {
      file->ref();
      fds_[static_cast<u32>(fd)] = file;
      cloexec_[static_cast<u32>(fd)] = cloexec;
    }
    return fd;
  }

  /// Select the source and replace the target atomically, releasing the old
  /// target's reference only after unlocking.
  /// An unfinished open owns its reserved slot: return -EBUSY, do not steal it.
  /// Returns fd on success.
  long duplicate_to(long source, long fd) noexcept {
    if (fd < 0 || fd >= MAX_FDS) {
      return -static_cast<long>(VfsError::BadFd);
    }
    File *previous = nullptr;
    {
      containers::LockGuard<containers::IrqSpinLock> guard(lock_);
      auto *file = lookup_file(source);
      if (!file)
        return -static_cast<long>(VfsError::BadFd);
      if (source == fd)
        return fd; // A no-op preserves descriptor flags and reference ownership.
      auto idx = static_cast<u32>(fd);
      if (reserved_[idx])
        return -static_cast<long>(VfsError::Busy);
      // Retain before replacing: file can also be the old slot's object.
      file->ref();
      previous = fds_[idx];
      fds_[idx] = file;
      cloexec_[idx] = false;
    }
    release_file(previous);
    return fd;
  }

  /// Borrow only when the caller prevents descriptor removal for the whole use.
  /// Production I/O uses acquire_file instead.
  [[nodiscard]] File *get_file(long fd) const noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    return lookup_file(fd);
  }

  [[nodiscard]] FileRef acquire_file(long fd) const noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    return FileRef{lookup_file(fd)};
  }

  /// Close fd: decrement File ref count, release if zero.
  /// Returns 0 on success, negative error on bad fd.
  long close_fd(long fd) noexcept {
    File *file = nullptr;
    {
      containers::LockGuard<containers::IrqSpinLock> guard(lock_);
      file = lookup_file(fd);
      if (!file)
        return -static_cast<long>(VfsError::BadFd);
      auto idx = static_cast<u32>(fd);
      fds_[idx] = nullptr;
      cloexec_[idx] = false;
    }
    // Endpoint close can wake tasks and acquire namespace/pipe locks.
    release_file(file);
    return 0;
  }

  /// Clone this fd table (for fork).  All File ref counts are incremented.
  /// Returns a new heap-allocated FdTable, or nullptr on OOM.
  [[nodiscard]] FdTable *clone() const noexcept;

  /// Close all open fds (for exit / exec).
  void close_all() noexcept {
    for (u32 fd = 0; fd < MAX_FDS; ++fd)
      (void)close_fd(fd);
  }

  long descriptor_flags(long fd) const noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    return lookup_file(fd) ? static_cast<long>(cloexec_[static_cast<u32>(fd)]) : -static_cast<long>(VfsError::BadFd);
  }
  long set_descriptor_flags(long fd, long flags) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    if (!lookup_file(fd))
      return -static_cast<long>(VfsError::BadFd);
    if (flags & ~1L)
      return -static_cast<long>(VfsError::InvalidArg);
    cloexec_[static_cast<u32>(fd)] = flags != 0;
    return 0;
  }
  void close_on_exec() noexcept {
    for (u32 fd = 0; fd < MAX_FDS; ++fd) {
      File *file = nullptr;
      {
        containers::LockGuard<containers::IrqSpinLock> guard(lock_);
        if (!cloexec_[fd])
          continue;
        file = fds_[fd];
        fds_[fd] = nullptr;
        cloexec_[fd] = false;
      }
      release_file(file);
    }
  }

private:
  [[nodiscard]] long find_free_fd(u32 minimum) const noexcept {
    for (u32 fd = minimum; fd < MAX_FDS; ++fd)
      if (!fds_[fd] && !reserved_[fd])
        return static_cast<long>(fd);
    return -static_cast<long>(VfsError::TooManyFiles);
  }
  [[nodiscard]] File *lookup_file(long fd) const noexcept {
    return fd < 0 || fd >= MAX_FDS ? nullptr : fds_[static_cast<u32>(fd)];
  }
  mutable containers::IrqSpinLock lock_;
  Dentry *cwd_ = nullptr; // nullptr selects the mounted namespace root.
  File *fds_[MAX_FDS] = {};
  bool cloexec_[MAX_FDS] = {};
  bool reserved_[MAX_FDS] = {};
};

} // namespace moss::kernel::vfs
