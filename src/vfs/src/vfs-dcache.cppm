// MOSS VFS Dentry Cache — Dentry structure and path resolution declarations
//
// A Dentry (directory entry) represents a name-to-inode binding in the
// directory tree.  The dcache is a flat hash table keyed by (parent, name)
// for O(1) lookups during path resolution.

export module moss.vfs:dcache;

import moss.std;
import moss.types;
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
    if (ref_count > 0)
      --ref_count;
  }
};

// ============================================================================
// DentryCache — global hash table for fast path resolution
// ============================================================================

/// Simple open-addressed hash table for dentry lookups.
/// Key: (parent Dentry*, name) → Dentry*
class DentryCache {
public:
  static constexpr u32 CACHE_SIZE = 256;

  /// Initialize the cache (zero all slots)
  void init() noexcept;

  /// Insert a dentry into the cache.
  void insert(Dentry *dentry) noexcept;

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

/// Allocate a new Dentry from the global dentry pool.
[[nodiscard]] Dentry *alloc_dentry(const char *name, u32 name_len, Inode *inode, Dentry *parent) noexcept;

/// Allocate a new Inode from the global inode pool.
[[nodiscard]] Inode *alloc_inode() noexcept;

// Global dcache instance
inline DentryCache g_dcache;

} // namespace moss::kernel::vfs
