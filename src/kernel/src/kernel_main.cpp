// MOSS混合内核主函数实现
// 系统启动入口和全局实例管理

#include "kernel/kernel_main.hpp"
#include "kernel/syscall_table.hpp"        // 系统调用表管理
#include "../include/arch/syscall_arch.hpp"  // 多架构系统调用支持
#include "mm/kernel_memory.hpp"  // 内核内存分配接口
#include "../../interrupts/include/interrupts/ipi_hardware_simple.hpp"      // 简化硬件IPI系统
#include "../../boot/include/boot/boot.hpp"                                  // Boot阶段全局变量
#include "arch/arch_abstraction.hpp"       // 多架构抽象层
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

extern "C" {

// 早期调试输出函数声明
void early_debug_print(const char *message) noexcept;

// C风格入口函数（从汇编启动代码调用）

// 内核主入口函数
[[noreturn]] void kernel_main(void) noexcept {
  using namespace moss::kernel;

  // 输出内核启动信息
  early_debug_print("\n=== MOSS 内核主程序启动 ===\n");
  early_debug_print("单核模式运行 (SMP功能暂时禁用)\n");

  // 创建内核实例
  early_debug_print("正在创建内核主实例...\n");
  g_kernel = new Kernel();
  if (!g_kernel) {
    early_debug_print("❌ 严重错误: 内核实例创建失败，系统无法继续\n");
    while (true) { arch::cpu_halt(); }
  }
  early_debug_print("✅ 内核实例创建成功\n");

  // 完整的内核初始化
  early_debug_print("开始完整内核子系统初始化过程...\n");
  auto init_result = g_kernel->initialize();
  if (!init_result) {
    early_debug_print("❌ 内核初始化失败，错误代码: ");
    early_debug_print("INIT_ERROR\n");
    while (true) { arch::cpu_halt(); }
  }
  early_debug_print("✅ 内核子系统初始化完成\n");

  // ⚡ 关键修复：连接Boot阶段初始化的GIC实例
  // Boot阶段的g_gic_controller已成功初始化，现在让Kernel可以访问它
  using namespace moss::boot;
  if (g_gic_controller && g_gic_hardware_available) {
    g_gic = g_gic_controller; // 连接Boot和Kernel阶段的GIC指针
    early_debug_print("🔗 GIC实例已连接：Boot阶段->Kernel阶段\n");
  } else {
    early_debug_print("⚠️ GIC硬件不可用，Kernel将正确报告状态\n");
  }

  // 显示详细系统信息
  early_debug_print("\n=== 内核系统状态详情 ===\n");
  early_debug_print("🔍 即将调用print_system_info()...\n");
  g_kernel->print_system_info();
  early_debug_print("🔍 print_system_info()调用完成...\n");

  // 实际功能验证（不是假的成功消息）
  early_debug_print("\n=== 实际功能状态验证 ===\n");
  early_debug_print("🔍 开始系统状态验证...\n");

  // 验证内存管理系统实际状态
  if (mm::is_memory_system_healthy()) {
    auto pressure = mm::get_memory_pressure();
    early_debug_print("✅ 内存管理系统: 运行正常, 压力等级=");
    switch (pressure) {
      case mm::MemoryPressure::LOW: early_debug_print("低"); break;
      case mm::MemoryPressure::MEDIUM: early_debug_print("中"); break;
      case mm::MemoryPressure::HIGH: early_debug_print("高"); break;
      case mm::MemoryPressure::CRITICAL: early_debug_print("严重"); break;
      default: early_debug_print("未知"); break;
    }
    early_debug_print("\n");
  } else {
    early_debug_print("⚠️ 内存管理系统: 状态异常\n");
  }

  // 验证中断系统状态
  if (g_gic) {
    early_debug_print("✅ 中断处理系统: GIC已初始化并就绪\n");
  } else {
    early_debug_print("⚠️ 中断处理系统: GIC未初始化\n");
  }

  // 验证调度系统状态
  if (process::g_scheduler) {
    early_debug_print("✅ 任务调度系统: CFS调度器已就绪, 准备创建和调度任务\n");
  } else {
    early_debug_print("❌ 任务调度系统: 调度器未初始化\n");
  }

  early_debug_print("\n🎉 MOSS内核初始化和验证完成!\n");

  early_debug_print("🚀 转入实际任务调度和执行阶段...\n\n");

  // 启动内核运行系统 (包含真实任务调度)
  early_debug_print("🔥 启动内核运行系统 (包含真实任务调度)\n");

  // 这将调用 scheduler_->start_scheduling() 并创建实际任务
  auto run_result = g_kernel->run();
  if (!run_result) {
    early_debug_print("💀 致命错误: 内核运行系统启动失败\n");
    while (true) { arch::cpu_halt(); }
  }

  // 不应该到达这里，但如果到达了说明出现了严重错误
  early_debug_print("💀 致命错误: 内核主运行系统异常退出\n");
  while (true) { arch::cpu_halt(); }
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
  return "MOSS v1.0.0 - ARM64 Hybrid Kernel";
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


