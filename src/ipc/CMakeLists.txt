# src/ipc/CMakeLists.txt - 重构后的目录结构版本

# 创建OBJECT库（统一策略）
add_library(moss_ipc OBJECT)

# 添加空源文件（满足OBJECT库需求）
target_sources(moss_ipc PRIVATE src/ipc.cpp)

# 标准包含目录配置（使用新的目录结构）
target_include_directories(moss_ipc PUBLIC
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 标准编译定义
target_compile_definitions(moss_ipc PRIVATE MOSS_KERNEL_MODULE MOSS_IPC_MODULE)

# 架构配置（使用统一函数）
moss_configure_module_arch(moss_ipc)

# 依赖关系（IPC层依赖基础层和内存管理层）
target_link_libraries(moss_ipc PRIVATE moss_containers moss_mm)

message(STATUS "IPC模块配置完成")