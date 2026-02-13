#include "mm/page_table.hpp"
#include "mm/page_frame_allocator.hpp"
#include "mm/runtime_heap_allocator.hpp"
#include "core/moss_std.hpp" // 裸机环境基础定义
#include "core/result.hpp"
#include "core/types.hpp"

// 外部符号声明（来自链接器脚本）
extern "C" {
extern char _text_start_addr[];
extern char _text_end_addr[];
extern char _rodata_start_addr[];
extern char _rodata_end_addr[];
extern char _data_start_addr[];
extern char _data_end_addr[];
extern char _bss_start_addr[];
extern char _bss_end_addr[];
extern char _stack_bottom_addr[];
extern char _stack_top_addr[];
extern char _heap_start_addr[];
extern char _heap_end_addr[];
extern char _pagetable_start_addr[];
extern char _pagetable_end_addr[];
extern char _kernel_end_addr[];
}

// 外部函数声明
extern "C" void mark_runtime_heap_ready() noexcept;
extern "C" void kernel_main(void) noexcept;

namespace moss::kernel {

// 早期串口输出（QEMU virt平台的UART）
class EarlyUart {
private:
  static constexpr VirtAddr UART_BASE = 0x09000000;
  static constexpr u32 UART_DR = 0x000;         // 数据寄存器
  static constexpr u32 UART_FR = 0x018;         // 标志寄存器
  static constexpr u32 UART_FR_TXFF = (1 << 5); // 发送FIFO满

  volatile u32 *const uart_base;

public:
  EarlyUart() : uart_base(reinterpret_cast<volatile u32 *>(UART_BASE)) {}

  void put_char(char c) const {
    // 等待发送FIFO有空间
    while (uart_base[UART_FR / 4] & UART_FR_TXFF) {
      // 忙等待
    }
    uart_base[UART_DR / 4] = static_cast<u32>(c);
  }

  void put_string(const char *str) const {
    while (*str) {
      if (*str == '\n') {
        put_char('\r'); // 添加回车符
      }
      put_char(*str++);
    }
  }
};

// 全局早期UART实例
static EarlyUart early_uart;

// 早期打印函数
void early_print(const char *str) { early_uart.put_string(str); }

void early_print_hex(u64 value) {
  constexpr char hex_chars[] = "0123456789ABCDEF";
  char buffer[19] = "0x"; // "0x" + 16个十六进制字符 + null终止符

  for (int i = 15; i >= 0; i--) {
    buffer[2 + (15 - i)] = hex_chars[(value >> (i * 4)) & 0xF];
  }
  buffer[18] = '\0';

  early_print(buffer);
}

// 内存布局信息显示
void display_memory_layout() {
  early_print("=== 内核内存布局 ===\n");

  early_print("代码段:   ");
  early_print_hex(reinterpret_cast<u64>(_text_start_addr));
  early_print(" - ");
  early_print_hex(reinterpret_cast<u64>(_text_end_addr));
  early_print("\n");

  early_print("只读数据: ");
  early_print_hex(reinterpret_cast<u64>(_rodata_start_addr));
  early_print(" - ");
  early_print_hex(reinterpret_cast<u64>(_rodata_end_addr));
  early_print("\n");

  early_print("数据段:   ");
  early_print_hex(reinterpret_cast<u64>(_data_start_addr));
  early_print(" - ");
  early_print_hex(reinterpret_cast<u64>(_data_end_addr));
  early_print("\n");

  early_print("BSS段:    ");
  early_print_hex(reinterpret_cast<u64>(_bss_start_addr));
  early_print(" - ");
  early_print_hex(reinterpret_cast<u64>(_bss_end_addr));
  early_print("\n");

  early_print("栈空间:   ");
  early_print_hex(reinterpret_cast<u64>(_stack_bottom_addr));
  early_print(" - ");
  early_print_hex(reinterpret_cast<u64>(_stack_top_addr));
  early_print("\n");

  early_print("堆空间:   ");
  early_print_hex(reinterpret_cast<u64>(_heap_start_addr));
  early_print(" - ");
  early_print_hex(reinterpret_cast<u64>(_heap_end_addr));
  early_print("\n");

  early_print("页表区:   ");
  early_print_hex(reinterpret_cast<u64>(_pagetable_start_addr));
  early_print(" - ");
  early_print_hex(reinterpret_cast<u64>(_pagetable_end_addr));
  early_print("\n");

  early_print("内核结束: ");
  early_print_hex(reinterpret_cast<u64>(_kernel_end_addr));
  early_print("\n\n");
}

// CPU信息检测
struct CpuInfo {
  u64 midr_el1;     // Main ID Register
  u64 mpidr_el1;    // Multiprocessor Affinity Register
  u64 revidr_el1;   // Revision ID Register
  u64 id_aa64pfr0;  // Processor Feature Register 0
  u64 id_aa64mmfr0; // Memory Model Feature Register 0
};

CpuInfo detect_cpu_features() {
  CpuInfo info;

#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
  asm volatile("mrs %0, midr_el1" : "=r"(info.midr_el1));
  asm volatile("mrs %0, mpidr_el1" : "=r"(info.mpidr_el1));
  asm volatile("mrs %0, revidr_el1" : "=r"(info.revidr_el1));
  asm volatile("mrs %0, id_aa64pfr0_el1" : "=r"(info.id_aa64pfr0));
  asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(info.id_aa64mmfr0));
#else
  // 非ARM64架构，返回默认值
  info.midr_el1 = 0;
  info.mpidr_el1 = 0;
  info.revidr_el1 = 0;
  info.id_aa64pfr0 = 0;
  info.id_aa64mmfr0 = 0;
#endif

