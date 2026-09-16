// MOSS VFS Dentry Cache — Dentry structure and path resolution declarations
//
// A Dentry (directory entry) represents a name-to-inode binding in the
// directory tree.  The dcache is a flat hash table keyed by (parent, name)
// for O(1) lookups during path resolution.

export module moss.vfs:dcache;

import moss.std;
import moss.types;
import moss.containers;
import :types;
import :inode;

export namespace moss::kernel::vfs {

// ============================================================================
// Dentry — cached directory entry
// ============================================================================

struct Dentry {
  char name[MAX_NAME_LEN + 1]; // NUL-terminated component name
  u32 name_len;                // strlen(name)
  Inode *inode;                // the inode this name resolves to
  Dentry *parent;              // parent directory dentry (null for root)
  u32 ref_count;

  void ref() noexcept { ++ref_count; }
  void unref() noexcept {
    if (ref_count > 0) {
      --ref_count;
    }
  }
};

// ============================================================================
// DentryCache — global hash table for fast path resolution
// ============================================================================

/// Simple open-addressed hash table for dentry lookups.
/// Key: (parent Dentry*, name) → Dentry*
class DentryCache {
public:
  // Fixed 256-entry cache keeps lookup storage bounded without heap allocation.
  // The exact capacity rationale is not recorded; a miss still walks the tree.
  static constexpr u32 CACHE_SIZE = 256;

  /// Initialize the cache (zero all slots)
  void init() noexcept;

  /// Insert a dentry into the cache.
  void insert(Dentry *dentry) noexcept;
  void remove(Dentry *dentry) noexcept;

  /// Look up a child dentry by parent + name.
  /// Returns nullptr if not cached.
  [[nodiscard]] Dentry *lookup(const Dentry *parent, const char *name, u32 name_len) noexcept;

private:
  Dentry *slots_[CACHE_SIZE];

  [[nodiscard]] static u32 hash(const Dentry *parent, const char *name, u32 name_len) noexcept;
};

// ============================================================================
// Path resolution — declared here, implemented in vfs_path.cpp
// ============================================================================

/// Resolve a full path (e.g. "/dev/console") starting from the VFS root.
/// On success returns the final Dentry*; on failure returns nullptr.
[[nodiscard]] Dentry *resolve_path(const char *path) noexcept;

// ponytail: one namespace lock for the bounded in-memory tree; split by directory if contention matters.
inline containers::IrqSpinLock namespace_lock;
/// Internal walk; the caller holds namespace_lock while using the borrowed result.
/// If supplied, error describes a failed walk; ignore it on success.
[[nodiscard]] Dentry *resolve_path_locked(const char *path, VfsError *error = nullptr, Dentry *start = nullptr,
                                          u32 uid = 0, u32 gid = 0) noexcept;

/// Requested R/W/X bits use the native access() mask (4/2/1).
[[nodiscard]] inline bool can_access(const Inode &inode, u32 uid, u32 gid, u32 mask) noexcept {
  if (uid == 0) {
    return !(mask & 1) || inode.is_directory() || (inode.mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
  }
  // POSIX mode packs three rwx triplets: owner at bit 6, group at 3, others at 0.
  u32 shift = 0;
  if (uid == inode.uid) {
    shift = 6;
  } else if (gid == inode.gid) {
    shift = 3;
  }
  return ((inode.mode >> shift) & mask) == mask;
}

[[nodiscard]] inline bool can_search(const Inode &inode, u32 uid, u32 gid) noexcept {
  return can_access(inode, uid, gid, 1);
}

/// Allocate a new Dentry from the global dentry pool.
[[nodiscard]] Dentry *alloc_dentry(const char *name, u32 name_len, Inode *inode, Dentry *parent) noexcept;
/// Drop a retained dentry reference with namespace_lock held. Final release
/// returns its inode/dentry slots and the reference to its parent.
void release_dentry(Dentry *dentry) noexcept;

/// Allocate a new Inode from the global inode pool.
[[nodiscard]] Inode *alloc_inode() noexcept;

/// Counts actual occupied pool slots, including unlinked but retained objects.
struct PoolUsage {
  u32 inodes = 0, dentries = 0, files = 0;
  bool operator==(const PoolUsage &) const noexcept = default;
};
[[nodiscard]] PoolUsage pool_usage() noexcept;

// Global dcache instance
inline DentryCache g_dcache;

} // namespace moss::kernel::vfs
