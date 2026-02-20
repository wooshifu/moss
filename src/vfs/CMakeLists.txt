# src/vfs/CMakeLists.txt

add_library(moss_vfs OBJECT)

target_sources(moss_vfs
    PUBLIC FILE_SET CXX_MODULES FILES
        src/vfs.cppm
        src/vfs-types.cppm
        src/vfs-inode.cppm
        src/vfs-dcache.cppm
        src/vfs-file.cppm
        src/vfs-mount.cppm
        src/vfs-devfs.cppm
        src/vfs-ramfs.cppm
        src/vfs-pipefs.cppm
        src/vfs-syscall.cppm
    PRIVATE
        src/vfs_init.cpp
        src/vfs_path.cpp
        src/vfs_syscall.cpp)

target_compile_definitions(moss_vfs PRIVATE MOSS_VFS_MODULE)

target_link_libraries(moss_vfs PRIVATE moss_core moss_aal moss_containers moss_mm moss_logging moss_initramfs moss_hal_uart)

message(STATUS "VFS module configured")