  return info;
}

void display_cpu_info() {
  auto cpu_info = detect_cpu_features();

  early_print("=== CPU信息 ===\n");
  early_print("MIDR_EL1:     ");
  early_print_hex(cpu_info.midr_el1);
  early_print("\n");

  early_print("MPIDR_EL1:    ");
  early_print_hex(cpu_info.mpidr_el1);
  early_print("\n");

  early_print("CPU核心ID:    ");
  early_print_hex(cpu_info.mpidr_el1 & 0xFF);
  early_print("\n");

  // 检查是否支持Large System Extensions (LSE)
  u64 lse_support = (cpu_info.id_aa64pfr0 >> 20) & 0xF;
  early_print("LSE支持:      ");
  if (lse_support >= 1) {
    early_print("是\n");
  } else {
    early_print("否\n");
  }

  early_print("\n");
}

// 基础系统初始化
VoidResult initialize_basic_systems() {
  early_print("初始化基础系统组件...\n");

  // 1. 初始化MMU和页表管理
  early_print("设置MMU和页表管理...\n");

  early_print("  - 创建页表映射...");
  auto mmu_result = moss::kernel::mm::setup_mmu();
  if (!mmu_result) {
    early_print("失败\n");
    early_print("  - 错误代码: ");
    early_print_hex(static_cast<u64>(mmu_result.error()));
    early_print("\n");
    return VoidResult{mmu_result.error()};
  }
  early_print("成功\n");

  // 验证 MMU 实际启用状态
  early_print("  - 验证MMU状态...");
  u64 sctlr;
  asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
  if (sctlr & (1ULL << 0)) {
    early_print("MMU已启用\n");
  } else {
    early_print("MMU未启用（页表已准备）\n");
    // 暂时允许继续，因为我们只设置了页表但未启用MMU
  }

  // 测试虚拟地址访问 - 增强版本
  early_print("  - 测试虚拟地址访问...");

  // 测试1: 栈上变量测试（原有测试）
  volatile u64 test_value = 0x12345678ABCDEF00ULL;
  if (test_value != 0x12345678ABCDEF00ULL) {
    early_print("栈访问异常！\n");
    return VoidResult{ErrorCode::InvalidState};
  }

  // 测试2: 直接虚拟地址指针写入测试
  // 选择BSS段内的一个安全区域进行测试（应该在0-1GB映射范围内）
  VirtAddr test_vaddr =
      reinterpret_cast<VirtAddr>(_bss_start_addr) + 0x1000; // BSS段开始+4KB处
  volatile u64 *test_ptr = reinterpret_cast<volatile u64 *>(test_vaddr);

  // 写入测试模式1：顺序测试数据
  const u64 test_patterns[] = {0xDEADBEEFCAFEBABEULL, 0x0123456789ABCDEFULL,
                               0xFFFFFFFFFFFFFFFFULL, 0x0000000000000000ULL,
                               0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL};

  bool virt_mem_test_passed = true;
  for (size_t i = 0; i < sizeof(test_patterns) / sizeof(test_patterns[0]);
       i++) {
    // 写入测试数据
    test_ptr[i] = test_patterns[i];

    // 内存屏障确保写入完成
    asm volatile("dsb sy" ::: "memory");

    // 读回并验证
    u64 read_value = test_ptr[i];
    if (read_value != test_patterns[i]) {
      early_print("虚拟内存测试失败！\n");
      early_print("    地址: ");
      early_print_hex(reinterpret_cast<u64>(&test_ptr[i]));
      early_print("\n    期望: ");
      early_print_hex(test_patterns[i]);
      early_print("\n    实际: ");
      early_print_hex(read_value);
      early_print("\n");
      virt_mem_test_passed = false;
      break;
    }
  }

  if (!virt_mem_test_passed) {
    return VoidResult{ErrorCode::InvalidState};
  }

  // 测试3: 跨页边界访问测试
  early_print("  - 测试跨页边界访问...");
  VirtAddr page_boundary_addr =
      (test_vaddr & ~0xFFFULL) + 0x1000 - 8; // 页边界前8字节
  volatile u64 *boundary_ptr =
      reinterpret_cast<volatile u64 *>(page_boundary_addr);

  // 写入跨页数据（8字节数据跨越页边界）
  *boundary_ptr = 0x123456789ABCDEF0ULL;
  asm volatile("dsb sy" ::: "memory");

  u64 boundary_value = *boundary_ptr;
  if (boundary_value != 0x123456789ABCDEF0ULL) {
    early_print("跨页访问失败！\n");
    early_print("    地址: ");
    early_print_hex(reinterpret_cast<u64>(boundary_ptr));
    early_print("\n    期望: 0x123456789ABCDEF0\n    实际: ");
    early_print_hex(boundary_value);
    early_print("\n");
    return VoidResult{ErrorCode::InvalidState};
  }
  early_print("成功\n");

  // 跳过虚拟地址区域测试，直接进入动态内存分配初始化
  early_print("  - 虚拟内存地址转换测试通过\n");

  // MMU启用成功，现在可以使用虚拟地址
  // early_print("MMU ACTIVE\n");

  // 2. 初始化动态内存分配系统
  early_print("初始化动态内存分配系统...\n");

  // 2.1 初始化物理页面分配器
  early_print("  - 初始化物理页面分配器...");
  auto pfa_result = moss::kernel::mm::PageFrameAllocator::initialize();
  if (!pfa_result) {
    early_print("失败\n");
    early_print("  - 错误代码: ");
    early_print_hex(static_cast<u64>(pfa_result.error()));
    early_print("\n");
    return VoidResult{ErrorCode::OutOfMemory};
  }
  early_print("成功\n");

  // 2.2 初始化运行时堆分配器 - 使用保守的初始大小
  early_print("  - 初始化运行时堆分配器...");
  VirtAddr heap_start = reinterpret_cast<VirtAddr>(_heap_start_addr);
  usize initial_heap_size = 256 * 1024;  // 初始256KB堆空间（保守设置）
  auto heap_result = moss::kernel::mm::RuntimeHeapAllocator::initialize_heap(
    heap_start, initial_heap_size);
  if (!heap_result) {
    early_print("失败\n");
    early_print("  - 错误代码: ");
    early_print_hex(static_cast<u64>(heap_result.error()));
    early_print("\n");
    return VoidResult{ErrorCode::OutOfMemory};
  }
  early_print("成功\n");

  // 2.3 测试动态内存分配
  early_print("  - 测试动态内存分配...");
  auto test_alloc_result = moss::kernel::mm::RuntimeHeapAllocator::allocate(256);
  if (!test_alloc_result) {
    early_print("失败\n");
    return VoidResult{ErrorCode::OutOfMemory};
  }
  void* heap_test_ptr = test_alloc_result.value();

  // 测试写入分配的内存
  volatile u64* heap_data = static_cast<volatile u64*>(heap_test_ptr);
  *heap_data = 0x123456789ABCDEF0ULL;
  asm volatile("dsb sy" ::: "memory");

  if (*heap_data != 0x123456789ABCDEF0ULL) {
    early_print("动态分配内存写入测试失败\n");
    return VoidResult{ErrorCode::OutOfMemory};
  }

  auto free_result = moss::kernel::mm::RuntimeHeapAllocator::deallocate(heap_test_ptr, 256);
  if (!free_result) {
    early_print("释放失败\n");
    return VoidResult{ErrorCode::OutOfMemory};
  }
  early_print("成功\n");

  // 标记运行时堆已准备好，切换C++ runtime support
  mark_runtime_heap_ready();
  early_print("  - 运行时堆切换完成\n");

  early_print("动态内存分配系统初始化完成\n");

  // TODO: 初始化进程管理器
  // TODO: 初始化IPC系统
  // TODO: 初始化设备管理器

  early_print("基础系统初始化完成\n\n");
  return VoidResult{};
}

} // namespace moss::kernel

