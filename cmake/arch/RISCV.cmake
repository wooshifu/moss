# RISC-V 64位架构配置
# 支持 RV64I, RV64IM, RV64GC 等变体

message(STATUS "配置 RISC-V 64位架构...")

# RISC-V 架构信息
set(MOSS_ARCH_NAME "RISCV")
set(MOSS_ARCH_BITS "64")
set(MOSS_TARGET_TRIPLE "riscv64-unknown-elf")

# 默认 ISA 配置
if(NOT MOSS_RISCV_ISA)
    set(MOSS_RISCV_ISA "rv64imac")
endif()

# 支持的 RISC-V ISA 配置
set(SUPPORTED_RISCV_ISAS
    "rv64i"        # 基础整数指令集
    "rv64im"       # 基础 + 乘除法
    "rv64ima"      # 基础 + 乘除法 + 原子操作
    "rv64imac"     # 基础 + 乘除法 + 原子操作 + 压缩指令
    "rv64imafdc"   # 全功能 (RV64GC 等价)
    "rv64gc"       # 标准通用配置
)

# 验证 ISA 配置
if(NOT MOSS_RISCV_ISA IN_LIST SUPPORTED_RISCV_ISAS)
    message(WARNING "不支持的 RISC-V ISA: ${MOSS_RISCV_ISA}，使用默认值: rv64imac")
    set(MOSS_RISCV_ISA "rv64imac")
endif()

# 默认 ABI
if(NOT MOSS_RISCV_ABI)
    set(MOSS_RISCV_ABI "lp64")
endif()

# 支持的 ABI
set(SUPPORTED_RISCV_ABIS
    "lp64"    # 长指针 + 长整型为 64 位
    "lp64f"   # lp64 + 硬件单精度浮点
    "lp64d"   # lp64 + 硬件双精度浮点
)

if(NOT MOSS_RISCV_ABI IN_LIST SUPPORTED_RISCV_ABIS)
    message(WARNING "不支持的 RISC-V ABI: ${MOSS_RISCV_ABI}，使用默认值: lp64")
    set(MOSS_RISCV_ABI "lp64")
endif()

# 代码模型
if(NOT MOSS_RISCV_CMODEL)
    set(MOSS_RISCV_CMODEL "medany")
endif()

message(STATUS "RISC-V ISA: ${MOSS_RISCV_ISA}")
message(STATUS "RISC-V ABI: ${MOSS_RISCV_ABI}")
message(STATUS "RISC-V 代码模型: ${MOSS_RISCV_CMODEL}")

# RISC-V 特定编译标志
set(RISCV_SPECIFIC_FLAGS
    "-march=${MOSS_RISCV_ISA}"
    "-mabi=${MOSS_RISCV_ABI}"
    "-mcmodel=${MOSS_RISCV_CMODEL}"
    "-mstrict-align"
)

# RISC-V 内核特定标志
set(RISCV_KERNEL_FLAGS
    "-fno-pic"
    "-fno-pie"
)

# RISC-V 编译器标志已迁移到 CMakePresets.json 中的 RISC-V 预设配置
# string(JOIN " " RISCV_FLAGS_STR ${RISCV_SPECIFIC_FLAGS} ${RISCV_KERNEL_FLAGS})
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${RISCV_FLAGS_STR}" PARENT_SCOPE)

# RISC-V 汇编器标志
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} --target=${MOSS_TARGET_TRIPLE} -march=${MOSS_RISCV_ISA}" PARENT_SCOPE)

# RISC-V 链接器标志
set(RISCV_LINKER_FLAGS
    "-nostdlib"
    "-static"
    "-z max-page-size=4096"
)

# RISC-V 链接器标志已迁移到 CMakePresets.json 中的 RISC-V 预设配置
# string(JOIN " " RISCV_LINKER_STR ${RISCV_LINKER_FLAGS})
# set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${RISCV_LINKER_STR}" PARENT_SCOPE)

# 定义 RISC-V 特定宏
add_compile_definitions(
    MOSS_ARCH_RISCV=1
    MOSS_ARCH_BITS=64
    MOSS_PAGE_SIZE=4096
    MOSS_CACHE_LINE_SIZE=64
)

# 根据 ISA 设置特性宏
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

# RISC-V 特定源文件
set(RISCV_ARCH_SOURCES "" PARENT_SCOPE)

message(STATUS "RISC-V 架构配置完成")