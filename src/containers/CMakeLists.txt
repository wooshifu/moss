# src/containers/CMakeLists.txt

add_library(moss_containers OBJECT)

target_sources(moss_containers
    PUBLIC FILE_SET CXX_MODULES FILES src/containers.cppm)

target_compile_definitions(moss_containers PRIVATE MOSS_CONTAINERS_MODULE)

target_link_libraries(moss_containers PUBLIC moss_core moss_abi moss_aal)

message(STATUS "容器模块配置完成")
