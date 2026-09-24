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
// Fixed lookup capacity keeps parsing independent of heap readiness. 64 is a
// kernel policy, not a CPIO limit; archives with more indexed entries are
// rejected so callers cannot mistake a partial index for the complete archive.
// The exact capacity rationale is not recorded, so growth must revisit it.
inline constexpr u32 MAX_INITRAMFS_FILES = 64;

// The newc wire format defines thirteen fixed-width hexadecimal fields after
// its six-byte magic. Validating every field keeps ignored metadata from making
// an otherwise malformed archive appear structurally sound.
inline constexpr usize CPIO_NEWC_FIELD_COUNT = 13;
inline constexpr usize CPIO_NEWC_HEX_FIELD_WIDTH = 8;

/// A single file entry in the initramfs
struct InitramfsEntry {
  const char *name; // filename (points into CPIO data, NUL-terminated)
  const u8 *data;   // file content (points into CPIO data)
  u32 data_size;    // file size in bytes
  u32 mode;         // permission bits from CPIO header
};

/// CPIO newc header: 110 bytes, all ASCII hex fields
/// Magic: "070701"
// The format fixes 6 magic bytes + thirteen 8-digit hexadecimal fields;
// changing these widths would move the filename and invalidate archive parsing.
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

/// Parse an 8-character ASCII hex field into u32.
inline bool parse_hex8(const char *s, u32 &value) noexcept {
  value = 0;
  for (usize i = 0; i < CPIO_NEWC_HEX_FIELD_WIDTH; ++i) {
    value <<= 4;
    char c = s[i];
    if (c >= '0' && c <= '9') {
      value |= static_cast<u32>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      value |= static_cast<u32>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      value |= static_cast<u32>(c - 'A' + 10);
    } else {
      return false;
    }
  }
  return true;
}

/// Align up to 4-byte boundary (CPIO newc padding)
inline usize align4(usize v) noexcept { return (v + 3) & ~static_cast<usize>(3); }

