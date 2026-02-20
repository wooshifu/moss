// MOSS VFS Types — core type definitions, enums, and constants
//
// Provides fundamental types used throughout the VFS layer:
// file types, open flags, seek whence, permission modes, error codes,
// and type aliases.

export module moss.vfs:types;

import moss.std;
import moss.types;
import moss.result;

export namespace moss::kernel::vfs {

// Re-export commonly used types for convenience
using moss::u8;
using moss::u16;
using moss::u32;
using moss::u64;
using moss::i32;
using moss::i64;
using moss::kernel::usize;
using moss::kernel::isize;
using moss::kernel::PhysAddr;
using moss::kernel::VirtAddr;

// ============================================================================
// Constants
// ============================================================================

/// Maximum number of open file descriptors per process
inline constexpr u32 MAX_FDS = 256;

/// Maximum number of simultaneous mount points
inline constexpr u32 MAX_MOUNTS = 16;

/// Maximum path component length (single name between '/' separators)
inline constexpr u32 MAX_NAME_LEN = 255;

/// Maximum full path length
inline constexpr u32 MAX_PATH_LEN = 1024;

/// Pipe buffer size (one page)
inline constexpr u32 PIPE_BUF_SIZE = 4096;

// ============================================================================
// Inode / file identity types
// ============================================================================

/// Inode number (filesystem-scoped)
using InodeNumber = u64;

/// Device number (major:minor packed into u32)
using DeviceNumber = u32;

/// Invalid sentinel values
inline constexpr InodeNumber INVALID_INO = 0;
inline constexpr DeviceNumber NO_DEVICE = 0;

// ============================================================================
// File type enumeration
// ============================================================================

enum class FileType : u8 {
    Regular   = 0,
    Directory = 1,
    CharDev   = 2,
    BlockDev  = 3,
    Fifo      = 4,  // named pipe / pipe
    Socket    = 5,
    Symlink   = 6,
};

// ============================================================================
// Open flags (POSIX-compatible bit flags)
// ============================================================================

inline constexpr u32 O_RDONLY   = 0x0000;
inline constexpr u32 O_WRONLY   = 0x0001;
inline constexpr u32 O_RDWR     = 0x0002;
inline constexpr u32 O_ACCMODE  = 0x0003;  // mask for access mode bits

inline constexpr u32 O_CREAT    = 0x0040;
inline constexpr u32 O_EXCL     = 0x0080;
inline constexpr u32 O_TRUNC    = 0x0200;
inline constexpr u32 O_APPEND   = 0x0400;
inline constexpr u32 O_NONBLOCK = 0x0800;
inline constexpr u32 O_CLOEXEC  = 0x80000;

// ============================================================================
// Seek whence
// ============================================================================

enum class SeekWhence : u32 {
    Set     = 0,  // SEEK_SET
    Current = 1,  // SEEK_CUR
    End     = 2,  // SEEK_END
};

// ============================================================================
// File mode / permission bits
// ============================================================================

inline constexpr u32 S_IFMT   = 0170000;  // file type mask
inline constexpr u32 S_IFREG  = 0100000;  // regular file
inline constexpr u32 S_IFDIR  = 0040000;  // directory
inline constexpr u32 S_IFCHR  = 0020000;  // character device
inline constexpr u32 S_IFBLK  = 0060000;  // block device
inline constexpr u32 S_IFIFO  = 0010000;  // FIFO/pipe
inline constexpr u32 S_IFSOCK = 0140000;  // socket
inline constexpr u32 S_IFLNK  = 0120000;  // symbolic link

inline constexpr u32 S_IRWXU = 00700;  // owner read/write/execute
inline constexpr u32 S_IRUSR = 00400;
inline constexpr u32 S_IWUSR = 00200;
inline constexpr u32 S_IXUSR = 00100;

inline constexpr u32 S_IRWXG = 00070;  // group read/write/execute
inline constexpr u32 S_IRGRP = 00040;
inline constexpr u32 S_IWGRP = 00020;
inline constexpr u32 S_IXGRP = 00010;

inline constexpr u32 S_IRWXO = 00007;  // others read/write/execute
inline constexpr u32 S_IROTH = 00004;
inline constexpr u32 S_IWOTH = 00002;
inline constexpr u32 S_IXOTH = 00001;

// ============================================================================
// VFS error codes
// ============================================================================

enum class VfsError : u32 {
    None          = 0,
    NoEntry       = 2,   // ENOENT
    IoError       = 5,   // EIO
    BadFd         = 9,   // EBADF
    NoMemory      = 12,  // ENOMEM
    PermDenied    = 13,  // EACCES
    FileExists    = 17,  // EEXIST
    NotDirectory  = 20,  // ENOTDIR
    IsDirectory   = 21,  // EISDIR
    InvalidArg    = 22,  // EINVAL
    TooManyFiles  = 24,  // EMFILE
    NameTooLong   = 36,  // ENAMETOOLONG
    NotSupported  = 95,  // EOPNOTSUPP
    IsPipe        = 29,  // ESPIPE  (illegal seek on pipe)
};

// ============================================================================
// Result type aliases
// ============================================================================

template <typename T>
using VfsResult = moss::kernel::Result<T, VfsError>;

using VfsVoidResult = moss::kernel::Result<void, VfsError>;

// ============================================================================
// stat structure (simplified POSIX stat)
// ============================================================================

struct Stat {
    InodeNumber  st_ino;
    u32          st_mode;
    u32          st_nlink;
    u64          st_size;
    DeviceNumber st_rdev;
};

} // namespace moss::kernel::vfs
