# src/drivers/CMakeLists.txt

# 创建驱动模块接口库（header-only）
add_library(moss_drivers INTERFACE)

# 添加头文件
set(DRIVER_HEADERS
    uart_driver.hpp
)

# 接口库不需要源文件

# 设置包含目录
target_include_directories(moss_drivers INTERFACE
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 链接依赖
target_link_libraries(moss_drivers INTERFACE
    moss_containers
    moss_mm
)

# 编译器定义
target_compile_definitions(moss_drivers INTERFACE
    MOSS_KERNEL_MODULE
    MOSS_DRIVERS_MODULE
)

# 架构特定设置
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    target_compile_definitions(moss_drivers INTERFACE MOSS_ARCH_ARM64)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    target_compile_definitions(moss_drivers INTERFACE MOSS_ARCH_X86_64)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    target_compile_definitions(moss_drivers INTERFACE MOSS_ARCH_RISCV)
endif()

message(STATUS "驱动模块配置完成")