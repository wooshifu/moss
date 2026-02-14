# src/interrupts/CMakeLists.txt

add_library(moss_interrupts OBJECT)

target_sources(moss_interrupts
    PUBLIC FILE_SET CXX_MODULES FILES src/interrupts.cppm
    PRIVATE
        src/interrupts.cpp
        src/ipi_simple.cpp
        src/ipi_hardware_simple.cpp)

target_compile_definitions(moss_interrupts PRIVATE MOSS_INTERRUPTS_MODULE)

target_link_libraries(moss_interrupts PUBLIC moss_core moss_containers)

message(STATUS "中断模块配置完成")