/// Simple string comparison (no libc)
inline bool str_equal(const char *a, const char *b) noexcept {
  while (*a && *b) {
    if (*a != *b) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == *b;
}

/// Read-only initramfs archive
// Entry names and contents borrow the boot archive's RAM; that region must
// remain mapped and reserved for every lookup and executable backing reference.
class InitramfsArchive {
public:
  /// Parse a CPIO newc archive from a RAM region.
  /// @param base  Start address of the CPIO data in physical/identity-mapped RAM
  /// @param size  Total size of the archive in bytes
  /// @return true only when the complete archive is structurally valid
  bool init(PhysAddr base, usize size) noexcept {
#ifdef MOSS_ARCH_X64
    // WORKAROUND for x64: use identity mapping instead of high-half mapping
    // because phys_to_virt() produces unmapped virtual addresses
    base_ = reinterpret_cast<const u8 *>(static_cast<VirtAddr>(base));
#else
    base_ = reinterpret_cast<const u8 *>(phys_to_virt(base));
#endif
    archive_size_ = size;
    file_count_ = 0;
    initialized_ = false;

    auto reject = [&](const char *reason, usize offset) noexcept {
      log::klog::warn("initramfs: invalid archive at offset {:#x}: {}", static_cast<u64>(offset), reason);
      base_ = nullptr;
      archive_size_ = 0;
      file_count_ = 0;
      return false;
    };

    if (!base || size < sizeof(CpioNewcHeader)) {
      return reject("missing header", 0);
    }

    usize offset = 0;
    bool saw_trailer = false;
    while (offset < size) {
      const usize remaining = size - offset;
      if (remaining < sizeof(CpioNewcHeader)) {
        return reject("truncated header or missing trailer", offset);
      }
      const auto *hdr = reinterpret_cast<const CpioNewcHeader *>(base_ + offset);

      // Verify magic
      if (hdr->c_magic[0] != '0' || hdr->c_magic[1] != '7' || hdr->c_magic[2] != '0' || hdr->c_magic[3] != '7' ||
          hdr->c_magic[4] != '0' || hdr->c_magic[5] != '1') {
        return reject("bad newc magic", offset);
      }

      const char *hex_fields = hdr->c_ino;
      for (usize field = 0; field < CPIO_NEWC_FIELD_COUNT; ++field) {
        u32 ignored_value = 0;
        if (!parse_hex8(hex_fields + field * CPIO_NEWC_HEX_FIELD_WIDTH, ignored_value)) {
          return reject("non-hexadecimal header field", offset);
        }
      }

      u32 namesize = 0;
      u32 filesize = 0;
      u32 mode = 0;
      // These fields have already passed syntax validation above; parse them
      // again to retain their values without storing all thirteen fields.
      static_cast<void>(parse_hex8(hdr->c_namesize, namesize));
      static_cast<void>(parse_hex8(hdr->c_filesize, filesize));
      static_cast<void>(parse_hex8(hdr->c_mode, mode));
      if (!namesize || namesize > remaining - sizeof(CpioNewcHeader)) {
        return reject("filename extends past archive end", offset);
      }

      // Name starts immediately after the 110-byte header
      const char *name = reinterpret_cast<const char *>(base_ + offset + sizeof(CpioNewcHeader));
      if (name[namesize - 1] != '\0') {
        return reject("filename is not NUL-terminated", offset);
      }
      for (u32 i = 0; i + 1 < namesize; ++i) {
        if (name[i] == '\0') {
          return reject("filename contains an embedded NUL", offset);
        }
      }

      // Compute every offset before forming a pointer so a forged u32 size
      // cannot move pointer arithmetic outside the supplied archive object.
      const usize name_end = sizeof(CpioNewcHeader) + namesize;
      const usize data_offset = align4(name_end);
      if (data_offset < name_end || data_offset > remaining || filesize > remaining - data_offset) {
        return reject("file data extends past archive end", offset);
      }
      const usize data_end = data_offset + filesize;
      const usize entry_total = align4(data_end);
      if (entry_total < data_end || entry_total > remaining) {
        return reject("file padding extends past archive end", offset);
      }

      // CPIO namesize includes NUL: the ten-byte "TRAILER!!!" uses 11 bytes.
      if (namesize == 11 && str_equal(name, "TRAILER!!!")) {
        if (filesize) {
          return reject("trailer contains file data", offset);
        }
        saw_trailer = true;
        break;
      }

      const u8 *data_ptr = base_ + offset + data_offset;

      // Skip "." directory entry
      if (namesize != 2 || name[0] != '.' || name[1] != '\0') {
        if (file_count_ == MAX_INITRAMFS_FILES) {
          return reject("file count exceeds the fixed lookup capacity", offset);
        }
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

      offset += entry_total;
    }

    if (!saw_trailer) {
      return reject("missing TRAILER!!! record", offset);
    }

    log::klog::info("initramfs: parsed {} files from {:#x} ({} bytes)", file_count_, base, size);
    for (u32 i = 0; i < file_count_; ++i) {
      log::klog::info("  [{}] '{}' {} bytes mode={:#o}", i, entries_[i].name, entries_[i].data_size, entries_[i].mode);
    }

    initialized_ = true;
    return true;
  }

  /// Look up a file by name (linear scan).
  /// Path matching: "busybox.elf" and "/busybox.elf" both match entry "busybox.elf".
  [[nodiscard]] const InitramfsEntry *lookup(const char *path) const noexcept {
    if (!initialized_ || !path) {
      return nullptr;
    }

    // Strip leading "/"
    const char *search = path;
    if (search[0] == '/') {
      search = path + 1;
    }

    for (u32 i = 0; i < file_count_; ++i) {
      if (str_equal(entries_[i].name, search)) {
        return &entries_[i];
      }
    }
    return nullptr;
  }

  [[nodiscard]] u32 file_count() const noexcept { return file_count_; }
  [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }
  [[nodiscard]] const u8 *bytes() const noexcept { return initialized_ ? base_ : nullptr; }
  [[nodiscard]] usize size_bytes() const noexcept { return initialized_ ? archive_size_ : 0; }

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
