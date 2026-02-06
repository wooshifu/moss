# X86_64 (AMD64) 架构配置
# 支持 Intel 和 AMD 64位处理器

message(STATUS "配置 X86_64 (AMD64) 架构...")

# X86_64 架构信息
set(MOSS_ARCH_NAME "X86_64")
set(MOSS_ARCH_BITS "64")
set(MOSS_TARGET_TRIPLE "x86_64-elf")

# 默认 CPU 配置
if(NOT MOSS_CPU_TYPE)
    set(MOSS_CPU_TYPE "x86-64")
endif()

# 支持的 CPU 类型
set(SUPPORTED_X86_64_CPUS
    "x86-64"
    "x86-64-v2"
    "x86-64-v3"
    "x86-64-v4"
    "core2"
    "nehalem"
    "westmere"
    "sandybridge"
    "ivybridge"
    "haswell"
    "broadwell"
    "skylake"
    "generic"
)

# 验证 CPU 类型
if(NOT MOSS_CPU_TYPE IN_LIST SUPPORTED_X86_64_CPUS)
    message(WARNING "不支持的 X86_64 CPU 类型: ${MOSS_CPU_TYPE}，使用默认值: x86-64")
    set(MOSS_CPU_TYPE "x86-64")
endif()

message(STATUS "X86_64 目标 CPU: ${MOSS_CPU_TYPE}")

# X86_64 特定编译标志
set(X86_64_SPECIFIC_FLAGS
    "-mtune=${MOSS_CPU_TYPE}"
    "-mno-red-zone"
    "-mno-mmx"
    "-mno-sse"
    "-mno-sse2"
    "-mno-sse3"
    "-mno-ssse3"
    "-mno-sse4.1"
    "-mno-sse4.2"
    "-mno-avx"
    "-mno-avx2"
    "-mno-80387"
    "-mno-fp-ret-in-387"
)

# X86_64 内核特定标志
set(X86_64_KERNEL_FLAGS
    "-fno-pic"
    "-fno-pie"
    "-mcmodel=kernel"
)

# x86_64 编译器标志已迁移到 CMakePresets.json 中的 x86_64 预设配置
# string(JOIN " " X86_64_FLAGS_STR ${X86_64_SPECIFIC_FLAGS} ${X86_64_KERNEL_FLAGS})
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${X86_64_FLAGS_STR}" PARENT_SCOPE)

# X86_64 汇编器标志
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} --target=${MOSS_TARGET_TRIPLE} -m64" PARENT_SCOPE)

# X86_64 链接器标志
set(X86_64_LINKER_FLAGS
    "-nostdlib"
    "-static"
    "-z max-page-size=4096"
    "-z common-page-size=4096"
)

# x86_64 链接器标志已迁移到 CMakePresets.json 中的 x86_64 预设配置
# string(JOIN " " X86_64_LINKER_STR ${X86_64_LINKER_FLAGS})
# set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${X86_64_LINKER_STR}" PARENT_SCOPE)

# 定义 X86_64 特定宏
add_compile_definitions(
    MOSS_ARCH_X86_64=1
    MOSS_ARCH_BITS=64
    MOSS_PAGE_SIZE=4096
    MOSS_CACHE_LINE_SIZE=64
)

# X86_64 特定源文件
set(X86_64_ARCH_SOURCES "" PARENT_SCOPE)

message(STATUS "X86_64 架构配置完成")