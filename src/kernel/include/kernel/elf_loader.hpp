#pragma once

// MOSS内核ELF程序加载器
// 支持加载64位ELF可执行文件到用户空间

#include "core/types.hpp"
#include "core/result.hpp"

namespace moss::kernel::elf {

// ELF文件头常量定义
constexpr u32 ELF_MAGIC = 0x464C457F;  // "\x7FELF"
constexpr u8  ELF_CLASS_64 = 2;         // 64位ELF
constexpr u8  ELF_DATA_LSB = 1;         // 小端字节序
constexpr u8  ELF_VERSION = 1;          // ELF版本1

// 支持的架构
constexpr u16 EM_NONE = 0;      // 未指定
constexpr u16 EM_X86_64 = 62;   // AMD64/x86_64
constexpr u16 EM_AARCH64 = 183; // ARM64/AArch64
constexpr u16 EM_RISCV = 243;   // RISC-V

// 文件类型
constexpr u16 ET_NONE = 0;      // 未知类型
constexpr u16 ET_REL = 1;       // 可重定位文件
constexpr u16 ET_EXEC = 2;      // 可执行文件
constexpr u16 ET_DYN = 3;       // 共享对象

// 程序头类型
constexpr u32 PT_NULL = 0;          // 未使用
constexpr u32 PT_LOAD = 1;          // 可加载段
constexpr u32 PT_DYNAMIC = 2;       // 动态链接信息
constexpr u32 PT_INTERP = 3;        // 解释器信息
constexpr u32 PT_NOTE = 4;          // 辅助信息
constexpr u32 PT_SHLIB = 5;         // 保留
constexpr u32 PT_PHDR = 6;          // 程序头表本身
constexpr u32 PT_TLS = 7;           // 线程局部存储

// 程序头标志
constexpr u32 PF_X = 0x1;           // 可执行
constexpr u32 PF_W = 0x2;           // 可写
constexpr u32 PF_R = 0x4;           // 可读

// 64位ELF文件头
struct ElfHeader {
    u8  e_ident[16];    // ELF标识信息
    u16 e_type;         // 文件类型
    u16 e_machine;      // 目标架构
    u32 e_version;      // 文件版本
    u64 e_entry;        // 程序入口点虚拟地址
    u64 e_phoff;        // 程序头表偏移
    u64 e_shoff;        // 节头表偏移
    u32 e_flags;        // 处理器特定标志
    u16 e_ehsize;       // ELF头大小
    u16 e_phentsize;    // 程序头表项大小
    u16 e_phnum;        // 程序头表项数量
    u16 e_shentsize;    // 节头表项大小
    u16 e_shnum;        // 节头表项数量
    u16 e_shstrndx;     // 节名字符串表索引
} __attribute__((packed));

// 64位程序头表项
struct ProgramHeader {
    u32 p_type;         // 段类型
    u32 p_flags;        // 段标志
    u64 p_offset;       // 段在文件中的偏移
    u64 p_vaddr;        // 段的虚拟地址
    u64 p_paddr;        // 段的物理地址(通常忽略)
    u64 p_filesz;       // 段在文件中的大小
    u64 p_memsz;        // 段在内存中的大小
    u64 p_align;        // 段对齐要求
} __attribute__((packed));

// ELF加载结果信息
struct LoadedProgram {
    VirtAddr entry_point;       // 程序入口点
    VirtAddr base_address;      // 程序基址
    VirtAddr stack_top;         // 用户栈顶
    VirtAddr heap_start;        // 堆起始地址
    usize total_size;           // 程序总内存大小
    u32 load_segments;          // 加载的段数量
};

// 内存段信息
struct MemorySegment {
    VirtAddr vaddr;             // 虚拟地址
    usize size;                 // 段大小
    u32 flags;                  // 访问权限标志
    u32 type;                   // 段类型
};

// ELF加载器类
class ElfLoader {
public:
    ElfLoader() noexcept = default;
    ~ElfLoader() noexcept = default;

    // 禁用拷贝和移动
    NON_COPYABLE_NON_MOVABLE(ElfLoader)

