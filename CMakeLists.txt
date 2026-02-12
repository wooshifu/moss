cmake_minimum_required(VERSION 3.31)

project(
  moss
  VERSION 0.9.9
  LANGUAGES C CXX ASM)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)
# set(CMAKE_CXX_MODULE_STD OFF) # 使用 freestanding C++, 不使用标准库模块

# Configure C++26 standard with strict settings
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 包含多架构支持模块
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(ArchSupport)
include(ToolchainDetection)
include(Modules)

# === 多架构配置 ===
message(STATUS "开始配置 Moss 内核构建系统...")

# 初始化 Clang 工具链
moss_initialize_clang_toolchain(${MOSS_TARGET_ARCH})

# 配置目标架构
moss_configure_architecture()

# 配置目标平台
moss_configure_platform()

# 设置通用内核编译标志 - 最严格模式
set(CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} -ffreestanding -fno-exceptions -fno-rtti")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-stack-protector")

# === 🚀 CLANG -Weverything 终极严格模式 ===
# 启用所有可能的警告选项
# === -Weverything 说明 ===
# 这是 Clang 最激进的警告设置，包含： ✅ 所有 -Wall, -Wextra 选项 ✅ 所有类型转换和安全检查 ✅ 所有代码质量和风格检查 ✅
# 所有现代C++最佳实践检查 ✅ 所有线程安全和并发检查 ✅ 所有性能和优化建议 ✅ 所有内核特定安全检查 ⚠️  超过 200+ 个不同的警告类别
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Weverything -Werror")

# === 必要的警告禁用 (维持可构建性) ===
# 禁用C++兼容性警告（项目专门使用C++26）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-c++-compat")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-c++98-compat")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-c++98-compat-pedantic")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-c++11-compat")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-c++14-compat")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-c++17-compat")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-c++20-compat")

# 禁用不适用于内核开发的警告
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-padded") # 结构体填充（开发期间太严格）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unsafe-buffer-usage"
)# 缓冲区安全（需要大量重构）

# 禁用GNU/Microsoft扩展警告（内核代码可能需要特定扩展）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-gnu-zero-variadic-macro-arguments")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-gnu-anonymous-struct")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-nested-anon-types")

# 禁用文档相关警告（开发期间） set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-documentation")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-documentation-pedantic")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-documentation-unknown-command")

# 禁用一些在内核环境中常见但可接受的警告
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-missing-prototypes") # C++中不适用
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-missing-variable-declarations")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-global-constructors") # 内核中有时需要
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-exit-time-destructors") # 内核中有时需要
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-weak-vtables") # 内联虚表有时有用

# 内联相关警告调整
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-inline") # 内联失败不报错（避免噪音）

# 禁用过于严格的宏和预处理器警告
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-reserved-identifier") # 保留标识符（内核需要）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-reserved-macro-identifier")

# 解决Switch语句冲突警告：禁用covered-switch-default，保留switch-default
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-covered-switch-default")

# 禁用NRVO警告（返回值优化相关，可能与内核代码风格冲突）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-nrvo")

# 优化级别设置
set(CMAKE_CXX_FLAGS_DEBUG "-Og -g3")
set(CMAKE_CXX_FLAGS_RELEASE "-g0 -O3 -DNDEBUG")

# === 全局头文件路径设置 ===
# 设置公共接口头文件路径
include_directories(${CMAKE_SOURCE_DIR}/src/include)

