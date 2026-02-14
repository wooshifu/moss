// MOSS ELF Loader — loads 64-bit ELF executables into user address space
//
// Uses klog for all diagnostic output (replaces legacy early_debug_print).

module;

#include "arch_detect.h"

#define MOSS_UINT64_MAX static_cast<unsigned long long>(-1)

module moss.kernel;

namespace log = moss::kernel::logging;

namespace moss::kernel::elf {

ElfLoaderStats g_elf_loader_stats = {0, 0, 0, 0, 0};

VoidResult ElfLoader::validate_elf_header(const ElfHeader* header) noexcept {
    if (!header) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    const u32* magic = reinterpret_cast<const u32*>(header->e_ident);
    if (*magic != ELF_MAGIC) {
        log::klog::error("ELF: invalid magic number");
        return VoidResult{ErrorCode::InvalidParameter};
    }

    if (header->e_ident[4] != ELF_CLASS_64) {
        log::klog::error("ELF: unsupported class (need 64-bit)");
        return VoidResult{ErrorCode::NotSupported};
    }

    if (header->e_ident[5] != ELF_DATA_LSB) {
        log::klog::error("ELF: unsupported endianness (need little-endian)");
        return VoidResult{ErrorCode::NotSupported};
    }

    if (header->e_ident[6] != ELF_VERSION || header->e_version != ELF_VERSION) {
        log::klog::error("ELF: unsupported version");
        return VoidResult{ErrorCode::NotSupported};
    }

    if (header->e_type != ET_EXEC) {
        log::klog::error("ELF: unsupported file type (need executable)");
        return VoidResult{ErrorCode::InvalidParameter};
    }

    return VoidResult{};
}

VoidResult ElfLoader::check_architecture_compatibility(u16 e_machine) noexcept {
    bool supported = false;

#if defined(MOSS_ARCH_ARM64)
    supported = (e_machine == EM_AARCH64);
#elif defined(MOSS_ARCH_X86_64)
    supported = (e_machine == EM_X86_64);
#elif defined(MOSS_ARCH_RISCV)
    supported = (e_machine == EM_RISCV);
#endif

    if (!supported) {
        log::klog::error("ELF: architecture mismatch (e_machine={:#x})", e_machine);
    }
    return supported ? VoidResult{} : VoidResult{ErrorCode::NotSupported};
}

VoidResult ElfLoader::parse_program_headers(
    const u8* elf_data, const ElfHeader* header,
    MemorySegment* segments, u32 max_segments, u32* segment_count) noexcept {

    if (!elf_data || !header || !segments || !segment_count) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    *segment_count = 0;

    if (header->e_phoff == 0 || header->e_phnum == 0) {
        log::klog::error("ELF: no program header table");
        return VoidResult{ErrorCode::InvalidParameter};
    }

    if (header->e_phentsize != sizeof(ProgramHeader)) {
        log::klog::error("ELF: program header size mismatch");
        return VoidResult{ErrorCode::InvalidParameter};
    }

    const auto* phdrs = reinterpret_cast<const ProgramHeader*>(
        elf_data + header->e_phoff);

    for (u16 i = 0; i < header->e_phnum; ++i) {
        const auto& phdr = phdrs[i];

        if (phdr.p_type == PT_LOAD) {
            if (*segment_count >= max_segments) {
                log::klog::error("ELF: too many LOAD segments (max {})", max_segments);
                return VoidResult{ErrorCode::OutOfMemory};
            }

            if (phdr.p_memsz < phdr.p_filesz) {
                log::klog::error("ELF: invalid segment size");
                return VoidResult{ErrorCode::InvalidParameter};
            }

            if (!is_valid_user_address(phdr.p_vaddr)) {
                log::klog::error("ELF: segment vaddr={:#x} not in user space", phdr.p_vaddr);
                return VoidResult{ErrorCode::InvalidParameter};
            }

            auto& seg = segments[*segment_count];
            seg.vaddr = phdr.p_vaddr;
            seg.size = phdr.p_memsz;
            seg.flags = elf_flags_to_memory_flags(phdr.p_flags);
            seg.type = phdr.p_type;

            (*segment_count)++;
        }
    }

    if (*segment_count == 0) {
        log::klog::error("ELF: no LOAD segments found");
        return VoidResult{ErrorCode::InvalidParameter};
    }

    return VoidResult{};
}

VoidResult ElfLoader::calculate_memory_layout(
    const MemorySegment* segments, u32 segment_count,
    VirtAddr* base_address, usize* total_size) noexcept {

    if (!segments || segment_count == 0 || !base_address || !total_size) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    VirtAddr min_addr = MOSS_UINT64_MAX;
    VirtAddr max_addr = 0;

    for (u32 i = 0; i < segment_count; ++i) {
        VirtAddr seg_start = segments[i].vaddr;
        VirtAddr seg_end = seg_start + segments[i].size;

        if (seg_start < min_addr) min_addr = seg_start;
        if (seg_end > max_addr) max_addr = seg_end;
    }

    *base_address = align_to_page(min_addr);
    *total_size = align_to_page(max_addr - min_addr);

    return VoidResult{};
}

VoidResult ElfLoader::allocate_user_address_space(
    VirtAddr base_address, usize total_size, VirtAddr* allocated_base) noexcept {

    if (!allocated_base || total_size == 0) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    (void)base_address;

    // TODO: Real user address space allocation
    constexpr VirtAddr KERNEL_USER_SPACE_BASE = 0x40800000ULL;
    *allocated_base = KERNEL_USER_SPACE_BASE;

    log::klog::debug("ELF: allocated user address space at {:#x}", KERNEL_USER_SPACE_BASE);

    return VoidResult{};
}

VoidResult ElfLoader::map_elf_segments(
    const u8* elf_data, const MemorySegment* segments, u32 segment_count,
    VirtAddr base_address) noexcept {

    if (!elf_data || !segments || segment_count == 0) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    (void)segments;

    const auto* header = reinterpret_cast<const ElfHeader*>(elf_data);
    const auto* phdrs = reinterpret_cast<const ProgramHeader*>(
        elf_data + header->e_phoff);

    VirtAddr original_base = MOSS_UINT64_MAX;
    for (u16 i = 0; i < header->e_phnum; ++i) {
        if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_vaddr < original_base) {
            original_base = phdrs[i].p_vaddr;
        }
    }

