# src/interrupts/CMakeLists.txt

add_library(moss_interrupts OBJECT)

target_sources(
    moss_interrupts
    PUBLIC FILE_SET CXX_MODULES FILES src/interrupts.cppm
    PRIVATE src/ipi_simple.cpp src/ipi_hardware_simple.cpp
)

target_link_libraries(
    moss_interrupts
    PUBLIC moss_core
           moss_abi
           moss_aal
           moss_platform
           moss_hal_intc
           moss_containers
           moss_logging
)

message(STATUS "中断模块配置完成")
