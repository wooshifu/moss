# MOSS内核现代化单元测试框架构建配置
# 基于精简的内核优化 ut.hpp 实现

# ========================================================================
# 内核优化测试框架
# ========================================================================

# 创建内核测试框架静态库
add_library(moss_test_framework STATIC)

# 内核测试源文件
target_sources(moss_test_framework PRIVATE
    test_main.cpp
    kernel_modern_validation.cpp
    boost_ut_demo.cpp
)

# 内核测试头文件
target_sources(moss_test_framework PUBLIC
    FILE_SET HEADERS
    BASE_DIRS .
    FILES
        ut_kernel.hpp
        moss_ut.hpp
)

# ========================================================================
# 编译选项配置
# ========================================================================

function(configure_moss_test_target target_name)
    # 编译选项 - 与主内核保持一致的严格模式
    target_compile_options(${target_name} PRIVATE
        # 基础编译选项
        -std=c++26
        -ffreestanding
        -fno-exceptions
        -fno-rtti
        -fno-stack-protector
        -fno-omit-frame-pointer

        # 警告选项 - 针对 ut.hpp 优化
        -Weverything
        -Werror
        -Wno-c++98-compat
        -Wno-c++98-compat-pedantic
        -Wno-padded
        -Wno-unused-macros           # ut.hpp 宏使用
        -Wno-global-constructors     # ut.hpp 静态初始化
        -Wno-exit-time-destructors   # ut.hpp 全局对象
        -Wno-user-defined-literals   # ut.hpp 字面值操作符
        -Wno-unused-private-field    # 现代测试代码风格
        -Wno-suggest-override        # 简化模板实现
        -Wno-suggest-destructor-override
        -Wno-deprecated-volatile     # 兼容性
        -Wno-unused-value            # ut.hpp 表达式模板
        -Wno-ctad-maybe-unsupported  # 类模板参数推导 (ut.hpp合法使用)
        -Wno-unused-variable         # 测试变量可能未使用
    )

    # 架构特定定义
    if(MOSS_TARGET_ARCH STREQUAL "arm64")
        target_compile_definitions(${target_name} PUBLIC MOSS_ARCH_ARM64=1)
    elseif(MOSS_TARGET_ARCH STREQUAL "x86_64")
        target_compile_definitions(${target_name} PUBLIC MOSS_ARCH_X86_64=1)
    elseif(MOSS_TARGET_ARCH STREQUAL "riscv")
        target_compile_definitions(${target_name} PUBLIC MOSS_ARCH_RISCV=1)
    endif()

    # 链接选项
    target_link_options(${target_name} PUBLIC
        -ffreestanding
        -nostdlib
        -static
    )

    # 调试构建符号信息
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${target_name} PRIVATE -g3)
    endif()

    # 包含目录
    target_include_directories(${target_name} PUBLIC
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/src/containers/include
        ${PROJECT_SOURCE_DIR}/src/process/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
endfunction()

# 应用配置到测试框架
configure_moss_test_target(moss_test_framework)

# ========================================================================
# ut.hpp 特定配置
# ========================================================================

# ut.hpp 编译时配置
target_compile_definitions(moss_test_framework PUBLIC
    # 配置 ut.hpp 为 freestanding 环境
    BOOST_UT_DISABLE_MODULE=1
    BOOST_UT_DISABLE_FILE_OUTPUT=1

    # 启用 MOSS 内核集成
    MOSS_USE_UT_HPP=1
    MOSS_FREESTANDING_MODE=1
)

# ========================================================================
# 构建信息输出
# ========================================================================

message(STATUS "MOSS Kernel-Optimized Test Framework Configuration:")
message(STATUS "  - Framework: Kernel-optimized ut.hpp")
message(STATUS "  - Standard Library: None (pure freestanding)")
message(STATUS "  - Architecture: ${MOSS_TARGET_ARCH}")
message(STATUS "  - Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  - C++ Standard: C++26")
message(STATUS "  - Environment: Kernel freestanding")

# ========================================================================
# Modern Test Framework (boost::ut compatible)
# ========================================================================

add_subdirectory(modern)