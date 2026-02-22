add_library(moss_logging OBJECT)

target_sources(
    moss_logging
    PUBLIC FILE_SET CXX_MODULES FILES src/logging.cppm
)

target_link_libraries(
    moss_logging
    PUBLIC moss_core moss_hal_uart moss_aal moss_containers
)
