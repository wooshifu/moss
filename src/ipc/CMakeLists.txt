# src/ipc/CMakeLists.txt

# Historical IPC algorithms remain available to the validation image while
# production uses capability-backed control IPC and Memory Objects.
add_library(moss_ipc_validation OBJECT)
target_sources(
    moss_ipc_validation
    PUBLIC FILE_SET CXX_MODULES FILES src/ipc.cppm
)

target_link_libraries(
    moss_ipc_validation
    PRIVATE moss_core
            moss_abi
            moss_aal
            moss_containers
            moss_mm
)

message(STATUS "Legacy IPC validation fixture configured")
