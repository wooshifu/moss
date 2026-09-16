cmake_minimum_required(VERSION 3.31)
# Initialize upstream standalone builtins inside the Moss target graph.
set(COMPILER_RT_STANDALONE_BUILD TRUE)
set(COMPILER_RT_BUILTINS_STANDALONE_BUILD TRUE)
set(COMPILER_RT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(LLVM_COMMON_CMAKE_UTILS "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
set(COMPILER_RT_DEFAULT_TARGET_ONLY ON)
set(COMPILER_RT_BAREMETAL_BUILD ON)
set(COMPILER_RT_INCLUDE_TESTS OFF)
set(LLVM_RUNTIMES_BUILD ON)
set(CMAKE_SYSTEM_PROCESSOR "${MLIBC_CPU}")
set(CMAKE_C_COMPILER_TARGET "${MLIBC_CPU}-unknown-elf")
set(CMAKE_ASM_COMPILER_TARGET "${MLIBC_CPU}-unknown-elf")
string(
    JOIN
    " "
    CMAKE_C_FLAGS
    ${MLIBC_ARCH_FLAGS}
    -O3
    -ffreestanding
)
set(CMAKE_ASM_FLAGS "${CMAKE_C_FLAGS}")
string(APPEND CMAKE_C_FLAGS " -isystem \"${MLIBC_SYSROOT}/usr/include\"")
list(
    PREPEND
    CMAKE_MODULE_PATH
    "${COMPILER_RT_SOURCE_DIR}/cmake"
    "${COMPILER_RT_SOURCE_DIR}/cmake/Modules"
    "${LLVM_COMMON_CMAKE_UTILS}"
    "${LLVM_COMMON_CMAKE_UTILS}/Modules"
)
include(base-config-ix)
include(CompilerRTUtils)
construct_compiler_rt_default_triple()
include(SetPlatformToolchainTools)
include(AddCompilerRT)
add_subdirectory(lib/builtins)
