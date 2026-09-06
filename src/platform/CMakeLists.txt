add_library(moss_platform OBJECT)

target_sources(
    moss_platform
    PUBLIC FILE_SET CXX_MODULES FILES src/platform.cppm
)

# Firmware data has no dependency on architecture operations.
target_link_libraries(
    moss_platform
    PUBLIC moss_core
)
