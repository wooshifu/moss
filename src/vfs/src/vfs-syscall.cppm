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
long do_open(void *fd_table_ptr, const char *path, u32 flags, u32 mode, u32 uid = 0, u32 gid = 0) noexcept;

/// Close a file descriptor.
long do_close(void *fd_table_ptr, long fd) noexcept;

/// Read from a file descriptor.
long do_read(void *fd_table_ptr, long fd, OutputBuffer buffer) noexcept;

/// Write to a file descriptor.
long do_write(void *fd_table_ptr, long fd, InputBuffer buffer) noexcept;

/// Seek within a file.
long do_lseek(void *fd_table_ptr, long fd, i64 offset, u32 whence) noexcept;

/// Dispatch a native device command. Payloads are command-specific.
long do_ioctl(void *fd_table_ptr, long fd, long command, u64 argument) noexcept;

/// Get file status.
long do_fstat(void *fd_table_ptr, long fd, void *stat_buf) noexcept;
long do_stat(const char *path, void *stat_buf, void *fd_table_ptr = nullptr, u32 uid = 0, u32 gid = 0) noexcept;
long do_access(void *fd_table_ptr, const char *path, long mode, u32 uid, u32 gid) noexcept;
long do_mkdir(const char *path, u32 mode, u32 uid, u32 gid, void *fd_table_ptr = nullptr) noexcept;
long do_rmdir(const char *path, void *fd_table_ptr = nullptr, u32 uid = 0, u32 gid = 0) noexcept;
long do_unlink(const char *path, void *fd_table_ptr = nullptr, u32 uid = 0, u32 gid = 0) noexcept;
long do_rename(const char *old_path, const char *new_path, void *fd_table_ptr = nullptr, u32 uid = 0,
               u32 gid = 0) noexcept;
long do_getdents(void *fd_table_ptr, long fd, OutputBuffer buffer) noexcept;
/// Return the current absolute directory path, including its NUL terminator.
long do_getcwd(void *fd_table_ptr, OutputBuffer buffer) noexcept;
long do_chdir(void *fd_table_ptr, const char *path, u32 uid = 0, u32 gid = 0) noexcept;

/// Native duplication and descriptor/status flag operations.
long do_fcntl(void *fd_table_ptr, long fd, long command, long argument = 0) noexcept;

/// Duplicate a file descriptor.
long do_dup(void *fd_table_ptr, long oldfd) noexcept;

/// Duplicate a file descriptor to a specific fd number.
long do_dup2(void *fd_table_ptr, long oldfd, long newfd) noexcept;

/// Create a pipe.  pipefd[0] = read end, pipefd[1] = write end.
long do_pipe(void *fd_table_ptr, long *pipefd) noexcept;
/// Copy descriptor numbers before publishing the pair; failed output cancels it.
long do_pipe(void *fd_table_ptr, OutputBuffer buffer) noexcept;

} // namespace moss::kernel::vfs::syscall
