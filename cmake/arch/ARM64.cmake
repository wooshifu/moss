# ARM64 (AArch64) architecture configuration

message(STATUS "配置 ARM64 (AArch64) 架构...")

# ARM64 architecture info
set(MOSS_ARCH_NAME "ARM64")
set(MOSS_ARCH_BITS "64")
set(MOSS_TARGET_TRIPLE "aarch64-unknown-elf")

# Default CPU configuration
if(NOT MOSS_CPU_TYPE)
  set(MOSS_CPU_TYPE "cortex-a57")
endif()

# Supported CPU types
set(SUPPORTED_ARM64_CPUS
    "cortex-a53" "cortex-a57" "cortex-a72" "cortex-a73"
    "cortex-a75" "cortex-a76" "cortex-a78" "cortex-x1" "generic")

# Validate CPU type
if(NOT MOSS_CPU_TYPE IN_LIST SUPPORTED_ARM64_CPUS)
  message(WARNING "不支持的 ARM64 CPU 类型: ${MOSS_CPU_TYPE}，使用默认值: cortex-a57")
  set(MOSS_CPU_TYPE "cortex-a57")
endif()

message(STATUS "ARM64 目标 CPU: ${MOSS_CPU_TYPE}")

# ARM64 ASM flags (compiler/linker flags are managed by CMakePresets.json)
set(CMAKE_ASM_FLAGS
    "${CMAKE_ASM_FLAGS} --target=${MOSS_TARGET_TRIPLE} -march=armv8-a -mno-outline-atomics"
    PARENT_SCOPE)

# ARM64 compile definitions
add_compile_definitions(MOSS_ARCH_ARM64=1 MOSS_ARCH_BITS=64 MOSS_PAGE_SIZE=4096
                        MOSS_CACHE_LINE_SIZE=64)

# ARM64 architecture sources
set(ARM64_ARCH_SOURCES "" PARENT_SCOPE)

message(STATUS "ARM64 架构配置完成")
