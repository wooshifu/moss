# X86_64 (AMD64) architecture configuration

message(STATUS "配置 X86_64 (AMD64) 架构...")

# X86_64 architecture info
set(MOSS_ARCH_NAME "X86_64")
set(MOSS_ARCH_BITS "64")
set(MOSS_TARGET_TRIPLE "x86_64-elf")

# Default CPU configuration
if(NOT MOSS_CPU_TYPE)
  set(MOSS_CPU_TYPE "x86-64")
endif()

# Supported CPU types
set(SUPPORTED_X86_64_CPUS
    "x86-64" "x86-64-v2" "x86-64-v3" "x86-64-v4"
    "core2" "nehalem" "westmere" "sandybridge" "ivybridge"
    "haswell" "broadwell" "skylake" "generic")

# Validate CPU type
if(NOT MOSS_CPU_TYPE IN_LIST SUPPORTED_X86_64_CPUS)
  message(WARNING "不支持的 X86_64 CPU 类型: ${MOSS_CPU_TYPE}，使用默认值: x86-64")
  set(MOSS_CPU_TYPE "x86-64")
endif()

message(STATUS "X86_64 目标 CPU: ${MOSS_CPU_TYPE}")

# X86_64 ASM flags (compiler/linker flags are managed by CMakePresets.json)
set(CMAKE_ASM_FLAGS
    "${CMAKE_ASM_FLAGS} --target=${MOSS_TARGET_TRIPLE} -m64"
    PARENT_SCOPE)

# X86_64 compile definitions
add_compile_definitions(MOSS_ARCH_X86_64=1 MOSS_ARCH_BITS=64
                        MOSS_PAGE_SIZE=4096 MOSS_CACHE_LINE_SIZE=64)

# X86_64 architecture sources
set(X86_64_ARCH_SOURCES "" PARENT_SCOPE)

message(STATUS "X86_64 架构配置完成")