    // 验证ELF文件格式
    [[nodiscard]] static VoidResult validate_elf_header(const ElfHeader* header) noexcept;

    // 检查架构兼容性
    [[nodiscard]] static VoidResult check_architecture_compatibility(u16 e_machine) noexcept;

    // 解析程序头表
    [[nodiscard]] static VoidResult parse_program_headers(
        const u8* elf_data, const ElfHeader* header,
        MemorySegment* segments, u32 max_segments, u32* segment_count) noexcept;

    // 计算程序内存布局
    [[nodiscard]] static VoidResult calculate_memory_layout(
        const MemorySegment* segments, u32 segment_count,
        VirtAddr* base_address, usize* total_size) noexcept;

    // 分配用户地址空间
    [[nodiscard]] static VoidResult allocate_user_address_space(
        VirtAddr base_address, usize total_size, VirtAddr* allocated_base) noexcept;

    // 映射ELF段到内存
    [[nodiscard]] static VoidResult map_elf_segments(
        const u8* elf_data, const MemorySegment* segments, u32 segment_count,
        VirtAddr base_address) noexcept;

    // 设置用户栈
    [[nodiscard]] static VoidResult setup_user_stack(
        VirtAddr* stack_top, usize stack_size = 8 * 1024 * 1024) noexcept; // 默认8MB栈

    // 设置用户堆
    [[nodiscard]] static VoidResult setup_user_heap(
        VirtAddr stack_top, VirtAddr* heap_start) noexcept;

    // 主加载函数：从内存中的ELF数据加载程序
    [[nodiscard]] static Result<LoadedProgram> load_elf_from_memory(
        const u8* elf_data, usize elf_size) noexcept;

    // 主加载函数：从文件加载程序(未来实现，需要文件系统支持)
    [[nodiscard]] static Result<LoadedProgram> load_elf_from_file(
        const char* filename) noexcept;

    // 调试：打印ELF文件信息
    static void print_elf_info(const ElfHeader* header) noexcept;

    // 调试：打印程序头信息
    static void print_program_headers(const u8* elf_data, const ElfHeader* header) noexcept;

    // 调试：打印加载结果
    static void print_loaded_program(const LoadedProgram& program) noexcept;

private:
    // 内部辅助函数：页面对齐
    [[nodiscard]] static VirtAddr align_to_page(VirtAddr addr) noexcept {
        constexpr usize PAGE_SIZE = 4096;
        return (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    }

    // 内部辅助函数：检查地址范围
    [[nodiscard]] static bool is_valid_user_address(VirtAddr addr) noexcept {
        // 用户地址空间：0x00000000_00000000 到 0x00007FFF_FFFFFFFF (128TB)
        return addr < 0x0000800000000000ULL;
    }

    // 内部辅助函数：权限标志转换
    [[nodiscard]] static u32 elf_flags_to_memory_flags(u32 elf_flags) noexcept;
};

// ELF加载器统计信息
struct ElfLoaderStats {
    u64 total_loads;            // 总加载次数
    u64 successful_loads;       // 成功加载次数
    u64 failed_loads;           // 失败加载次数
    u64 bytes_loaded;           // 加载的总字节数
    u64 memory_allocated;       // 分配的内存总量
};

// 全局ELF加载器统计
extern ElfLoaderStats g_elf_loader_stats;

// ELF加载错误码
enum class ElfLoadError : u32 {
    InvalidMagic = 1,           // 无效的ELF魔数
    InvalidClass = 2,           // 不支持的ELF类别
    InvalidEndian = 3,          // 不支持的字节序
    InvalidVersion = 4,         // 不支持的ELF版本
    UnsupportedArch = 5,        // 不支持的架构
    InvalidFileType = 6,        // 无效的文件类型
    InvalidProgramHeaders = 7,  // 无效的程序头
    MemoryAllocationFailed = 8, // 内存分配失败
    AddressSpaceExhausted = 9,  // 地址空间耗尽
    InvalidSegment = 10,        // 无效的内存段
    MappingFailed = 11,         // 内存映射失败
};

} // namespace moss::kernel::elf
