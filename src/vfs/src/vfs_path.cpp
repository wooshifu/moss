// MOSS VFS path resolution — implementation
// Resolves absolute paths through mount table + dentry cache.

module;

module moss.vfs;

namespace moss::kernel::vfs {

Dentry* resolve_path(const char* path) noexcept {
    if (path == nullptr || path[0] != '/') return nullptr;

    // Find which mount point this path belongs to
    MountLookupResult mount_result{};
    if (!g_mount_table.lookup(path, mount_result)) {
        return nullptr;
    }

    // Start from the mount's root dentry
    Dentry* current = mount_result.mount->root;
    if (current == nullptr) return nullptr;

    // If residual is empty, return root of the mount
    const char* residual = mount_result.residual;
    if (residual[0] == '\0') {
        return current;
    }

    // Walk each path component
    const char* p = residual;
    while (*p != '\0') {
        // Skip leading '/'
        while (*p == '/') ++p;
        if (*p == '\0') break;

        // Extract component name
        const char* name_start = p;
        u32 name_len = 0;
        while (p[name_len] != '\0' && p[name_len] != '/') ++name_len;
        p += name_len;

        if (current->inode == nullptr || !current->inode->is_directory()) {
            return nullptr;  // not a directory
        }

        // Try dcache first
        Dentry* child = g_dcache.lookup(current, name_start, name_len);
        if (child != nullptr) {
            current = child;
            continue;
        }

        // dcache miss → ask the filesystem via inode_ops->lookup
        if (current->inode->inode_ops != nullptr &&
            current->inode->inode_ops->lookup != nullptr) {
            child = current->inode->inode_ops->lookup(current->inode,
                                                       name_start, name_len);
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
            Dentry* d = current->inode->children[i];
            if (d != nullptr && d->name_len == name_len) {
                bool eq = true;
                for (u32 j = 0; j < name_len; ++j) {
                    if (d->name[j] != name_start[j]) { eq = false; break; }
                }
                if (eq) {
                    current = d;
                    found = true;
                    break;
                }
            }
        }
        if (!found) return nullptr;
    }

    return current;
}

} // namespace moss::kernel::vfs
