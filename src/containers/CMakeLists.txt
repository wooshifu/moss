# src/containers/CMakeLists.txt

add_library(moss_containers OBJECT)

target_sources(moss_containers
    PUBLIC FILE_SET CXX_MODULES FILES src/containers.cppm
    PRIVATE src/containers_defs.cpp)

target_compile_definitions(moss_containers PRIVATE MOSS_CONTAINERS_MODULE)

# arch_detect.h is in src/aal/ (provided via moss_aal's PUBLIC include dirs)

target_link_libraries(moss_containers PUBLIC moss_core moss_aal)

message(STATUS "容器模块配置完成")
