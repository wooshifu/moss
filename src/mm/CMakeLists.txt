# src/mm/CMakeLists.txt

# 创建内存管理模块 OBJECT 库
add_library(moss_mm OBJECT)

# 添加源文件
set(MM_SOURCES
    page_table.cpp
    page_frame_allocator.cpp
    runtime_heap_allocator.cpp
    kernel_memory.cpp
    mm_interface_impl.cpp
)

set(MM_HEADERS "")

target_sources(moss_mm PRIVATE ${MM_SOURCES} ${MM_HEADERS})

# 设置包含目录
target_include_directories(moss_mm PUBLIC ${CMAKE_SOURCE_DIR}/src/include)

target_include_directories(moss_mm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 链接依赖（容器库）
target_link_libraries(moss_mm PRIVATE moss_containers)

# 编译器定义
target_compile_definitions(moss_mm PRIVATE MOSS_KERNEL_MODULE MOSS_MM_MODULE)

# 架构特定设置
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
  target_compile_definitions(moss_mm PRIVATE MOSS_ARCH_ARM64 MOSS_MM_ARM64)
  # ARM64 特定的内存管理标志
  target_compile_options(moss_mm PRIVATE -mgeneral-regs-only)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
  target_compile_definitions(moss_mm PRIVATE MOSS_ARCH_X86_64 MOSS_MM_X86_64)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
  target_compile_definitions(moss_mm PRIVATE MOSS_ARCH_RISCV MOSS_MM_RISCV)
endif()

message(STATUS "内存管理模块配置完成")
