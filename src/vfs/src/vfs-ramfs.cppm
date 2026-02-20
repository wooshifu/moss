// MOSS VFS ramfs — read-only RAM filesystem backed by initramfs CPIO data
//
// Builds an inode tree from the parsed InitramfsArchive.  File content
// is zero-copy: inode->data points directly into the CPIO memory region.
// The root "/" is the ramfs root; files appear as /hello.elf etc.

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
[[nodiscard]] SuperBlock* ramfs_init() noexcept;

} // namespace moss::kernel::vfs::ramfs
