cmake_minimum_required(VERSION 3.31)

project(
    moss
    VERSION 0.9.9
    LANGUAGES C CXX ASM
)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)

# Configure C++26 standard with strict settings
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 包含多架构支持模块
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(arch_support)
include(ensure_prerequisites)
include(toolchain_detection)

# === 多架构配置 ===
message(STATUS "开始配置 Moss 内核构建系统...")

# Bootstrap: ensure uv is available, then run full prerequisite check
moss_ensure_uv()
moss_check_prerequisites(${MOSS_TARGET_ARCH})

# 初始化 Clang 工具链
moss_initialize_clang_toolchain(${MOSS_TARGET_ARCH})

# Validate target architecture
if(NOT MOSS_TARGET_ARCH)
    message(FATAL_ERROR "MOSS_TARGET_ARCH must be set. Supported: ARM64, X86_64, RISCV")
endif()
set(_SUPPORTED_ARCHITECTURES "ARM64" "X86_64" "RISCV")
if(NOT MOSS_TARGET_ARCH IN_LIST _SUPPORTED_ARCHITECTURES)
    message(FATAL_ERROR "Unsupported architecture: ${MOSS_TARGET_ARCH}. Supported: ${_SUPPORTED_ARCHITECTURES}")
endif()

# === QEMU 运行环境配置 ===
if(NOT MOSS_CPU_CORES)
    set(MOSS_CPU_CORES 8)
endif()

# 根据架构选择 QEMU 命令名
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    set(_QEMU_CMD "qemu-system-aarch64")
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    set(_QEMU_CMD "qemu-system-x86_64")
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    set(_QEMU_CMD "qemu-system-riscv64")
endif()

# 查找 QEMU 可执行文件（Windows 上通常不在 PATH 中）
set(_QEMU_HINTS "")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND _QEMU_HINTS "C:/Program Files/qemu" "$ENV{LOCALAPPDATA}/Programs/qemu")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND _QEMU_HINTS "/opt/homebrew/bin" "/usr/local/bin")
endif()

find_program(QEMU_EXECUTABLE ${_QEMU_CMD} HINTS ${_QEMU_HINTS})
if(QEMU_EXECUTABLE)
    set(MOSS_QEMU_PATH "${QEMU_EXECUTABLE}")
    message(STATUS "Found QEMU: ${QEMU_EXECUTABLE}")
else()
    set(MOSS_QEMU_PATH "${_QEMU_CMD}")
    message(STATUS "${_QEMU_CMD} not found, will rely on PATH at runtime")
endif()

# 生成 QEMU 配置文件和运行脚本
configure_file("${CMAKE_SOURCE_DIR}/scripts/qemu_config.json.in" "${CMAKE_BINARY_DIR}/qemu_config.json" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/scripts/run_qemu.sh.in" "${CMAKE_BINARY_DIR}/run_qemu.sh" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/scripts/run_qemu.ps1.in" "${CMAKE_BINARY_DIR}/run_qemu.ps1" @ONLY)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    file(
        CHMOD
        "${CMAKE_BINARY_DIR}/run_qemu.sh"
        PERMISSIONS
        OWNER_READ
        OWNER_WRITE
        OWNER_EXECUTE
        GROUP_READ
        GROUP_EXECUTE
        WORLD_READ
        WORLD_EXECUTE
    )
endif()

# 按主机平台选择 CMake 目标使用的 runner
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(MOSS_QEMU_RUNNER "powershell.exe")
    set(MOSS_QEMU_RUNNER_ARGS "-NoProfile;-ExecutionPolicy;Bypass;-File;${CMAKE_BINARY_DIR}/run_qemu.ps1")
else()
    set(MOSS_QEMU_RUNNER "bash")
    set(MOSS_QEMU_RUNNER_ARGS "${CMAKE_BINARY_DIR}/run_qemu.sh")
endif()

