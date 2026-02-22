// MOSS VFS File Layer — File descriptor and FdTable
//
// File represents an open file description (shared across dup/fork).
// FdTable is the per-process file descriptor table mapping int fd → File*.

export module moss.vfs:file;

import moss.std;
import moss.types;
import :types;
import :inode;
import :dcache;

export namespace moss::kernel::vfs {

// ============================================================================
// File — open file description (like Linux struct file)
// ============================================================================

struct File {
  Inode *inode;         // backing inode
  Dentry *dentry;       // dentry used to open this file
  const FileOps *f_ops; // cached from inode->file_ops at open time
  u32 flags;            // O_RDONLY / O_WRONLY / O_RDWR / ...
  i64 pos;              // current file position (lseek)
  void *private_data;   // driver/fs-specific (e.g. PipeState*)
  u32 ref_count;        // reference count (dup, fork)

  void ref() noexcept { ++ref_count; }
  void unref() noexcept {
    if (ref_count > 0)
      --ref_count;
  }
};

// ============================================================================
// File pool — simple static allocator for File objects
// ============================================================================

inline constexpr u32 MAX_FILES = 512;

/// Allocate a File from the global file pool.
[[nodiscard]] File *alloc_file() noexcept;

/// Return a File to the global file pool.
void free_file(File *file) noexcept;

// ============================================================================
// FdTable — per-process file descriptor table
// ============================================================================

/// Maps integer file descriptors [0..MAX_FDS) to File* pointers.
/// Designed to be embedded (via void*) in Process to avoid circular deps.
class FdTable {
public:
  /// Initialize all slots to nullptr.
  void init() noexcept {
    for (u32 i = 0; i < MAX_FDS; ++i) {
      fds_[i] = nullptr;
    }
  }

  /// Allocate the lowest available fd and install `file`.
  /// Returns fd on success, or -EMFILE if table is full.
  [[nodiscard]] long alloc_fd(File *file) noexcept {
    for (u32 i = 0; i < MAX_FDS; ++i) {
      if (fds_[i] == nullptr) {
        fds_[i] = file;
        file->ref();
        return static_cast<long>(i);
      }
    }
    return -static_cast<long>(VfsError::TooManyFiles);
  }

  /// Install `file` at a specific fd (used by dup2).
  /// If the slot is occupied, the old file is closed first.
  /// Returns fd on success.
  long install_fd(long fd, File *file) noexcept {
    if (fd < 0 || static_cast<u32>(fd) >= MAX_FDS) {
      return -static_cast<long>(VfsError::BadFd);
    }
    auto idx = static_cast<u32>(fd);
    // Close existing file in this slot if any
    if (fds_[idx] != nullptr) {
      release_file(fds_[idx]);
      fds_[idx] = nullptr;
    }
    fds_[idx] = file;
    file->ref();
    return fd;
  }

  /// Get File* for a given fd (does NOT increment ref).
  /// Returns nullptr for invalid/unoccupied fds.
  [[nodiscard]] File *get_file(long fd) const noexcept {
    if (fd < 0 || static_cast<u32>(fd) >= MAX_FDS) {
      return nullptr;
    }
    return fds_[static_cast<u32>(fd)];
  }

  /// Close fd: decrement File ref count, release if zero.
  /// Returns 0 on success, negative error on bad fd.
  long close_fd(long fd) noexcept {
    if (fd < 0 || static_cast<u32>(fd) >= MAX_FDS) {
      return -static_cast<long>(VfsError::BadFd);
    }
    auto idx = static_cast<u32>(fd);
    File *f = fds_[idx];
    if (f == nullptr) {
      return -static_cast<long>(VfsError::BadFd);
    }
    fds_[idx] = nullptr;
    release_file(f);
    return 0;
  }

  /// Clone this fd table (for fork).  All File ref counts are incremented.
  /// Returns a new heap-allocated FdTable, or nullptr on OOM.
  [[nodiscard]] FdTable *clone() const noexcept;

  /// Close all open fds (for exit / exec).
  void close_all() noexcept {
    for (u32 i = 0; i < MAX_FDS; ++i) {
      if (fds_[i] != nullptr) {
        release_file(fds_[i]);
        fds_[i] = nullptr;
      }
    }
  }

private:
  File *fds_[MAX_FDS] = {};

  /// Decrement ref and call f_ops->release when ref hits zero.
  static void release_file(File *f) noexcept;
};

} // namespace moss::kernel::vfs
