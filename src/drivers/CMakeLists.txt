# src/drivers/CMakeLists.txt

add_library(moss_drivers OBJECT)

target_sources(moss_drivers
    PUBLIC FILE_SET CXX_MODULES FILES src/drivers.cppm)

target_compile_definitions(moss_drivers PRIVATE MOSS_DRIVERS_MODULE)

target_link_libraries(moss_drivers PRIVATE moss_core moss_aal moss_containers moss_mm moss_interrupts)

message(STATUS "驱动模块配置完成")
