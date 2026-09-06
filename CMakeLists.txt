cmake_minimum_required(VERSION 3.31)

if(NOT MOSS_CONFIGURED_BY_PRESET)
    message(FATAL_ERROR "Moss must be configured with a CMake preset. Run: cmake --preset <name>")
endif()

set(_SUPPORTED_BUILD_TYPES "Debug" "Release" "RelWithDebInfo")
if(NOT CMAKE_BUILD_TYPE IN_LIST _SUPPORTED_BUILD_TYPES)
    message(FATAL_ERROR "Unsupported CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}. Supported: ${_SUPPORTED_BUILD_TYPES}")
endif()

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
# intrinsics 是零依赖的基础模块，必须最先编译
add_subdirectory(src/intrinsics)
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
string(TOLOWER "${MOSS_TARGET_ARCH}" _link_arch)
set(MOSS_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/src/linker/kernel_${_link_arch}.ld")
set_property(
    TARGET moss.elf
    APPEND
    PROPERTY LINK_DEPENDS "${MOSS_LINKER_SCRIPT}"
)

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

# Check the static boot header and every runtime relocation before handoff.
if(MOSS_TARGET_ARCH STREQUAL "ARM64" OR MOSS_TARGET_ARCH STREQUAL "RISCV")
    add_custom_command(
        TARGET moss.elf
        POST_BUILD
        COMMAND uv run ${CMAKE_SOURCE_DIR}/scripts/verify_linux_image.py ${CMAKE_BINARY_DIR}/moss.bin --elf
                $<TARGET_FILE:moss.elf>
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Verifying native Linux Image header and bootstrap relocations"
    )
endif()
message(STATUS "将生成文件: moss.dis, moss.sym, moss.bin, moss_boot.bin, moss_code.bin")

# === Userspace programs + initramfs CPIO ===
if(MOSS_TARGET_ARCH STREQUAL "ARM64"
   OR MOSS_TARGET_ARCH STREQUAL "RISCV"
   OR MOSS_TARGET_ARCH STREQUAL "X86_64"
)
    option(MOSS_BUILD_TESTS "构建MOSS内核单元测试" ON)
    add_subdirectory(src/userspace)
    add_dependencies(moss.elf initramfs)
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

    enable_testing()
    set(_validation
        uv
        run
        ${CMAKE_SOURCE_DIR}/scripts/kernel_validation.py
        run
        --manifest
        ${CMAKE_BINARY_DIR}/moss-artifacts.json
    )
    add_test(NAME moss-functional COMMAND ${_validation})
    add_test(NAME moss-framework COMMAND ${_validation} --selftest)
    set_tests_properties(
        moss-functional moss-framework
        PROPERTIES TIMEOUT
                   600 # Nine guests may each use 60 s, plus process cleanup/reporting.
                   RUN_SERIAL
                   TRUE
                   LABELS
                   "kernel"
                   WORKING_DIRECTORY
                   ${CMAKE_SOURCE_DIR}
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_test(NAME moss-benchmark COMMAND ${_validation} --benchmark)
        set_tests_properties(
            moss-benchmark
            PROPERTIES TIMEOUT
                       420
                       RUN_SERIAL
                       TRUE
                       LABELS
                       "benchmark"
                       WORKING_DIRECTORY
                       ${CMAKE_SOURCE_DIR}
        )
        add_custom_target(
            benchmark-kernel
            COMMAND ${_validation} --benchmark
            DEPENDS moss.test.elf
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
    endif()

    add_custom_target(
        test-kernel
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} -L kernel --output-on-failure
        DEPENDS moss.test.elf
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running MOSS kernel validation (${MOSS_TARGET_ARCH})"
    )
else()
    message(STATUS "跳过单元测试构建")
endif()

# Generic artifact metadata: no runner executable, machine or runtime settings.
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    set(_boot_protocol "linux-image")
    set(_kernel_image "moss.bin")
    set(_validation_image "moss.test.bin")
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    set(_boot_protocol "sbi")
    set(_kernel_image "moss.bin")
    set(_validation_image "moss.test.bin")
else()
    set(_boot_protocol "xen-pvh")
    set(_kernel_image "bin/moss.elf")
    set(_validation_image "bin/moss.test.elf")
endif()
if(MOSS_BUILD_TESTS)
    set(_validation_json "\"${_validation_image}\"")
    set(_validation_initrd_json "\"validation-initramfs.cpio\"")
else()
    set(_validation_json "null")
    set(_validation_initrd_json "null")
endif()
string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_config)
# JSON escaping also handles Windows paths and quoted compiler flags.
function(moss_json_string out value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "\n" "\\n" value "${value}")
    set(${out}
        "\"${value}\""
        PARENT_SCOPE
    )
endfunction()
moss_json_string(_compiler "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
moss_json_string(_compile_flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_build_config}}")
moss_json_string(_link_flags "${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_EXE_LINKER_FLAGS_${_build_config}}")
file(
    GENERATE
    OUTPUT "${CMAKE_BINARY_DIR}/moss-artifacts.json"
    CONTENT
        "{\n  \"schema_version\": 1,
  \"target\": { \"architecture\": \"${MOSS_TARGET_ARCH}\", \"boot_protocol\": \"${_boot_protocol}\" },
  \"build\": { \"type\": \"${CMAKE_BUILD_TYPE}\", \"compiler\": ${_compiler}, \"flags\": { \"compile\": ${_compile_flags}, \"link\": ${_link_flags} } },
  \"artifacts\": {
    \"kernel\": \"${_kernel_image}\", \"debug_symbols\": \"bin/moss.elf\",
    \"initramfs\": \"initramfs.cpio\",
    \"validation_kernel\": ${_validation_json}, \"validation_initramfs\": ${_validation_initrd_json}
  }\n}\n"
)

# === 配置摘要 ===
moss_print_config_summary()

message(STATUS "=== 模块化构建配置完成 ===")
message(STATUS "主可执行文件: moss.elf")
message(STATUS "===============================")
