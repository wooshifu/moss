# src/kernel/CMakeLists.txt - 重构后的目录结构版本

# 创建OBJECT库
add_library(moss_kernel OBJECT)

# 添加源文件（从src目录）
set(KERNEL_SOURCES
    src/kernel_main.cpp
    src/runtime_support.cpp
    src/syscall_table.cpp
    src/elf_loader.cpp
)

# 添加架构特定的汇编文件（这些文件在全局include目录）
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    list(APPEND KERNEL_SOURCES ${CMAKE_SOURCE_DIR}/src/include/arch/arm64_syscall.S)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    list(APPEND KERNEL_SOURCES ${CMAKE_SOURCE_DIR}/src/include/arch/x86_64_syscall.S)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    list(APPEND KERNEL_SOURCES ${CMAKE_SOURCE_DIR}/src/include/arch/riscv_syscall.S)
endif()

target_sources(moss_kernel PRIVATE ${KERNEL_SOURCES})

# 标准包含目录配置（使用新的目录结构）
target_include_directories(moss_kernel PUBLIC
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 为源文件添加包含目录（让src目录下的cpp文件能找到头文件）
target_include_directories(moss_kernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/kernel
    ${CMAKE_SOURCE_DIR}/src/process/include
    ${CMAKE_SOURCE_DIR}/src/interrupts/include
    ${CMAKE_SOURCE_DIR}/src/drivers/include
    ${CMAKE_SOURCE_DIR}/src/ipc/include
)

# 标准编译定义
target_compile_definitions(moss_kernel PRIVATE MOSS_KERNEL_MODULE MOSS_KERNEL_CORE)

# 架构配置（使用统一函数）
moss_configure_module_arch(moss_kernel)

# 依赖关系（内核核心依赖所有模块）
target_link_libraries(moss_kernel PRIVATE moss_containers moss_mm moss_drivers moss_interrupts moss_ipc moss_process)

message(STATUS "内核核心模块配置完成")