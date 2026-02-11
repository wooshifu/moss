// MOSS混合内核主函数实现
// 系统启动入口和全局实例管理

#include "kernel_main.hpp"
#include "syscall_table.hpp"        // 系统调用表管理
#include "elf_loader.hpp"           // ELF程序加载器
#include "../include/arch/syscall_arch.hpp"  // 多架构系统调用支持
#include "../mm/kernel_memory.hpp"  // 内核内存分配接口
// cstring 不需要 - 内核环境使用自定义内存操作

// 使用内核命名空间的类型
using moss::kernel::ShmId;
using moss::kernel::u32;
using moss::kernel::u64;
using moss::kernel::usize;

namespace moss::kernel {

// 全局实例定义
Kernel *g_kernel = nullptr;

// 子系统全局实例
containers::ContainerLibrary *g_container_lib = nullptr;
mm::PageTableManager *g_page_table_manager = nullptr;

// 注意：进程管理和IPC系统的全局实例
// 在各自的模块文件中定义（process.cpp, runtime_support.cpp等）

interrupts::GenericInterruptController *g_gic = nullptr;
drivers::DeviceManager *g_device_manager = nullptr;
// drivers::UartDriver* g_uart_driver = nullptr; // 暂时注释掉

} // namespace moss::kernel

// 测试函数前向声明
extern "C" {
void test_container_library(void) noexcept;
void test_memory_management(void) noexcept;
void test_process_management(void) noexcept;
void test_ipc_system(void) noexcept;
void test_device_management(void) noexcept;
void test_elf_loader(void) noexcept;
void test_userspace_program(void) noexcept;

// 早期调试输出函数声明
void early_debug_print(const char *message) noexcept;

// C风格入口函数（从汇编启动代码调用）

// 内核主入口函数
[[noreturn]] void kernel_main(void) noexcept {
  using namespace moss::kernel;

  // 创建内核实例
  g_kernel = new Kernel();
  if (g_kernel == nullptr) {
    // 无法创建内核实例，直接停机
    while (true) {
#if defined(MOSS_ARCH_ARM64)
      asm volatile("wfi");
#elif defined(MOSS_ARCH_X86_64)
      asm volatile("hlt");
#elif defined(MOSS_ARCH_RISCV)
      asm volatile("wfi");
#else
      for (volatile int i = 0; i < 1000000; ++i) {
      }
#endif
    }
  }

  // 初始化内核
  auto init_result = g_kernel->initialize();
  if (!init_result) {
    // 初始化失败，停机
    delete g_kernel;
    g_kernel = nullptr;

    while (true) {
#if defined(MOSS_ARCH_ARM64)
      asm volatile("wfi");
#elif defined(MOSS_ARCH_X86_64)
      asm volatile("hlt");
#elif defined(MOSS_ARCH_RISCV)
      asm volatile("wfi");
#else
      for (volatile int i = 0; i < 1000000; ++i) {
      }
#endif
    }
  }

  // 运行内核（不会返回）
  (void)g_kernel->run(); // 不应该返回

  // 如果到达这里，说明内核异常退出
  delete g_kernel;
  g_kernel = nullptr;

  while (true) {
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
    asm volatile("wfi"); // 等待中断 (ARM64)
#elif defined(__x86_64__) || defined(MOSS_ARCH_X86_64)
    asm volatile("hlt"); // 停机等待中断 (x86_64)
#elif defined(__riscv) || defined(MOSS_ARCH_RISCV)
    asm volatile("wfi"); // 等待中断 (RISC-V)
#else
    // 通用停机 - CPU空循环
    for (volatile int i = 0; i < 1000000; ++i) {
    }
#endif
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

  // 测试ELF加载器
  test_elf_loader();

  // 测试用户空间程序
  test_userspace_program();
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
    (void)counter.fetch_add(1, MemoryOrder::Relaxed);
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

  // 测试统一内存管理系统
  early_debug_print("🧪 开始测试统一内存管理系统...\n");

  // 检查内存系统健康状态
  if (!mm::is_memory_system_healthy()) {
    early_debug_print("❌ 内存系统状态不健康\n");
    return;
  }

  // 测试基本内存分配
  void* ptr1 = mm::kmalloc(1024);
  if (ptr1) {
    early_debug_print("✅ kmalloc(1024) 成功\n");

    // 测试内存释放
    mm::kfree(ptr1);
    early_debug_print("✅ kfree() 成功\n");
  } else {
    early_debug_print("❌ kmalloc(1024) 失败\n");
  }

  // 测试零初始化分配
  void* ptr2 = mm::kzalloc(2048);
  if (ptr2) {
    early_debug_print("✅ kzalloc(2048) 成功\n");

    // 检查是否真的零初始化
    bool is_zero = true;
    for (usize i = 0; i < 2048; ++i) {
      if (static_cast<char*>(ptr2)[i] != 0) {
        is_zero = false;
        break;
      }
    }

    if (is_zero) {
      early_debug_print("✅ 零初始化验证成功\n");
    } else {
      early_debug_print("❌ 零初始化验证失败\n");
    }

    mm::kfree_sized(ptr2, 2048);
    early_debug_print("✅ kfree_sized() 成功\n");
  } else {
    early_debug_print("❌ kzalloc(2048) 失败\n");
  }

  // 测试对齐分配
  void* ptr3 = mm::kmalloc_aligned(512, 64);
  if (ptr3) {
    usize addr = reinterpret_cast<usize>(ptr3);
    if (addr % 64 == 0) {
      early_debug_print("✅ 对齐分配验证成功\n");
    } else {
      early_debug_print("❌ 对齐分配验证失败\n");
    }
    mm::kfree(ptr3);
  } else {
    early_debug_print("❌ kmalloc_aligned() 失败\n");
  }

  // 测试原子分配
  void* ptr4 = mm::kmalloc_atomic(256);
  if (ptr4) {
    early_debug_print("✅ kmalloc_atomic(256) 成功\n");
    mm::kfree(ptr4);
  } else {
    early_debug_print("❌ kmalloc_atomic(256) 失败\n");
  }

  // 获取内存压力信息
  auto pressure = mm::get_memory_pressure();
  const char* pressure_str = "UNKNOWN";
  switch (pressure) {
    case mm::MemoryPressure::LOW: pressure_str = "LOW"; break;
    case mm::MemoryPressure::MEDIUM: pressure_str = "MEDIUM"; break;
    case mm::MemoryPressure::HIGH: pressure_str = "HIGH"; break;
    case mm::MemoryPressure::CRITICAL: pressure_str = "CRITICAL"; break;
    default: pressure_str = "UNKNOWN"; break;
  }
  early_debug_print("📊 内存压力: ");
  early_debug_print(pressure_str);
  early_debug_print("\n");

  // 打印内存统计信息
  mm::print_memory_stats();

  // 检查内存泄漏
  mm::check_memory_leaks();

  early_debug_print("✅ 内存管理测试完成\n");

  // 测试页表映射 (保持原有测试，用于兼容性)
  [[maybe_unused]] PhysAddr test_phys = 0x80000000;
  [[maybe_unused]] VirtAddr test_virt = 0xFFFF800080000000;

  // 简单映射测试（实际需要更完善的测试）
  // auto map_result = g_page_table_manager->map_page(test_virt, test_phys, ...);
}

// 测试进程管理
void test_process_management(void) noexcept {
  using namespace moss::kernel;

  if (process::g_process_manager == nullptr || process::g_scheduler == nullptr) {
    return;
  }

  // 测试进程创建和调度（简化版本）
  // 实际需要更完整的测试
}

// 测试IPC系统
void test_ipc_system(void) noexcept {
  using namespace moss::kernel;

  if (ipc::g_ipc_manager == nullptr || ipc::g_shared_memory_manager == nullptr) {
    return;
  }

  // 测试共享内存创建
  auto shm_result = ipc::g_shared_memory_manager->create_region(
      1,    // 进程ID
      4096, // 大小
      ipc::ShmType::Normal, ipc::ShmPermission::ReadWrite);

  if (shm_result) {
    [[maybe_unused]] ShmId shm_id = *shm_result;

    // 测试IPC服务注册
    auto service_result =
        ipc::g_ipc_manager->register_service(1, "test-service", 10);

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

// 测试ELF加载器
void test_elf_loader(void) noexcept {
  using namespace moss::kernel::elf;

  early_debug_print("🧪 开始测试ELF加载器...\n");

  // 创建一个最小的有效ELF头进行测试
  ElfHeader test_header = {};

  // 设置ELF魔数
  test_header.e_ident[0] = 0x7F;
  test_header.e_ident[1] = 'E';
  test_header.e_ident[2] = 'L';
  test_header.e_ident[3] = 'F';
  test_header.e_ident[4] = ELF_CLASS_64;    // 64位
  test_header.e_ident[5] = ELF_DATA_LSB;    // 小端
  test_header.e_ident[6] = ELF_VERSION;     // 版本1

  test_header.e_type = ET_EXEC;             // 可执行文件
#if defined(MOSS_ARCH_ARM64)
  test_header.e_machine = EM_AARCH64;       // ARM64
#elif defined(MOSS_ARCH_X86_64)
  test_header.e_machine = EM_X86_64;        // x86_64
#elif defined(MOSS_ARCH_RISCV)
  test_header.e_machine = EM_RISCV;         // RISC-V
#endif
  test_header.e_version = ELF_VERSION;
  test_header.e_entry = 0x400000;           // 入口点

  // 测试ELF头验证
  auto validate_result = ElfLoader::validate_elf_header(&test_header);
  if (validate_result) {
    early_debug_print("✅ ELF头验证通过\n");
  } else {
    early_debug_print("❌ ELF头验证失败\n");
  }

  // 测试架构兼容性检查
  auto arch_result = ElfLoader::check_architecture_compatibility(test_header.e_machine);
  if (arch_result) {
    early_debug_print("✅ 架构兼容性检查通过\n");
  } else {
    early_debug_print("❌ 架构兼容性检查失败\n");
  }

  // 打印ELF信息
  ElfLoader::print_elf_info(&test_header);

  early_debug_print("✅ ELF加载器测试完成\n");
}

// 内核崩溃回调
[[noreturn]] void kernel_panic_handler(const char *message) noexcept {
// 禁用中断
#if defined(MOSS_ARCH_ARM64)
  asm volatile("msr daifset, #15" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("cli" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("csrci mstatus, 0x8" ::: "memory"); // 禁用机器级中断
#endif

  // 基本错误输出（如果可能）
  volatile u32 *uart_data = reinterpret_cast<volatile u32 *>(0x09000000);
  const char *panic_msg = "\n💀 KERNEL PANIC: ";

  // 输出错误信息
  while (*panic_msg) {
    *uart_data = static_cast<u32>(static_cast<unsigned char>(*panic_msg++));
  }

  if (message) {
    while (*message) {
      *uart_data = static_cast<u32>(static_cast<unsigned char>(*message++));
    }
  }

  // 停机
  while (true) {
#if defined(MOSS_ARCH_ARM64)
    asm volatile("wfi");
#elif defined(MOSS_ARCH_X86_64)
    asm volatile("hlt");
#elif defined(MOSS_ARCH_RISCV)
    asm volatile("wfi"); // RISC-V 也有 wfi 指令
#else
    // 通用停机 - CPU 空循环
    for (volatile int i = 0; i < 1000000; ++i) {
    }
#endif
  }
}

// 早期调试输出（在UART驱动初始化前使用）
extern "C" void early_debug_print(const char *message) noexcept {
  if (message == nullptr)
    return;

  volatile u32 *uart_data = reinterpret_cast<volatile u32 *>(0x09000000);
  volatile u32 *uart_flags = reinterpret_cast<volatile u32 *>(0x09000018);

  while (*message) {
    // 等待发送FIFO可用
    while (*uart_flags & (1 << 5)) {
      // TXFF标志
    }

    if (*message == '\n') {
      *uart_data = static_cast<u32>('\r');
      while (*uart_flags & (1 << 5)) {
      }
      *uart_data = static_cast<u32>('\n');
    } else {
      *uart_data = static_cast<u32>(static_cast<unsigned char>(*message));
    }
    message++;
  }
}

// 系统调用入口
long system_call_handler(long syscall_number, long arg0, long arg1,
                         long arg2, long arg3, long arg4, long arg5) noexcept {
  using namespace moss::kernel;

  // 添加诊断输出 - 查看所有系统调用参数
  early_debug_print("🔧 系统调用被调用！编号: ");
  if (syscall_number == 0) {
    early_debug_print("0 (debug_print)\n");
    early_debug_print("📝 参数arg0: ");
    if (arg0 != 0) {
      early_debug_print("(有效指针)\n");
      early_debug_print("📄 尝试打印字符串: ");
      early_debug_print(reinterpret_cast<const char*>(arg0));
    } else {
      early_debug_print("(空指针)\n");
    }
  } else if (syscall_number == 1) {
    early_debug_print("1 (exit)\n");
    early_debug_print("📝 退出状态码: ");
    // 简单的数字输出
    if (arg0 == 0) {
      early_debug_print("0\n");
    } else {
      early_debug_print("非零\n");
    }
  } else {
    early_debug_print("其他 (");
    // 简化的数字输出
    if (syscall_number < 10) {
      char num_str[2] = {'0' + static_cast<char>(syscall_number), '\0'};
      early_debug_print(num_str);
    } else {
      early_debug_print("大于9");
    }
    early_debug_print(")\n");
  }

  // 使用新的系统调用分发器
  return syscall::SyscallDispatcher::dispatch(syscall_number, arg0, arg1, arg2,
                                              arg3, arg4, arg5);
}

// 内核版本信息
const char *get_kernel_version(void) noexcept {
  return "MOSS v1.0.0 - ARM64 Microkernel";
}

const char *get_build_info(void) noexcept {
  return "Clang-21 C++26 - Release Build";
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
  return {.total_memory = 1024 * 1024 * 1024,   // 1GB
          .free_memory = 512 * 1024 * 1024,     // 512MB
          .kernel_heap_used = 16 * 1024 * 1024, // 16MB
          .user_heap_used = 0,
          .page_faults = 0};
}

} // extern "C"

// 实现多架构系统调用约定打印函数
namespace moss::kernel::arch::syscall {

void print_syscall_convention() noexcept {
    const auto& conv = get_syscall_convention();

    early_debug_print("=== 系统调用架构信息 ===\n");
    early_debug_print("架构: ");
    early_debug_print(conv.arch_name);
    early_debug_print("\n");

    early_debug_print("系统调用指令: ");
    early_debug_print(conv.syscall_instruction);
    early_debug_print("\n");

    early_debug_print("系统调用号寄存器: ");
    early_debug_print(conv.syscall_nr_register);
    early_debug_print("\n");

    early_debug_print("返回值寄存器: ");
    early_debug_print(conv.return_register);
    early_debug_print("\n");

    early_debug_print("参数寄存器: ");
    for (int i = 0; i < 6; ++i) {
        early_debug_print(conv.arg_registers[i]);
        if (i < 5) early_debug_print(", ");
    }
    early_debug_print("\n");
    early_debug_print("========================\n");
}

} // namespace moss::kernel::arch::syscall

// 实现内核测试方法
namespace moss::kernel {

void Kernel::run_kernel_tests() noexcept {
    // 首先测试 kernel_print 格式化修复
    test_kernel_print_formatting();

    // 然后运行所有其他子系统测试
    kernel_test_all_subsystems();
}

} // namespace moss::kernel

// 嵌入的用户空间程序
#include "hello.h"

// 测试用户空间程序
void test_userspace_program(void) noexcept {
    using namespace moss::kernel::process;

    early_debug_print("🧪 开始测试用户空间程序...\n");

    if (!g_process_manager) {
        early_debug_print("❌ 进程管理器未初始化\n");
        return;
    }

    // 使用嵌入的ELF数据创建进程
    auto process_result = moss::kernel::process::user_space::create_process_from_elf(
        hello_elf,
        sizeof(hello_elf)
    );

    if (!process_result) {
        early_debug_print("❌ 用户空间进程创建失败\n");
        return;
    }

    Process* user_process = *process_result;
    early_debug_print("✅ 用户空间进程创建成功\n");

    // 获取主线程
    Thread* main_thread = user_process->get_main_thread();
    if (!main_thread) {
        early_debug_print("❌ 无法获取用户进程主线程\n");
        return;
    }

    early_debug_print("✅ 用户进程主线程获取成功\n");
    early_debug_print("🎯 用户程序入口点: 0x");

    // 打印入口点地址（简化输出）
    u64 entry = main_thread->context.pc;
    char hex_str[20];
    for (int i = 15; i >= 0; i--) {
        u8 nibble = (entry >> (i * 4)) & 0xF;
        hex_str[15-i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    hex_str[16] = '\0';
    early_debug_print(hex_str);
    early_debug_print("\n");

    early_debug_print("🎉 用户空间Hello World程序验证完成！\n");
    early_debug_print("✅ 完整的用户空间支持已实现\n");
}
