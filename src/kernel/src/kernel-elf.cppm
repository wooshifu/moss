// MOSS Kernel Module - ELF Loader Partition
// ELF types, structures, and ElfLoader class declarations.

export module moss.kernel:elf;

import moss.std;
import moss.types;
import moss.result;

export namespace moss::kernel::elf {

// ELF file header constants
inline constexpr u32 ELF_MAGIC = 0x464C457F;  // "\x7FELF"
inline constexpr u8  ELF_CLASS_64 = 2;         // 64-bit ELF
inline constexpr u8  ELF_DATA_LSB = 1;         // Little-endian byte order
inline constexpr u8  ELF_VERSION = 1;          // ELF version 1

// Supported architectures
inline constexpr u16 EM_NONE = 0;       // Unspecified
inline constexpr u16 EM_X86_64 = 62;    // AMD64/x86_64
inline constexpr u16 EM_AARCH64 = 183;  // ARM64/AArch64
inline constexpr u16 EM_RISCV = 243;    // RISC-V

// File types
inline constexpr u16 ET_NONE = 0;  // Unknown type
inline constexpr u16 ET_REL = 1;   // Relocatable file
inline constexpr u16 ET_EXEC = 2;  // Executable file
inline constexpr u16 ET_DYN = 3;   // Shared object

// Program header types
inline constexpr u32 PT_NULL = 0;             // Unused
inline constexpr u32 PT_LOAD = 1;             // Loadable segment
inline constexpr u32 PT_DYNAMIC = 2;          // Dynamic linking info
inline constexpr u32 PT_INTERP = 3;           // Interpreter info
inline constexpr u32 PT_NOTE = 4;             // Auxiliary info
inline constexpr u32 PT_SHLIB = 5;            // Reserved
inline constexpr u32 PT_PHDR = 6;             // Program header table itself
inline constexpr u32 PT_TLS = 7;              // Thread-local storage

// Program header flags
inline constexpr u32 PF_X = 0x1;  // Executable
inline constexpr u32 PF_W = 0x2;  // Writable
inline constexpr u32 PF_R = 0x4;  // Readable

// 64-bit ELF file header
struct [[gnu::packed]] ElfHeader {
    u8  e_ident[16];     // ELF identification
    u16 e_type;          // File type
    u16 e_machine;       // Target architecture
    u32 e_version;       // File version
    u64 e_entry;         // Entry point virtual address
    u64 e_phoff;         // Program header table offset
    u64 e_shoff;         // Section header table offset
    u32 e_flags;         // Processor-specific flags
    u16 e_ehsize;        // ELF header size
    u16 e_phentsize;     // Program header table entry size
    u16 e_phnum;         // Program header table entry count
    u16 e_shentsize;     // Section header table entry size
    u16 e_shnum;         // Section header table entry count
    u16 e_shstrndx;      // Section name string table index
};

// 64-bit program header table entry
struct [[gnu::packed]] ProgramHeader {
    u32 p_type;          // Segment type
    u32 p_flags;         // Segment flags
    u64 p_offset;        // Segment offset in file
    u64 p_vaddr;         // Segment virtual address
    u64 p_paddr;         // Segment physical address (usually ignored)
    u64 p_filesz;        // Segment size in file
    u64 p_memsz;         // Segment size in memory
    u64 p_align;         // Segment alignment
};

// ELF load result information
struct LoadedProgram {
    VirtAddr entry_point;       // Program entry point
    VirtAddr base_address;      // Program base address
    VirtAddr stack_top;         // User stack top
    VirtAddr heap_start;        // Heap start address
    usize total_size;           // Total program memory size
    u32 load_segments;          // Number of loaded segments
};

// Memory segment information
struct MemorySegment {
    VirtAddr vaddr;             // Virtual address
    usize size;                 // Segment size
    u32 flags;                  // Access permission flags
    u32 type;                   // Segment type
};

// ELF loader class
class ElfLoader {
public:
    ElfLoader() noexcept = default;
    ~ElfLoader() noexcept = default;

    // Non-copyable, non-movable
    ElfLoader(const ElfLoader&) = delete;
    ElfLoader& operator=(const ElfLoader&) = delete;
    ElfLoader(ElfLoader&&) = delete;
    ElfLoader& operator=(ElfLoader&&) = delete;

