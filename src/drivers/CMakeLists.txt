# src/drivers/CMakeLists.txt

add_library(moss_driver_console OBJECT)

target_sources(
    moss_driver_console
    PUBLIC FILE_SET CXX_MODULES FILES src/console.cppm
)

target_sources(
    moss_driver_console
    PRIVATE src/console.cpp
)

target_link_libraries(
    moss_driver_console
    PRIVATE moss_core
            moss_abi
            moss_aal
            moss_containers
            moss_interrupts
            moss_driver_uart
            moss_platform
)

# Keep the former in-kernel matching/callback framework available for its lifecycle regressions without making it part
# of the production image.
add_library(moss_drivers_validation OBJECT EXCLUDE_FROM_ALL)
target_sources(
    moss_drivers_validation
    PUBLIC FILE_SET CXX_MODULES FILES src/drivers.cppm
)
target_link_libraries(
    moss_drivers_validation
    PRIVATE moss_core
            moss_abi
            moss_aal
            moss_containers
            moss_interrupts
)

message(STATUS "驱动模块配置完成")
