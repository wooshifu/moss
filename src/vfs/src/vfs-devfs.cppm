// MOSS VFS devfs — /dev/console, /dev/null, /dev/zero
//
// Provides character device nodes backed by UART (console) or
// trivial implementations (null, zero).  Mounted at "/dev".
//
// Device table:
//   /dev/console  — write → UART putc; read → echoed line input via the kernel RX owner
//   /dev/null     — write → discard; read → returns 0 (EOF)
//   /dev/zero     — write → discard; read → fills buffer with zeros

export module moss.vfs:devfs;

import moss.std;
import moss.types;
import moss.hal.uart;
import :types;
import :inode;
import :dcache;
import :file;

export namespace moss::kernel::vfs::devfs {

/// Initialize devfs: create superblock, root inode, and device inodes.
/// Returns the devfs SuperBlock on success, nullptr on failure.
[[nodiscard]] SuperBlock *devfs_init() noexcept;

/// Get the console FileOps (used by vfs_init_stdio to open /dev/console)
[[nodiscard]] const FileOps &console_file_ops() noexcept;

} // namespace moss::kernel::vfs::devfs
