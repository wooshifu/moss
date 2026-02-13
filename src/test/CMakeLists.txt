# MOSS kernel unit test framework (ut_kernel.hpp based)
#
# Directory structure:
#   framework/  - Testing framework (ut_kernel.hpp, moss_ut.hpp, boot support)
#   cases/      - Actual test cases
#   test_main.cpp - Test entry point

# ========================================================================
# Test framework static library
# ========================================================================

add_library(moss_test_framework STATIC)

# Framework + entry point sources
target_sources(moss_test_framework PRIVATE
    test_main.cpp
    framework/minimal_boot_support.cpp
    cases/kernel_modern_validation.cpp
    cases/boost_ut_demo.cpp
    cases/boost_ut_kernel_validation.cpp
)

# Framework headers
target_sources(moss_test_framework PUBLIC
    FILE_SET HEADERS
    BASE_DIRS framework
    FILES
        framework/ut_kernel.hpp
        framework/moss_ut.hpp
)

# ========================================================================
# Compile options
# ========================================================================

function(configure_moss_test_target target_name)
    target_compile_options(${target_name} PRIVATE
        -std=c++26
        -ffreestanding
        -fno-exceptions
        -fno-rtti
        -fno-stack-protector
        -fno-omit-frame-pointer

        -Weverything
        -Werror
        -Wno-c++98-compat
        -Wno-c++98-compat-pedantic
        -Wno-padded
        -Wno-unused-macros
        -Wno-global-constructors
        -Wno-exit-time-destructors
        -Wno-user-defined-literals
        -Wno-unused-private-field
        -Wno-suggest-override
        -Wno-suggest-destructor-override
        -Wno-deprecated-volatile
        -Wno-unused-value
        -Wno-ctad-maybe-unsupported
        -Wno-unused-variable
    )

    # Architecture definitions
    if(MOSS_TARGET_ARCH STREQUAL "arm64")
        target_compile_definitions(${target_name} PUBLIC MOSS_ARCH_ARM64=1)
    elseif(MOSS_TARGET_ARCH STREQUAL "x86_64")
        target_compile_definitions(${target_name} PUBLIC MOSS_ARCH_X86_64=1)
    elseif(MOSS_TARGET_ARCH STREQUAL "riscv")
        target_compile_definitions(${target_name} PUBLIC MOSS_ARCH_RISCV=1)
    endif()

    target_link_options(${target_name} PUBLIC
        -ffreestanding
        -nostdlib
        -static
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${target_name} PRIVATE -g3)
    endif()

    target_include_directories(${target_name} PUBLIC
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/src/containers/include
        ${PROJECT_SOURCE_DIR}/src/process/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/framework
    )
endfunction()

configure_moss_test_target(moss_test_framework)

# ========================================================================
# ut.hpp freestanding configuration
# ========================================================================

target_compile_definitions(moss_test_framework PUBLIC
    BOOST_UT_DISABLE_MODULE=1
    BOOST_UT_DISABLE_FILE_OUTPUT=1
    MOSS_USE_UT_HPP=1
    MOSS_FREESTANDING_MODE=1
)

# ========================================================================
# Build info
# ========================================================================

message(STATUS "MOSS Test Framework:")
message(STATUS "  Framework: ut_kernel.hpp (kernel-optimized)")
message(STATUS "  Architecture: ${MOSS_TARGET_ARCH}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
