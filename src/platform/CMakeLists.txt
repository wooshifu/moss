add_library(moss_platform OBJECT)

target_sources(
    moss_platform
    PUBLIC FILE_SET CXX_MODULES FILES src/platform.cppm
)

# Needs arch_detect.h from AAL and types from core
target_link_libraries(
    moss_platform
    PUBLIC moss_core moss_aal
)
