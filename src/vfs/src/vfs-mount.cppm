// MOSS VFS Mount Layer — MountEntry, MountTable, vfs_init declarations
//
// The MountTable records all mounted filesystems and provides
// longest-prefix matching for path resolution across mount boundaries.

export module moss.vfs:mount;

import moss.std;
import moss.types;
import :types;
import :inode;
import :dcache;

export namespace moss::kernel::vfs {

// ============================================================================
// MountEntry — a single mount point
// ============================================================================

struct MountEntry {
  char path[MAX_PATH_LEN]; // mount point path (e.g. "/", "/dev")
  u32 path_len;            // strlen(path)
  SuperBlock *sb;          // filesystem superblock
  Dentry *root;            // root dentry of mounted fs
  bool active;             // slot in use?
};

// ============================================================================
// LookupResult — result of mount-table path lookup
// ============================================================================

struct MountLookupResult {
  MountEntry *mount;    // deepest matching mount
  const char *residual; // remaining path after mount prefix
  u32 residual_len;     // strlen(residual)
};

// ============================================================================
// MountTable — global table of mount points
// ============================================================================

class MountTable {
public:
  /// Initialize all mount slots.
  void init() noexcept {
    for (u32 i = 0; i < MAX_MOUNTS; ++i) {
      mounts_[i].active = false;
    }
    count_ = 0;
  }

  /// Mount a filesystem at the given path.
  /// Returns 0 on success, negative error on failure.
  long mount(const char *path, SuperBlock *sb, Dentry *root) noexcept;

  /// Find the deepest mount point matching the given path.
  /// Returns true on success (result is filled in), false if no match.
  [[nodiscard]] bool lookup(const char *path, MountLookupResult &result) noexcept;

private:
  MountEntry mounts_[MAX_MOUNTS];
  u32 count_;
};

// Global mount table instance
inline MountTable g_mount_table;

// ============================================================================
// VFS initialization — implemented in vfs_init.cpp
// ============================================================================

/// Initialize the VFS: mount root ramfs, mount devfs at /dev.
/// Called once from kernel_main after memory subsystem is ready.
void vfs_init() noexcept;

/// Open stdin/stdout/stderr (fd 0/1/2) on /dev/console for a process.
/// `fd_table` is actually a vfs::FdTable* passed as void* from Process.
void vfs_init_stdio(void *fd_table) noexcept;

} // namespace moss::kernel::vfs
