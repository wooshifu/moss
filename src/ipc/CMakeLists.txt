# src/ipc/CMakeLists.txt

add_library(moss_ipc OBJECT)
target_sources(moss_ipc PRIVATE src/ipc.cpp)

target_include_directories(moss_ipc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_definitions(moss_ipc PRIVATE MOSS_IPC_MODULE)

target_link_libraries(moss_ipc PRIVATE moss_core moss_containers moss_mm
    PUBLIC moss_process_headers)

message(STATUS "IPC模块配置完成")