# === 通用内核编译标志 ===
# freestanding 环境标志（所有架构通用）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffreestanding -fno-exceptions -fno-rtti")
# 内核开发编译器标志配置
#
# -fno-stack-protector: 禁用栈保护机制 原因: 内核代码运行在特权模式下,栈保护需要额外的运行时支持(如__stack_chk_fail函数) 在内核初始化阶段这些支持可能尚未就绪,且内核有自己的安全机制
#
# -fno-pic: 禁用位置无关代码(Position Independent Code) 原因: 内核代码运行在固定的虚拟地址空间,不需要重定位 PIC会引入额外的间接寻址开销,影响内核性能
# 内核使用自己的加载和链接机制,不需要动态链接器支持
#
# -fno-pie: 禁用位置无关可执行文件(Position Independent Executable) 原因: 内核不是普通的用户空间可执行文件,有固定的加载地址
# PIE是为了实现ASLR(地址空间布局随机化),但内核有自己的KASLR机制 避免生成不必要的重定位信息,减小内核镜像大小
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-stack-protector -fno-pic -fno-pie")

# === Clang -Weverything 终极严格模式 ===
set(MOSS_WARNING_SUPPRESSIONS
    # C++ 兼容性警告（项目专门使用 C++26）
    -Wno-c++-compat
    -Wno-c++98-compat
    -Wno-c++98-compat-pedantic
    -Wno-c++11-compat
    -Wno-c++14-compat
    -Wno-c++17-compat
    -Wno-c++20-compat
    # 不适用于内核开发的警告
    -Wno-padded
    -Wno-unsafe-buffer-usage
    # GNU/Microsoft 扩展警告
    -Wno-gnu-zero-variadic-macro-arguments
    -Wno-gnu-anonymous-struct
    -Wno-nested-anon-types
    # 内核环境中常见但可接受的警告
    -Wno-missing-prototypes
    -Wno-missing-variable-declarations
    -Wno-global-constructors
    -Wno-exit-time-destructors
    -Wno-weak-vtables
    -Wno-inline
    # 宏和预处理器警告
    -Wno-reserved-identifier
    -Wno-reserved-macro-identifier
    # Switch 和 NRVO 相关
    -Wno-covered-switch-default
    -Wno-nrvo
)
string(JOIN " " MOSS_WARNING_STR ${MOSS_WARNING_SUPPRESSIONS})
# Compiler flags configuration for strict code quality enforcement -Weverything: Enables ALL available compiler warnings
# to ensure maximum code quality and maintainability. This is especially critical for kernel-level development where
# bugs can have severe system-wide consequences. -Werror: Treats all warnings as errors, enforcing zero-tolerance policy
# for potential issues during the build process.
#
# Note: The -Weverything flag is used to maintain the rigor required for kernel code development, where reliability,
# safety, and correctness are paramount.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Weverything -Werror ${MOSS_WARNING_STR}")

# 优化级别设置
set(CMAKE_CXX_FLAGS_DEBUG "-Og -g3")
set(CMAKE_CXX_FLAGS_RELEASE "-g0 -O3 -DNDEBUG")

# 汇编文件调试信息：生成 DWARF .debug_line 行号表， 使 GDB 的 next/step 命令能在汇编代码中正常工作。 （.type/.size 只提供 ELF 符号边界，行号表才是 GDB 单步调试的依据）
set(CMAKE_ASM_FLAGS_DEBUG "-g3")
set(CMAKE_ASM_FLAGS_RELEASE "-g0")

# === 架构特定配置 ===
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    add_compile_definitions(MOSS_ARCH_ARM64=1)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    add_compile_definitions(MOSS_ARCH_X86_64=1)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    add_compile_definitions(MOSS_ARCH_RISCV=1)
    # RISC-V ISA extension feature macros (derived from CMAKE_CXX_FLAGS -march=)
    if(CMAKE_CXX_FLAGS MATCHES "-march=rv64[^ ]*m")
        add_compile_definitions(MOSS_RISCV_M_EXT=1)
    endif()
    if(CMAKE_CXX_FLAGS MATCHES "-march=rv64[^ ]*a")
        add_compile_definitions(MOSS_RISCV_A_EXT=1)
    endif()
    if(CMAKE_CXX_FLAGS MATCHES "-march=rv64[^ ]*c")
        add_compile_definitions(MOSS_RISCV_C_EXT=1)
    endif()
    if(CMAKE_CXX_FLAGS MATCHES "-march=rv64[^ ]*f")
        add_compile_definitions(MOSS_RISCV_F_EXT=1)
    endif()
    if(CMAKE_CXX_FLAGS MATCHES "-march=rv64[^ ]*d")
        add_compile_definitions(MOSS_RISCV_D_EXT=1)
    endif()