    // Validate ELF file format
    [[nodiscard]] static VoidResult validate_elf_header(const ElfHeader* header) noexcept;

    // Check architecture compatibility
    [[nodiscard]] static VoidResult check_architecture_compatibility(u16 e_machine) noexcept;

    // Parse program header table
    [[nodiscard]] static VoidResult parse_program_headers(
        const u8* elf_data, const ElfHeader* header,
        MemorySegment* segments, u32 max_segments, u32* segment_count) noexcept;

    // Calculate program memory layout
    [[nodiscard]] static VoidResult calculate_memory_layout(
        const MemorySegment* segments, u32 segment_count,
        VirtAddr* base_address, usize* total_size) noexcept;

    // Allocate user address space
    [[nodiscard]] static VoidResult allocate_user_address_space(
        VirtAddr base_address, usize total_size, VirtAddr* allocated_base) noexcept;

    // Map ELF segments to memory
    [[nodiscard]] static VoidResult map_elf_segments(
        const u8* elf_data, const MemorySegment* segments, u32 segment_count,
        VirtAddr base_address) noexcept;

    // Set up user stack
    [[nodiscard]] static VoidResult setup_user_stack(
        VirtAddr* stack_top, usize stack_size = 8 * 1024 * 1024) noexcept; // Default 8MB stack

    // Set up user heap
    [[nodiscard]] static VoidResult setup_user_heap(
        VirtAddr stack_top, VirtAddr* heap_start) noexcept;

    // Main load function: load program from ELF data in memory
    [[nodiscard]] static Result<LoadedProgram> load_elf_from_memory(
        const u8* elf_data, usize elf_size) noexcept;

    // Main load function: load program from file (future, needs filesystem support)
    [[nodiscard]] static Result<LoadedProgram> load_elf_from_file(
        const char* filename) noexcept;

    // Debug: print ELF file information
    static void print_elf_info(const ElfHeader* header) noexcept;

    // Debug: print program header information
    static void print_program_headers(const u8* elf_data, const ElfHeader* header) noexcept;

    // Debug: print load result
    static void print_loaded_program(const LoadedProgram& program) noexcept;

private:
    // Internal helper: page alignment
    [[nodiscard]] static VirtAddr align_to_page(VirtAddr addr) noexcept {
        constexpr usize PAGE_SIZE_LOCAL = 4096;
        return (addr + PAGE_SIZE_LOCAL - 1) & ~(PAGE_SIZE_LOCAL - 1);
    }

    // Internal helper: check address range
    [[nodiscard]] static bool is_valid_user_address(VirtAddr addr) noexcept {
        // User address space: 0x00000000_00000000 to 0x00007FFF_FFFFFFFF (128TB)
        return addr < 0x0000800000000000ULL;
    }

    // Internal helper: permission flag conversion
    [[nodiscard]] static u32 elf_flags_to_memory_flags(u32 elf_flags) noexcept;
};

// ELF loader statistics
struct ElfLoaderStats {
    u64 total_loads;             // Total load count
    u64 successful_loads;        // Successful load count
    u64 failed_loads;            // Failed load count
    u64 bytes_loaded;            // Total bytes loaded
    u64 memory_allocated;        // Total memory allocated
};

// Global ELF loader statistics
extern ElfLoaderStats g_elf_loader_stats;

// ELF load error codes
enum class ElfLoadError : u32 {
    InvalidMagic = 1,            // Invalid ELF magic number
    InvalidClass = 2,            // Unsupported ELF class
    InvalidEndian = 3,           // Unsupported byte order
    InvalidVersion = 4,          // Unsupported ELF version
    UnsupportedArch = 5,         // Unsupported architecture
    InvalidFileType = 6,         // Invalid file type
    InvalidProgramHeaders = 7,   // Invalid program headers
    MemoryAllocationFailed = 8,  // Memory allocation failed
    AddressSpaceExhausted = 9,   // Address space exhausted
    InvalidSegment = 10,         // Invalid memory segment
    MappingFailed = 11,          // Memory mapping failed
};

} // namespace moss::kernel::elf