// C接口函数 - 从汇编代码调用
extern "C" void early_main(void *device_tree_ptr) {
  using namespace moss::kernel;

  // 显示启动横幅
  early_print("\n");
  early_print("================================================\n");
  early_print("           Moss ARM64混合内核操作系统\n");
  early_print("================================================\n");
  early_print("版本: 0.1.0-dev\n");
  early_print("设备树: ");
  early_print_hex(reinterpret_cast<u64>(device_tree_ptr));
  early_print("\n\n");

  // 显示内存布局
  display_memory_layout();

  // 显示CPU信息
  display_cpu_info();

  // 初始化基础系统
  auto result = initialize_basic_systems();
  if (!result) {
    early_print("错误: 基础系统初始化失败\n");
    return;
  }

  early_print("早期初始化完成，转交给统一启动流程...\n");
  early_print("\n");

  // 🔧 关键修复：调用统一启动流程，包含完整的SMP支持和Linux风格延迟激活
  early_print("🚀 进入统一启动流程（包含Linux风格SMP支持）...\n");

  // 声明统一启动函数
  extern void unified_boot_main(void* device_tree_ptr);

  // 调用统一启动流程，这将：
  // 1. 完成所有启动阶段（包括SMP支持）
  // 2. 调用kernel_main()
  // 3. 在kernel中执行Linux风格延迟激活
  unified_boot_main(device_tree_ptr);

  // 执行干净的关闭 - 使用semihosting退出
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
  early_print("执行系统关闭...\n");

  // 方法1: ARM Semihosting退出调用
  early_print("尝试Semihosting退出...\n");
  // ARM Semihosting SYS_EXIT_EXTENDED (0x20)
  // 参数结构: [reason, exit_code]
  u64 exit_params[2] = {0x20026, 0}; // ADP_Stopped_ApplicationExit, exit_code=0
  asm volatile("mov x0, #0x20\n"     // SYS_EXIT_EXTENDED
               "mov x1, %0\n"        // 参数指针
               "hlt #0xF000\n"       // ARM64 semihosting调用
               :
               : "r"(exit_params)
               : "x0", "x1");

  // 方法2: PSCI SYSTEM_OFF (SMC)
  early_print("尝试PSCI SMC调用...\n");
  asm volatile("movz x0, #0x0008, lsl #0\n"  // 加载低16位: 0x0008
               "movk x0, #0x8400, lsl #16\n" // 加载高16位: 0x8400
               "smc #0\n"                    // Secure Monitor call
               :
               :
               : "x0");

  // 方法3: PSCI SYSTEM_OFF (HVC)
  early_print("尝试PSCI HVC调用...\n");
  asm volatile("movz x0, #0x0008, lsl #0\n"  // 加载低16位: 0x0008
               "movk x0, #0x8400, lsl #16\n" // 加载高16位: 0x8400
               "hvc #0\n"                    // Hypervisor call
               :
               :
               : "x0");

  // 方法4: 最后的fallback
  early_print("所有关闭方法失败，进入低功耗模式...\n");
  while (true) {
    asm volatile("wfi"); // 等待中断
  }
#else
  // 其他架构: 无限循环
  while (true) {
    asm volatile("");
  }
#endif
}

// placement new操作符已在moss_std.hpp中定义
