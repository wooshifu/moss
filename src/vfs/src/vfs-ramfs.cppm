// MOSS VFS ramfs — immutable initramfs files and mutable runtime-created files
//
// Builds an inode tree from the parsed InitramfsArchive.  File content
// is zero-copy for archive files: inode->data borrows the CPIO memory region.
// Archive bytes remain immutable; runtime-created regular files own heap buffers.
// The root "/" is the ramfs root; the production runtime appears as /busybox.elf.

export module moss.vfs:ramfs;

import moss.std;
import moss.types;
import moss.initramfs;
import moss.logging;
import :types;
import :inode;
import :dcache;
import :file;

export namespace moss::kernel::vfs::ramfs {

/// Initialize ramfs from the global initramfs archive.
/// Creates a root directory inode + one regular file inode per archive entry.
/// Returns the ramfs SuperBlock on success, nullptr on failure.
[[nodiscard]] SuperBlock *ramfs_init() noexcept;

} // namespace moss::kernel::vfs::ramfs