    for (u16 i = 0; i < header->e_phnum; ++i) {
        const auto& phdr = phdrs[i];

        if (phdr.p_type == PT_LOAD) {
            VirtAddr offset = phdr.p_vaddr - original_base;
            VirtAddr target_addr = base_address + offset;

            if (phdr.p_filesz > 0) {
                const u8* src = elf_data + phdr.p_offset;
                u8* dst = reinterpret_cast<u8*>(target_addr);
                for (usize j = 0; j < phdr.p_filesz; ++j) {
                    dst[j] = src[j];
                }
                log::klog::debug("ELF: mapped LOAD segment {} bytes to {:#x}",
                                 phdr.p_filesz, target_addr);
            }

            if (phdr.p_memsz > phdr.p_filesz) {
                VirtAddr bss_start = phdr.p_vaddr + phdr.p_filesz;
                usize bss_size = phdr.p_memsz - phdr.p_filesz;
                u8* bss_dst = reinterpret_cast<u8*>(bss_start);
                for (usize j = 0; j < bss_size; ++j) {
                    bss_dst[j] = 0;
                }
                log::klog::debug("ELF: zeroed BSS {} bytes at {:#x}",
                                 bss_size, bss_start);
            }
        }
    }

    return VoidResult{};
}

VoidResult ElfLoader::setup_user_stack(VirtAddr* stack_top, usize stack_size) noexcept {
    if (!stack_top) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    (void)stack_size;

    constexpr VirtAddr USER_STACK_BASE = 0x00007FFF00000000ULL;
    *stack_top = USER_STACK_BASE;

    log::klog::debug("ELF: user stack top at {:#x}", USER_STACK_BASE);
    return VoidResult{};
}

VoidResult ElfLoader::setup_user_heap(VirtAddr stack_top, VirtAddr* heap_start) noexcept {
    if (!heap_start) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    (void)stack_top;

    *heap_start = 0x0000000100000000ULL;

    log::klog::debug("ELF: user heap starts at {:#x}", *heap_start);
    return VoidResult{};
}

