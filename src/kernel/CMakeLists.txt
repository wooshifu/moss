# src/kernel/CMakeLists.txt

add_library(moss_kernel OBJECT)

set(KERNEL_SOURCES src/kernel_main.cpp src/runtime_support.cpp src/syscall_table.cpp src/cpu-topology.cpp)

# Architecture-specific assembly files
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    list(APPEND KERNEL_SOURCES src/arch/arm64_syscall.S src/arch/arm64_user_program.S)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    list(APPEND KERNEL_SOURCES src/arch/x86_64_syscall.S)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    list(APPEND KERNEL_SOURCES src/arch/riscv_syscall.S)
endif()

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
           src/cpu-topology.cppm
           src/cpu-mask.cppm
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
            moss_ipc
            moss_timer
            moss_process
            moss_vfs
            moss_boot
)

message(STATUS "内核核心模块配置完成")
