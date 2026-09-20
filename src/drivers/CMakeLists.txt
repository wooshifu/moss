# src/drivers/CMakeLists.txt

add_library(moss_drivers OBJECT)

target_sources(
    moss_drivers
    PUBLIC FILE_SET CXX_MODULES FILES src/drivers.cppm src/console.cppm
)

target_sources(moss_drivers PRIVATE src/console.cpp)

target_link_libraries(
    moss_drivers
    PRIVATE moss_core
            moss_abi
            moss_aal
            moss_containers
            moss_mm
            moss_interrupts
            moss_driver_uart
            moss_driver_irqchip
            moss_driver_timer
            moss_platform
            moss_timer
)

message(STATUS "驱动模块配置完成")
