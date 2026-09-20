add_library(moss_driver_uart OBJECT)
target_sources(moss_driver_uart PUBLIC FILE_SET CXX_MODULES FILES src/uart.cppm)
target_link_libraries(moss_driver_uart PUBLIC moss_core moss_platform)
