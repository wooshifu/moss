add_library(moss_hal_uart OBJECT)

target_sources(
    moss_hal_uart
    PUBLIC FILE_SET CXX_MODULES FILES src/uart_hal.cppm
)

target_link_libraries(
    moss_hal_uart
    PUBLIC moss_core moss_platform
)
