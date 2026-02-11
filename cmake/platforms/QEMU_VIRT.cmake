# QEMU Virt 平台配置 支持 QEMU 虚拟化的通用 virt 平台

message(STATUS "配置 QEMU Virt 平台...")

# 平台信息
set(MOSS_PLATFORM_NAME "QEMU_VIRT")
set(MOSS_PLATFORM_VENDOR "QEMU")

# 内存配置 (默认 256MB)
if(NOT MOSS_MEMORY_SIZE)
  set(MOSS_MEMORY_SIZE "256M")
endif()

# 基础内存地址 (根据架构设置)
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
  set(MOSS_MEMORY_BASE "0x40000000")
  set(MOSS_KERNEL_LOAD_ADDR "0x40080000")
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
  set(MOSS_MEMORY_BASE "0x00100000")
  set(MOSS_KERNEL_LOAD_ADDR "0x00100000")
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
  set(MOSS_MEMORY_BASE "0x80000000")
  set(MOSS_KERNEL_LOAD_ADDR "0x80200000")
endif()

message(STATUS "QEMU Virt 内存基址: ${MOSS_MEMORY_BASE}")
message(STATUS "QEMU Virt 内核加载地址: ${MOSS_KERNEL_LOAD_ADDR}")

# CPU 核心数 (默认 4 核)
if(NOT MOSS_CPU_CORES)
  set(MOSS_CPU_CORES "4")
endif()

# QEMU 设备配置
set(QEMU_DEVICES
    # UART (串口)
    "pl011"
    # 中断控制器
    "gicv2"
    # 定时器
    "generic_timer"
    # RTC
    "pl031")

# 根据架构设置 QEMU 命令
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
  set(QEMU_SYSTEM_CMD "qemu-system-aarch64")
  set(QEMU_MACHINE "virt")
  set(QEMU_CPU "cortex-a57")
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
  set(QEMU_SYSTEM_CMD "qemu-system-x86_64")
  set(QEMU_MACHINE "q35")
  set(QEMU_CPU "qemu64")
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
  set(QEMU_SYSTEM_CMD "qemu-system-riscv64")
  set(QEMU_MACHINE "virt")
  set(QEMU_CPU "rv64")
endif()

# 设置链接器脚本路径 (向后兼容)
set(LINKER_SCRIPT_TEMPLATE
    "${CMAKE_SOURCE_DIR}/platform/qemu-virt/${MOSS_TARGET_ARCH}/linker.ld.in")
set(LINKER_SCRIPT_OUTPUT "${CMAKE_BINARY_DIR}/linker.ld")

# 生成链接器脚本
if(EXISTS ${LINKER_SCRIPT_TEMPLATE})
  configure_file(${LINKER_SCRIPT_TEMPLATE} ${LINKER_SCRIPT_OUTPUT} @ONLY)
  set(CMAKE_EXE_LINKER_FLAGS
      "${CMAKE_EXE_LINKER_FLAGS} -T ${LINKER_SCRIPT_OUTPUT}"
      PARENT_SCOPE)
  message(STATUS "使用链接器脚本: ${LINKER_SCRIPT_OUTPUT}")
else()
  # 回退到原始链接器脚本 (向后兼容)
  set(FALLBACK_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/linker.ld")
  if(EXISTS ${FALLBACK_LINKER_SCRIPT})
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -T ${FALLBACK_LINKER_SCRIPT}"
        PARENT_SCOPE)
    message(STATUS "使用回退链接器脚本: ${FALLBACK_LINKER_SCRIPT}")
  else()
    message(FATAL_ERROR "未找到任何可用的链接器脚本")
  endif()
endif()

# 生成 QEMU 运行脚本 (向后兼容)
set(QEMU_SCRIPT_TEMPLATE
    "${CMAKE_SOURCE_DIR}/platform/qemu-virt/run_qemu.sh.in")
set(QEMU_SCRIPT_OUTPUT "${CMAKE_BINARY_DIR}/run_qemu.sh")

if(EXISTS ${QEMU_SCRIPT_TEMPLATE})
  configure_file(${QEMU_SCRIPT_TEMPLATE} ${QEMU_SCRIPT_OUTPUT} @ONLY)
  file(
    CHMOD
    ${QEMU_SCRIPT_OUTPUT}
    PERMISSIONS
    OWNER_READ
    OWNER_WRITE
    OWNER_EXECUTE
    GROUP_READ
    GROUP_EXECUTE
    WORLD_READ
    WORLD_EXECUTE)
  message(STATUS "生成 QEMU 运行脚本: ${QEMU_SCRIPT_OUTPUT}")
else()
  # 回退到原始 QEMU 脚本 (向后兼容)
  set(FALLBACK_QEMU_TEMPLATE "${CMAKE_SOURCE_DIR}/scripts/run_qemu.sh.in")
  if(EXISTS ${FALLBACK_QEMU_TEMPLATE})
    configure_file(${FALLBACK_QEMU_TEMPLATE} ${QEMU_SCRIPT_OUTPUT} @ONLY)
    file(
      CHMOD
      ${QEMU_SCRIPT_OUTPUT}
      PERMISSIONS
      OWNER_READ
      OWNER_WRITE
      OWNER_EXECUTE
      GROUP_READ
      GROUP_EXECUTE
      WORLD_READ
      WORLD_EXECUTE)
    message(STATUS "使用回退 QEMU 运行脚本: ${QEMU_SCRIPT_OUTPUT}")
  endif()
endif()

# 平台特定编译定义
add_compile_definitions(
  MOSS_PLATFORM_QEMU_VIRT=1 MOSS_MEMORY_BASE=${MOSS_MEMORY_BASE}
  MOSS_KERNEL_LOAD_ADDR=${MOSS_KERNEL_LOAD_ADDR}
  MOSS_CPU_CORES=${MOSS_CPU_CORES})

# 设置平台特定源文件目录
set(PLATFORM_SOURCE_DIR
    "${CMAKE_SOURCE_DIR}/platform/qemu-virt/${MOSS_TARGET_ARCH}")
if(IS_DIRECTORY ${PLATFORM_SOURCE_DIR})
  file(GLOB_RECURSE PLATFORM_SOURCES "${PLATFORM_SOURCE_DIR}/*.cpp"
       "${PLATFORM_SOURCE_DIR}/*.S")
  set(MOSS_PLATFORM_SOURCES
      ${PLATFORM_SOURCES}
      PARENT_SCOPE)
  message(STATUS "加载平台源文件: ${PLATFORM_SOURCE_DIR}")
else()
  message(WARNING "平台源文件目录不存在: ${PLATFORM_SOURCE_DIR}")
  set(MOSS_PLATFORM_SOURCES
      ""
      PARENT_SCOPE)
endif()

message(STATUS "QEMU Virt 平台配置完成")
