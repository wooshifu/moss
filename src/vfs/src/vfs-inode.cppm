// MOSS VFS Inode Layer — Inode, SuperBlock, FileOps, InodeOps
//
// Models the Linux VFS inode abstraction.  FileOps and InodeOps are
// plain C function-pointer tables (not virtual classes) to minimize
// per-call overhead in syscall hot paths.

export module moss.vfs:inode;

import moss.std;
import moss.types;
import :types;
import :buffer;

export namespace moss::kernel::vfs {

// Forward declarations
struct File;
struct Inode;
struct Dentry;
struct SuperBlock;

// ============================================================================
// FileOps — per-file-type operation table (like Linux file_operations)
// ============================================================================

/// Operations dispatched through an open File object.
/// Each filesystem / device driver provides a const FileOps instance.
/// Null function pointers mean "not supported" — the VFS syscall layer
/// returns VfsError::NotSupported for null slots.
struct FileOps {
  long (*open)(File *file, Inode *inode, u32 flags) noexcept;
  long (*release)(File *file) noexcept;
  long (*read)(File *file, OutputBuffer buffer) noexcept;
  long (*write)(File *file, InputBuffer buffer) noexcept;
  long (*lseek)(File *file, i64 offset, SeekWhence whence) noexcept;
  long (*ioctl)(File *file, u32 cmd, u64 arg) noexcept;
};

// ============================================================================
// InodeOps — directory / inode-level operations (like Linux inode_operations)
// ============================================================================

/// Operations on directory inodes for name resolution and creation.
struct InodeOps {
  /// Look up a child name inside a directory inode.
  /// Returns the child Dentry* on success, nullptr if not found.
  Dentry *(*lookup)(Inode *dir, const char *name, u32 name_len) noexcept;

  /// Create a new file/directory/device inside a directory.
  long (*create)(Inode *dir, const char *name, u32 name_len, FileType type, u32 mode) noexcept;
  long (*remove)(Inode *dir, Dentry *child) noexcept = nullptr;
  long (*rename)(Dentry *source, Dentry *parent, const char *name, u32 name_len) noexcept = nullptr;
};

// ============================================================================
// SuperBlock — per-filesystem instance metadata
// ============================================================================

/// Identifies a mounted filesystem instance.
struct SuperBlock {
  const char *fs_name;             // "ramfs", "devfs", "pipefs"
  Inode *root_inode;               // root inode of this filesystem
  u32 block_size;                  // logical block size (usually PAGE_SIZE)
  void *fs_private;                // filesystem-specific data
  DeviceNumber device = NO_DEVICE; // Stable filesystem identity, never a kernel address.
};

// ============================================================================
// Inode — in-core inode representation
// ============================================================================

/// Each file, directory, device, or pipe in the VFS has exactly one Inode.
/// The inode owns the metadata; file content is accessed through file_ops.
struct Inode {
  InodeNumber ino;
  FileType type;
  u32 mode;          // permission + type bits (S_IF... | rwxrwxrwx)
  u32 nlink;         // hard link count
  u32 uid, gid;      // inode owner, independent of the inspecting process
  u64 size;          // file size in bytes (0 for devices/pipes)
  DeviceNumber rdev; // device number (char/block devices only)

  SuperBlock *sb; // owning superblock

  const FileOps *file_ops;   // operations for open files on this inode
  const InodeOps *inode_ops; // directory operations (null for non-dirs)

  // -- Filesystem-specific inline data --
  const u8 *data;      // ramfs: pointer into initramfs CPIO data
  void *private_data;  // pipefs: PipeState*; others: fs-specific
  usize data_capacity; // Mutable ramfs buffer allocation size (CPIO remains borrowed).
  bool ramfs_mutable;

  // -- Directory children (used by ramfs/devfs directory inodes) --
  // Simple inline array to avoid dynamic allocation for small dirs.
  static constexpr u32 MAX_CHILDREN = 64;
  Dentry *children[MAX_CHILDREN];
  u32 child_count;

  // -- Reference counting --
  u32 ref_count;

  // Helpers
  [[nodiscard]] bool is_directory() const noexcept { return type == FileType::Directory; }
  [[nodiscard]] bool is_regular() const noexcept { return type == FileType::Regular; }
  [[nodiscard]] bool is_char_device() const noexcept { return type == FileType::CharDev; }
  [[nodiscard]] bool is_pipe() const noexcept { return type == FileType::Fifo; }

  void ref() noexcept { ++ref_count; }
  void unref() noexcept {
    if (ref_count > 0) {
      --ref_count;
    }
  }
};

} // namespace moss::kernel::vfs
