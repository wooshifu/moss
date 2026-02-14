# src/ipc/CMakeLists.txt

add_library(moss_ipc OBJECT)
target_sources(moss_ipc
    PUBLIC FILE_SET CXX_MODULES FILES src/ipc.cppm)

target_compile_definitions(moss_ipc PRIVATE MOSS_IPC_MODULE)

target_link_libraries(moss_ipc PRIVATE moss_core moss_containers moss_mm)

message(STATUS "IPC模块配置完成")
