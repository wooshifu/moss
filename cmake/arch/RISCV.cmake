# RISC-V 64-bit architecture configuration

message(STATUS "配置 RISC-V 64位架构...")

# RISC-V architecture info
set(MOSS_ARCH_NAME "RISCV64")
set(MOSS_ARCH_BITS "64")
set(MOSS_TARGET_TRIPLE "riscv64-unknown-elf")

# Default ISA configuration
if(NOT MOSS_RISCV_ISA)
  set(MOSS_RISCV_ISA "rv64imac")
endif()

# Supported RISC-V ISA configurations
set(SUPPORTED_RISCV_ISAS
    "rv64i" "rv64im" "rv64ima" "rv64imac" "rv64imafdc" "rv64gc")

# Validate ISA configuration
if(NOT MOSS_RISCV_ISA IN_LIST SUPPORTED_RISCV_ISAS)
  message(WARNING "不支持的 RISC-V ISA: ${MOSS_RISCV_ISA}，使用默认值: rv64imac")
  set(MOSS_RISCV_ISA "rv64imac")
endif()

# Default ABI
if(NOT MOSS_RISCV_ABI)
  set(MOSS_RISCV_ABI "lp64")
endif()

# Supported ABIs
set(SUPPORTED_RISCV_ABIS "lp64" "lp64f" "lp64d")

if(NOT MOSS_RISCV_ABI IN_LIST SUPPORTED_RISCV_ABIS)
  message(WARNING "不支持的 RISC-V ABI: ${MOSS_RISCV_ABI}，使用默认值: lp64")
  set(MOSS_RISCV_ABI "lp64")
endif()

message(STATUS "RISC-V ISA: ${MOSS_RISCV_ISA}")
message(STATUS "RISC-V ABI: ${MOSS_RISCV_ABI}")

# RISC-V ASM flags (compiler/linker flags are managed by CMakePresets.json)
set(CMAKE_ASM_FLAGS
    "${CMAKE_ASM_FLAGS} --target=${MOSS_TARGET_TRIPLE} -march=${MOSS_RISCV_ISA}"
    PARENT_SCOPE)

# RISC-V compile definitions
add_compile_definitions(MOSS_ARCH_RISCV=1 MOSS_ARCH_BITS=64 MOSS_PAGE_SIZE=4096
                        MOSS_CACHE_LINE_SIZE=64)

# ISA extension feature macros
if(MOSS_RISCV_ISA MATCHES ".*m.*")
  add_compile_definitions(MOSS_RISCV_M_EXT=1)
endif()
if(MOSS_RISCV_ISA MATCHES ".*a.*")
  add_compile_definitions(MOSS_RISCV_A_EXT=1)
endif()
if(MOSS_RISCV_ISA MATCHES ".*c.*")
  add_compile_definitions(MOSS_RISCV_C_EXT=1)
endif()
if(MOSS_RISCV_ISA MATCHES ".*f.*")
  add_compile_definitions(MOSS_RISCV_F_EXT=1)
endif()
if(MOSS_RISCV_ISA MATCHES ".*d.*")
  add_compile_definitions(MOSS_RISCV_D_EXT=1)
endif()

# RISC-V architecture sources
set(RISCV_ARCH_SOURCES "" PARENT_SCOPE)

message(STATUS "RISC-V 架构配置完成")
