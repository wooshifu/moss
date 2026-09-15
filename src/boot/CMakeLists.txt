# src/boot/CMakeLists.txt

set(BOOT_SOURCES src/boot.cpp)
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    list(APPEND BOOT_SOURCES src/arch/arm64/boot_impl.cpp src/arch/arm64/start_arm64.S)
elseif(MOSS_TARGET_ARCH STREQUAL "X64")
    list(
        APPEND
        BOOT_SOURCES
        src/arch/x64/start_x64.S
        src/arch/x64/isr_x64.S
        src/arch/x64/boot_impl.cpp
    )
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV64")
    list(APPEND BOOT_SOURCES src/arch/riscv64/start_riscv64.S src/arch/riscv64/boot_impl.cpp)
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
