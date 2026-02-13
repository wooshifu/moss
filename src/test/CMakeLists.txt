# MOSS内核单元测试框架构建配置

# 测试框架静态库
add_library(moss_test_framework STATIC)

# 测试框架源文件
target_sources(moss_test_framework PRIVATE
    framework/test_framework.cpp
    framework/test_registry.cpp
    suites/test_containers.cpp
    suites/test_memory.cpp
)

# 测试框架头文件
target_sources(moss_test_framework PUBLIC
    FILE_SET HEADERS
    BASE_DIRS framework
    FILES
        framework/test_framework.hpp
        framework/test_registry.hpp
)

# 包含目录
target_include_directories(moss_test_framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
    ${PROJECT_SOURCE_DIR}/src/include
)

# 编译选项 - 与主内核保持一致的严格模式
target_compile_options(moss_test_framework PRIVATE
    # 基础编译选项
    -std=c++26
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-stack-protector
    -fno-omit-frame-pointer

    # 警告选项 - 与主内核一致
    -Weverything
    -Werror
    -Wno-c++98-compat
    -Wno-c++98-compat-pedantic
    -Wno-padded
    -Wno-unused-macros  # 测试框架宏在使用时才有意义
    -Wno-global-constructors
    -Wno-exit-time-destructors
)

# 架构特定定义
if(MOSS_TARGET_ARCH STREQUAL "arm64")
    target_compile_definitions(moss_test_framework PUBLIC MOSS_ARCH_ARM64=1)
elseif(MOSS_TARGET_ARCH STREQUAL "x86_64")
    target_compile_definitions(moss_test_framework PUBLIC MOSS_ARCH_X86_64=1)
elseif(MOSS_TARGET_ARCH STREQUAL "riscv")
    target_compile_definitions(moss_test_framework PUBLIC MOSS_ARCH_RISCV=1)
endif()

# 链接选项
target_link_options(moss_test_framework PUBLIC
    -ffreestanding
    -nostdlib
    -static
)

# 为调试构建添加符号信息
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(moss_test_framework PRIVATE -g3)
endif()

# 依赖关系 - 测试框架需要访问内核基础设施
# 注意: 这里不链接具体模块，避免循环依赖，只使用头文件