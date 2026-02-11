# src/mm/CMakeLists.txt - 重构后的目录结构版本

# 创建OBJECT库（已是OBJECT库）
add_library(moss_mm OBJECT)

# 添加源文件（从src目录）
set(MM_SOURCES
    src/page_table.cpp
    src/page_frame_allocator.cpp
    src/runtime_heap_allocator.cpp
    src/kernel_memory.cpp
    src/mm_interface_impl.cpp
)
target_sources(moss_mm PRIVATE ${MM_SOURCES})

# 标准包含目录配置（使用新的目录结构）
target_include_directories(moss_mm PUBLIC
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# 为源文件添加包含目录（让src目录下的cpp文件能找到头文件）
target_include_directories(moss_mm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/mm
)

# 标准编译定义
target_compile_definitions(moss_mm PRIVATE MOSS_KERNEL_MODULE MOSS_MM_MODULE)

# 架构配置（使用统一函数）
moss_configure_module_arch(moss_mm)

# 架构特定的内存管理设置
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    target_compile_definitions(moss_mm PRIVATE MOSS_MM_ARM64)
    target_compile_options(moss_mm PRIVATE -mgeneral-regs-only)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    target_compile_definitions(moss_mm PRIVATE MOSS_MM_X86_64)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    target_compile_definitions(moss_mm PRIVATE MOSS_MM_RISCV)
endif()

# 依赖关系（内存管理依赖基础容器层）
target_link_libraries(moss_mm PRIVATE moss_containers)

message(STATUS "内存管理模块配置完成")