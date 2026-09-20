---
status: accepted
date: 2026-09-19
---

# Move VFS and Filesystems to Userspace

Moss will implement pathname and mount policy in a Filesystem Namespace Service and every filesystem object model and format in one or more Filesystem Services. The Mechanism Kernel Domain will not retain a VFS object model and will not interpret paths, mounts, inodes, dentries, superblocks or filesystem media formats.

A successful lookup or open returns a File Object Capability associated with the serving filesystem. Subsequent operations use that authority directly and need not traverse a global namespace service again. POSIX file descriptors, current directories and related behavior are a POSIX Compatibility View over capabilities and userspace namespace state. Kernel support remains limited to generic capabilities, IPC, process lifecycle and Memory Object or paging mechanisms selected separately.

## Considered Options

Keeping namespace traversal and a common inode or dentry cache in the kernel while moving only disk-format implementations to userspace was rejected because path, mount and permission policy would still enlarge the Mechanism Kernel Domain and require kernel callback protocols for every filesystem. Routing every file operation through one global userspace VFS endpoint was rejected because an opened object can carry direct authority to its owning service. Keeping the existing kernel VFS as a second native interface was rejected because competing object and authority models would make semantics and recovery ambiguous.

## Consequences

The existing kernel `File`, inode, dentry, mount table, pathname resolution and filesystem callback code requires an incremental replacement rather than becoming the native Moss ABI. Userspace protocols must define mount traversal, credentials or delegated authority, cross-filesystem operations, directory-relative lookup and explicit recovery after a Filesystem Service dies. Process creation and execution need defined handle-inheritance rules for POSIX compatibility. File-backed mappings, cache coherence, page supply and writeback remain separate decisions.
