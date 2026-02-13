# MOSS内核现代化单元测试框架构建配置
# 完全基于 ut.hpp v2.3.1 + 自定义 freestanding 标准库实现

# ========================================================================
# Freestanding 标准库兼容层
# ========================================================================

# 创建 freestanding 标准库接口库
add_library(moss_freestanding_std INTERFACE)

# freestanding 标准库头文件
target_sources(moss_freestanding_std PUBLIC
    FILE_SET HEADERS
    BASE_DIRS freestanding_std
    FILES
        freestanding_std/type_traits.hpp
        freestanding_std/vector.hpp
        freestanding_std/string.hpp
        freestanding_std/string_view.hpp
        freestanding_std/array.hpp
        freestanding_std/unordered_map.hpp
        freestanding_std/memory.hpp
        freestanding_std/iostream.hpp
        freestanding_std/sstream.hpp
        freestanding_std/chrono.hpp
        freestanding_std/exception.hpp
        freestanding_std/unistd.hpp
        freestanding_std/sys/wait.hpp
)

# freestanding 标准库包含目录
target_include_directories(moss_freestanding_std INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/freestanding_std
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ========================================================================
# ut.hpp 现代测试框架
# ========================================================================

# ut.hpp 测试框架静态库
add_library(moss_test_framework STATIC)

# Kernel testing framework源文件 - 轻量级实现
target_sources(moss_test_framework PRIVATE
    kernel_validation_test.cpp
    # 移除复杂的freestanding std依赖文件
    # moss_ut.cpp - 不再需要复杂的ut.hpp集成
    # validation_test.cpp - 被kernel_validation_test.cpp替代
)

# Kernel testing framework头文件
target_sources(moss_test_framework PUBLIC
    FILE_SET HEADERS
    BASE_DIRS .
    FILES
        kernel_ut.hpp
        # 移除复杂的ut.hpp相关文件
        # moss_ut.hpp - 被kernel_ut.hpp替代
        # ut.hpp - 原始Boost.UT，不再需要
)

# Kernel testing framework 不需要复杂的freestanding标准库
# target_link_libraries(moss_test_framework PUBLIC moss_freestanding_std)

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
        -Wno-ctad-maybe-unsupported  # C++26 类模板参数推导
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
        ${CMAKE_CURRENT_SOURCE_DIR}/freestanding_std
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

message(STATUS "MOSS Modern Test Framework Configuration:")
message(STATUS "  - Framework: ut.hpp v2.3.1 (Boost.UT)")
message(STATUS "  - Standard Library: Custom freestanding implementation")
message(STATUS "  - Architecture: ${MOSS_TARGET_ARCH}")
message(STATUS "  - Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  - C++ Standard: C++26")
message(STATUS "  - Environment: Kernel freestanding")