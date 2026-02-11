# src/boot/CMakeLists.txt - 重构后的目录结构版本

# 创建OBJECT库（统一策略）
add_library(moss_boot OBJECT)

# 统一源文件（从src目录）
set(BOOT_SOURCES src/boot.cpp)

# 架构特定源文件（使用新的路径结构）
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    list(APPEND BOOT_SOURCES src/arch/arm64/boot_impl.cpp src/arch/arm64/start_arm64.S)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    list(APPEND BOOT_SOURCES src/arch/x86_64/start_x86_64.S src/arch/x86_64/boot_impl.cpp)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    list(APPEND BOOT_SOURCES src/arch/riscv/start_riscv.S src/arch/riscv/boot_impl.cpp)
endif()

target_sources(moss_boot PRIVATE ${BOOT_SOURCES})

# 标准包含目录配置（使用新的目录结构）
target_include_directories(moss_boot PUBLIC
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/mm/include
    ${CMAKE_SOURCE_DIR}/src/containers/include
)
# 为源文件添加包含目录（让src目录下的cpp文件能找到头文件）
target_include_directories(moss_boot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/boot
)

# 标准编译定义
target_compile_definitions(moss_boot PRIVATE MOSS_KERNEL_MODULE MOSS_BOOT_MODULE MOSS_UNIFIED_BOOT)

# 架构配置（使用统一函数）
moss_configure_module_arch(moss_boot)

# 内核环境编译选项
target_compile_options(moss_boot PRIVATE
    -fno-exceptions
    -fno-rtti
    -fno-stack-protector
    -ffreestanding
    -fno-builtin
)

# boot模块独立，无其他依赖

message(STATUS "启动模块配置完成")