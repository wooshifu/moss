cmake_minimum_required(VERSION 3.31)

project(
  moss
  VERSION 0.9.9
  LANGUAGES C CXX ASM)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)

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

# === 通用内核编译标志 ===
# freestanding 环境标志（所有架构通用）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffreestanding -fno-exceptions -fno-rtti")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-stack-protector -fno-pic -fno-pie")

# === Clang -Weverything 终极严格模式 ===
set(MOSS_WARNING_SUPPRESSIONS
    # C++ 兼容性警告（项目专门使用 C++26）
    -Wno-c++-compat -Wno-c++98-compat -Wno-c++98-compat-pedantic
    -Wno-c++11-compat -Wno-c++14-compat -Wno-c++17-compat -Wno-c++20-compat
    # 不适用于内核开发的警告
    -Wno-padded -Wno-unsafe-buffer-usage
    # GNU/Microsoft 扩展警告
    -Wno-gnu-zero-variadic-macro-arguments -Wno-gnu-anonymous-struct -Wno-nested-anon-types
    # 内核环境中常见但可接受的警告
    -Wno-missing-prototypes -Wno-missing-variable-declarations
    -Wno-global-constructors -Wno-exit-time-destructors -Wno-weak-vtables
    -Wno-inline
    # 宏和预处理器警告
    -Wno-reserved-identifier -Wno-reserved-macro-identifier
    # Switch 和 NRVO 相关
    -Wno-covered-switch-default -Wno-nrvo)
string(JOIN " " MOSS_WARNING_STR ${MOSS_WARNING_SUPPRESSIONS})
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Weverything -Werror ${MOSS_WARNING_STR}")

# 优化级别设置
set(CMAKE_CXX_FLAGS_DEBUG "-Og -g3")
set(CMAKE_CXX_FLAGS_RELEASE "-g0 -O3 -DNDEBUG")

# === 全局编译定义 ===
add_compile_definitions(MOSS_KERNEL_MODULE)

# === 全局头文件路径设置 ===
# moss_core 的 PUBLIC include 通过 target_link_libraries 传播到各模块