else()
    message(FATAL_ERROR "Unsupported architecture: ${MOSS_TARGET_ARCH}")
endif()

# === 添加子模块（按依赖顺序）===
add_subdirectory(src/core)
add_subdirectory(src/abi)
add_subdirectory(src/aal)
add_subdirectory(src/platform)
add_subdirectory(src/hal)
add_subdirectory(src/fdt)
add_subdirectory(src/initramfs)
add_subdirectory(src/containers)
add_subdirectory(src/mm)
add_subdirectory(src/drivers)
add_subdirectory(src/interrupts)
add_subdirectory(src/ipc)
add_subdirectory(src/logging)
add_subdirectory(src/timer)
add_subdirectory(src/process)
add_subdirectory(src/vfs)
add_subdirectory(src/kernel)
add_subdirectory(src/boot)

# === 创建最终内核可执行文件 ===
add_executable(moss.elf)

target_link_libraries(
    moss.elf
    PRIVATE moss_core
            moss_abi
            moss_aal
            moss_platform
            moss_hal_mmu
            moss_hal_intc
            moss_hal_uart
            moss_hal_timer
            moss_logging
            moss_fdt
            moss_initramfs
            moss_libfdt_c
            moss_boot
            moss_kernel
            moss_containers
            moss_mm
            moss_drivers
            moss_interrupts
            moss_ipc
            moss_timer
            moss_process
            moss_vfs
)

# 设置输出目录
set_target_properties(moss.elf PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# LLVM tool paths are set by moss_check_prerequisites() in ensure_prerequisites.cmake
set(OBJDUMP_CMD "${MOSS_LLVM_OBJDUMP}")
set(OBJCOPY_CMD "${MOSS_LLVM_OBJCOPY}")

# 生成反汇编、符号表和二进制文件
add_custom_command(
    TARGET moss.elf
    POST_BUILD
    COMMAND ${OBJDUMP_CMD} -Cd $<TARGET_FILE:moss.elf> > moss.dis
    COMMAND ${OBJDUMP_CMD} -Ct $<TARGET_FILE:moss.elf> | sort > moss.sym
    COMMAND ${OBJCOPY_CMD} -O binary $<TARGET_FILE:moss.elf> moss.bin
    COMMAND ${OBJCOPY_CMD} -O binary -j .text.boot $<TARGET_FILE:moss.elf> moss_boot.bin
    COMMAND ${OBJCOPY_CMD} -O binary -j .text.boot -j .text -j .rodata -j .data -j .bss $<TARGET_FILE:moss.elf>
            moss_code.bin
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generating disassembly, symbol table, and binary images for ${MOSS_TARGET_ARCH}"
)

# Verify ARM64 Linux Image header in moss.bin (ARM64 only) The header is embedded in start_arm64.S; moss.bin already IS
# the Linux Image.
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    add_custom_command(
        TARGET moss.elf
        POST_BUILD
        COMMAND uv run ${CMAKE_SOURCE_DIR}/scripts/verify_linux_image.py ${CMAKE_BINARY_DIR}/moss.bin
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Verifying ARM64 Linux Image header in moss.bin"
    )
endif()
message(STATUS "将生成文件: moss.dis, moss.sym, moss.bin, moss_boot.bin, moss_code.bin")

# === Userspace programs + initramfs CPIO (ARM64 only) ===
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    set(USERSPACE_DIR "${CMAKE_SOURCE_DIR}/userspace")
    set(USERSPACE_LD "${USERSPACE_DIR}/userspace.ld")

    # Common compiler flags for userspace programs
    set(USERSPACE_CFLAGS
        --target=aarch64-unknown-elf
        -ffreestanding
        -fno-stack-protector
        -nostdlib
        -nostartfiles
        -fuse-ld=lld
        -O2
        -Wall
        -I${USERSPACE_DIR}
        -Wl,-T,${USERSPACE_LD}
    )

    # Build hello.elf userspace program
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/userspace/hello.elf
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/userspace
        COMMAND ${CMAKE_C_COMPILER} ${USERSPACE_CFLAGS} -o ${CMAKE_BINARY_DIR}/userspace/hello.elf
                ${USERSPACE_DIR}/hello.c
        DEPENDS ${USERSPACE_DIR}/hello.c ${USERSPACE_DIR}/syscall.h ${USERSPACE_LD}
        COMMENT "Building userspace hello.elf"
    )

    # Build shell.elf interactive shell
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/userspace/shell.elf
        COMMAND ${CMAKE_C_COMPILER} ${USERSPACE_CFLAGS} -o ${CMAKE_BINARY_DIR}/userspace/shell.elf
                ${USERSPACE_DIR}/shell.c
        DEPENDS ${USERSPACE_DIR}/shell.c ${USERSPACE_DIR}/syscall.h ${USERSPACE_LD}
        COMMENT "Building userspace shell.elf"
    )

    # Build top.elf system monitoring program
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/userspace/top.elf
        COMMAND ${CMAKE_C_COMPILER} ${USERSPACE_CFLAGS} -o ${CMAKE_BINARY_DIR}/userspace/top.elf ${USERSPACE_DIR}/top.c
        DEPENDS ${USERSPACE_DIR}/top.c ${USERSPACE_DIR}/syscall.h ${USERSPACE_LD}
        COMMENT "Building userspace top.elf"
    )

    # Generate initramfs.cpio from all userspace programs Uses GNU cpio "newc" format (070701 magic, required by the
    # CPIO parser)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/initramfs.cpio
        COMMAND uv run ${CMAKE_SOURCE_DIR}/scripts/gen_initramfs.py ${CMAKE_BINARY_DIR}/initramfs.cpio
                ${CMAKE_BINARY_DIR}/userspace hello.elf shell.elf top.elf
        DEPENDS ${CMAKE_BINARY_DIR}/userspace/hello.elf ${CMAKE_BINARY_DIR}/userspace/shell.elf
                ${CMAKE_BINARY_DIR}/userspace/top.elf
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Generating initramfs.cpio"
    )

    add_custom_target(initramfs ALL DEPENDS ${CMAKE_BINARY_DIR}/initramfs.cpio)
    add_dependencies(moss.elf initramfs)

    message(STATUS "Userspace + initramfs configured (ARM64)")
