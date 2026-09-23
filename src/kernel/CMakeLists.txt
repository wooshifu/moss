# src/kernel/CMakeLists.txt

add_library(moss_kernel OBJECT)

set(KERNEL_SOURCES src/kernel_main.cpp src/runtime_support.cpp src/syscall_table.cpp src/control_ipc.cpp)

# Architecture-specific assembly files
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    set(MOSS_UNAME_MACHINE "aarch64")
    list(APPEND KERNEL_SOURCES src/arch/arm64_syscall.S src/arch/arm64_user_program.S)
elseif(MOSS_TARGET_ARCH STREQUAL "X64")
    set(MOSS_UNAME_MACHINE "x86_64")
    list(APPEND KERNEL_SOURCES src/arch/x64_syscall.S src/arch/x64_user_program.S)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV64")
    set(MOSS_UNAME_MACHINE "riscv64")
    list(APPEND KERNEL_SOURCES src/arch/riscv64_syscall.S src/arch/riscv64_user_program.S)
endif()

target_compile_definitions(
    moss_kernel
    PRIVATE MOSS_KERNEL_RELEASE="${CMAKE_PROJECT_VERSION}"
            MOSS_KERNEL_VERSION="${CMAKE_PROJECT_VERSION}-${CMAKE_BUILD_TYPE}"
            MOSS_KERNEL_MACHINE="${MOSS_UNAME_MACHINE}"
)

target_sources(
    moss_kernel
    PUBLIC FILE_SET
           CXX_MODULES
           FILES
           src/kernel.cppm
           src/kernel-elf.cppm
           src/kernel-syscall_table.cppm
           src/kernel-syscall_arch.cppm
           src/kernel-main.cppm
    PRIVATE ${KERNEL_SOURCES}
)

target_link_libraries(
    moss_kernel
    PRIVATE moss_core
            moss_abi
            moss_aal
            moss_platform
            moss_hal_uart
            moss_hal_timer
            moss_logging
            moss_fdt
            moss_initramfs
            moss_libfdt_c
            moss_containers
            moss_mm
            moss_drivers
            moss_interrupts
            moss_capability
            moss_timer
            moss_process
            moss_vfs
            moss_boot
)

message(STATUS "内核核心模块配置完成")