# === 配置系统生成 ===
string(TIMESTAMP MOSS_BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S UTC" UTC)

# 实用函数：将CMake布尔值转换为1/0数值
function(moss_bool_to_int var)
  if(${${var}})
    set(${var} 1 PARENT_SCOPE)
  else()
    set(${var} 0 PARENT_SCOPE)
  endif()
endfunction()

# 设置架构特定的配置变量（MOSS_ARCH_NAME 由 cmake/arch/*.cmake 设置）
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
  set(MOSS_ARCH_ARM64 ON)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
  set(MOSS_ARCH_X86_64 ON)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
  set(MOSS_ARCH_RISCV ON)
else()
  message(FATAL_ERROR "Unsupported architecture: ${MOSS_TARGET_ARCH}")
endif()

# 所有 64 位架构的公共配置值
set(MOSS_PAGE_SIZE 4096)
set(MOSS_CACHE_LINE_SIZE 64)
set(MOSS_POINTER_SIZE 8)

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

# 将所有标准模块选项转换为1/0数值（用于config.h模板）
foreach(_opt
    MOSS_ENABLE_DRIVERS MOSS_ENABLE_INTERRUPTS MOSS_ENABLE_IPC MOSS_ENABLE_PROCESS
    MOSS_ENABLE_LOCKFREE_QUEUE MOSS_ENABLE_SLAB_ALLOCATOR MOSS_ENABLE_RCU_LIST
    MOSS_ENABLE_PER_CPU_DATA MOSS_ENABLE_ATOMIC_TYPES MOSS_ENABLE_CFS_SCHEDULER
    MOSS_ENABLE_LOAD_BALANCER MOSS_ENABLE_PROCESS_TREE MOSS_ENABLE_SHARED_MEMORY
    MOSS_ENABLE_ZERO_COPY_CHANNEL MOSS_ENABLE_IPC_MANAGER MOSS_ENABLE_UART_DRIVER
    MOSS_ENABLE_DEVICE_MANAGER MOSS_ENABLE_GIC)
  moss_bool_to_int(${_opt})
endforeach()

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

# 转换条件调试选项为数值（未定义则默认为0）
set(_MOSS_CONDITIONAL_OPTIONS
    MOSS_ENABLE_MEMORY_DEBUG MOSS_ENABLE_STACK_OVERFLOW_CHECK
    MOSS_ENABLE_HEAP_CORRUPTION_CHECK MOSS_ENABLE_DOUBLE_FREE_CHECK
    MOSS_ENABLE_ASSERTIONS MOSS_ENABLE_DATA_STRUCTURE_CHECK
    MOSS_ENABLE_FUNCTION_TRACING MOSS_ENABLE_LOCK_PROFILING
    MOSS_ENABLE_SCHEDULER_STATS MOSS_ENABLE_SYSCALL_TRACING)
foreach(_opt IN LISTS _MOSS_CONDITIONAL_OPTIONS)
  if(DEFINED ${_opt})
    moss_bool_to_int(${_opt})
  else()
    set(${_opt} 0)
  endif()
endforeach()

# 生成配置头文件
configure_file(${CMAKE_SOURCE_DIR}/src/core/config/config.h.in
               ${CMAKE_BINARY_DIR}/include/config/config.h @ONLY)

# 添加生成的配置头文件目录到包含路径
include_directories(${CMAKE_BINARY_DIR}/include)

message(STATUS "配置系统已生成: ${CMAKE_BINARY_DIR}/include/config/config.h")
message(STATUS "目标架构: ${MOSS_ARCH_NAME}")
message(STATUS "目标平台: ${MOSS_PLATFORM_NAME}")
message(STATUS "构建类型: ${MOSS_BUILD_TYPE} (调试级别: ${MOSS_DEBUG_LEVEL})")

# === 添加子模块（按依赖顺序）===
add_subdirectory(src/core)
add_subdirectory(src/containers)
add_subdirectory(src/mm)
add_subdirectory(src/drivers)
add_subdirectory(src/interrupts)
add_subdirectory(src/ipc)
add_subdirectory(src/process)
add_subdirectory(src/kernel)
add_subdirectory(src/boot)

# === 创建最终内核可执行文件 ===
add_executable(moss.elf)

target_link_libraries(
  moss.elf
  PRIVATE moss_std
          moss_core
          moss_boot
          moss_kernel
          moss_containers
          moss_mm
          moss_drivers
          moss_interrupts
          moss_ipc
          moss_process
          )

# 链接配置 - 统一使用lld链接器
set_target_properties(
  moss.elf PROPERTIES LINK_FLAGS "-fuse-ld=lld -nostdlib -nostartfiles")

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

# 生成反汇编、符号表和二进制文件
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
if(EXISTS "${CMAKE_BINARY_DIR}/run_qemu.sh")
  add_custom_target(
    run-qemu
    COMMAND bash ${CMAKE_BINARY_DIR}/run_qemu.sh
    DEPENDS moss.elf
    COMMENT "Running moss in QEMU (${MOSS_TARGET_ARCH}/${MOSS_TARGET_PLATFORM})"
  )

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

# === 单元测试支持 (独立的 moss.test.elf) ===
option(MOSS_BUILD_TESTS "构建MOSS内核单元测试" ON)

if(MOSS_BUILD_TESTS)
  message(STATUS "启用单元测试构建 (独立 moss.test.elf)")
  add_subdirectory(src/test)

  enable_testing()
  add_test(
    NAME moss-unit-tests
    COMMAND bash ${CMAKE_BINARY_DIR}/run_qemu.sh --test
  )
  set_tests_properties(moss-unit-tests PROPERTIES TIMEOUT 30)

  add_custom_target(
    test-kernel
    COMMAND bash ${CMAKE_BINARY_DIR}/run_qemu.sh --test
    DEPENDS moss.test.elf
    COMMENT "Running MOSS unit tests in QEMU (${MOSS_TARGET_ARCH})"
  )
else()
  message(STATUS "跳过单元测试构建")
endif()

# === 配置摘要 ===
moss_print_config_summary()

message(STATUS "=== 模块化构建配置完成 ===")
message(
  STATUS
    "已启用的模块: containers, mm, drivers, interrupts, ipc, process, kernel, boot")
message(STATUS "主可执行文件: moss.elf")
message(STATUS "===============================")
