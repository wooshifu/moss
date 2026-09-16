// MOSS VFS pipefs — anonymous pipe implementation
//
// Provides sys_pipe() support with a 4KB ring buffer per pipe.

export module moss.vfs:pipefs;

import moss.std;
import moss.types;
import :types;
import :inode;
import :file;

export namespace moss::kernel::vfs::pipefs {

/// Create a pipe: two File* objects (read end + write end).
/// Returns 0 on success, negative VfsError on failure.
/// On success, read_file and write_file each carry one caller-owned reference.
long create_pipe(File *&read_file, File *&write_file) noexcept;

} // namespace moss::kernel::vfs::pipefs
