# src/interrupts/CMakeLists.txt

add_library(moss_interrupts OBJECT)

target_sources(moss_interrupts PRIVATE
    src/interrupts.cpp
    src/ipi_simple.cpp
    src/ipi_hardware_simple.cpp)

target_include_directories(moss_interrupts PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_definitions(moss_interrupts PRIVATE MOSS_INTERRUPTS_MODULE)

target_link_libraries(moss_interrupts PRIVATE moss_core moss_containers)

message(STATUS "中断模块配置完成")
