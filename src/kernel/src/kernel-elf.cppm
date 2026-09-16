// MOSS Kernel Module - ELF Format Partition
// ELF constants, header structures, and program header definitions.

export module moss.kernel:elf;

import moss.std;
import moss.types;
import moss.result;
import moss.arch;

export namespace moss::kernel::elf {

// These tags, machine IDs, segment types and permission bits are ELF wire
// values, not tuning parameters; changing them would reinterpret linked files.
// ELF_MAGIC packs the identification bytes as read by a little-endian CPU.
// ELF file header constants
inline constexpr u32 ELF_MAGIC = 0x464C457F; // "\x7FELF"
inline constexpr u8 ELF_CLASS_64 = 2;        // 64-bit ELF
inline constexpr u8 ELF_DATA_LSB = 1;        // Little-endian byte order
inline constexpr u8 ELF_VERSION = 1;         // ELF version 1

// Supported architectures
inline constexpr u16 EM_NONE = 0;      // Unspecified
inline constexpr u16 EM_X64 = 62;      // AMD64/x64
inline constexpr u16 EM_AARCH64 = 183; // ARM64/AArch64
inline constexpr u16 EM_RISCV64 = 243; // RISC-V 64

// File types
inline constexpr u16 ET_NONE = 0; // Unknown type
inline constexpr u16 ET_REL = 1;  // Relocatable file
inline constexpr u16 ET_EXEC = 2; // Executable file
inline constexpr u16 ET_DYN = 3;  // Shared object

// Program header types
inline constexpr u32 PT_NULL = 0;    // Unused
inline constexpr u32 PT_LOAD = 1;    // Loadable segment
inline constexpr u32 PT_DYNAMIC = 2; // Dynamic linking info
inline constexpr u32 PT_INTERP = 3;  // Interpreter info
inline constexpr u32 PT_NOTE = 4;    // Auxiliary info
inline constexpr u32 PT_SHLIB = 5;   // Reserved
inline constexpr u32 PT_PHDR = 6;    // Program header table itself
inline constexpr u32 PT_TLS = 7;     // Thread-local storage

// Program header flags
inline constexpr u32 PF_X = 0x1; // Executable
inline constexpr u32 PF_W = 0x2; // Writable
inline constexpr u32 PF_R = 0x4; // Readable

// 64-bit ELF file header
struct [[gnu::packed]] ElfHeader {
  u8 e_ident[16];  // ELF fixes its identification prefix at 16 bytes; fields below must retain wire offsets.
  u16 e_type;      // File type
  u16 e_machine;   // Target architecture
  u32 e_version;   // File version
  u64 e_entry;     // Entry point virtual address
  u64 e_phoff;     // Program header table offset
  u64 e_shoff;     // Section header table offset
  u32 e_flags;     // Processor-specific flags
  u16 e_ehsize;    // ELF header size
  u16 e_phentsize; // Program header table entry size
  u16 e_phnum;     // Program header table entry count
  u16 e_shentsize; // Section header table entry size
  u16 e_shnum;     // Section header table entry count
  u16 e_shstrndx;  // Section name string table index
};

// 64-bit program header table entry
struct [[gnu::packed]] ProgramHeader {
  u32 p_type;   // Segment type
  u32 p_flags;  // Segment flags
  u64 p_offset; // Segment offset in file
  u64 p_vaddr;  // Segment virtual address
  u64 p_paddr;  // Segment physical address (usually ignored)
  u64 p_filesz; // Segment size in file
  u64 p_memsz;  // Segment size in memory
  u64 p_align;  // Segment alignment
};

// ============================================================================
// ELF validation
// ============================================================================

/// Validate an ELF64 header for the current architecture
[[nodiscard]] inline bool validate_elf_header(const ElfHeader *hdr, usize data_size) noexcept {
  if (data_size < sizeof(ElfHeader)) {
    return false;
  }

  // Check magic
  if (*reinterpret_cast<const u32 *>(hdr->e_ident) != ELF_MAGIC) {
    return false;
  }

  // ELF e_ident[4] is EI_CLASS; the loader below uses ELF64 field widths.
  if (hdr->e_ident[4] != ELF_CLASS_64) {
    return false;
  }

  // ELF e_ident[5] is EI_DATA; headers are read directly without byte swapping.
  if (hdr->e_ident[5] != ELF_DATA_LSB) {
    return false;
  }

  // Require a fixed-address executable: this loader does not relocate ET_DYN images.
  if (hdr->e_type != ET_EXEC) {
    return false;
  }

  // Check architecture
  using moss::kernel::arch::is_arm64;
  using moss::kernel::arch::is_riscv64;
  using moss::kernel::arch::is_x64;
  if constexpr (is_arm64) {
    if (hdr->e_machine != EM_AARCH64) {
      return false;
    }
  } else if constexpr (is_x64) {
    if (hdr->e_machine != EM_X64) {
      return false;
    }
  } else if constexpr (is_riscv64) {
    if (hdr->e_machine != EM_RISCV64) {
      return false;
    }
  }

  // e_ident[6] is EI_VERSION. Reject a truncated or differently sized table
  // before pointer arithmetic; division below avoids overflow from count * size.
  if (hdr->e_ident[6] != ELF_VERSION || hdr->e_version != ELF_VERSION || hdr->e_ehsize != sizeof(ElfHeader) ||
      hdr->e_phentsize != sizeof(ProgramHeader) || hdr->e_phoff < sizeof(ElfHeader) || hdr->e_phnum == 0) {
    return false;
  }
  if (hdr->e_phoff > data_size || static_cast<u64>(hdr->e_phnum) > (data_size - hdr->e_phoff) / sizeof(ProgramHeader)) {
    return false;
  }

  return true;
}

/// Get pointer to program header table
[[nodiscard]] inline const ProgramHeader *get_program_headers(const ElfHeader *hdr) noexcept {
  return reinterpret_cast<const ProgramHeader *>(reinterpret_cast<const u8 *>(hdr) + hdr->e_phoff);
}

} // namespace moss::kernel::elf
