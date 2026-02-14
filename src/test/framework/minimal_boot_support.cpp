// MOSS内核测试框架 - 最小启动支持
// 为测试内核提供必需的启动相关函数

import moss.std;
import moss.types;
import moss.result;

using namespace moss::kernel;

// ============================================================================
// UART调试输出支持 - 简化版本
// ============================================================================

// QEMU UART0 基础地址
static constexpr VirtAddr QEMU_UART0_BASE = 0x09000000;

// 简单的UART字符输出
static void uart_putchar(char c) noexcept {
    volatile char* uart_base = reinterpret_cast<volatile char*>(QEMU_UART0_BASE);
    *uart_base = c;
}

// 简单的字符串输出
static void uart_puts(const char* str) noexcept {
    if (!str) return;
    while (*str) {
        uart_putchar(*str);
        str++;
    }
}

extern "C" {

// ============================================================================
// 启动相关的必需外部函数 - 简化实现
// ============================================================================

// 早期调试输出 - 测试内核版本
void early_debug_print(const char* message) noexcept {
    uart_puts(message);
}

// 内核恐慌处理 - 测试内核版本
[[noreturn]] void kernel_panic(const char* message) noexcept {
    uart_puts("\n💥 测试内核崩溃: ");
    uart_puts(message);
    uart_puts("\n");

    // 在测试环境中进入死循环
    #if defined(MOSS_ARCH_ARM64)
    while (true) {
        asm volatile("wfi");
    }
    #elif defined(MOSS_ARCH_X86_64)
    while (true) {
        asm volatile("hlt");
    }
    #elif defined(MOSS_ARCH_RISCV)
    while (true) {
        asm volatile("wfi");
    }
    #else
    while (true) {
        asm volatile("nop");
    }
    #endif
}

// ============================================================================
// 基础内存操作函数 - 简化实现
// ============================================================================

// 内存复制
void* memcpy(void* dest, const void* src, usize n) noexcept {
    if (!dest || !src || n == 0) return dest;

    char* d = static_cast<char*>(dest);
    const char* s = static_cast<const char*>(src);

    while (n > 0) {
        *d = *s;
        d++;
        s++;
        n--;
    }

    return dest;
}

// 内存设置
void* memset(void* ptr, int value, usize num) noexcept {
    if (!ptr || num == 0) return ptr;

    char* p = static_cast<char*>(ptr);
    char c = static_cast<char>(value);

    while (num > 0) {
        *p = c;
        p++;
        num--;
    }

    return ptr;
}

// 内存比较
int memcmp(const void* ptr1, const void* ptr2, usize num) noexcept {
    if (!ptr1 || !ptr2 || num == 0) return 0;

    const char* p1 = static_cast<const char*>(ptr1);
    const char* p2 = static_cast<const char*>(ptr2);

    while (num > 0) {
        if (*p1 != *p2) {
            return (*p1 < *p2) ? -1 : 1;
        }
        p1++;
        p2++;
        num--;
    }

    return 0;
}

// 系统调用处理程序 - 测试内核不支持系统调用
[[noreturn]] void syscall_handler() noexcept {
    early_debug_print("⚠️ 测试内核不支持系统调用\n");
    kernel_panic("测试内核收到系统调用");
}

// ARM64架构特定的系统调用入口
#if defined(MOSS_ARCH_ARM64)
[[noreturn]] void handle_syscall() noexcept {
    syscall_handler();
}
#endif

// ============================================================================
// C++ 操作符重载 - 简化实现用于测试
// ============================================================================

} // extern "C"

// ============================================================================
// C++ new/delete 操作符 - 测试内核专用简化版本
// ============================================================================

// 简单的内存分配器状态
static char test_memory_pool[64 * 1024]; // 64KB用于测试
static usize test_memory_offset = 0;

void* operator new(usize size) {
    // 简单的线性分配器，仅用于测试
    if (test_memory_offset + size > sizeof(test_memory_pool)) {
        early_debug_print("⚠️ 测试内存池不足\n");
        kernel_panic("测试内存池耗尽");
    }

    void* ptr = &test_memory_pool[test_memory_offset];
    test_memory_offset += size;
    // 8字节对齐
    test_memory_offset = (test_memory_offset + 7) & ~static_cast<usize>(7);

    return ptr;
}

void* operator new[](usize size) {
    return operator new(size);
}

void operator delete(void* ptr) noexcept {
    // 简化版本：测试内核不实际释放内存
    static_cast<void>(ptr);
}

void operator delete[](void* ptr) noexcept {
    operator delete(ptr);
}

void operator delete(void* ptr, usize size) noexcept {
    static_cast<void>(ptr);
    static_cast<void>(size);
}

void operator delete[](void* ptr, usize size) noexcept {
    static_cast<void>(ptr);
    static_cast<void>(size);
}

// ============================================================================
// ELF加载器存根 - 测试内核不需要ELF加载
// ============================================================================

namespace moss::kernel::elf {

// ELF加载结果信息 - 测试内核简化版本
struct LoadedProgram {
    VirtAddr entry_point = 0;       // 程序入口点
    VirtAddr base_address = 0;      // 程序基址
    VirtAddr stack_top = 0;         // 用户栈顶
    VirtAddr heap_start = 0;        // 堆起始地址
    usize total_size = 0;           // 程序总内存大小
    u32 load_segments = 0;          // 加载的段数量
};

class ElfLoader {
public:
    [[nodiscard]] static Result<LoadedProgram> load_elf_from_memory(const u8* elf_data, usize elf_size) noexcept;
};

// ELF加载器实现
[[nodiscard]] Result<LoadedProgram> ElfLoader::load_elf_from_memory(const u8* /*elf_data*/, usize /*elf_size*/) noexcept {
    early_debug_print("⚠️ 测试内核不支持ELF加载\n");
    return Result<LoadedProgram>{ErrorCode::NotSupported};
}

} // namespace moss::kernel::elf
