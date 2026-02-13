# src/containers/CMakeLists.txt

add_library(moss_containers OBJECT)
target_sources(moss_containers PRIVATE src/containers.cpp)

target_include_directories(moss_containers PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_definitions(moss_containers PRIVATE MOSS_CONTAINERS_MODULE)

message(STATUS "容器模块配置完成")
