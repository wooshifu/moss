# src/boot/CMakeLists.txt

set(BOOT_SOURCES src/boot.cpp)
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    list(APPEND BOOT_SOURCES src/arch/arm64/boot_impl.cpp src/arch/arm64/start_arm64.S)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    list(
        APPEND
        BOOT_SOURCES
        src/arch/x86_64/start_x86_64.S
        src/arch/x86_64/isr_x86_64.S
        src/arch/x86_64/boot_impl.cpp
    )
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    list(APPEND BOOT_SOURCES src/arch/riscv/start_riscv.S src/arch/riscv/boot_impl.cpp)
endif()
add_library(moss_boot OBJECT ${BOOT_SOURCES})

target_sources(
    moss_boot
    PUBLIC FILE_SET CXX_MODULES FILES src/boot.cppm
)

target_link_libraries(
    moss_boot
    PRIVATE moss_core
            moss_abi
            moss_aal
            moss_platform
            moss_fdt
            moss_libfdt_c
            moss_mm
            moss_containers
            moss_process
            moss_interrupts
)

message(STATUS "Boot module configured")
