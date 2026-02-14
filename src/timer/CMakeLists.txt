add_library(moss_timer OBJECT)

target_sources(moss_timer
  PUBLIC FILE_SET CXX_MODULES FILES
    src/timer.cppm
  PRIVATE
    src/timer.cpp
)

target_link_libraries(moss_timer PRIVATE moss_core moss_aal moss_platform moss_hal_timer)
