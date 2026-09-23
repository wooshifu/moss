add_library(moss_capability OBJECT)
target_sources(
    moss_capability
    PUBLIC FILE_SET CXX_MODULES FILES src/capability.cppm
)
target_link_libraries(
    moss_capability
    PRIVATE moss_core moss_containers
)
