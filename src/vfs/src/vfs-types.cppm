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
using moss::i32;
using moss::i64;
using moss::u16;
using moss::u32;
using moss::u64;
using moss::u8;
using moss::kernel::isize;
using moss::kernel::PhysAddr;
using moss::kernel::usize;
using moss::kernel::VirtAddr;

// ============================================================================
// Constants
// ============================================================================

// These static budgets bound table storage and error boundaries. The exact
// sizing rationale for 256 descriptors, 16 mounts and 1024-byte paths is not
// recorded; changing them alters accepted workloads and per-process storage.
/// Maximum number of open file descriptors per process
inline constexpr u32 MAX_FDS = 256;

/// Maximum number of simultaneous mount points
inline constexpr u32 MAX_MOUNTS = 16;

/// 255 name bytes plus a NUL fit the fixed 256-byte native DirEntry name field.
inline constexpr u32 MAX_NAME_LEN = 255;

/// Maximum full path length
inline constexpr u32 MAX_PATH_LEN = 1024;

/// 4096 bytes equals one base page and is the small-write atomicity boundary.
/// Keep the pipe implementation and userspace capacity tests consistent.
inline constexpr u32 PIPE_BUF_SIZE = 4096;

/// Moss-native, no-payload terminal query; not a Linux termios ioctl.
/// 0x4d01 is shared with syscall.h/mlibc; its original allocation is unrecorded.
inline constexpr u32 MOSS_IOCTL_ISATTY = 0x4d01;

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
  Regular = 0,
  Directory = 1,
  CharDev = 2,
  BlockDev = 3,
  Fifo = 4, // named pipe / pipe
  Socket = 5,
  Symlink = 6,
};

// ============================================================================
// Open flags use the Linux-style numeric encoding consumed by pinned mlibc;
// POSIX defines their behavior but does not fix these wire bit values.
// ============================================================================

inline constexpr u32 O_RDONLY = 0x0000;
inline constexpr u32 O_WRONLY = 0x0001;
inline constexpr u32 O_RDWR = 0x0002;
inline constexpr u32 O_ACCMODE = 0x0003; // mask for access mode bits

inline constexpr u32 O_CREAT = 0x0040;
inline constexpr u32 O_EXCL = 0x0080;
inline constexpr u32 O_TRUNC = 0x0200;
inline constexpr u32 O_APPEND = 0x0400;
inline constexpr u32 O_NONBLOCK = 0x0800;
inline constexpr u32 O_CLOEXEC = 0x80000;

// ============================================================================
// Seek whence
// ============================================================================

enum class SeekWhence : u32 {
  Set = 0,     // SEEK_SET
  Current = 1, // SEEK_CUR
  End = 2,     // SEEK_END
};

// ============================================================================
// File mode / permission bits
// ============================================================================

inline constexpr u32 S_IFMT = 0170000;   // file type mask
inline constexpr u32 S_IFREG = 0100000;  // regular file
inline constexpr u32 S_IFDIR = 0040000;  // directory
inline constexpr u32 S_IFCHR = 0020000;  // character device
inline constexpr u32 S_IFBLK = 0060000;  // block device
inline constexpr u32 S_IFIFO = 0010000;  // FIFO/pipe
inline constexpr u32 S_IFSOCK = 0140000; // socket
inline constexpr u32 S_IFLNK = 0120000;  // symbolic link

inline constexpr u32 S_IRWXU = 00700; // owner read/write/execute
inline constexpr u32 S_IRUSR = 00400;
inline constexpr u32 S_IWUSR = 00200;
inline constexpr u32 S_IXUSR = 00100;

inline constexpr u32 S_IRWXG = 00070; // group read/write/execute
inline constexpr u32 S_IRGRP = 00040;
inline constexpr u32 S_IWGRP = 00020;
inline constexpr u32 S_IXGRP = 00010;

inline constexpr u32 S_IRWXO = 00007; // others read/write/execute
inline constexpr u32 S_IROTH = 00004;
inline constexpr u32 S_IWOTH = 00002;
inline constexpr u32 S_IXOTH = 00001;

// ============================================================================
// VFS error codes
// ============================================================================

// Keep these errno values aligned with kernel:syscall_table and the userspace
// adapter. Negative values are syscall errors, not filesystem-specific status IDs.
enum class VfsError : u32 {
  None = 0,
  NoEntry = 2,         // ENOENT
  Interrupted = 4,     // EINTR
  IoError = 5,         // EIO
  BadFd = 9,           // EBADF
  WouldBlock = 11,     // EAGAIN
  NoMemory = 12,       // ENOMEM
  PermDenied = 13,     // EACCES
  BadAddress = 14,     // EFAULT
  Busy = 16,           // EBUSY
  FileExists = 17,     // EEXIST
  CrossDevice = 18,    // EXDEV
  NotDirectory = 20,   // ENOTDIR
  IsDirectory = 21,    // EISDIR
  InvalidArg = 22,     // EINVAL
  TooManyFiles = 24,   // EMFILE
  NotTerminal = 25,    // ENOTTY
  FileTooLarge = 27,   // EFBIG
  BrokenPipe = 32,     // EPIPE
  Range = 34,          // ERANGE
  NameTooLong = 36,    // ENAMETOOLONG
  NotImplemented = 38, // ENOSYS
  NotEmpty = 39,       // ENOTEMPTY
  Overflow = 75,       // EOVERFLOW
  NotSupported = 95,   // EOPNOTSUPP
  IsPipe = 29,         // ESPIPE  (illegal seek on pipe)
};

// ============================================================================
// Result type aliases
// ============================================================================

template <typename T> using VfsResult = moss::kernel::Result<T, VfsError>;

using VfsVoidResult = moss::kernel::Result<void, VfsError>;

// ============================================================================
// stat structure (simplified POSIX stat)
// ============================================================================

struct Stat {
  InodeNumber st_ino;
  u32 st_mode;
  u32 st_nlink;
  u64 st_size;
  DeviceNumber st_rdev;
  u32 st_uid, st_gid;
  u32 reserved;
  u64 st_dev;
};
// Native stat ABI: 40 bytes precede the final u64 device field, totaling 48.
// The adapter copies this layout; accidental padding changes would break it.
static_assert(sizeof(Stat) == 48 && __builtin_offsetof(Stat, st_dev) == 40);

// Native fixed-size directory record. Offsets match the pinned mlibc dirent
// prefix; syscall numbers and record batching remain Moss-specific.
struct DirEntry {
  u64 ino;
  i64 offset;
  u16 record_size;
  u8 type;
  char name[MAX_NAME_LEN + 1];
  u8 reserved[5]; // Explicitly initialized bytes instead of implicit ABI tail padding.
};
// Prefix: 8-byte inode + 8-byte cookie + 2-byte size + 1-byte type = 19 bytes;
// a 256-byte name and 5 explicit tail bytes round the record to 280 (8-aligned).
static_assert(sizeof(DirEntry) == 280 && __builtin_offsetof(DirEntry, name) == 19);

} // namespace moss::kernel::vfs
