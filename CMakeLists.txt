cmake_minimum_required(VERSION 3.25)

# === Moss 微内核多架构构建系统 ===
project(moss VERSION 0.1.0 LANGUAGES CXX ASM)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 包含多架构支持模块
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(ArchSupport)
include(ToolchainDetection)

# === 多架构配置 ===
message(STATUS "开始配置 Moss 内核构建系统...")

# 初始化 Clang 工具链
moss_initialize_clang_toolchain(${MOSS_TARGET_ARCH})

# 配置目标架构
moss_configure_architecture()

# 配置目标平台
moss_configure_platform()

# 设置通用内核编译标志
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffreestanding -fno-exceptions -fno-rtti")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-stack-protector")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror")

# 优化级别设置
set(CMAKE_CXX_FLAGS_DEBUG "-Og -g3")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# 启用汇编语言支持
enable_language(ASM)

# === 全局头文件路径设置 ===
# 设置公共接口头文件路径
include_directories(${CMAKE_SOURCE_DIR}/src/include)

# === 配置系统生成 ===
# 获取当前时间戳用于构建信息
string(TIMESTAMP MOSS_BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S UTC" UTC)

# 实用函数：将CMake布尔值转换为1/0数值
function(moss_bool_to_int var)
    if(${${var}})
        set(${var} 1 PARENT_SCOPE)
    else()
        set(${var} 0 PARENT_SCOPE)
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
    option(MOSS_ENABLE_DATA_STRUCTURE_CHECK "Enable data structure consistency checks" ON)
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
configure_file(
    ${CMAKE_SOURCE_DIR}/src/include/config/config.h.in
    ${CMAKE_BINARY_DIR}/include/config/config.h
    @ONLY
)

# 添加生成的配置头文件目录到包含路径
include_directories(${CMAKE_BINARY_DIR}/include)

message(STATUS "配置系统已生成: ${CMAKE_BINARY_DIR}/include/config/config.h")
message(STATUS "目标架构: ${MOSS_ARCH_NAME}")
message(STATUS "目标平台: ${MOSS_PLATFORM_NAME}")
message(STATUS "构建类型: ${MOSS_BUILD_TYPE} (调试级别: ${MOSS_DEBUG_LEVEL})")

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
target_link_libraries(moss.elf PRIVATE
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
    set_target_properties(moss.elf PROPERTIES
        LINK_FLAGS "-fuse-ld=lld -nostdlib -nostartfiles"
    )
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    set_target_properties(moss.elf PROPERTIES
        LINK_FLAGS "-fuse-ld=lld -nostdlib -nostartfiles"
    )
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    set_target_properties(moss.elf PROPERTIES
        LINK_FLAGS "-fuse-ld=lld -nostdlib -nostartfiles"
    )
endif()

# 设置输出目录
set_target_properties(moss.elf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# === 后处理和工具 ===
# 生成反汇编文件（调试用）
set(OBJDUMP_CMD "")

# 查找 LLVM objdump 作为首选
find_program(LLVM_OBJDUMP llvm-objdump)
if(LLVM_OBJDUMP)
    set(OBJDUMP_CMD ${LLVM_OBJDUMP})
    message(STATUS "使用 LLVM objdump: ${LLVM_OBJDUMP}")
else()
    # 查找架构特定的 objdump
    if(MOSS_TARGET_ARCH STREQUAL "ARM64")
        find_program(ARCH_OBJDUMP aarch64-linux-gnu-objdump)
    elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
        find_program(ARCH_OBJDUMP objdump)
    elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
        find_program(ARCH_OBJDUMP riscv64-linux-gnu-objdump)
    endif()

    if(ARCH_OBJDUMP)
        set(OBJDUMP_CMD ${ARCH_OBJDUMP})
        message(STATUS "使用架构特定 objdump: ${ARCH_OBJDUMP}")
    endif()
endif()

# 只有找到 objdump 工具时才生成反汇编
if(OBJDUMP_CMD)
    add_custom_command(TARGET moss.elf POST_BUILD
        COMMAND ${OBJDUMP_CMD} -d $<TARGET_FILE:moss.elf> > moss.dis
        COMMAND ${OBJDUMP_CMD} -t $<TARGET_FILE:moss.elf> | sort > moss.sym
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Generating disassembly and symbol table for ${MOSS_TARGET_ARCH}"
    )
else()
    message(STATUS "未找到 objdump 工具，跳过反汇编文件生成")
endif()

# === 构建目标 ===
# 测试目标 - 在 QEMU 中运行内核
if(EXISTS "${CMAKE_BINARY_DIR}/run_qemu.sh")
    add_custom_target(test
        COMMAND bash ${CMAKE_BINARY_DIR}/run_qemu.sh
        DEPENDS moss.elf
        COMMENT "Running moss in QEMU (${MOSS_TARGET_ARCH}/${MOSS_TARGET_PLATFORM})"
    )

    # 调试目标 - 在 QEMU 中启动 GDB 调试
    add_custom_target(debug
        COMMAND bash ${CMAKE_BINARY_DIR}/run_qemu.sh --debug
        DEPENDS moss.elf
        COMMENT "Running moss in QEMU with GDB server (${MOSS_TARGET_ARCH})"
    )
else()
    message(WARNING "QEMU 运行脚本未生成，test 和 debug 目标不可用")
endif()

# 清理目标
add_custom_target(clean-all
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}
    COMMENT "Cleaning all build files"
)

# === 配置摘要 ===
moss_print_config_summary()

message(STATUS "=== 模块化构建配置完成 ===")
message(STATUS "已启用的模块: containers, mm, drivers, interrupts, ipc, process, kernel, boot")
message(STATUS "主可执行文件: moss.elf")
message(STATUS "===============================")