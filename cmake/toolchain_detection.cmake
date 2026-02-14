# Moss Clang toolchain verification module
#
# Prerequisite detection (LLVM tool paths, versions) is handled by
# cmake/ensure_prerequisites.cmake + scripts/check_prerequisites.py.
#
# This module validates the CMake-internal compiler state that is only
# available after project() and configures the linker / cross-compilation.
#
# Public API:
#   moss_initialize_clang_toolchain(arch)

# Minimum version shared with the Python checker
set(MOSS_MIN_LLVM_VERSION "21.0")

# =============================================================================
# Clang compiler validation (uses CMake built-in variables from project())
# =============================================================================

function(moss_detect_clang_compiler)
  message(STATUS "Validating Clang compiler...")

  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
      "Moss requires the Clang compiler.\n"
      "Current compiler: ${CMAKE_CXX_COMPILER_ID}\n"
      "Set the environment variable: export CXX=clang++")
  endif()

  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS ${MOSS_MIN_LLVM_VERSION})
    message(FATAL_ERROR
      "Clang version too old: ${CMAKE_CXX_COMPILER_VERSION}\n"
      "Minimum required: ${MOSS_MIN_LLVM_VERSION}")
  endif()

  get_filename_component(CLANG_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  set(CLANG_INSTALL_PREFIX "${CLANG_BIN_DIR}/.." PARENT_SCOPE)
  set(CLANG_BIN_DIR "${CLANG_BIN_DIR}" PARENT_SCOPE)

  message(STATUS "Clang ${CMAKE_CXX_COMPILER_VERSION}: ${CMAKE_CXX_COMPILER}")
endfunction()

# =============================================================================
# Cross-compilation verification
# =============================================================================

function(moss_verify_clang_cross_compile arch)
  message(STATUS "Verifying cross-compilation support: ${arch}")

  if(arch STREQUAL "ARM64")
    set(test_target "aarch64-unknown-elf")
  elseif(arch STREQUAL "X86_64")
    set(test_target "x86_64-unknown-linux-elf")
  elseif(arch STREQUAL "RISCV")
    set(test_target "riscv64-unknown-elf")
  else()
    message(FATAL_ERROR "Unknown architecture: ${arch}")
  endif()

  set(test_file "${CMAKE_BINARY_DIR}/clang_cross_test.cpp")
  file(WRITE "${test_file}"
    "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
    "int test_function() { return 42; }\n"
    "#ifdef __cplusplus\n}\n#endif\n")

  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} --target=${test_target}
            -c "${test_file}" -o "${CMAKE_BINARY_DIR}/test.o"
    RESULT_VARIABLE compile_result
    OUTPUT_QUIET ERROR_QUIET)

  file(REMOVE "${test_file}" "${CMAKE_BINARY_DIR}/test.o")

  if(compile_result EQUAL 0)
    message(STATUS "Clang supports ${arch} cross-compilation")
  else()
    message(FATAL_ERROR "Clang does not support ${arch} cross-compilation")
  endif()
endfunction()

# =============================================================================
# Entry point
# =============================================================================

function(moss_initialize_clang_toolchain arch)
  message(STATUS "Initializing Clang toolchain (${arch})...")

  moss_detect_clang_compiler()
  moss_verify_clang_cross_compile(${arch})

  message(STATUS "Clang toolchain initialized")
endfunction()
