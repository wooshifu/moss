# src/initramfs/CMakeLists.txt
# initramfs CPIO parser module

add_library(moss_initramfs OBJECT)

target_sources(moss_initramfs
    PUBLIC FILE_SET CXX_MODULES FILES
        src/initramfs.cppm)

target_link_libraries(moss_initramfs PRIVATE moss_core moss_logging)

message(STATUS "Initramfs module configured")
