# src/interrupts/CMakeLists.txt - 重构后的目录结构版本

# 创建OBJECT库（统一策略）
add_library(moss_interrupts OBJECT)

# 添加源文件
target_sources(moss_interrupts PRIVATE
    src/interrupts.cpp
    src/ipi_simple.cpp
)

# 标准包含目录配置（使用新的目录结构）
target_include_directories(moss_interrupts PUBLIC
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 标准编译定义
target_compile_definitions(moss_interrupts PRIVATE MOSS_KERNEL_MODULE MOSS_INTERRUPTS_MODULE)

# 架构配置（使用统一函数）
moss_configure_module_arch(moss_interrupts)

# 依赖关系（中断层依赖基础容器层）
target_link_libraries(moss_interrupts PRIVATE moss_containers)

message(STATUS "中断模块配置完成")