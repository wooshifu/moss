add_library(moss_hal_intc OBJECT)

target_sources(moss_hal_intc
  PUBLIC FILE_SET CXX_MODULES FILES
    src/intc_hal.cppm
)

target_link_libraries(moss_hal_intc PUBLIC moss_core moss_aal moss_platform)
