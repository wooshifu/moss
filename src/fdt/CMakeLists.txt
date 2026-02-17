# src/fdt/CMakeLists.txt
# FDT (Flattened Device Tree) 解析模块
# 包含 vendored libfdt C 库和 C++26 模块封装

# === Vendored libfdt C 库 ===
# 来源: https://git.kernel.org/pub/scm/utils/dtc/dtc.git (libfdt)
# 以 OBJECT 库方式编译，使用 -w 抑制第三方 C 代码警告
add_library(moss_libfdt_c OBJECT
    libfdt/fdt.c
    libfdt/fdt_ro.c
    libfdt/fdt_strerror.c)

# 架构特定的 C 编译标志（与 CXX 预设保持一致）
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    set(LIBFDT_ARCH_FLAGS -mcpu=cortex-a57 -march=armv8-a -mgeneral-regs-only -mstrict-align)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    set(LIBFDT_ARCH_FLAGS "")
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    set(LIBFDT_ARCH_FLAGS -march=rv64imac_zicsr_zifencei -mabi=lp64)
endif()

# 使用与 C++ 相同的交叉编译 target（CMAKE_CXX_COMPILER_TARGET 由预设设置）
target_compile_options(moss_libfdt_c PRIVATE
    --target=${CMAKE_CXX_COMPILER_TARGET}
    ${LIBFDT_ARCH_FLAGS}
    -ffreestanding -fno-stack-protector -fno-pic -fno-pie
    -w)  # 抑制 vendored C 代码的所有警告

target_include_directories(moss_libfdt_c PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/libfdt)

# === C++26 模块封装 ===
add_library(moss_fdt OBJECT)

target_sources(moss_fdt
    PUBLIC FILE_SET CXX_MODULES FILES src/fdt.cppm)

# SYSTEM 标记确保 vendored C 头文件在 C++ 编译时不触发 -Weverything 警告
target_include_directories(moss_fdt SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/libfdt)

target_link_libraries(moss_fdt PRIVATE moss_core moss_libfdt_c)

message(STATUS "FDT 模块配置完成 (libfdt + moss.fdt)")
