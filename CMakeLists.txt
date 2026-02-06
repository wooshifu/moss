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

# 编译器标志现已迁移到 CMakePresets.json 中各架构的预设配置里
# 这样可以实现更精细的架构特定控制和更好的组织结构

# 启用汇编语言支持
enable_language(ASM)

# === 项目目录结构 ===
include_directories(
    ${CMAKE_SOURCE_DIR}/kernel/include
    ${CMAKE_SOURCE_DIR}/boot/include
)

# 如果存在 HAL 层，添加包含路径
if(EXISTS "${CMAKE_SOURCE_DIR}/kernel/hal/include")
    include_directories(${CMAKE_SOURCE_DIR}/kernel/hal/include)
endif()

# === 源文件收集 ===
# 平台特定的启动代码
set(BOOT_SOURCES "")
if(DEFINED MOSS_PLATFORM_SOURCES AND MOSS_PLATFORM_SOURCES)
    # 使用平台特定的源文件
    set(BOOT_SOURCES ${MOSS_PLATFORM_SOURCES})
    message(STATUS "使用平台特定启动代码: ${MOSS_PLATFORM_SOURCES}")
else()
    # 只有 ARM64 架构使用启动代码 (其他架构暂时跳过)
    if(MOSS_TARGET_ARCH STREQUAL "ARM64")
        if(EXISTS "${CMAKE_SOURCE_DIR}/boot/start.S")
            list(APPEND BOOT_SOURCES "boot/start.S")
        endif()
        if(EXISTS "${CMAKE_SOURCE_DIR}/boot/early_init.cpp")
            list(APPEND BOOT_SOURCES "boot/early_init.cpp")
        endif()
        message(STATUS "使用默认ARM64启动代码: ${BOOT_SOURCES}")
    else()
        message(STATUS "架构 ${MOSS_TARGET_ARCH} 暂时跳过启动代码，只编译内核核心")
    endif()
endif()

# 内核核心源文件 (架构无关的文件)
set(KERNEL_SOURCES "")

# 添加架构特定的内核源文件
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    # ARM64 特定的内核源文件
    list(APPEND KERNEL_SOURCES
        kernel/mm/page_table.cpp
    )
    message(STATUS "添加 ARM64 特定内核源文件")
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    # x86_64 暂时没有特定的内核源文件
    message(STATUS "X86_64 架构暂时跳过架构特定内核代码")
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    # RISC-V 暂时没有特定的内核源文件
    message(STATUS "RISC-V 架构暂时跳过架构特定内核代码")
endif()

# TODO: 后续添加更多内核源文件
# file(GLOB_RECURSE KERNEL_SOURCES
#     "kernel/*.cpp"
#     "kernel/*.hpp"
# )

# 检查是否有足够的源文件来创建可执行文件
if(BOOT_SOURCES OR KERNEL_SOURCES)
    # 创建内核可执行文件
    add_executable(moss.elf
        ${BOOT_SOURCES}
        ${KERNEL_SOURCES}
    )
    message(STATUS "创建 moss.elf 可执行文件，包含源文件: ${BOOT_SOURCES} ${KERNEL_SOURCES}")
else()
    # 创建一个简单的占位符可执行文件用于测试
    file(WRITE "${CMAKE_BINARY_DIR}/placeholder.cpp"
        "// 占位符文件用于 ${MOSS_TARGET_ARCH} 架构\n"
        "int main() { return 0; }\n"
    )
    add_executable(moss.elf "${CMAKE_BINARY_DIR}/placeholder.cpp")
    message(STATUS "创建占位符 moss.elf (${MOSS_TARGET_ARCH} 架构特定代码暂未实现)")
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
# 启用测试功能
enable_testing()

# 测试目标 - 在 QEMU 中运行内核
if(EXISTS "${CMAKE_BINARY_DIR}/run_qemu.sh")
    # 标准的CMake测试，支持testPresets的timeout功能
    add_test(NAME qemu_test
        COMMAND bash ${CMAKE_BINARY_DIR}/run_qemu.sh
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
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