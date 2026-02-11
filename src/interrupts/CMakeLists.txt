# src/interrupts/CMakeLists.txt

# 创建中断模块接口库（header-only）
add_library(moss_interrupts INTERFACE)

# 添加头文件
set(INTERRUPT_HEADERS gic.hpp)

# 接口库不需要源文件

# 设置包含目录
target_include_directories(
  moss_interrupts INTERFACE ${CMAKE_SOURCE_DIR}/src/include
                            ${CMAKE_CURRENT_SOURCE_DIR})

# 链接依赖
target_link_libraries(moss_interrupts INTERFACE moss_containers)

# 编译器定义
target_compile_definitions(moss_interrupts INTERFACE MOSS_KERNEL_MODULE
                                                     MOSS_INTERRUPTS_MODULE)

# 架构特定设置
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
  target_compile_definitions(moss_interrupts INTERFACE MOSS_ARCH_ARM64)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
  target_compile_definitions(moss_interrupts INTERFACE MOSS_ARCH_X86_64)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
  target_compile_definitions(moss_interrupts INTERFACE MOSS_ARCH_RISCV)
endif()

message(STATUS "中断模块配置完成")