# === 配置系统生成 ===
# 获取当前时间戳用于构建信息
string(TIMESTAMP MOSS_BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S UTC" UTC)

# 实用函数：将CMake布尔值转换为1/0数值
function(moss_bool_to_int var)
  if(${${var}})
    set(${var}
        1
        PARENT_SCOPE)
  else()
    set(${var}
        0
        PARENT_SCOPE)
  endif()
endfunction()

# 设置架构特定的配置变量
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
  set(MOSS_ARCH_ARM64 ON)
  set(MOSS_ARCH_NAME "ARM64")
  set(MOSS_PAGE_SIZE 4096)
  set(MOSS_CACHE_LINE_SIZE 64)
  set(MOSS_POINTER_SIZE 8)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
  set(MOSS_ARCH_X86_64 ON)
  set(MOSS_ARCH_NAME "X86_64")
  set(MOSS_PAGE_SIZE 4096)
  set(MOSS_CACHE_LINE_SIZE 64)
  set(MOSS_POINTER_SIZE 8)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
  set(MOSS_ARCH_RISCV ON)
  set(MOSS_ARCH_NAME "RISCV64")
  set(MOSS_PAGE_SIZE 4096)
  set(MOSS_CACHE_LINE_SIZE 64)
  set(MOSS_POINTER_SIZE 8)
else()
  message(FATAL_ERROR "Unsupported architecture: ${MOSS_TARGET_ARCH}")
endif()

# 设置平台特定的配置
if(MOSS_TARGET_PLATFORM STREQUAL "QEMU_VIRT")
  set(MOSS_PLATFORM_QEMU_VIRT ON)
  set(MOSS_PLATFORM_NAME "QEMU_VIRT")
else()
  set(MOSS_PLATFORM_NAME "UNKNOWN")
endif()

# 设置构建类型相关配置
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(MOSS_DEBUG_BUILD ON)
  set(MOSS_BUILD_TYPE "Debug")
  set(MOSS_DEBUG_LEVEL 3)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(MOSS_RELEASE_BUILD ON)
  set(MOSS_BUILD_TYPE "Release")
  set(MOSS_DEBUG_LEVEL 1)
else()
  set(MOSS_BUILD_TYPE "Unknown")
  set(MOSS_DEBUG_LEVEL 2)
endif()

# 模块开关选项（可通过CMake -D 选项覆盖）
option(MOSS_ENABLE_DRIVERS "Enable device driver support" ON)
option(MOSS_ENABLE_INTERRUPTS "Enable interrupt handling" ON)
option(MOSS_ENABLE_IPC "Enable inter-process communication" ON)
option(MOSS_ENABLE_PROCESS "Enable process management" ON)

# 容器库功能选项
option(MOSS_ENABLE_LOCKFREE_QUEUE "Enable lockfree queue implementation" ON)
option(MOSS_ENABLE_SLAB_ALLOCATOR "Enable slab allocator" ON)
option(MOSS_ENABLE_RCU_LIST "Enable RCU list implementation" ON)
option(MOSS_ENABLE_PER_CPU_DATA "Enable per-CPU data structures" ON)
option(MOSS_ENABLE_ATOMIC_TYPES "Enable atomic type support" ON)

# 进程管理功能选项
option(MOSS_ENABLE_CFS_SCHEDULER "Enable CFS scheduler" ON)
option(MOSS_ENABLE_LOAD_BALANCER "Enable load balancer" ON)
option(MOSS_ENABLE_PROCESS_TREE "Enable process tree management" ON)

# IPC功能选项
option(MOSS_ENABLE_SHARED_MEMORY "Enable shared memory support" ON)
option(MOSS_ENABLE_ZERO_COPY_CHANNEL "Enable zero-copy channel" ON)
option(MOSS_ENABLE_IPC_MANAGER "Enable IPC manager" ON)

# 驱动功能选项
option(MOSS_ENABLE_UART_DRIVER "Enable UART driver" ON)
option(MOSS_ENABLE_DEVICE_MANAGER "Enable device manager" ON)

# 中断功能选项
option(MOSS_ENABLE_GIC "Enable ARM GIC support" ON)

# 将所有布尔选项转换为1/0数值（用于config.h模板）
moss_bool_to_int(MOSS_ENABLE_DRIVERS)
moss_bool_to_int(MOSS_ENABLE_INTERRUPTS)
moss_bool_to_int(MOSS_ENABLE_IPC)
moss_bool_to_int(MOSS_ENABLE_PROCESS)
moss_bool_to_int(MOSS_ENABLE_LOCKFREE_QUEUE)
moss_bool_to_int(MOSS_ENABLE_SLAB_ALLOCATOR)
moss_bool_to_int(MOSS_ENABLE_RCU_LIST)
moss_bool_to_int(MOSS_ENABLE_PER_CPU_DATA)
moss_bool_to_int(MOSS_ENABLE_ATOMIC_TYPES)
moss_bool_to_int(MOSS_ENABLE_CFS_SCHEDULER)
moss_bool_to_int(MOSS_ENABLE_LOAD_BALANCER)
moss_bool_to_int(MOSS_ENABLE_PROCESS_TREE)
moss_bool_to_int(MOSS_ENABLE_SHARED_MEMORY)
moss_bool_to_int(MOSS_ENABLE_ZERO_COPY_CHANNEL)
moss_bool_to_int(MOSS_ENABLE_IPC_MANAGER)
moss_bool_to_int(MOSS_ENABLE_UART_DRIVER)
moss_bool_to_int(MOSS_ENABLE_DEVICE_MANAGER)
moss_bool_to_int(MOSS_ENABLE_GIC)

# 调试功能选项（根据调试级别自动启用）
if(MOSS_DEBUG_LEVEL GREATER_EQUAL 2)
  option(MOSS_ENABLE_MEMORY_DEBUG "Enable memory debugging" ON)
  option(MOSS_ENABLE_STACK_OVERFLOW_CHECK "Enable stack overflow checking" ON)
  option(MOSS_ENABLE_HEAP_CORRUPTION_CHECK "Enable heap corruption checking" ON)
  option(MOSS_ENABLE_DOUBLE_FREE_CHECK "Enable double-free checking" ON)
endif()

if(MOSS_DEBUG_LEVEL GREATER_EQUAL 1)
  option(MOSS_ENABLE_ASSERTIONS "Enable runtime assertions" ON)
  option(MOSS_ENABLE_DATA_STRUCTURE_CHECK
         "Enable data structure consistency checks" ON)
endif()

if(MOSS_DEBUG_LEVEL GREATER_EQUAL 3)
  option(MOSS_ENABLE_FUNCTION_TRACING "Enable function call tracing" ON)
  option(MOSS_ENABLE_LOCK_PROFILING "Enable lock profiling" ON)
  option(MOSS_ENABLE_SCHEDULER_STATS "Enable scheduler statistics" ON)
  option(MOSS_ENABLE_SYSCALL_TRACING "Enable system call tracing" ON)
endif()

# 转换调试选项为数值（需要检查是否已定义）
if(DEFINED MOSS_ENABLE_MEMORY_DEBUG)
  moss_bool_to_int(MOSS_ENABLE_MEMORY_DEBUG)
else()
  set(MOSS_ENABLE_MEMORY_DEBUG 0)
endif()

if(DEFINED MOSS_ENABLE_STACK_OVERFLOW_CHECK)
  moss_bool_to_int(MOSS_ENABLE_STACK_OVERFLOW_CHECK)
else()
  set(MOSS_ENABLE_STACK_OVERFLOW_CHECK 0)
endif()

if(DEFINED MOSS_ENABLE_HEAP_CORRUPTION_CHECK)
  moss_bool_to_int(MOSS_ENABLE_HEAP_CORRUPTION_CHECK)
else()
  set(MOSS_ENABLE_HEAP_CORRUPTION_CHECK 0)
endif()

if(DEFINED MOSS_ENABLE_DOUBLE_FREE_CHECK)
  moss_bool_to_int(MOSS_ENABLE_DOUBLE_FREE_CHECK)
else()
  set(MOSS_ENABLE_DOUBLE_FREE_CHECK 0)
endif()

if(DEFINED MOSS_ENABLE_ASSERTIONS)
  moss_bool_to_int(MOSS_ENABLE_ASSERTIONS)
else()
  set(MOSS_ENABLE_ASSERTIONS 0)
endif()

if(DEFINED MOSS_ENABLE_DATA_STRUCTURE_CHECK)
  moss_bool_to_int(MOSS_ENABLE_DATA_STRUCTURE_CHECK)
else()
  set(MOSS_ENABLE_DATA_STRUCTURE_CHECK 0)
endif()

if(DEFINED MOSS_ENABLE_FUNCTION_TRACING)
  moss_bool_to_int(MOSS_ENABLE_FUNCTION_TRACING)
else()
  set(MOSS_ENABLE_FUNCTION_TRACING 0)
endif()

if(DEFINED MOSS_ENABLE_LOCK_PROFILING)
  moss_bool_to_int(MOSS_ENABLE_LOCK_PROFILING)
else()
  set(MOSS_ENABLE_LOCK_PROFILING 0)
endif()

if(DEFINED MOSS_ENABLE_SCHEDULER_STATS)
  moss_bool_to_int(MOSS_ENABLE_SCHEDULER_STATS)
else()
  set(MOSS_ENABLE_SCHEDULER_STATS 0)
endif()

if(DEFINED MOSS_ENABLE_SYSCALL_TRACING)
  moss_bool_to_int(MOSS_ENABLE_SYSCALL_TRACING)
else()
  set(MOSS_ENABLE_SYSCALL_TRACING 0)
endif()

# 生成配置头文件
configure_file(${CMAKE_SOURCE_DIR}/src/include/config/config.h.in
               ${CMAKE_BINARY_DIR}/include/config/config.h @ONLY)

# 添加生成的配置头文件目录到包含路径
include_directories(${CMAKE_BINARY_DIR}/include)

message(STATUS "配置系统已生成: ${CMAKE_BINARY_DIR}/include/config/config.h")
message(STATUS "目标架构: ${MOSS_ARCH_NAME}")
message(STATUS "目标平台: ${MOSS_PLATFORM_NAME}")
message(STATUS "构建类型: ${MOSS_BUILD_TYPE} (调试级别: ${MOSS_DEBUG_LEVEL})")

# === 添加模块 ===
add_subdirectory(src/modules)

# === 添加子模块（按依赖顺序）===
# 基础模块：容器库（最基础）
add_subdirectory(src/containers)

# 核心子系统：内存管理（依赖容器）
add_subdirectory(src/mm)

# 设备和系统服务：驱动、中断（依赖容器、内存管理）
add_subdirectory(src/drivers)
add_subdirectory(src/interrupts)

# 进程间通信：IPC（依赖容器、内存管理）
add_subdirectory(src/ipc)

# 进程管理：进程调度（依赖所有模块）
add_subdirectory(src/process)

# 内核核心：主内核逻辑（依赖所有模块）
add_subdirectory(src/kernel)

# 启动模块：启动代码（独立）
add_subdirectory(src/boot)

# === 创建最终内核可执行文件 ===
add_executable(moss.elf)

# 链接所有模块库
target_link_libraries(
  moss.elf
  PRIVATE moss_std
          moss_boot
          moss_kernel
          moss_containers
          moss_mm
          moss_drivers
          moss_interrupts
          moss_ipc
          moss_process
          )

# 架构特定链接配置 - 统一使用lld链接器
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
  set_target_properties(
    moss.elf PROPERTIES LINK_FLAGS "-fuse-ld=lld -nostdlib -nostartfiles")
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
  set_target_properties(
    moss.elf PROPERTIES LINK_FLAGS "-fuse-ld=lld -nostdlib -nostartfiles")
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
  set_target_properties(
    moss.elf PROPERTIES LINK_FLAGS "-fuse-ld=lld -nostdlib -nostartfiles")
endif()

# 设置输出目录
set_target_properties(moss.elf PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                          ${CMAKE_BINARY_DIR}/bin)

# 查找 LLVM 工具链
find_program(LLVM_OBJDUMP llvm-objdump)
find_program(LLVM_OBJCOPY llvm-objcopy)

if(LLVM_OBJDUMP)
  set(OBJDUMP_CMD ${LLVM_OBJDUMP})
  message(STATUS "使用 LLVM objdump: ${LLVM_OBJDUMP}")
else()
  message(FATAL_ERROR "未找到 llvm-objdump 工具，请安装 LLVM 工具链以生成反汇编文件")
endif()

if(LLVM_OBJCOPY)
  set(OBJCOPY_CMD ${LLVM_OBJCOPY})
  message(STATUS "使用 LLVM objcopy: ${LLVM_OBJCOPY}")
else()
  message(FATAL_ERROR "未找到 llvm-objcopy 工具，请安装 LLVM 工具链以生成二进制镜像")
endif()
# 生成反汇编、符号表和二进制文件 注意：llvm-objdump 和 llvm-objcopy 都是必需的，缺失任一个都会导致构建失败
add_custom_command(
  TARGET moss.elf
  POST_BUILD
  COMMAND ${OBJDUMP_CMD} -Cd $<TARGET_FILE:moss.elf> > moss.dis
  COMMAND ${OBJDUMP_CMD} -Ct $<TARGET_FILE:moss.elf> | sort > moss.sym
  COMMAND ${OBJCOPY_CMD} -O binary $<TARGET_FILE:moss.elf> moss.bin
  COMMAND ${OBJCOPY_CMD} -O binary -j .text.boot $<TARGET_FILE:moss.elf> moss_boot.bin
  COMMAND ${OBJCOPY_CMD} -O binary -j .text.boot -j .text -j .rodata -j .data -j
          .bss $<TARGET_FILE:moss.elf> moss_code.bin
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT
    "Generating disassembly, symbol table, and binary images for ${MOSS_TARGET_ARCH}"
)
message(STATUS "将生成文件: moss.dis, moss.sym, moss.bin, moss_boot.bin, moss_code.bin")

# === 构建目标 ===
# 测试目标 - 在 QEMU 中运行内核
if(EXISTS "${CMAKE_BINARY_DIR}/run_qemu.sh")
  add_custom_target(
    test
    COMMAND bash ${CMAKE_BINARY_DIR}/run_qemu.sh
    DEPENDS moss.elf
    COMMENT "Running moss in QEMU (${MOSS_TARGET_ARCH}/${MOSS_TARGET_PLATFORM})"
  )

  # 调试目标 - 在 QEMU 中启动 GDB 调试
  add_custom_target(
    debug
    COMMAND bash ${CMAKE_BINARY_DIR}/run_qemu.sh --debug
    DEPENDS moss.elf
    COMMENT "Running moss in QEMU with GDB server (${MOSS_TARGET_ARCH})")
else()
  message(WARNING "QEMU 运行脚本未生成，test 和 debug 目标不可用")
endif()

# 清理目标
add_custom_target(
  clean-all
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}
  COMMENT "Cleaning all build files")

# === 配置摘要 ===
moss_print_config_summary()

message(STATUS "=== 模块化构建配置完成 ===")
message(
  STATUS
    "已启用的模块: containers, mm, drivers, interrupts, ipc, process, kernel, boot")
message(STATUS "主可执行文件: moss.elf")
message(STATUS "===============================")
