// MOSS内核ELF程序加载器实现
// 支持加载64位ELF可执行文件到用户空间

#include "elf_loader.hpp"
// #include "../mm/kernel_memory.hpp"  // TODO: 将来实现内存分配时需要

// 用于调试输出
extern "C" void early_debug_print(const char* message) noexcept;

namespace moss::kernel::elf {

// 全局ELF加载器统计
ElfLoaderStats g_elf_loader_stats = {0, 0, 0, 0, 0};

// 验证ELF文件格式
VoidResult ElfLoader::validate_elf_header(const ElfHeader* header) noexcept {
    if (!header) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    // 检查ELF魔数
    const u32* magic = reinterpret_cast<const u32*>(header->e_ident);
    if (*magic != ELF_MAGIC) {
        early_debug_print("❌ ELF: 无效的魔数\n");
        return VoidResult{ErrorCode::InvalidParameter};
    }

    // 检查ELF类别(64位)
    if (header->e_ident[4] != ELF_CLASS_64) {
        early_debug_print("❌ ELF: 不支持的文件类别(需要64位)\n");
        return VoidResult{ErrorCode::NotSupported};
    }

    // 检查字节序(小端)
    if (header->e_ident[5] != ELF_DATA_LSB) {
        early_debug_print("❌ ELF: 不支持的字节序(需要小端)\n");
        return VoidResult{ErrorCode::NotSupported};
    }

    // 检查ELF版本
    if (header->e_ident[6] != ELF_VERSION || header->e_version != ELF_VERSION) {
        early_debug_print("❌ ELF: 不支持的ELF版本\n");
        return VoidResult{ErrorCode::NotSupported};
    }

    // 检查文件类型(必须是可执行文件)
    if (header->e_type != ET_EXEC) {
        early_debug_print("❌ ELF: 不支持的文件类型(需要可执行文件)\n");
        return VoidResult{ErrorCode::InvalidParameter};
    }

    return VoidResult{};
}

// 检查架构兼容性
VoidResult ElfLoader::check_architecture_compatibility(u16 e_machine) noexcept {
    bool supported = false;

#if defined(MOSS_ARCH_ARM64)
    supported = (e_machine == EM_AARCH64);
    if (!supported) {
        early_debug_print("❌ ELF: ARM64架构不支持此ELF文件\n");
    }
#elif defined(MOSS_ARCH_X86_64)
    supported = (e_machine == EM_X86_64);
    if (!supported) {
        early_debug_print("❌ ELF: x86_64架构不支持此ELF文件\n");
    }
#elif defined(MOSS_ARCH_RISCV)
    supported = (e_machine == EM_RISCV);
    if (!supported) {
        early_debug_print("❌ ELF: RISC-V架构不支持此ELF文件\n");
    }
#else
    early_debug_print("❌ ELF: 未知目标架构\n");
    supported = false;
#endif

    return supported ? VoidResult{} : VoidResult{ErrorCode::NotSupported};
}

// 解析程序头表
VoidResult ElfLoader::parse_program_headers(
    const u8* elf_data, const ElfHeader* header,
    MemorySegment* segments, u32 max_segments, u32* segment_count) noexcept {

    if (!elf_data || !header || !segments || !segment_count) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    *segment_count = 0;

    // 检查程序头表
    if (header->e_phoff == 0 || header->e_phnum == 0) {
        early_debug_print("❌ ELF: 无程序头表\n");
        return VoidResult{ErrorCode::InvalidParameter};
    }

    if (header->e_phentsize != sizeof(ProgramHeader)) {
        early_debug_print("❌ ELF: 程序头大小不匹配\n");
        return VoidResult{ErrorCode::InvalidParameter};
    }

    const u8* phdr_data = elf_data + header->e_phoff;
    const ProgramHeader* phdrs = reinterpret_cast<const ProgramHeader*>(phdr_data);

    // 解析所有LOAD类型的段
    for (u16 i = 0; i < header->e_phnum; ++i) {
        const ProgramHeader& phdr = phdrs[i];

        if (phdr.p_type == PT_LOAD) {
            if (*segment_count >= max_segments) {
                early_debug_print("❌ ELF: 段数量超过限制\n");
                return VoidResult{ErrorCode::OutOfMemory};
            }

            // 验证段的有效性
            if (phdr.p_memsz < phdr.p_filesz) {
                early_debug_print("❌ ELF: 段大小无效\n");
                return VoidResult{ErrorCode::InvalidParameter};
            }

            if (!is_valid_user_address(phdr.p_vaddr)) {
                early_debug_print("❌ ELF: 段地址不在用户空间范围内\n");
                return VoidResult{ErrorCode::InvalidParameter};
            }

            // 保存段信息
            MemorySegment& seg = segments[*segment_count];
            seg.vaddr = phdr.p_vaddr;
            seg.size = phdr.p_memsz;
            seg.flags = elf_flags_to_memory_flags(phdr.p_flags);
            seg.type = phdr.p_type;

            (*segment_count)++;
        }
    }

    if (*segment_count == 0) {
        early_debug_print("❌ ELF: 没有找到可加载段\n");
        return VoidResult{ErrorCode::InvalidParameter};
    }

    return VoidResult{};
}

// 计算程序内存布局
VoidResult ElfLoader::calculate_memory_layout(
    const MemorySegment* segments, u32 segment_count,
    VirtAddr* base_address, usize* total_size) noexcept {

    if (!segments || segment_count == 0 || !base_address || !total_size) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    VirtAddr min_addr = UINT64_MAX;
    VirtAddr max_addr = 0;

    // 找到最小和最大虚拟地址
    for (u32 i = 0; i < segment_count; ++i) {
        VirtAddr seg_start = segments[i].vaddr;
        VirtAddr seg_end = seg_start + segments[i].size;

        if (seg_start < min_addr) {
            min_addr = seg_start;
        }
        if (seg_end > max_addr) {
            max_addr = seg_end;
        }
    }

    // 对齐到页边界
    *base_address = align_to_page(min_addr);
    *total_size = align_to_page(max_addr - min_addr);

    return VoidResult{};
}

// 分配用户地址空间
VoidResult ElfLoader::allocate_user_address_space(
    VirtAddr base_address, usize total_size, VirtAddr* allocated_base) noexcept {

    if (!allocated_base || total_size == 0) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    // 暂时忽略原始base_address，使用内核可访问的地址
    (void)base_address;

    // TODO: 实现真正的用户地址空间分配
    // 快速修复：将用户程序映射到内核可访问的地址空间
    // 使用内核堆区域的高地址部分作为用户程序空间
    constexpr VirtAddr KERNEL_USER_SPACE_BASE = 0x40800000ULL; // 内核堆之后
    *allocated_base = KERNEL_USER_SPACE_BASE;

    early_debug_print("📍 ELF: 分配用户地址空间 - 基址: ");
    // TODO: 添加十六进制打印辅助函数

    return VoidResult{};
}

// 映射ELF段到内存
VoidResult ElfLoader::map_elf_segments(
    const u8* elf_data, const MemorySegment* segments, u32 segment_count,
    VirtAddr base_address) noexcept {

    if (!elf_data || !segments || segment_count == 0) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    (void)segments;      // TODO: 使用segments信息进行映射验证

    const ElfHeader* header = reinterpret_cast<const ElfHeader*>(elf_data);
    const u8* phdr_data = elf_data + header->e_phoff;
    const ProgramHeader* phdrs = reinterpret_cast<const ProgramHeader*>(phdr_data);

    // 计算原始ELF的基地址，用于重定位
    VirtAddr original_base = UINT64_MAX;
    for (u16 i = 0; i < header->e_phnum; ++i) {
        const ProgramHeader& phdr = phdrs[i];
        if (phdr.p_type == PT_LOAD && phdr.p_vaddr < original_base) {
            original_base = phdr.p_vaddr;
        }
    }

    // 映射每个LOAD段到新的基地址
    for (u16 i = 0; i < header->e_phnum; ++i) {
        const ProgramHeader& phdr = phdrs[i];

        if (phdr.p_type == PT_LOAD) {
            // 计算重定位后的目标地址
            VirtAddr offset = phdr.p_vaddr - original_base;
            VirtAddr target_addr = base_address + offset;

            // 实际拷贝文件内容到新地址
            if (phdr.p_filesz > 0) {
                const u8* source_data = elf_data + phdr.p_offset;

                // 简单的内存拷贝实现
                u8* dest = reinterpret_cast<u8*>(target_addr);
                for (usize j = 0; j < phdr.p_filesz; ++j) {
                    dest[j] = source_data[j];
                }

                early_debug_print("📋 ELF: 映射段到内存 - 已拷贝 ");
                // 简化的数字输出
                usize size = phdr.p_filesz;
                if (size > 0 && size < 100000) {
                    char size_str[16];
                    int pos = 0;
                    while (size > 0 && pos < 15) {
                        size_str[pos++] = '0' + static_cast<char>(size % 10);
                        size /= 10;
                    }
                    // 反转字符串
                    for (int k = 0; k < pos / 2; k++) {
                        char temp = size_str[k];
                        size_str[k] = size_str[pos - 1 - k];
                        size_str[pos - 1 - k] = temp;
                    }
                    size_str[pos] = '\0';
                    early_debug_print(size_str);
                }
                early_debug_print(" 字节\n");
            }

            // 如果内存大小大于文件大小，清零剩余部分(BSS段)
            if (phdr.p_memsz > phdr.p_filesz) {
                VirtAddr bss_start = phdr.p_vaddr + phdr.p_filesz;
                usize bss_size = phdr.p_memsz - phdr.p_filesz;

                // 清零BSS段
                u8* bss_dest = reinterpret_cast<u8*>(bss_start);
                for (usize j = 0; j < bss_size; ++j) {
                    bss_dest[j] = 0;
                }

                early_debug_print("🧹 ELF: 清零BSS段\n");
            }
        }
    }

    return VoidResult{};
}

// 设置用户栈
VoidResult ElfLoader::setup_user_stack(VirtAddr* stack_top, usize stack_size) noexcept {
    if (!stack_top) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    (void)stack_size; // TODO: 实际使用stack_size参数

    // 用户栈通常位于用户地址空间的高地址
    // 对于64位系统，用户空间上限通常是0x00007FFFFFFFFFFF
    constexpr VirtAddr USER_STACK_BASE = 0x00007FFF00000000ULL;

    *stack_top = USER_STACK_BASE;

    // TODO: 实现真正的栈分配
    // 1. 分配物理页面
    // 2. 建立虚拟内存映射
    // 3. 设置栈权限(可读写，不可执行)

    early_debug_print("📚 ELF: 设置用户栈\n");
    return VoidResult{};
}

// 设置用户堆
VoidResult ElfLoader::setup_user_heap(VirtAddr stack_top, VirtAddr* heap_start) noexcept {
    if (!heap_start) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    (void)stack_top; // TODO: 基于stack_top计算合适的堆地址

    // 堆通常位于程序段之后，栈之前
    // TODO: 计算合适的堆起始地址
    *heap_start = 0x0000000100000000ULL; // 4GB处开始(简化实现)

    early_debug_print("🏗️ ELF: 设置用户堆\n");
    return VoidResult{};
}

// 主加载函数：从内存中的ELF数据加载程序
Result<LoadedProgram> ElfLoader::load_elf_from_memory(
    const u8* elf_data, usize elf_size) noexcept {

    ++g_elf_loader_stats.total_loads;

    if (!elf_data || elf_size < sizeof(ElfHeader)) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{ErrorCode::InvalidArgument};
    }