endif()

# === 构建目标 (QEMU) ===
if(MOSS_QEMU_RUNNER)
    add_custom_target(
        run-qemu
        COMMAND ${MOSS_QEMU_RUNNER} ${MOSS_QEMU_RUNNER_ARGS}
        DEPENDS moss.elf
        COMMENT "Running moss in QEMU (${MOSS_TARGET_ARCH}/${MOSS_TARGET_PLATFORM})"
    )

    add_custom_target(
        debug
        COMMAND ${MOSS_QEMU_RUNNER} ${MOSS_QEMU_RUNNER_ARGS} --debug
        DEPENDS moss.elf
        COMMENT "Running moss in QEMU with GDB server (${MOSS_TARGET_ARCH})"
    )
else()
    message(STATUS "QEMU 运行脚本未生成，run-qemu 和 debug 目标不可用")
endif()

# 清理目标
add_custom_target(
    clean-all
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}
    COMMENT "Cleaning all build files"
)

# === 单元测试支持 (独立的 moss.test.elf) ===
option(MOSS_BUILD_TESTS "构建MOSS内核单元测试" ON)

if(MOSS_BUILD_TESTS)
    message(STATUS "启用单元测试构建 (独立 moss.test.elf)")
    add_subdirectory(src/test)

    if(MOSS_QEMU_RUNNER)
        enable_testing()
        add_test(NAME moss-unit-tests COMMAND ${MOSS_QEMU_RUNNER} ${MOSS_QEMU_RUNNER_ARGS} --test)
        set_tests_properties(moss-unit-tests PROPERTIES TIMEOUT 30)

        add_custom_target(
            test-kernel
            COMMAND ${MOSS_QEMU_RUNNER} ${MOSS_QEMU_RUNNER_ARGS} --test
            DEPENDS moss.test.elf
            COMMENT "Running MOSS unit tests in QEMU (${MOSS_TARGET_ARCH})"
        )
    else()
        message(STATUS "QEMU 运行脚本未配置，test-kernel 目标不可用")
    endif()
else()
    message(STATUS "跳过单元测试构建")
endif()

# === 配置摘要 ===
moss_print_config_summary()

message(STATUS "=== 模块化构建配置完成 ===")
message(STATUS "主可执行文件: moss.elf")
message(STATUS "===============================")
