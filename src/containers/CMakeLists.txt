# src/containers/CMakeLists.txt

# 创建容器模块 OBJECT 库
add_library(moss_containers OBJECT)

# 添加头文件源（header-only库）
set(CONTAINER_HEADERS
    atomic_types.hpp
    lockfree_queue.hpp
    per_cpu_data.hpp
    rcu_list.hpp
    slab_allocator.hpp
)

# 如果有实现文件，添加这里
set(CONTAINER_SOURCES "")

# 将头文件作为源文件添加到库中（确保它们被包含在构建中）
target_sources(moss_containers PRIVATE ${CONTAINER_HEADERS})

# 如果有源文件，添加它们
if(CONTAINER_SOURCES)
    target_sources(moss_containers PRIVATE ${CONTAINER_SOURCES})
endif()

# 设置包含目录
target_include_directories(moss_containers PUBLIC
    ${CMAKE_SOURCE_DIR}/src/include
)

target_include_directories(moss_containers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 编译器定义
target_compile_definitions(moss_containers PRIVATE
    MOSS_KERNEL_MODULE
    MOSS_CONTAINERS_MODULE
)

# 架构特定设置
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    target_compile_definitions(moss_containers PRIVATE MOSS_ARCH_ARM64)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    target_compile_definitions(moss_containers PRIVATE MOSS_ARCH_X86_64)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    target_compile_definitions(moss_containers PRIVATE MOSS_ARCH_RISCV)
endif()

message(STATUS "容器模块配置完成")