Result<LoadedProgram> ElfLoader::load_elf_from_memory(
    const u8* elf_data, usize elf_size) noexcept {

    ++g_elf_loader_stats.total_loads;

    if (!elf_data || elf_size < sizeof(ElfHeader)) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{ErrorCode::InvalidArgument};
    }

    log::klog::info("ELF: loading program ({} bytes)", elf_size);

    const auto* header = reinterpret_cast<const ElfHeader*>(elf_data);

    auto validate_result = validate_elf_header(header);
    if (!validate_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{validate_result.error()};
    }

    auto arch_result = check_architecture_compatibility(header->e_machine);
    if (!arch_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{arch_result.error()};
    }

    constexpr u32 MAX_SEGMENTS = 16;
    MemorySegment segments[MAX_SEGMENTS];
    u32 segment_count = 0;

    auto parse_result = parse_program_headers(
        elf_data, header, segments, MAX_SEGMENTS, &segment_count);
    if (!parse_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{parse_result.error()};
    }

    VirtAddr base_address = 0;
    usize total_size = 0;
    auto layout_result = calculate_memory_layout(
        segments, segment_count, &base_address, &total_size);
    if (!layout_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{layout_result.error()};
    }

    VirtAddr allocated_base = 0;
    auto alloc_result = allocate_user_address_space(
        base_address, total_size, &allocated_base);
    if (!alloc_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{alloc_result.error()};
    }

    auto map_result = map_elf_segments(
        elf_data, segments, segment_count, allocated_base);
    if (!map_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{map_result.error()};
    }

    VirtAddr stack_top = 0;
    auto stack_result = setup_user_stack(&stack_top);
    if (!stack_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{stack_result.error()};
    }

    VirtAddr heap_start = 0;
    auto heap_result = setup_user_heap(stack_top, &heap_start);
    if (!heap_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{heap_result.error()};
    }

    VirtAddr original_base = MOSS_UINT64_MAX;
    const auto* phdrs2 = reinterpret_cast<const ProgramHeader*>(
        elf_data + header->e_phoff);
    for (u16 i = 0; i < header->e_phnum; ++i) {
        if (phdrs2[i].p_type == PT_LOAD && phdrs2[i].p_vaddr < original_base) {
            original_base = phdrs2[i].p_vaddr;
        }
    }
    VirtAddr entry_offset = header->e_entry - original_base;
    VirtAddr relocated_entry = allocated_base + entry_offset;

    LoadedProgram program = {
        .entry_point = relocated_entry,
        .base_address = allocated_base,
        .stack_top = stack_top,
        .heap_start = heap_start,
        .total_size = total_size,
        .load_segments = segment_count
    };

    ++g_elf_loader_stats.successful_loads;
    g_elf_loader_stats.bytes_loaded += elf_size;
    g_elf_loader_stats.memory_allocated += total_size;

    log::klog::info("ELF: loaded successfully entry={:#x} base={:#x} size={}",
                    relocated_entry, allocated_base, total_size);

    return Result<LoadedProgram>{program};
}

Result<LoadedProgram> ElfLoader::load_elf_from_file(const char* filename) noexcept {
    (void)filename;
    ++g_elf_loader_stats.failed_loads;
    return Result<LoadedProgram>{ErrorCode::NotSupported};
}

void ElfLoader::print_elf_info(const ElfHeader* header) noexcept {
    if (!header) return;

    const char* type_str = "unknown";
    switch (header->e_type) {
        case ET_EXEC: type_str = "executable"; break;
        case ET_DYN:  type_str = "shared library"; break;
        case ET_REL:  type_str = "relocatable"; break;
        default: break;
    }

    const char* arch_str = "unknown";
    switch (header->e_machine) {
        case EM_AARCH64: arch_str = "ARM64"; break;
        case EM_X86_64:  arch_str = "x86_64"; break;
        case EM_RISCV:   arch_str = "RISC-V"; break;
        default: break;
    }

    log::klog::info("ELF: type={} arch={} phnum={} entry={:#x}",
                    type_str, arch_str, header->e_phnum, header->e_entry);
}

void ElfLoader::print_program_headers(const u8* elf_data, const ElfHeader* header) noexcept {
    if (!elf_data || !header) return;

    const auto* phdrs = reinterpret_cast<const ProgramHeader*>(
        elf_data + header->e_phoff);

    for (u16 i = 0; i < header->e_phnum; ++i) {
        if (phdrs[i].p_type == PT_LOAD) {
            log::klog::debug("  LOAD[{}]: vaddr={:#x} memsz={} filesz={} flags={:#x}",
                             i, phdrs[i].p_vaddr, phdrs[i].p_memsz,
                             phdrs[i].p_filesz, phdrs[i].p_flags);
        }
    }
}

void ElfLoader::print_loaded_program(const LoadedProgram& program) noexcept {
    log::klog::info("ELF loaded: entry={:#x} base={:#x} stack={:#x} heap={:#x}",
                    program.entry_point, program.base_address,
                    program.stack_top, program.heap_start);
}

u32 ElfLoader::elf_flags_to_memory_flags(u32 elf_flags) noexcept {
    u32 mem_flags = 0;
    if (elf_flags & PF_R) mem_flags |= 0x1;
    if (elf_flags & PF_W) mem_flags |= 0x2;
    if (elf_flags & PF_X) mem_flags |= 0x4;
    return mem_flags;
}

} // namespace moss::kernel::elf
