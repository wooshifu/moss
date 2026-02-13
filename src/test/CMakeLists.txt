# MOSS kernel unit test executable (standalone moss.test.elf)
#
# This builds a completely independent executable that runs
# unit tests directly on QEMU without the kernel.
#
# Directory structure:
#   framework/  - Testing framework (ut_kernel.hpp, moss_ut.hpp, boot support)
#   cases/      - Actual test cases
#   test_main.cpp - Standalone entry point with _start

# ========================================================================
# Standalone test executable
# ========================================================================

add_executable(moss.test.elf)

target_sources(moss.test.elf PRIVATE
    test_main.cpp
    framework/minimal_boot_support.cpp
    cases/kernel_modern_validation.cpp
    cases/boost_ut_demo.cpp
    cases/boost_ut_kernel_validation.cpp
)


set_target_properties(moss.test.elf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ========================================================================
# Architecture definitions
# ========================================================================

if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    target_compile_definitions(moss.test.elf PUBLIC MOSS_ARCH_ARM64=1)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    target_compile_definitions(moss.test.elf PUBLIC MOSS_ARCH_X86_64=1)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    target_compile_definitions(moss.test.elf PUBLIC MOSS_ARCH_RISCV=1)
endif()

# ========================================================================
# Include paths & freestanding definitions
# ========================================================================

target_include_directories(moss.test.elf PRIVATE
    ${PROJECT_SOURCE_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/src/containers/include
    ${PROJECT_SOURCE_DIR}/src/process/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
)

target_compile_definitions(moss.test.elf PRIVATE
    BOOST_UT_DISABLE_MODULE=1
    BOOST_UT_DISABLE_FILE_OUTPUT=1
    MOSS_USE_UT_HPP=1
    MOSS_FREESTANDING_MODE=1
)

# ========================================================================
# Build info
# ========================================================================

message(STATUS "MOSS Test Executable: moss.test.elf (standalone)")
message(STATUS "  Framework: ut_kernel.hpp (kernel-optimized)")
message(STATUS "  Architecture: ${MOSS_TARGET_ARCH}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
