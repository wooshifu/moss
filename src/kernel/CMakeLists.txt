# src/kernel/CMakeLists.txt

# 创建内核核心模块 OBJECT 库
add_library(moss_kernel OBJECT)

# 添加源文件
set(KERNEL_SOURCES
    kernel_main.cpp
    runtime_support.cpp
)

set(KERNEL_HEADERS
    kernel_main.hpp
)

# 添加源文件
target_sources(moss_kernel PRIVATE
    ${KERNEL_SOURCES}
    ${KERNEL_HEADERS}
)

# 设置包含目录
target_include_directories(moss_kernel PUBLIC
    ${CMAKE_SOURCE_DIR}/src/include
)

target_include_directories(moss_kernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/process
    ${CMAKE_SOURCE_DIR}/src/interrupts
    ${CMAKE_SOURCE_DIR}/src/drivers
    ${CMAKE_SOURCE_DIR}/src/ipc
)

# 链接依赖（内核核心依赖所有模块）
target_link_libraries(moss_kernel PRIVATE
    moss_containers
    moss_mm
    moss_drivers
    moss_interrupts
    moss_ipc
    moss_process
)

# 编译器定义
target_compile_definitions(moss_kernel PRIVATE
    MOSS_KERNEL_MODULE
    MOSS_KERNEL_CORE
)

# 架构特定设置
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    target_compile_definitions(moss_kernel PRIVATE MOSS_ARCH_ARM64)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    target_compile_definitions(moss_kernel PRIVATE MOSS_ARCH_X86_64)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    target_compile_definitions(moss_kernel PRIVATE MOSS_ARCH_RISCV)
endif()

message(STATUS "内核核心模块配置完成")