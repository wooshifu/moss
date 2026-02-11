# ARM64 (AArch64) 架构配置 支持 Cortex-A 系列处理器

message(STATUS "配置 ARM64 (AArch64) 架构...")

# ARM64 架构信息
set(MOSS_ARCH_NAME "ARM64")
set(MOSS_ARCH_BITS "64")
set(MOSS_TARGET_TRIPLE "aarch64-unknown-elf")

# 默认 CPU 配置
if(NOT MOSS_CPU_TYPE)
  set(MOSS_CPU_TYPE "cortex-a57")
endif()

# 支持的 CPU 类型
set(SUPPORTED_ARM64_CPUS
    "cortex-a53"
    "cortex-a57"
    "cortex-a72"
    "cortex-a73"
    "cortex-a75"
    "cortex-a76"
    "cortex-a78"
    "cortex-x1"
    "generic")

# 验证 CPU 类型
if(NOT MOSS_CPU_TYPE IN_LIST SUPPORTED_ARM64_CPUS)
  message(WARNING "不支持的 ARM64 CPU 类型: ${MOSS_CPU_TYPE}，使用默认值: cortex-a57")
  set(MOSS_CPU_TYPE "cortex-a57")
endif()

message(STATUS "ARM64 目标 CPU: ${MOSS_CPU_TYPE}")

# ARM64 特定编译标志
set(ARM64_SPECIFIC_FLAGS
    "-mcpu=${MOSS_CPU_TYPE}" "-march=armv8-a" "-mgeneral-regs-only"
    "-mstrict-align" "-mno-outline-atomics")

# ARM64 内核特定标志
set(ARM64_KERNEL_FLAGS "-fno-pic" "-fno-pie")

# ARM64 编译器标志应用
string(JOIN " " ARM64_FLAGS_STR ${ARM64_SPECIFIC_FLAGS} ${ARM64_KERNEL_FLAGS})
set(CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} --target=${MOSS_TARGET_TRIPLE} ${ARM64_FLAGS_STR}"
    PARENT_SCOPE)
set(CMAKE_C_FLAGS
    "${CMAKE_C_FLAGS} --target=${MOSS_TARGET_TRIPLE} ${ARM64_FLAGS_STR}"
    PARENT_SCOPE)

# ARM64 汇编器标志
set(CMAKE_ASM_FLAGS
    "${CMAKE_ASM_FLAGS} --target=${MOSS_TARGET_TRIPLE} -march=armv8-a -mno-outline-atomics"
    PARENT_SCOPE)

# ARM64 链接器标志
set(ARM64_LINKER_FLAGS "-nostdlib" "-static" "-z max-page-size=4096")

# ARM64 链接器标志应用
string(JOIN " " ARM64_LINKER_STR ${ARM64_LINKER_FLAGS})
set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} ${ARM64_LINKER_STR} -mcmodel=small"
    PARENT_SCOPE)

# 定义 ARM64 特定宏
add_compile_definitions(MOSS_ARCH_ARM64=1 MOSS_ARCH_BITS=64 MOSS_PAGE_SIZE=4096
                        MOSS_CACHE_LINE_SIZE=64)

# ARM64 特定源文件
set(ARM64_ARCH_SOURCES
    ""
    PARENT_SCOPE)

message(STATUS "ARM64 架构配置完成")
