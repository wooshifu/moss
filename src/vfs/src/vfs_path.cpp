// MOSS VFS path resolution — implementation
// Resolves absolute paths through mount table + dentry cache.

module;

module moss.vfs;

namespace moss::kernel::vfs {

Dentry *resolve_path(const char *path) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(namespace_lock);
  return resolve_path_locked(path);
}

Dentry *resolve_path_locked(const char *path, VfsError *error, Dentry *start, u32 uid, u32 gid) noexcept {
  if (error)
    *error = VfsError::NoEntry;
  if (path == nullptr || !*path) {
    return nullptr;
  }

  // Walk from the namespace root or the retained process directory. Choosing
  // a mount by the original string prefix would mishandle /dir/../dev and /dev/.. .
  MountLookupResult mount_result{};
  if (!g_mount_table.lookup("/", mount_result)) {
    return nullptr;
  }

  // Start from the mount's root dentry
  Dentry *current = *path == '/' || !start ? mount_result.mount->root : start;
  if (current == nullptr) {
    return nullptr;
  }

  // Walk each path component
  const char *p = path;
  while (*p != '\0') {
    // Skip leading '/'
    while (*p == '/') {
      ++p;
    }
    if (*p == '\0') {
      break;
    }

    // Extract component name
    const char *name_start = p;
    u32 name_len = 0;
    while (p[name_len] != '\0' && p[name_len] != '/') {
      ++name_len;
    }
    p += name_len;

    if (current->inode == nullptr || !current->inode->is_directory()) {
      if (error)
        *error = VfsError::NotDirectory;
      return nullptr; // not a directory
    }
    if (!can_search(*current->inode, uid, gid)) {
      if (error)
        *error = VfsError::PermDenied;
      return nullptr;
    }

    if (name_len > MAX_NAME_LEN) {
      if (error)
        *error = VfsError::NameTooLong;
      return nullptr;
    }
    if (name_len == 1 && *name_start == '.')
      continue;
    if (name_len == 2 && name_start[0] == '.' && name_start[1] == '.') {
      auto *parent = current->parent ? current->parent : g_mount_table.parent_of_root(current);
      if (parent)
        current = parent;
      continue;
    }

    if (auto *mounted = g_mount_table.child_mount(current, name_start, name_len)) {
      current = mounted;
      continue;
    }

    // Try dcache first
    Dentry *child = g_dcache.lookup(current, name_start, name_len);
    if (child != nullptr) {
      current = child;
      continue;
    }

    // dcache miss → ask the filesystem via inode_ops->lookup
    if (current->inode->inode_ops != nullptr && current->inode->inode_ops->lookup != nullptr) {
      child = current->inode->inode_ops->lookup(current->inode, name_start, name_len);
      if (child != nullptr) {
        current = child;
        continue;
      }
    }

    // Not found — try cross-mount lookup
    // Build the full sub-path and check if another fs is mounted there
    // For now, simple linear scan of children
    bool found = false;
    for (u32 i = 0; i < current->inode->child_count; ++i) {
      Dentry *d = current->inode->children[i];
      if (d != nullptr && d->name_len == name_len) {
        bool eq = true;
        for (u32 j = 0; j < name_len; ++j) {
          if (d->name[j] != name_start[j]) {
            eq = false;
            break;
          }
        }
        if (eq) {
          current = d;
          found = true;
          break;
        }
      }
    }
    if (!found) {
      return nullptr;
    }
  }

  if (p > path && p[-1] == '/' && (!current->inode || !current->inode->is_directory())) {
    if (error)
      *error = VfsError::NotDirectory;
    return nullptr;
  }
  return current;
}

} // namespace moss::kernel::vfs
