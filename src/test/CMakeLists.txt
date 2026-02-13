# MOSS内核单元测试框架构建配置
# 支持现代化 ut.hpp 测试框架和传统 MOSS 框架

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
add_library(moss_ut_framework STATIC)

# ut.hpp 框架源文件
target_sources(moss_ut_framework PRIVATE
    moss_ut.cpp
    validation_test.cpp
)

# ut.hpp 框架头文件
target_sources(moss_ut_framework PUBLIC
    FILE_SET HEADERS
    BASE_DIRS .
    FILES
        moss_ut.hpp
        moss_compat.hpp
        ut.hpp
)

# ut.hpp 框架依赖 freestanding 标准库
target_link_libraries(moss_ut_framework PUBLIC moss_freestanding_std)

# ========================================================================
# 传统 MOSS 测试框架（向后兼容）
# ========================================================================

# 测试框架静态库
add_library(moss_test_framework STATIC)

# 测试框架源文件
target_sources(moss_test_framework PRIVATE
    framework/test_framework.cpp
    framework/test_registry.cpp
    suites/test_containers.cpp
    suites/test_memory.cpp
    suites/test_scheduler.cpp
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
    ${PROJECT_SOURCE_DIR}/src/containers/include
    ${PROJECT_SOURCE_DIR}/src/process/include
)

# ========================================================================
# 通用编译选项函数（避免重复配置）
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

        # 警告选项 - 与主内核一致，但放宽测试框架相关警告
        -Weverything
        -Werror
        -Wno-c++98-compat
        -Wno-c++98-compat-pedantic
        -Wno-padded
        -Wno-unused-macros           # 测试框架宏在使用时才有意义
        -Wno-global-constructors     # 测试框架需要静态初始化
        -Wno-exit-time-destructors   # 测试框架需要全局对象
        -Wno-user-defined-literals   # 允许 ut.hpp 字面值
        -Wno-unused-private-field    # 测试代码可能有未使用字段
        -Wno-suggest-override        # 简化继承实现
        -Wno-suggest-destructor-override
        -Wno-deprecated-volatile     # 兼容性需要
        -Wno-unused-value            # 测试代码可能有未使用的值
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

    # 为调试构建添加符号信息
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${target_name} PRIVATE -g3)
    endif()

    # 通用包含目录
    target_include_directories(${target_name} PUBLIC
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/src/containers/include
        ${PROJECT_SOURCE_DIR}/src/process/include
    )
endfunction()

# 应用通用配置到所有测试目标
configure_moss_test_target(moss_test_framework)
configure_moss_test_target(moss_ut_framework)

# ========================================================================
# ut.hpp 框架特定配置
# ========================================================================

# ut.hpp 框架额外包含目录
target_include_directories(moss_ut_framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/freestanding_std
)

# ut.hpp 框架特定定义
target_compile_definitions(moss_ut_framework PUBLIC
    # 配置 ut.hpp 为 freestanding 环境
    BOOST_UT_DISABLE_MODULE=1
    BOOST_UT_DISABLE_FILE_OUTPUT=1

    # 启用 MOSS ut.hpp 集成
    MOSS_USE_UT_HPP=1
)

# ========================================================================
# 测试执行目标（可选）
# ========================================================================

# 创建测试执行器目标，整合两套测试框架
add_executable(moss_test_runner)

target_sources(moss_test_runner PRIVATE
    test_main.cpp
)

# 链接两套测试框架
target_link_libraries(moss_test_runner PRIVATE
    moss_ut_framework
    moss_test_framework
)

# 应用通用配置
configure_moss_test_target(moss_test_runner)

# 测试运行器特定配置
target_compile_definitions(moss_test_runner PRIVATE
    MOSS_TEST_RUNNER=1
    ENABLE_LEGACY_TESTS=1
    ENABLE_UT_HPP_TESTS=1
)

# ========================================================================
# 构建总结和验证
# ========================================================================

# 输出构建配置信息
message(STATUS "MOSS Test Framework Configuration:")
message(STATUS "  - ut.hpp Framework: ON (modern C++ testing)")
message(STATUS "  - Legacy Framework: ON (backward compatibility)")
message(STATUS "  - Freestanding Mode: ON (kernel environment)")
message(STATUS "  - Architecture: ${MOSS_TARGET_ARCH}")
message(STATUS "  - Build Type: ${CMAKE_BUILD_TYPE}")