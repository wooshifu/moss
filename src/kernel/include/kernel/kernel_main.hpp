#pragma once

// MOSS混合内核主系统集成
// 统一初始化和管理所有内核子系统

// 外部C函数声明
extern "C" {
void kernel_test_all_subsystems(void) noexcept;
}

#include "ipc/ipc_manager.hpp"
#include "interrupts/gic.hpp"
#include "ipc/shared_memory.hpp"
#include "process/cfs_scheduler.hpp"
#include "process/load_balancer.hpp"
#include "process/process.hpp"
#include "containers/containers.hpp"
#include "drivers/device_manager.hpp"
#include "mm/page_table.hpp"
#include "mm/mm_interface.hpp"
#include "mm/kernel_memory.hpp"
#include "arch/syscall_arch.hpp"
#include "result.hpp"
#include "types.hpp"
// #include "uart_driver.hpp" // 暂时注释掉，稍后修复

// 内核环境下的 va_list 支持
extern "C" {
#ifdef __GNUC__
typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v, l) __builtin_va_arg(v, l)
#else
#error "Unsupported compiler for va_list"
#endif
}

// Linux风格SMP延迟激活函数前向声明
namespace moss::boot {
void activate_secondary_cpus() noexcept;
u32 wait_for_all_cpus_active(u32 timeout_ms = 5000) noexcept;
}

namespace moss::kernel {

// 内核子系统状态
enum class SubsystemState : u8 {
  Uninitialized = 0,
  Initializing = 1,
  Active = 2,
  Error = 3
};

// 内核启动阶段
enum class BootPhase : u8 {
  EarlyInit = 0,     // 早期初始化（汇编完成后）
  MemoryInit = 1,    // 内存管理初始化
  SchedulerInit = 2, // 调度器初始化
  IpcInit = 3,       // IPC系统初始化
  DeviceInit = 4,    // 设备管理初始化
  ServiceInit = 5,   // 系统服务启动
  UserInit = 6,      // 用户空间初始化
  Completed = 7      // 启动完成
};

// 内核统计信息
struct KernelStats {
  u64 boot_time;          // 启动时间
  u64 uptime;             // 运行时间
  u64 total_memory;       // 总内存
  u64 free_memory;        // 可用内存
  u32 active_processes;   // 活跃进程数
  u32 total_threads;      // 总线程数
  u64 context_switches;   // 上下文切换次数
  u64 interrupts_handled; // 处理的中断数
  u64 ipc_messages;       // IPC消息数
  u32 registered_devices; // 注册的设备数
};

// 内核主类
class Kernel {
private:
  // 启动状态
  BootPhase current_phase_;
  [[maybe_unused]] SubsystemState subsystem_states_[8]; // 各子系统状态

  // 核心子系统实例
  containers::ContainerLibrary *container_lib_;
  mm::PageTableManager *page_table_manager_;
  process::ProcessManager *process_manager_;
  process::CfsScheduler *scheduler_;
  process::LoadBalancer *load_balancer_;
  ipc::SharedMemoryManager *shared_memory_manager_;
  ipc::IpcManager *ipc_manager_;
  interrupts::GenericInterruptController *gic_;
  drivers::DeviceManager *device_manager_;
  // drivers::UartDriver* uart_driver_; // 暂时注释掉

  // 启动时间记录
  u64 boot_start_time_;
  u64 phase_start_times_[8];

