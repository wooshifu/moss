# src/containers/CMakeLists.txt - 重构后的目录结构版本

# 创建OBJECT库（统一策略）
add_library(moss_containers OBJECT)

# 添加空源文件（满足OBJECT库需求）
target_sources(moss_containers PRIVATE src/containers.cpp)

# 标准包含目录配置（使用新的目录结构）
target_include_directories(moss_containers PUBLIC
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 标准编译定义
target_compile_definitions(moss_containers PRIVATE MOSS_KERNEL_MODULE MOSS_CONTAINERS_MODULE)

# 架构配置（使用统一函数）
moss_configure_module_arch(moss_containers)

# containers模块无其他依赖（基础层）

message(STATUS "容器模块配置完成")