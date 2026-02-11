# src/containers/CMakeLists.txt

# 创建容器模块接口库（header-only）
add_library(moss_containers INTERFACE)

# 添加头文件到接口库
set(CONTAINER_HEADERS atomic_types.hpp lockfree_queue.hpp per_cpu_data.hpp
                      rcu_list.hpp slab_allocator.hpp)

# 接口库不需要源文件

# 设置包含目录
target_include_directories(
  moss_containers INTERFACE ${CMAKE_SOURCE_DIR}/src/include
                            ${CMAKE_CURRENT_SOURCE_DIR})

# 编译器定义
target_compile_definitions(moss_containers INTERFACE MOSS_KERNEL_MODULE
                                                     MOSS_CONTAINERS_MODULE)

# 架构特定设置
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
  target_compile_definitions(moss_containers INTERFACE MOSS_ARCH_ARM64)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
  target_compile_definitions(moss_containers INTERFACE MOSS_ARCH_X86_64)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
  target_compile_definitions(moss_containers INTERFACE MOSS_ARCH_RISCV)
endif()

message(STATUS "容器模块配置完成")
