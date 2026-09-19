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

// The existing 64-header limit bounds pairwise page-overlap validation. Its
// original calibration is unrecorded; changing it also changes the fixed plan
// size and the n*(n-1)/2 overlap cost (2,016 comparisons when n is 64).
inline constexpr usize MAX_PROGRAM_HEADERS = 64;

struct LoadRange {
  u64 start;
  u64 end;
};

struct LoadSegment {
  u64 page_start;
  u64 page_end;
  usize backing_offset;
  usize backing_size;
  u32 flags;
};

struct LoadPlanPolicy {
  u64 page_size;
  u64 user_begin;
  u64 user_end;
  const LoadRange *reserved;
  usize reserved_count;
};

// A successful plan is a complete immutable description of every PT_LOAD.
// The caller discards this object on failure and consumes it read-only after
// success, so VMA construction cannot diverge from validation arithmetic.
struct LoadPlan {
  LoadSegment segments[MAX_PROGRAM_HEADERS]{};
  usize segment_count{};
  u64 entry{};
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

/// Validate an image and produce the page/file intervals consumed by demand paging.
/// `out` is valid only when this function succeeds.
[[nodiscard]] inline bool build_load_plan(const u8 *image, usize image_size, const LoadPlanPolicy &policy,
                                          LoadPlan &out) noexcept {
  if (!image || policy.page_size == 0 || (policy.page_size & (policy.page_size - 1)) != 0 ||
      policy.user_begin >= policy.user_end || (policy.reserved_count != 0 && !policy.reserved)) {
    return false;
  }
  const auto *header = reinterpret_cast<const ElfHeader *>(image);
  if (!validate_elf_header(header, image_size) || header->e_phnum > MAX_PROGRAM_HEADERS) {
    return false;
  }

  out = LoadPlan{};
  out.entry = header->e_entry;
  const auto *headers = get_program_headers(header);
  const u64 page_mask = policy.page_size - 1;
  bool executable_entry = false;

  for (u16 i = 0; i < header->e_phnum; ++i) {
    const auto &program = headers[i];
    if (program.p_type == PT_INTERP || program.p_type == PT_DYNAMIC) {
      // This fixed-address loader has no interpreter or relocator.
      return false;
    }
    if (program.p_type != PT_LOAD && program.p_type != PT_TLS) {
      continue;
    }
    if (program.p_offset > image_size || program.p_filesz > image_size - program.p_offset ||
        program.p_filesz > program.p_memsz) {
      return false;
    }
    if (program.p_type == PT_TLS) {
      if (program.p_memsz != 0 &&
          (program.p_vaddr < policy.user_begin || program.p_vaddr >= policy.user_end ||
           program.p_memsz > policy.user_end - program.p_vaddr || (program.p_flags & ~(PF_R | PF_W)) != 0 ||
           (program.p_align > 1 && ((program.p_align & (program.p_align - 1)) != 0 ||
                                    ((program.p_offset ^ program.p_vaddr) & (program.p_align - 1)) != 0)))) {
        return false;
      }
      continue;
    }
    if (program.p_memsz == 0) {
      continue;
    }
    if (program.p_vaddr < policy.user_begin || program.p_vaddr >= policy.user_end ||
        program.p_memsz > policy.user_end - program.p_vaddr || (program.p_flags & ~(PF_R | PF_W | PF_X)) != 0 ||
        (program.p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) {
      return false;
    }

    const u64 memory_end = program.p_vaddr + program.p_memsz;
    if (memory_end > ~u64{0} - page_mask) {
      return false;
    }
    const u64 page_start = program.p_vaddr & ~page_mask;
    const u64 page_end = (memory_end + page_mask) & ~page_mask;
    const u64 prefix = program.p_vaddr - page_start;
    if (page_start < policy.user_begin || page_end > policy.user_end || program.p_offset < prefix ||
        ((program.p_offset ^ program.p_vaddr) & page_mask) != 0 ||
        (program.p_align > 1 && ((program.p_align & (program.p_align - 1)) != 0 ||
                                 ((program.p_offset ^ program.p_vaddr) & (program.p_align - 1)) != 0)) ||
        program.p_filesz > ~u64{0} - prefix) {
      return false;
    }

    for (usize reserved = 0; reserved < policy.reserved_count; ++reserved) {
      const auto &range = policy.reserved[reserved];
      if (range.start > range.end || (page_start < range.end && page_end > range.start)) {
        return false;
      }
    }
    for (usize existing = 0; existing < out.segment_count; ++existing) {
      const auto &segment = out.segments[existing];
      if (page_start < segment.page_end && page_end > segment.page_start) {
        return false; // Shared pages would combine unrelated bytes or permissions.
      }
    }

    auto &segment = out.segments[out.segment_count++];
    segment.page_start = page_start;
    segment.page_end = page_end;
    segment.backing_offset = program.p_filesz ? static_cast<usize>(program.p_offset - prefix) : 0;
    segment.backing_size = program.p_filesz ? static_cast<usize>(prefix + program.p_filesz) : 0;
    segment.flags = program.p_flags;
    if ((program.p_flags & PF_X) != 0 && header->e_entry >= program.p_vaddr && header->e_entry < memory_end) {
      executable_entry = true;
    }
  }

  // PT_TLS is runtime metadata rather than a separate mapping. The static
  // userspace runtime owns thread-pointer setup; a NOBITS-only template needs no
  // file mapping. When file bytes do exist, require one LOAD to contain both
  // their memory interval and translation so the template is actually mapped.
  for (u16 i = 0; i < header->e_phnum; ++i) {
    const auto &tls = headers[i];
    if (tls.p_type != PT_TLS || tls.p_filesz == 0) {
      continue;
    }
    bool covered = false;
    for (u16 j = 0; j < header->e_phnum; ++j) {
      const auto &load = headers[j];
      if (load.p_type != PT_LOAD || load.p_memsz == 0 || tls.p_vaddr < load.p_vaddr ||
          tls.p_vaddr > load.p_vaddr + load.p_memsz || tls.p_memsz > load.p_vaddr + load.p_memsz - tls.p_vaddr) {
        continue;
      }
      if (tls.p_filesz != 0 && (tls.p_offset < load.p_offset || tls.p_offset > load.p_offset + load.p_filesz ||
                                tls.p_filesz > load.p_offset + load.p_filesz - tls.p_offset ||
                                tls.p_vaddr - load.p_vaddr != tls.p_offset - load.p_offset)) {
        continue;
      }
      covered = true;
      break;
    }
    if (!covered) {
      return false;
    }
  }

  return executable_entry;
}

} // namespace moss::kernel::elf
