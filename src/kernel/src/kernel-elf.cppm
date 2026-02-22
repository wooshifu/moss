// MOSS Kernel Module - ELF Format Partition
// ELF constants, header structures, and program header definitions.

export module moss.kernel:elf;

import moss.std;
import moss.types;
import moss.result;
import moss.arch;

export namespace moss::kernel::elf {

// ELF file header constants
inline constexpr u32 ELF_MAGIC = 0x464C457F; // "\x7FELF"
inline constexpr u8 ELF_CLASS_64 = 2;        // 64-bit ELF
inline constexpr u8 ELF_DATA_LSB = 1;        // Little-endian byte order
inline constexpr u8 ELF_VERSION = 1;         // ELF version 1

// Supported architectures
inline constexpr u16 EM_NONE = 0;      // Unspecified
inline constexpr u16 EM_X86_64 = 62;   // AMD64/x86_64
inline constexpr u16 EM_AARCH64 = 183; // ARM64/AArch64
inline constexpr u16 EM_RISCV = 243;   // RISC-V

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
  u8 e_ident[16];  // ELF identification
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

  // Check class (64-bit)
  if (hdr->e_ident[4] != ELF_CLASS_64) {
    return false;
  }

  // Check endianness (little-endian)
  if (hdr->e_ident[5] != ELF_DATA_LSB) {
    return false;
  }

  // Check type (executable)
  if (hdr->e_type != ET_EXEC) {
    return false;
  }

  // Check architecture
  using moss::kernel::arch::is_arm64;
  using moss::kernel::arch::is_riscv;
  using moss::kernel::arch::is_x86_64;
  if constexpr (is_arm64) {
    if (hdr->e_machine != EM_AARCH64) {
      return false;
    }
  } else if constexpr (is_x86_64) {
    if (hdr->e_machine != EM_X86_64) {
      return false;
    }
  } else if constexpr (is_riscv) {
    if (hdr->e_machine != EM_RISCV) {
      return false;
    }
  }

  // Check program header table
  if (hdr->e_phoff == 0 || hdr->e_phnum == 0) {
    return false;
  }
  if (hdr->e_phoff + static_cast<u64>(hdr->e_phnum) * hdr->e_phentsize > data_size) {
    return false;
  }

  return true;
}

/// Get pointer to program header table
[[nodiscard]] inline const ProgramHeader *get_program_headers(const ElfHeader *hdr) noexcept {
  return reinterpret_cast<const ProgramHeader *>(reinterpret_cast<const u8 *>(hdr) + hdr->e_phoff);
}

} // namespace moss::kernel::elf
