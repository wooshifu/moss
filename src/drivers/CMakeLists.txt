# src/drivers/CMakeLists.txt

add_library(moss_drivers OBJECT)
target_sources(moss_drivers PRIVATE src/drivers.cpp)

target_include_directories(moss_drivers PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_definitions(moss_drivers PRIVATE MOSS_DRIVERS_MODULE)

target_link_libraries(moss_drivers PRIVATE moss_core moss_containers moss_mm moss_interrupts)

message(STATUS "驱动模块配置完成")
