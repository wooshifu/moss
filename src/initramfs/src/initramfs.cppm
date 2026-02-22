// MOSS Initramfs Module — CPIO newc format parser
//
// Parses a CPIO "newc" archive (070701 magic) located in RAM and provides
// a read-only name->data lookup table.  Used by execve() to find ELF
// binaries without requiring a full VFS.
//
// The archive is typically passed by the bootloader via QEMU -initrd;
// its RAM address is discovered from DTB /chosen/linux,initrd-start.

export module moss.initramfs;

import moss.std;
import moss.types;
import moss.logging;

export namespace moss::kernel::initramfs {

namespace log = moss::kernel::logging;

/// Maximum number of files in the initramfs archive
inline constexpr u32 MAX_INITRAMFS_FILES = 64;

/// A single file entry in the initramfs
struct InitramfsEntry {
  const char *name; // filename (points into CPIO data, NUL-terminated)
  const u8 *data;   // file content (points into CPIO data)
  u32 data_size;    // file size in bytes
  u32 mode;         // permission bits from CPIO header
};

/// CPIO newc header: 110 bytes, all ASCII hex fields
/// Magic: "070701"
struct CpioNewcHeader {
  char c_magic[6];
  char c_ino[8];
  char c_mode[8];
  char c_uid[8];
  char c_gid[8];
  char c_nlink[8];
  char c_mtime[8];
  char c_filesize[8];
  char c_devmajor[8];
  char c_devminor[8];
  char c_rdevmajor[8];
  char c_rdevminor[8];
  char c_namesize[8];
  char c_check[8];
};

static_assert(sizeof(CpioNewcHeader) == 110, "CPIO newc header must be 110 bytes");

/// Parse an 8-character ASCII hex field into u32
inline u32 parse_hex8(const char *s) noexcept {
  u32 val = 0;
  for (int i = 0; i < 8; ++i) {
    val <<= 4;
    char c = s[i];
    if (c >= '0' && c <= '9')
      val |= static_cast<u32>(c - '0');
    else if (c >= 'a' && c <= 'f')
      val |= static_cast<u32>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      val |= static_cast<u32>(c - 'A' + 10);
  }
  return val;
}

/// Align up to 4-byte boundary (CPIO newc padding)
inline usize align4(usize v) noexcept { return (v + 3) & ~static_cast<usize>(3); }

/// Simple string comparison (no libc)
inline bool str_equal(const char *a, const char *b) noexcept {
  while (*a && *b) {
    if (*a != *b)
      return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

/// Read-only initramfs archive
class InitramfsArchive {
public:
  /// Parse a CPIO newc archive from a RAM region.
  /// @param base  Start address of the CPIO data in physical/identity-mapped RAM
  /// @param size  Total size of the archive in bytes
  /// @return true if at least one file was found
  bool init(PhysAddr base, usize size) noexcept {
    base_ = reinterpret_cast<const u8 *>(base);
    archive_size_ = size;
    file_count_ = 0;

    if (size < sizeof(CpioNewcHeader)) {
      log::klog::warn("initramfs: archive too small ({} bytes)", size);
      return false;
    }

    const u8 *ptr = base_;
    const u8 *end = base_ + size;

    while (ptr + sizeof(CpioNewcHeader) <= end && file_count_ < MAX_INITRAMFS_FILES) {
      auto *hdr = reinterpret_cast<const CpioNewcHeader *>(ptr);

      // Verify magic
      if (hdr->c_magic[0] != '0' || hdr->c_magic[1] != '7' || hdr->c_magic[2] != '0' || hdr->c_magic[3] != '7' ||
          hdr->c_magic[4] != '0' || hdr->c_magic[5] != '1') {
        log::klog::warn("initramfs: bad magic at offset {:#x}", static_cast<u64>(ptr - base_));
        break;
      }

      u32 namesize = parse_hex8(hdr->c_namesize);
      u32 filesize = parse_hex8(hdr->c_filesize);
      u32 mode = parse_hex8(hdr->c_mode);

      // Name starts immediately after the 110-byte header
      const char *name = reinterpret_cast<const char *>(ptr + sizeof(CpioNewcHeader));

      // Check for TRAILER (end of archive)
      if (namesize == 11 && str_equal(name, "TRAILER!!!")) {
        break;
      }

      // Data starts after header + name, aligned to 4 bytes
      usize name_end = sizeof(CpioNewcHeader) + namesize;
      usize data_offset = align4(name_end);
      const u8 *data_ptr = ptr + data_offset;

      // Bounds check
      usize entry_total = align4(data_offset + filesize);
      if (ptr + entry_total > end) {
        log::klog::warn("initramfs: entry '{}' extends past archive end", name);
        break;
      }

      // Skip "." directory entry
      if (!(namesize == 2 && name[0] == '.' && name[1] == '\0')) {
        // Strip leading "./" if present
        const char *clean_name = name;
        if (namesize > 2 && name[0] == '.' && name[1] == '/') {
          clean_name = name + 2;
        }
        // Strip leading "/" for absolute paths
        if (clean_name[0] == '/') {
          clean_name = clean_name + 1;
        }

        entries_[file_count_] = InitramfsEntry{
            .name = clean_name,
            .data = data_ptr,
            .data_size = filesize,
            .mode = mode,
        };
        ++file_count_;
      }

      ptr += entry_total;
    }

    log::klog::info("initramfs: parsed {} files from {:#x} ({} bytes)", file_count_, base, size);
    for (u32 i = 0; i < file_count_; ++i) {
      log::klog::info("  [{}] '{}' {} bytes mode={:#o}", i, entries_[i].name, entries_[i].data_size, entries_[i].mode);
    }

    initialized_ = true;
    return file_count_ > 0;
  }

  /// Look up a file by name (linear scan).
  /// Path matching: "hello.elf" matches entry "hello.elf", "/hello.elf" matches "hello.elf".
  [[nodiscard]] const InitramfsEntry *lookup(const char *path) const noexcept {
    if (!initialized_ || !path)
      return nullptr;

    // Strip leading "/"
    const char *search = path;
    if (search[0] == '/')
      search = path + 1;

    for (u32 i = 0; i < file_count_; ++i) {
      if (str_equal(entries_[i].name, search)) {
        return &entries_[i];
      }
    }
    return nullptr;
  }

  [[nodiscard]] u32 file_count() const noexcept { return file_count_; }
  [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

  /// Iterate over all entries
  template <typename F> void for_each(F &&callback) const noexcept {
    for (u32 i = 0; i < file_count_; ++i) {
      callback(entries_[i]);
    }
  }

private:
  const u8 *base_ = nullptr;
  usize archive_size_ = 0;
  InitramfsEntry entries_[MAX_INITRAMFS_FILES] = {};
  u32 file_count_ = 0;
  bool initialized_ = false;
};

/// Global initramfs instance
inline InitramfsArchive g_initramfs;

} // namespace moss::kernel::initramfs