  // 内核配置
  struct KernelConfig {
    bool enable_smp;             // 启用多核支持
    bool enable_preemption;      // 启用抢占式调度
    u32 max_processes;           // 最大进程数
    u32 max_threads_per_process; // 每进程最大线程数
    usize kernel_heap_size;      // 内核堆大小
    bool enable_debug_output;    // 启用调试输出
    u32 scheduler_timeslice_ms;  // 调度时间片(毫秒)
  } config_;

public:
  Kernel() noexcept
      : current_phase_(BootPhase::EarlyInit),
        subsystem_states_{SubsystemState::Uninitialized},
        container_lib_(nullptr), page_table_manager_(nullptr),
        process_manager_(nullptr), scheduler_(nullptr), load_balancer_(nullptr),
        shared_memory_manager_(nullptr), ipc_manager_(nullptr), gic_(nullptr),
        device_manager_(nullptr), // uart_driver_(nullptr), // 暂时注释掉
        boot_start_time_(0), phase_start_times_{0} {
    // 初始化内核配置
    config_ = {.enable_smp = true,
               .enable_preemption = true,
               .max_processes = 256,
               .max_threads_per_process = 16,
               .kernel_heap_size = 16 * 1024 * 1024, // 16MB
               .enable_debug_output = true,
               .scheduler_timeslice_ms = 10};
  }

  ~Kernel() noexcept { shutdown(); }

  // 禁用拷贝和移动
  NON_COPYABLE_NON_MOVABLE(Kernel)

  // 内核初始化主入口
  [[nodiscard]] VoidResult initialize() noexcept {
    boot_start_time_ = get_current_time();

    print_banner();

    // 按阶段初始化内核
    auto result = initialize_phase_by_phase();
    if (!result) {
      kernel_panic("Kernel initialization failed", result.error());
      return result;
    }

    current_phase_ = BootPhase::Completed;

    u64 boot_time = get_current_time() - boot_start_time_;
    kernel_print("✅ MOSS内核启动完成 (用时: %llu cycles)\n", boot_time);

    return VoidResult{};
  }

  // 内核主循环
  [[nodiscard]] VoidResult run() noexcept {
    kernel_print("🚀 MOSS内核开始运行...\n");

    // 启用中断
    enable_interrupts();

    // 创建初始用户进程
    auto init_result = create_init_process();
    if (!init_result) {
      kernel_panic("Failed to create init process", init_result.error());
      return init_result;
    }

    // 进入调度循环
    kernel_print("🔄 开始调度循环\n");
    scheduler_->start_scheduling();

    // 不应该到达这里
    kernel_panic("Scheduler returned unexpectedly", ErrorCode::InternalError);
  }

  // 内核关闭
  void shutdown() noexcept {
    kernel_print("🔄 MOSS内核正在关闭...\n");

    // 按相反顺序关闭子系统
    if (device_manager_) {
      (void)device_manager_->suspend_all_devices();
      delete device_manager_;
      device_manager_ = nullptr;
    }

    if (ipc_manager_) {
      delete ipc_manager_;
      ipc_manager_ = nullptr;
    }

    if (shared_memory_manager_) {
      delete shared_memory_manager_;
      shared_memory_manager_ = nullptr;
    }

    if (scheduler_) {
      delete scheduler_;
      scheduler_ = nullptr;
    }

    if (process_manager_) {
      delete process_manager_;
      process_manager_ = nullptr;
    }

    if (page_table_manager_) {
      delete page_table_manager_;
      page_table_manager_ = nullptr;
    }

    if (container_lib_) {
      containers::ContainerLibrary::cleanup();
    }

    // 最后关闭内存管理系统
    kernel_print("🧠 关闭统一内存管理系统...\n");
    mm::shutdown_kernel_memory();

    kernel_print("✅ MOSS内核已关闭\n");
  }

  // 获取内核统计信息
  [[nodiscard]] KernelStats get_statistics() const noexcept {
    KernelStats stats = {};

    stats.boot_time = boot_start_time_;
    stats.uptime = get_current_time() - boot_start_time_;

    if (process_manager_) {
      // stats.active_processes = process_manager_->get_process_count();
    }

    if (gic_) {
      auto gic_stats = gic_->get_statistics();
      stats.interrupts_handled = gic_stats.total_interrupts;
    }

    if (ipc_manager_) {
      auto ipc_stats = ipc_manager_->get_statistics();
      stats.ipc_messages = ipc_stats.messages_processed;
    }

    if (device_manager_) {
      auto dev_stats = device_manager_->get_statistics();
      stats.registered_devices = dev_stats.total_devices;
    }

    return stats;
  }

