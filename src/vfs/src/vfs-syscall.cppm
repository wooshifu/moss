// MOSS VFS Syscall Layer — VFS system call handler declarations
//
// Each do_xxx function is the VFS-level implementation called from
// the architecture-specific syscall dispatch table.

export module moss.vfs:syscall;

import moss.std;
import moss.types;
import :types;
import :buffer;

export namespace moss::kernel::vfs::syscall {

/// Open a file by path.  Returns fd on success, negative error on failure.
/// `fd_table_ptr` is a void* to the current process's FdTable.
long do_open(void *fd_table_ptr, const char *path, u32 flags, u32 mode) noexcept;

/// Close a file descriptor.
long do_close(void *fd_table_ptr, long fd) noexcept;

/// Read from a file descriptor.
long do_read(void *fd_table_ptr, long fd, OutputBuffer buffer) noexcept;

/// Write to a file descriptor.
long do_write(void *fd_table_ptr, long fd, InputBuffer buffer) noexcept;

/// Seek within a file.
long do_lseek(void *fd_table_ptr, long fd, i64 offset, u32 whence) noexcept;

/// Get file status.
long do_fstat(void *fd_table_ptr, long fd, void *stat_buf) noexcept;

/// Duplicate a file descriptor.
long do_dup(void *fd_table_ptr, long oldfd) noexcept;

/// Duplicate a file descriptor to a specific fd number.
long do_dup2(void *fd_table_ptr, long oldfd, long newfd) noexcept;

/// Create a pipe.  pipefd[0] = read end, pipefd[1] = write end.
long do_pipe(void *fd_table_ptr, long *pipefd) noexcept;

} // namespace moss::kernel::vfs::syscall
