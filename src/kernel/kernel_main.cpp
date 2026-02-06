// MOSS微内核主函数实现
// 系统启动入口和全局实例管理

#include "kernel_main.hpp"
#include <cstring>

// 使用内核命名空间的类型
using moss::kernel::u32;
using moss::kernel::u64;
using moss::kernel::usize;
using moss::kernel::ShmId;

namespace moss::kernel {

// 全局实例定义
Kernel* g_kernel = nullptr;

// 子系统全局实例
containers::ContainerLibrary* g_container_lib = nullptr;
mm::PageTableManager* g_page_table_manager = nullptr;
process::ProcessManager* g_process_manager = nullptr;
process::CfsScheduler* g_scheduler = nullptr;
ipc::SharedMemoryManager* g_shared_memory_manager = nullptr;
ipc::IpcManager* g_ipc_manager = nullptr;
interrupts::GenericInterruptController* g_gic = nullptr;
drivers::DeviceManager* g_device_manager = nullptr;
drivers::UartDriver* g_uart_driver = nullptr;

} // namespace moss::kernel

// 测试函数前向声明
extern "C" {
void test_container_library(void) noexcept;
void test_memory_management(void) noexcept;
void test_process_management(void) noexcept;
void test_ipc_system(void) noexcept;
void test_device_management(void) noexcept;

// C风格入口函数（从汇编启动代码调用）

// 内核主入口函数
[[noreturn]] void kernel_main(void) noexcept {
    using namespace moss::kernel;

    // 创建内核实例
    g_kernel = new Kernel();
    if (g_kernel == nullptr) {
        // 无法创建内核实例，直接停机
        while (true) {
            asm volatile("wfi");
        }
    }

    // 初始化内核
    auto init_result = g_kernel->initialize();
    if (!init_result) {
        // 初始化失败，停机
        delete g_kernel;
        g_kernel = nullptr;

        while (true) {
            asm volatile("wfi");
        }
    }

    // 运行内核（不会返回）
    auto run_result = g_kernel->run();

    // 如果到达这里，说明内核异常退出
    delete g_kernel;
    g_kernel = nullptr;

    while (true) {
        asm volatile("wfi");
    }
}

// 内核调试和测试接口
void kernel_test_all_subsystems(void) noexcept {
    using namespace moss::kernel;

    if (g_kernel == nullptr) {
        return;
    }

    // 打印系统信息
    g_kernel->print_system_info();

    // 测试容器库
    test_container_library();

    // 测试内存管理
    test_memory_management();

    // 测试进程管理
    test_process_management();

    // 测试IPC系统
    test_ipc_system();

    // 测试设备管理
    test_device_management();
}

// 测试容器库
void test_container_library(void) noexcept {
    using namespace moss::kernel::containers;

    // 测试SPSC队列
    SPSCQueue<u32, 16> queue;

    // 入队测试
    for (u32 i = 0; i < 10; ++i) {
        bool success = queue.try_enqueue(i);
        if (success) {
            // 成功入队
        }
    }

    // 出队测试
    for (u32 i = 0; i < 10; ++i) {
        u32 value;
        bool success = queue.try_dequeue(value);
        if (success && value == i) {
            // 成功出队且值正确
        }
    }

    // 测试原子计数器
    AtomicU64 counter{0};
    for (int i = 0; i < 100; ++i) {
        counter.fetch_add(1, MemoryOrder::Relaxed);
    }

    u64 final_value = counter.load(MemoryOrder::Relaxed);
    if (final_value == 100) {
        // 原子计数器工作正常
    }
}

// 测试内存管理
void test_memory_management(void) noexcept {
    using namespace moss::kernel;

    if (g_page_table_manager == nullptr) {
        return;
    }

    // 测试页表映射
    PhysAddr test_phys = 0x80000000;
    VirtAddr test_virt = 0xFFFF800080000000;

    // 简单映射测试（实际需要更完善的测试）
    // auto map_result = g_page_table_manager->map_page(test_virt, test_phys, ...);
}

// 测试进程管理
void test_process_management(void) noexcept {
    using namespace moss::kernel;

    if (g_process_manager == nullptr || g_scheduler == nullptr) {
        return;
    }

    // 测试进程创建和调度（简化版本）
    // 实际需要更完整的测试
}

// 测试IPC系统
void test_ipc_system(void) noexcept {
    using namespace moss::kernel;

    if (g_ipc_manager == nullptr || g_shared_memory_manager == nullptr) {
        return;
    }

    // 测试共享内存创建
    auto shm_result = g_shared_memory_manager->create_region(
        1,  // 进程ID
        4096,  // 大小
        ipc::ShmType::Normal,
        ipc::ShmPermission::ReadWrite
    );

    if (shm_result) {
        ShmId shm_id = *shm_result;

        // 测试IPC服务注册
        auto service_result = g_ipc_manager->register_service(1, "test-service", 10);

        if (service_result) {
            // IPC系统基本功能正常
        }
    }
}

// 测试设备管理
void test_device_management(void) noexcept {
    using namespace moss::kernel;

    if (g_device_manager == nullptr) {
        return;
    }

    // 获取设备统计
    auto stats = g_device_manager->get_statistics();

    // 简单验证设备管理器状态
    if (stats.registered_drivers > 0) {
        // 设备管理系统正常
    }
}

// 内核崩溃回调
void kernel_panic_handler(const char* message) noexcept {
    // 禁用中断
    asm volatile("msr daifset, #15" ::: "memory");

    // 基本错误输出（如果可能）
    volatile u32* uart_data = reinterpret_cast<volatile u32*>(0x09000000);
    const char* panic_msg = "\n💀 KERNEL PANIC: ";

    // 输出错误信息
    while (*panic_msg) {
        *uart_data = *panic_msg++;
    }

    if (message) {
        while (*message) {
            *uart_data = *message++;
        }
    }

    // 停机
    while (true) {
        asm volatile("wfi");
    }
}

// 早期调试输出（在UART驱动初始化前使用）
void early_debug_print(const char* message) noexcept {
    if (message == nullptr) return;

    volatile u32* uart_data = reinterpret_cast<volatile u32*>(0x09000000);
    volatile u32* uart_flags = reinterpret_cast<volatile u32*>(0x09000018);

    while (*message) {
        // 等待发送FIFO可用
        while (*uart_flags & (1 << 5)) {
            // TXFF标志
        }

        if (*message == '\n') {
            *uart_data = '\r';
            while (*uart_flags & (1 << 5)) {}
            *uart_data = '\n';
        } else {
            *uart_data = *message;
        }
        message++;
    }
}

// 系统调用入口
long system_call_handler(long syscall_number, long arg0, long arg1,
                        long arg2, long arg3, long arg4, long arg5) noexcept {
    using namespace moss::kernel;

    // 基本的系统调用分发
    switch (syscall_number) {
        case 0: // sys_debug_print
            if (arg0 != 0) {
                early_debug_print(reinterpret_cast<const char*>(arg0));
            }
            return 0;

        case 1: // sys_exit
            // 处理进程退出
            return 0;

        case 2: // sys_getpid
            // 返回当前进程ID
            return 1;  // 简化返回值

        default:
            return -1;  // 未知系统调用
    }
}

// 内核版本信息
const char* get_kernel_version(void) noexcept {
    return "MOSS v1.0.0 - ARM64 Microkernel";
}

const char* get_build_info(void) noexcept {
    return __DATE__ " " __TIME__ " - Clang-21 C++23";
}

// 内核内存统计
struct KernelMemoryInfo {
    usize total_memory;
    usize free_memory;
    usize kernel_heap_used;
    usize user_heap_used;
    u32 page_faults;
};

KernelMemoryInfo get_kernel_memory_info(void) noexcept {
    // 简化实现
    return {
        .total_memory = 1024 * 1024 * 1024,  // 1GB
        .free_memory = 512 * 1024 * 1024,    // 512MB
        .kernel_heap_used = 16 * 1024 * 1024, // 16MB
        .user_heap_used = 0,
        .page_faults = 0
    };
}

} // extern "C"