  // 内核调试接口
  void print_system_info() const noexcept {
    kernel_print("=== MOSS内核系统信息 ===\n");
    kernel_print("启动阶段: %d\n", static_cast<int>(current_phase_));
    kernel_print("SMP支持: %s\n", config_.enable_smp ? "启用" : "禁用");
    kernel_print("抢占调度: %s\n", config_.enable_preemption ? "启用" : "禁用");

    auto stats = get_statistics();
    kernel_print("运行时间: %llu cycles\n", stats.uptime);
    kernel_print("活跃进程: %u\n", stats.active_processes);
    kernel_print("中断处理: %llu\n", stats.interrupts_handled);
    kernel_print("IPC消息: %llu\n", stats.ipc_messages);
    kernel_print("注册设备: %u\n", stats.registered_devices);
    kernel_print("========================\n");
  }

private:
  // 测试 kernel_print 格式化修复
  void test_kernel_print_formatting() noexcept {
    kernel_print("=== kernel_print 格式化测试 ===\n");

    // 测试所有主要格式说明符
    kernel_print("%%d 测试: %d\n", -12345);
    kernel_print("%%u 测试: %u\n", 54321u);
    kernel_print("%%x 测试: %x\n", 0xabcd);
    kernel_print("%%X 测试: %X\n", 0xABCD);
    kernel_print("%%s 测试: %s\n", "Hello MOSS!");
    kernel_print("%%c 测试: %c\n", 'M');

    // 关键测试：%llu 格式（之前的bug）
    u64 large_number = 0x123456789ABCDEF0ULL;
    kernel_print("%%llu 测试: %llu\n", large_number);
    kernel_print("%%llx 测试: %llx\n", large_number);
    kernel_print("%%llX 测试: %llX\n", large_number);

    // 测试指针格式
    void* test_ptr = reinterpret_cast<void*>(0x40080000);
    kernel_print("%%p 测试: %p\n", test_ptr);

    // 边界情况测试
    kernel_print("零值测试: %llu\n", 0ULL);
    kernel_print("最大值测试: %llu\n", 0xFFFFFFFFFFFFFFFFULL);

    kernel_print("=== kernel_print 测试完成 ===\n");
  }

  // 分阶段初始化
  [[nodiscard]] VoidResult initialize_phase_by_phase() noexcept {
    const char *phase_names[] = {"早期初始化", "内存管理", "调度器",
                                 "IPC系统",    "设备管理", "系统服务",
                                 "用户空间",   "完成"};

    for (int phase = 0; phase < 7; ++phase) {
      current_phase_ = static_cast<BootPhase>(phase);
      phase_start_times_[phase] = get_current_time();

      kernel_print("⏳ 阶段 %d: %s\n", phase, phase_names[phase]);

      VoidResult result = VoidResult{ErrorCode::NotSupported};

      switch (current_phase_) {
      case BootPhase::EarlyInit:
        result = initialize_early();
        break;
      case BootPhase::MemoryInit:
        result = initialize_memory();
        break;
      case BootPhase::SchedulerInit:
        result = initialize_scheduler();
        break;
      case BootPhase::IpcInit:
        result = initialize_ipc();
        break;
      case BootPhase::DeviceInit:
        result = initialize_devices();
        break;
      case BootPhase::ServiceInit:
        result = initialize_services();
        break;
      case BootPhase::UserInit:
        result = initialize_userspace();
        break;
      case BootPhase::Completed:
        result = VoidResult{}; // 启动已完成，返回成功
        break;
      default:
        break;
      }

      if (!result) {
        kernel_print("❌ 阶段 %d 失败: %d\n", phase,
                     static_cast<int>(result.error()));
        return result;
      }

      u64 phase_time = get_current_time() - phase_start_times_[phase];
      kernel_print("✅ 阶段 %d 完成 (用时: %llu cycles)\n", phase, phase_time);
    }

    return VoidResult{};
  }