    early_debug_print("🚀 ELF: 开始加载程序...\n");

    const ElfHeader* header = reinterpret_cast<const ElfHeader*>(elf_data);

    // 1. 验证ELF文件头
    auto validate_result = validate_elf_header(header);
    if (!validate_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{validate_result.error()};
    }

    // 2. 检查架构兼容性
    auto arch_result = check_architecture_compatibility(header->e_machine);
    if (!arch_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{arch_result.error()};
    }

    // 3. 解析程序头表
    constexpr u32 MAX_SEGMENTS = 16;
    MemorySegment segments[MAX_SEGMENTS];
    u32 segment_count = 0;

    auto parse_result = parse_program_headers(
        elf_data, header, segments, MAX_SEGMENTS, &segment_count);
    if (!parse_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{parse_result.error()};
    }

    // 4. 计算内存布局
    VirtAddr base_address = 0;
    usize total_size = 0;
    auto layout_result = calculate_memory_layout(
        segments, segment_count, &base_address, &total_size);
    if (!layout_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{layout_result.error()};
    }

    // 5. 分配用户地址空间
    VirtAddr allocated_base = 0;
    auto alloc_result = allocate_user_address_space(
        base_address, total_size, &allocated_base);
    if (!alloc_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{alloc_result.error()};
    }

