# MOSS kernel unit test executable (standalone moss.test.elf)

add_executable(moss.test.elf)

target_sources(moss.test.elf PRIVATE
    test_main.cpp
    framework/minimal_boot_support.cpp
    cases/kernel_modern_validation.cpp
    cases/boost_ut_kernel_validation.cpp)

set_target_properties(moss.test.elf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Include paths & freestanding definitions
# Note: MOSS_ARCH_* definitions are already set globally by cmake/arch/*.cmake
target_include_directories(moss.test.elf PRIVATE
    ${PROJECT_SOURCE_DIR}/src/containers/include
    ${PROJECT_SOURCE_DIR}/src/process/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/framework)

target_compile_definitions(moss.test.elf PRIVATE
    BOOST_UT_DISABLE_MODULE=1
    BOOST_UT_DISABLE_FILE_OUTPUT=1
    MOSS_USE_UT_HPP=1
    MOSS_FREESTANDING_MODE=1)

message(STATUS "MOSS Test Executable: moss.test.elf (standalone)")
message(STATUS "  Architecture: ${MOSS_TARGET_ARCH}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
