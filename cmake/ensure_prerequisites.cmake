# Moss prerequisite bootstrap module
#
# Two-phase approach:
#   Phase 1 (pure CMake): ensure `uv` is available, auto-install if missing
#   Phase 2 (Python via uv): run check_prerequisites.py for full toolchain detection
#
# Public API:
#   moss_ensure_uv()                  - find or install uv
#   moss_check_prerequisites(arch)    - detect LLVM tools, set cache variables

# =============================================================================
# Phase 1 - Bootstrap uv
# =============================================================================

function(moss_ensure_uv)
  find_program(UV_EXECUTABLE uv)
  if(UV_EXECUTABLE)
    message(STATUS "Found uv: ${UV_EXECUTABLE}")
    return()
  endif()

  message(STATUS "uv not found - attempting automatic installation...")

  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    execute_process(
      COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
              "irm https://astral.sh/uv/install.ps1 | iex"
      RESULT_VARIABLE _uv_install_rc
      OUTPUT_QUIET)
  else()
    execute_process(
      COMMAND sh -c "curl -LsSf https://astral.sh/uv/install.sh | sh"
      RESULT_VARIABLE _uv_install_rc
      OUTPUT_QUIET)
  endif()

  if(NOT _uv_install_rc EQUAL 0)
    message(WARNING "Automatic uv installation returned non-zero (${_uv_install_rc})")
  endif()

  # Re-search common install locations
  find_program(UV_EXECUTABLE uv
    HINTS "$ENV{HOME}/.cargo/bin"
          "$ENV{HOME}/.local/bin"
          "$ENV{USERPROFILE}/.cargo/bin"
          "$ENV{LOCALAPPDATA}/uv/bin")

  if(NOT UV_EXECUTABLE)
    message(FATAL_ERROR
      "uv is required but could not be found or installed.\n"
      "Install manually: https://docs.astral.sh/uv/getting-started/installation/\n"
      "  Windows : powershell -c \"irm https://astral.sh/uv/install.ps1 | iex\"\n"
      "  Linux/macOS: curl -LsSf https://astral.sh/uv/install.sh | sh")
  endif()

  message(STATUS "Installed uv: ${UV_EXECUTABLE}")
endfunction()

# =============================================================================
# Phase 2 - Full prerequisite check via Python
# =============================================================================

function(moss_check_prerequisites arch)
  set(_script "${CMAKE_SOURCE_DIR}/scripts/check_prerequisites.py")

  message(STATUS "Running prerequisite check (arch=${arch})...")

  execute_process(
    COMMAND ${UV_EXECUTABLE} run "${_script}"
            --json --arch ${arch} --build-dir "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE _json_output
    ERROR_VARIABLE  _stderr
    RESULT_VARIABLE _rc
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )

  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
      "Prerequisite check failed (exit ${_rc}).\n"
      "${_stderr}\n${_json_output}")
  endif()

  # Parse top-level fields
  string(JSON _status GET "${_json_output}" "status")
  string(JSON _install_guide GET "${_json_output}" "install_guide")
  string(JSON _llvm_bin_dir GET "${_json_output}" "llvm_bin_dir")

  if(NOT _status STREQUAL "ok")
    string(JSON _missing GET "${_json_output}" "missing_required")
    string(JSON _ver_err GET "${_json_output}" "version_errors")
    message(FATAL_ERROR
      "Required build tools are missing or outdated.\n"
      "Missing : ${_missing}\n"
      "Version : ${_ver_err}\n\n"
      "${_install_guide}")
  endif()

  # Extract tool paths into cache variables
  _moss_extract_tool_path("${_json_output}" "clang"        MOSS_CLANG_PATH)
  _moss_extract_tool_path("${_json_output}" "clang++"      MOSS_CLANGXX_PATH)
  _moss_extract_tool_path("${_json_output}" "lld"          MOSS_LLD_PATH)
  _moss_extract_tool_path("${_json_output}" "llvm-objdump" MOSS_LLVM_OBJDUMP)
  _moss_extract_tool_path("${_json_output}" "llvm-objcopy" MOSS_LLVM_OBJCOPY)
  _moss_extract_tool_path("${_json_output}" "llvm-nm"      MOSS_LLVM_NM)

  set(MOSS_LLVM_BIN_DIR "${_llvm_bin_dir}" CACHE PATH "LLVM bin directory" FORCE)

  message(STATUS "LLVM bin dir : ${_llvm_bin_dir}")
  message(STATUS "Prerequisite check passed")
endfunction()

# Internal helper: extract a tool path from the JSON tools object
macro(_moss_extract_tool_path json_str tool_name cache_var)
  string(JSON _tool_obj GET "${json_str}" "tools" "${tool_name}")
  string(JSON _tool_path GET "${_tool_obj}" "path")
  set(${cache_var} "${_tool_path}" CACHE FILEPATH "${tool_name} path" FORCE)
endmacro()
