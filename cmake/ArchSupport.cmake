# Moss 内核多架构支持模块 只支持 Clang 编译器，简化交叉编译配置

# 支持的架构列表
set(SUPPORTED_ARCHITECTURES "ARM64" "X86_64" "RISCV")

# 支持的平台列表
set(SUPPORTED_PLATFORMS "QEMU_VIRT" "RASPBERRY_PI" "GENERIC_X86_64"
                        "SIFIVE_UNLEASHED")

# 架构到 Clang target 的映射
set(ARCH_TARGET_MAP_ARM64 "aarch64-unknown-elf")
set(ARCH_TARGET_MAP_X86_64 "x86_64-elf")
set(ARCH_TARGET_MAP_RISCV "riscv64-unknown-elf")

# 架构到 CPU 类型的默认映射
set(ARCH_CPU_MAP_ARM64 "cortex-a57")
set(ARCH_CPU_MAP_X86_64 "x86-64")
set(ARCH_CPU_MAP_RISCV "generic-rv64")

# 架构检测和配置主函数
function(moss_configure_architecture)
  message(STATUS "开始配置多架构支持...")

  # 检查 MOSS_TARGET_ARCH 是否已设置
  if(NOT MOSS_TARGET_ARCH)
    message(
      FATAL_ERROR "必须设置 MOSS_TARGET_ARCH 变量。支持的架构: ${SUPPORTED_ARCHITECTURES}")
  endif()

  # 验证架构支持
  if(NOT MOSS_TARGET_ARCH IN_LIST SUPPORTED_ARCHITECTURES)
    message(
      FATAL_ERROR
        "不支持的架构: ${MOSS_TARGET_ARCH}。支持的架构: ${SUPPORTED_ARCHITECTURES}")
  endif()

  message(STATUS "目标架构: ${MOSS_TARGET_ARCH}")

  # 强制使用 Clang
  moss_ensure_clang_compiler()

  # 加载架构特定配置
  include(${CMAKE_SOURCE_DIR}/cmake/arch/${MOSS_TARGET_ARCH}.cmake)

  # 配置 Clang 交叉编译工具链
  moss_configure_clang_toolchain(${MOSS_TARGET_ARCH})

  # 设置架构特定的编译标志
  moss_set_arch_compile_flags(${MOSS_TARGET_ARCH})

  message(STATUS "架构配置完成: ${MOSS_TARGET_ARCH}")
endfunction()

# 确保使用 Clang 编译器
function(moss_ensure_clang_compiler)
  # 设置 Clang 为默认编译器
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Moss 内核只支持 Clang 编译器，当前编译器: ${CMAKE_CXX_COMPILER_ID}")
  endif()

  # 检查 Clang 版本 (建议 18+)
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "18.0")
    message(WARNING "建议使用 Clang 18.0+ 版本，当前版本: ${CMAKE_CXX_COMPILER_VERSION}")
  endif()

  message(STATUS "使用 Clang 编译器版本: ${CMAKE_CXX_COMPILER_VERSION}")
endfunction()

# 配置 Clang 交叉编译工具链
function(moss_configure_clang_toolchain arch)
  # 获取架构对应的 target triple
  set(target_triple ${ARCH_TARGET_MAP_${arch}})
  if(NOT target_triple)
    message(FATAL_ERROR "未找到架构 ${arch} 对应的 target triple")
  endif()

  message(STATUS "设置 Clang target: ${target_triple}")

  # 设置交叉编译目标
  set(CMAKE_SYSTEM_NAME
      "Generic"
      PARENT_SCOPE)
  set(CMAKE_CROSSCOMPILING
      TRUE
      PARENT_SCOPE)

  # 设置编译器 target
  set(CMAKE_CXX_COMPILER_TARGET
      ${target_triple}
      PARENT_SCOPE)
  set(CMAKE_ASM_COMPILER_TARGET
      ${target_triple}
      PARENT_SCOPE)

  # 架构特定设置
  if(arch STREQUAL "ARM64")
    set(CMAKE_SYSTEM_PROCESSOR
        "aarch64"
        PARENT_SCOPE)
  elseif(arch STREQUAL "X86_64")
    set(CMAKE_SYSTEM_PROCESSOR
        "x86_64"
        PARENT_SCOPE)
  elseif(arch STREQUAL "RISCV")
    set(CMAKE_SYSTEM_PROCESSOR
        "riscv64"
        PARENT_SCOPE)
  endif()
endfunction()

# 设置架构特定的编译标志
function(moss_set_arch_compile_flags arch)
  message(STATUS "设置架构特定编译标志: ${arch}")

  # 获取 CPU 类型
  set(cpu_type ${ARCH_CPU_MAP_${arch}})

  # 基础内核编译标志 (所有架构通用)
  set(KERNEL_FLAGS
      "-ffreestanding"
      "-fno-exceptions"
      "-fno-rtti"
      "-fno-stack-protector"
      "-Wall"
      "-Wextra"
      "-Werror")

  # 架构特定标志
  if(arch STREQUAL "ARM64")
    list(APPEND KERNEL_FLAGS "-mgeneral-regs-only" "-mcpu=${cpu_type}"
         "-march=armv8-a+lse")
  elseif(arch STREQUAL "X86_64")
    list(
      APPEND
      KERNEL_FLAGS
      "-mno-red-zone"
      "-mno-mmx"
      "-mno-sse"
      "-mno-sse2"
      "-mcpu=${cpu_type}")
  elseif(arch STREQUAL "RISCV")
    list(APPEND KERNEL_FLAGS "-march=rv64imac" "-mabi=lp64" "-mcmodel=medany")
  endif()

  # 应用编译标志
  string(JOIN " " KERNEL_FLAGS_STR ${KERNEL_FLAGS})
  set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} ${KERNEL_FLAGS_STR}"
      PARENT_SCOPE)
  set(CMAKE_ASM_FLAGS
      "${CMAKE_ASM_FLAGS} --target=${ARCH_TARGET_MAP_${arch}}"
      PARENT_SCOPE)

  message(STATUS "架构编译标志已设置: ${KERNEL_FLAGS_STR}")
endfunction()

# 平台配置函数
function(moss_configure_platform)
  if(NOT MOSS_TARGET_PLATFORM)
    set(MOSS_TARGET_PLATFORM
        "QEMU_VIRT"
        PARENT_SCOPE)
    message(STATUS "未指定平台，使用默认平台: QEMU_VIRT")
  endif()

  # 验证平台支持
  if(NOT MOSS_TARGET_PLATFORM IN_LIST SUPPORTED_PLATFORMS)
    message(
      FATAL_ERROR
        "不支持的平台: ${MOSS_TARGET_PLATFORM}。支持的平台: ${SUPPORTED_PLATFORMS}")
  endif()

  message(STATUS "目标平台: ${MOSS_TARGET_PLATFORM}")

  # 加载平台特定配置
  include(${CMAKE_SOURCE_DIR}/cmake/platforms/${MOSS_TARGET_PLATFORM}.cmake)

  message(STATUS "平台配置完成: ${MOSS_TARGET_PLATFORM}")
endfunction()

# 显示配置摘要
function(moss_print_config_summary)
  message(STATUS "=== Moss 内核构建配置摘要 ===")
  message(STATUS "架构: ${MOSS_TARGET_ARCH}")
  message(STATUS "平台: ${MOSS_TARGET_PLATFORM}")
  message(STATUS "编译器: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
  message(STATUS "目标: ${CMAKE_CXX_COMPILER_TARGET}")
  message(STATUS "构建类型: ${CMAKE_BUILD_TYPE}")
  message(STATUS "==============================")
endfunction()
