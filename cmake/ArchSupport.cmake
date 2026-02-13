# Moss kernel multi-architecture support module (Clang only)

# Supported architectures
set(SUPPORTED_ARCHITECTURES "ARM64" "X86_64" "RISCV")

# Supported platforms
set(SUPPORTED_PLATFORMS "QEMU_VIRT" "RASPBERRY_PI" "GENERIC_X86_64"
                        "SIFIVE_UNLEASHED")

# Architecture detection and configuration
function(moss_configure_architecture)
  message(STATUS "开始配置多架构支持...")

  if(NOT MOSS_TARGET_ARCH)
    message(
      FATAL_ERROR "必须设置 MOSS_TARGET_ARCH 变量。支持的架构: ${SUPPORTED_ARCHITECTURES}")
  endif()

  if(NOT MOSS_TARGET_ARCH IN_LIST SUPPORTED_ARCHITECTURES)
    message(
      FATAL_ERROR
        "不支持的架构: ${MOSS_TARGET_ARCH}。支持的架构: ${SUPPORTED_ARCHITECTURES}")
  endif()

  message(STATUS "目标架构: ${MOSS_TARGET_ARCH}")

  # Load architecture-specific configuration (CPU validation, ASM flags, compile definitions)
  include(${CMAKE_SOURCE_DIR}/cmake/arch/${MOSS_TARGET_ARCH}.cmake)

  message(STATUS "架构配置完成: ${MOSS_TARGET_ARCH}")
endfunction()

# Platform configuration
function(moss_configure_platform)
  if(NOT MOSS_TARGET_PLATFORM)
    set(MOSS_TARGET_PLATFORM
        "QEMU_VIRT"
        PARENT_SCOPE)
    message(STATUS "未指定平台，使用默认平台: QEMU_VIRT")
  endif()

  if(NOT MOSS_TARGET_PLATFORM IN_LIST SUPPORTED_PLATFORMS)
    message(
      FATAL_ERROR
        "不支持的平台: ${MOSS_TARGET_PLATFORM}。支持的平台: ${SUPPORTED_PLATFORMS}")
  endif()

  message(STATUS "目标平台: ${MOSS_TARGET_PLATFORM}")

  # Load platform-specific configuration
  include(${CMAKE_SOURCE_DIR}/cmake/platforms/${MOSS_TARGET_PLATFORM}.cmake)

  message(STATUS "平台配置完成: ${MOSS_TARGET_PLATFORM}")
endfunction()

# Display configuration summary
function(moss_print_config_summary)
  message(STATUS "=== Moss 内核构建配置摘要 ===")
  message(STATUS "架构: ${MOSS_TARGET_ARCH}")
  message(STATUS "平台: ${MOSS_TARGET_PLATFORM}")
  message(STATUS "编译器: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
  message(STATUS "目标: ${CMAKE_CXX_COMPILER_TARGET}")
  message(STATUS "构建类型: ${CMAKE_BUILD_TYPE}")
  message(STATUS "==============================")
endfunction()
