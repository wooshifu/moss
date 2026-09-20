add_library(moss_hal_timer OBJECT)

target_sources(
    moss_hal_timer
    PUBLIC FILE_SET CXX_MODULES FILES src/timer_hal.cppm
)

target_link_libraries(
    moss_hal_timer
    PUBLIC moss_driver_timer moss_core moss_aal moss_platform
)