  // 早期初始化
  [[nodiscard]] VoidResult initialize_early() noexcept {
    // 初始化容器库
    if (!containers::ContainerLibrary::initialize()) {
      return VoidResult{ErrorCode::InternalError};
    }
    container_lib_ =
        nullptr; // ContainerLibrary is a singleton/static, no instance needed

    return VoidResult{};
  }

  // 内存管理初始化
  [[nodiscard]] VoidResult initialize_memory() noexcept {
    kernel_print("🧠 初始化统一内存管理系统...\n");

    // 首先初始化统一内存管理系统
    if (!mm::initialize_kernel_memory()) {
      kernel_print("❌ 统一内存管理系统初始化失败\n");
      return VoidResult{ErrorCode::InternalError};
    }
    kernel_print("✅ 统一内存管理系统初始化成功\n");

    // 创建页表管理器（现在使用新的内存管理系统）
    page_table_manager_ = new mm::PageTableManager();
    if (!page_table_manager_) {
      kernel_print("❌ PageTableManager创建失败\n");
      mm::shutdown_kernel_memory();
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // 初始化页表管理器（使用当前页表）
    auto init_result = page_table_manager_->initialize_from_current();
    if (!init_result) {
      kernel_print("❌ PageTableManager初始化失败\n");
      delete page_table_manager_;
      page_table_manager_ = nullptr;
      mm::shutdown_kernel_memory();
      return init_result;
    }

    // 检查内存系统状态
    if (!mm::is_memory_system_healthy()) {
      kernel_print("⚠️ 内存系统状态异常\n");
    }

    // 打印内存系统信息
    auto pressure = mm::get_memory_pressure();
    const char* pressure_str = "UNKNOWN";
    switch (pressure) {
      case mm::MemoryPressure::LOW: pressure_str = "LOW"; break;
      case mm::MemoryPressure::MEDIUM: pressure_str = "MEDIUM"; break;
      case mm::MemoryPressure::HIGH: pressure_str = "HIGH"; break;
      case mm::MemoryPressure::CRITICAL: pressure_str = "CRITICAL"; break;
      default: pressure_str = "UNKNOWN"; break;
    }
    kernel_print("📊 内存压力等级: %s\n", pressure_str);

    return VoidResult{};
  }

  // 调度器初始化
  [[nodiscard]] VoidResult initialize_scheduler() noexcept {
    // 创建进程管理器
    process_manager_ = new process::ProcessManager();
    if (!process_manager_) {
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // 设置全局进程管理器指针
    ::moss::kernel::process::g_process_manager = process_manager_;

    // 创建CFS调度器
    scheduler_ = new process::CfsScheduler();
    if (!scheduler_) {
      delete process_manager_;
      process_manager_ = nullptr;
      ::moss::kernel::process::g_process_manager = nullptr;
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // 设置全局调度器指针
    ::moss::kernel::process::g_scheduler = scheduler_;

    // 创建负载均衡器
    load_balancer_ = new process::LoadBalancer();
    if (!load_balancer_) {
      delete scheduler_;
      delete process_manager_;
      scheduler_ = nullptr;
      process_manager_ = nullptr;
      ::moss::kernel::process::g_scheduler = nullptr;
      ::moss::kernel::process::g_process_manager = nullptr;
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // 🚀 Linux风格SMP延迟激活：调度器就绪后激活从CPU
    if (config_.enable_smp) {
      kernel_print("🔄 调度器就绪，开始激活停放的从CPU...\n");

      // 激活所有停放的从CPU
      moss::boot::activate_secondary_cpus();

      // 等待从CPU完成激活
      u32 active_cpus = moss::boot::wait_for_all_cpus_active(5000);

      kernel_print("✅ CPU激活完成: %u个CPU已激活\n", active_cpus);

      if (active_cpus > 1) {
        kernel_print("🎉 Linux风格多CPU调度器启动成功！\n");
      } else {
        kernel_print("⚠️  回退到单核模式运行\n");
      }
    }

    return VoidResult{};
  }

  // IPC系统初始化
  [[nodiscard]] VoidResult initialize_ipc() noexcept {
    // 创建共享内存管理器
    shared_memory_manager_ = new ipc::SharedMemoryManager();
    if (!shared_memory_manager_) {
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // 设置全局共享内存管理器指针
    ::moss::kernel::ipc::g_shared_memory_manager = shared_memory_manager_;

    // 创建IPC管理器
    ipc_manager_ = new ipc::IpcManager(shared_memory_manager_);
    if (!ipc_manager_) {
      delete shared_memory_manager_;
      shared_memory_manager_ = nullptr;
      ::moss::kernel::ipc::g_shared_memory_manager = nullptr;
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // 设置全局IPC管理器指针
    ::moss::kernel::ipc::g_ipc_manager = ipc_manager_;

    return VoidResult{};
  }

  // 设备管理初始化
  [[nodiscard]] VoidResult initialize_devices() noexcept {
    // 创建GIC中断控制器
    gic_ = new interrupts::GenericInterruptController();
    if (!gic_) {
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // 初始化GIC（使用固定地址，实际应从设备树读取）
    VirtAddr gic_dist_base = 0x08000000; // GIC分发器基址
    VirtAddr gic_cpu_base = 0x08010000;  // GIC CPU接口基址

    auto gic_result = gic_->initialize(gic_dist_base, gic_cpu_base);
    if (!gic_result) {
      delete gic_;
      gic_ = nullptr;
      return gic_result;
    }

    // 创建设备管理器
    device_manager_ = new drivers::DeviceManager();
    if (!device_manager_) {
      delete gic_;
      gic_ = nullptr;
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // 注册UART驱动 (暂时注释掉)
    /*
    uart_driver_ = new drivers::UartDriver();
    if (!uart_driver_) {
        delete device_manager_;
        delete gic_;
        device_manager_ = nullptr;
        gic_ = nullptr;
        return VoidResult{ErrorCode::OutOfMemory};
    }

    auto uart_reg_result = device_manager_->register_driver(uart_driver_);
    if (!uart_reg_result) {
        delete uart_driver_;
        delete device_manager_;
        delete gic_;
        uart_driver_ = nullptr;
        device_manager_ = nullptr;
        gic_ = nullptr;
        return uart_reg_result;
    }
    */

    // 初始化多架构系统调用支持
    kernel_print("🔧 初始化多架构系统调用支持...\n");
    if (!arch::syscall::initialize_architecture_syscalls()) {
        kernel_print("❌ 系统调用架构初始化失败\n");
        return VoidResult{ErrorCode::NotSupported};
    }
    kernel_print("✅ 系统调用架构初始化成功\n");

    // 打印系统调用架构信息
    arch::syscall::print_syscall_convention();

    return VoidResult{};
  }

  // 系统服务初始化
  [[nodiscard]] VoidResult initialize_services() noexcept {
    // 这里可以启动内核服务线程
    // 如内存回收、定时器服务等
    return VoidResult{};
  }

  // 用户空间初始化
  [[nodiscard]] VoidResult initialize_userspace() noexcept {
    // 准备用户空间环境
    // 设置用户态页表、加载初始程序等
    return VoidResult{};
  }

  // 创建初始进程
  [[nodiscard]] VoidResult create_init_process() noexcept {
    // 简化实现：创建内核线程作为初始"进程"
    return VoidResult{};
  }

  // 启用中断
  void enable_interrupts() noexcept {
#if defined(MOSS_ARCH_ARM64)
    asm volatile("msr daifclr, #2" ::: "memory"); // 启用IRQ
#elif defined(MOSS_ARCH_X86_64)
    asm volatile("sti" ::: "memory"); // 启用中断
#elif defined(MOSS_ARCH_RISCV)
    asm volatile("csrsi mstatus, 0x8" ::: "memory"); // 启用机器级中断
#endif
  }

  // 内核崩溃处理
  [[noreturn]] void kernel_panic(const char *message,
                                 ErrorCode error) noexcept {
// 禁用中断
#if defined(MOSS_ARCH_ARM64)
    asm volatile("msr daifset, #2" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
    asm volatile("cli" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
    asm volatile("csrci mstatus, 0x8" ::: "memory"); // 禁用机器级中断
#endif

    kernel_print("\n💀 KERNEL PANIC 💀\n");
    kernel_print("错误: %s\n", message);
    kernel_print("错误代码: %d\n", static_cast<int>(error));
    kernel_print("当前阶段: %d\n", static_cast<int>(current_phase_));

    // 打印调用栈
    print_stack_trace();

    // 停机
    while (true) {
#if defined(MOSS_ARCH_ARM64)
      asm volatile("wfi" ::: "memory"); // 等待中断
#elif defined(MOSS_ARCH_X86_64)
      asm volatile("hlt" ::: "memory"); // 停机等待中断
#elif defined(MOSS_ARCH_RISCV)
      asm volatile("wfi" ::: "memory"); // RISC-V也有wfi指令
#else
      // 通用停机 - CPU空循环
      for (volatile int i = 0; i < 1000000; ++i) {
      }
#endif
    }
  }

  // 打印启动横幅
  void print_banner() const noexcept {
    kernel_print("\n");
    kernel_print("███╗   ███╗ ██████╗ ███████╗███████╗\n");
    kernel_print("████╗ ████║██╔═══██╗██╔════╝██╔════╝\n");
    kernel_print("██╔████╔██║██║   ██║███████╗███████╗\n");
    kernel_print("██║╚██╔╝██║██║   ██║╚════██║╚════██║\n");
    kernel_print("██║ ╚═╝ ██║╚██████╔╝███████║███████║\n");
    kernel_print("╚═╝     ╚═╝ ╚═════╝ ╚══════╝╚══════╝\n");
    kernel_print("\n🚀      MOSS混合内核 v1.0\n");
  }

  // 打印调用栈
  void print_stack_trace() const noexcept {
    kernel_print("📍 调用栈:\n");

    u64 fp = 0;
#if defined(MOSS_ARCH_ARM64)
    asm volatile("mov %0, x29" : "=r"(fp));
#elif defined(MOSS_ARCH_X86_64)
    asm volatile("mov %%rbp, %0" : "=r"(fp));
#elif defined(MOSS_ARCH_RISCV)
    asm volatile("mv %0, s0" : "=r"(fp));
#endif

    for (int i = 0; i < 10 && fp != 0; i++) {
      u64 *frame = reinterpret_cast<u64 *>(fp);
      if (frame != nullptr) {
        u64 lr = frame[1]; // 返回地址
        fp = frame[0];     // 下一帧指针

        kernel_print("  [%d] 0x%016llx\n", i, lr);
      } else {
        break;
      }
    }
  }

  // 获取当前时间
  [[nodiscard]] static u64 get_current_time() noexcept {
    u64 count;
#if defined(MOSS_ARCH_ARM64)
    asm volatile("mrs %0, cntvct_el0" : "=r"(count));
#elif defined(MOSS_ARCH_X86_64)
    asm volatile("rdtsc" : "=A"(count));
#elif defined(MOSS_ARCH_RISCV)
    asm volatile("rdcycle %0" : "=r"(count));
#else
    count = 0; // 回退实现
#endif
    return count;
  }

  // 数字格式化辅助函数
  static void print_signed_number(i64 num) noexcept {
    if (num < 0) {
      uart_putc('-');
      print_unsigned_number(static_cast<u64>(-num), 10);
    } else {
      print_unsigned_number(static_cast<u64>(num), 10);
    }
  }

  static void print_unsigned_number(u64 num, u32 base, bool uppercase = false) noexcept {
    if (num == 0) {
      uart_putc('0');
      return;
    }

    char buffer[32];
    u32 idx = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    while (num > 0) {
      buffer[idx++] = digits[num % base];
      num /= base;
    }

    for (i32 i = static_cast<i32>(idx) - 1; i >= 0; i--) {
      uart_putc(buffer[i]);
    }
  }

  // 内核打印函数 - 完整实现
  static void kernel_print(const char *format, ...) noexcept {
    if (format == nullptr) return;

    va_list args;
    va_start(args, format);

    const char *ptr = format;
    while (*ptr) {
      if (*ptr == '\n') {
        uart_putc('\r');
        uart_putc('\n');
      } else if (*ptr == '%') {
        ptr++;
        if (*ptr == '\0') break;

        // 解析长度修饰符
        bool is_long = false;
        bool is_long_long = false;

        if (*ptr == 'l') {
          is_long = true;
          ptr++;
          if (*ptr == 'l') {
            is_long_long = true;
            ptr++;
          }
        }

        // 处理格式说明符
        switch (*ptr) {
        case 'd': {
          if (is_long_long) {
            signed long long value = va_arg(args, signed long long);
            print_signed_number(static_cast<i64>(value));
          } else {
            int value = va_arg(args, int);
            print_signed_number(static_cast<i64>(value));
          }
          break;
        }
        case 'u': {
          if (is_long_long) {
            unsigned long long value = va_arg(args, unsigned long long);
            print_unsigned_number(static_cast<u64>(value), 10);
          } else {
            unsigned int value = va_arg(args, unsigned int);
            print_unsigned_number(static_cast<u64>(value), 10);
          }
          break;
        }
        case 'x': {
          if (is_long_long) {
            unsigned long long value = va_arg(args, unsigned long long);
            print_unsigned_number(static_cast<u64>(value), 16);
          } else {
            unsigned int value = va_arg(args, unsigned int);
            print_unsigned_number(static_cast<u64>(value), 16);
          }
          break;
        }
        case 'X': {
          if (is_long_long) {
            unsigned long long value = va_arg(args, unsigned long long);
            print_unsigned_number(static_cast<u64>(value), 16, true);
          } else {
            unsigned int value = va_arg(args, unsigned int);
            print_unsigned_number(static_cast<u64>(value), 16, true);
          }
          break;
        }
        case 's': {
          const char *str = va_arg(args, char*);
          if (str) {
            while (*str) {
              uart_putc(*str);
              str++;
            }
          } else {
            const char *null_str = "(null)";
            while (*null_str) {
              uart_putc(*null_str);
              null_str++;
            }
          }
          break;
        }
        case 'c': {
          char ch = static_cast<char>(va_arg(args, int));
          uart_putc(ch);
          break;
        }
        case 'p': {
          void *ptr_val = va_arg(args, void*);
          uart_putc('0');
          uart_putc('x');
          print_unsigned_number(reinterpret_cast<u64>(ptr_val), 16);
          break;
        }
        case '%': {
          uart_putc('%');
          break;
        }
        default:
          // 不支持的格式
          uart_putc('%');
          if (is_long_long) {
            uart_putc('l');
            uart_putc('l');
          } else if (is_long) {
            uart_putc('l');
          }
          uart_putc(*ptr);
          break;
        }
      } else {
        uart_putc(*ptr);
      }
      ptr++;
    }

    va_end(args);
  }

  // 简化UART输出
  static void uart_putc(char c) noexcept {
    // 简化实现：直接写入UART寄存器
    // 实际应该通过设备驱动
    volatile u32 *uart_data = reinterpret_cast<volatile u32 *>(0x09000000);
    volatile u32 *uart_flags = reinterpret_cast<volatile u32 *>(0x09000018);

    // 等待发送FIFO可用
    while (*uart_flags & (1 << 5)) {
      // TXFF标志
    }

    *uart_data = static_cast<u32>(c);
  }
};

// 全局内核实例
extern Kernel *g_kernel;

} // namespace moss::kernel
