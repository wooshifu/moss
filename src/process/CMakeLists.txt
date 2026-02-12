# src/process/CMakeLists.txt - 重构后的目录结构版本

# 创建OBJECT库（统一策略，消除双重逻辑）
add_library(moss_process OBJECT)

# 添加源文件（从src目录）
set(PROCESS_SOURCES
    src/process.cpp
    src/idle_process.cpp
)

# 添加架构特定汇编文件（使用新的路径结构）
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    list(APPEND PROCESS_SOURCES src/context_switch.S)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    list(APPEND PROCESS_SOURCES src/arch/x86_64/context_switch.S)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    list(APPEND PROCESS_SOURCES src/arch/riscv/context_switch.S)
endif()

target_sources(moss_process PRIVATE ${PROCESS_SOURCES})

# 标准包含目录配置（使用新的目录结构）
target_include_directories(moss_process PUBLIC
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/kernel/include
)
# 为源文件添加包含目录（让src目录下的cpp文件能找到头文件）
target_include_directories(moss_process PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/process
)

# 标准编译定义
target_compile_definitions(moss_process PRIVATE MOSS_KERNEL_MODULE MOSS_PROCESS_MODULE)

# 架构配置（使用统一函数）
moss_configure_module_arch(moss_process)

# 依赖关系（进程管理依赖所有其他模块）
target_link_libraries(moss_process PRIVATE moss_containers moss_mm moss_drivers moss_interrupts moss_ipc)

message(STATUS "进程模块配置完成")