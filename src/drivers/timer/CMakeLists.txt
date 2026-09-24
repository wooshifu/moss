add_library(moss_driver_timer OBJECT)
target_sources(
    moss_driver_timer
    PUBLIC FILE_SET CXX_MODULES FILES src/timer.cppm
)
target_link_libraries(
    moss_driver_timer
    PUBLIC moss_core moss_aal moss_platform
)