    // 6. 映射ELF段到内存
    auto map_result = map_elf_segments(elf_data, segments, segment_count, allocated_base);
    if (!map_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{map_result.error()};
    }

    // 7. 设置用户栈
    VirtAddr stack_top = 0;
    auto stack_result = setup_user_stack(&stack_top);
    if (!stack_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{stack_result.error()};
    }

    // 8. 设置用户堆
    VirtAddr heap_start = 0;
    auto heap_result = setup_user_heap(stack_top, &heap_start);
    if (!heap_result) {
        ++g_elf_loader_stats.failed_loads;
        return Result<LoadedProgram>{heap_result.error()};
    }

    // 9. 构造加载结果 - 重定位入口点
    // 重新计算原始基地址以计算入口点偏移
    VirtAddr original_base = UINT64_MAX;
    const u8* phdr_data = elf_data + header->e_phoff;
    const ProgramHeader* phdrs = reinterpret_cast<const ProgramHeader*>(phdr_data);
    for (u16 i = 0; i < header->e_phnum; ++i) {
        const ProgramHeader& phdr = phdrs[i];
        if (phdr.p_type == PT_LOAD && phdr.p_vaddr < original_base) {
            original_base = phdr.p_vaddr;
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

    // 更新统计信息
    ++g_elf_loader_stats.successful_loads;
    g_elf_loader_stats.bytes_loaded += elf_size;
    g_elf_loader_stats.memory_allocated += total_size;

    early_debug_print("✅ ELF: 程序加载完成\n");

    return Result<LoadedProgram>{program};
}

// 主加载函数：从文件加载程序(未来实现)
Result<LoadedProgram> ElfLoader::load_elf_from_file(const char* filename) noexcept {
    // TODO: 实现文件系统支持后再实现此功能
    (void)filename;
    ++g_elf_loader_stats.failed_loads;
    return Result<LoadedProgram>{ErrorCode::NotSupported};
}

// 调试：打印ELF文件信息
void ElfLoader::print_elf_info(const ElfHeader* header) noexcept {
    if (!header) return;

    early_debug_print("=== ELF文件信息 ===\n");
    early_debug_print("类型: ");
    switch (header->e_type) {
        case ET_EXEC: early_debug_print("可执行文件\n"); break;
        case ET_DYN: early_debug_print("共享库\n"); break;
        case ET_REL: early_debug_print("可重定位文件\n"); break;
        default: early_debug_print("未知\n"); break;
    }

    early_debug_print("架构: ");
    switch (header->e_machine) {
        case EM_AARCH64: early_debug_print("ARM64\n"); break;
        case EM_X86_64: early_debug_print("x86_64\n"); break;
        case EM_RISCV: early_debug_print("RISC-V\n"); break;
        default: early_debug_print("未知\n"); break;
    }

    early_debug_print("程序头数量: ");
    // TODO: 打印数字
    early_debug_print("\n入口点: ");
    // TODO: 打印十六进制地址
    early_debug_print("\n==================\n");
}

// 调试：打印程序头信息
void ElfLoader::print_program_headers(const u8* elf_data, const ElfHeader* header) noexcept {
    if (!elf_data || !header) return;

    early_debug_print("=== 程序头表 ===\n");
    const u8* phdr_data = elf_data + header->e_phoff;
    const ProgramHeader* phdrs = reinterpret_cast<const ProgramHeader*>(phdr_data);

    for (u16 i = 0; i < header->e_phnum; ++i) {
        const ProgramHeader& phdr = phdrs[i];
        if (phdr.p_type == PT_LOAD) {
            early_debug_print("段 ");
            // TODO: 打印段编号和详细信息
            early_debug_print(": LOAD\n");
        }
    }
    early_debug_print("================\n");
}

// 调试：打印加载结果
void ElfLoader::print_loaded_program(const LoadedProgram& program) noexcept {
    (void)program; // TODO: 实际打印program信息

    early_debug_print("=== 加载结果 ===\n");
    early_debug_print("入口点: ");
    // TODO: 打印program.entry_point
    early_debug_print("\n基址: ");
    // TODO: 打印program.base_address
    early_debug_print("\n栈顶: ");
    // TODO: 打印program.stack_top
    early_debug_print("\n堆起始: ");
    // TODO: 打印program.heap_start
    early_debug_print("\n================\n");
}

// 内部辅助函数：权限标志转换
u32 ElfLoader::elf_flags_to_memory_flags(u32 elf_flags) noexcept {
    u32 mem_flags = 0;

    if (elf_flags & PF_R) {
        mem_flags |= 0x1; // 可读
    }
    if (elf_flags & PF_W) {
        mem_flags |= 0x2; // 可写
    }
    if (elf_flags & PF_X) {
        mem_flags |= 0x4; // 可执行
    }

    return mem_flags;
}

} // namespace moss::kernel::elf
