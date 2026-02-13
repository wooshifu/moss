# src/core/CMakeLists.txt

add_library(moss_core OBJECT)
target_sources(moss_core PRIVATE src/core.cpp)

target_include_directories(moss_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

message(STATUS "核心模块配置完成")
