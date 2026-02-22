# src/process/CMakeLists.txt

add_library(moss_process OBJECT)

set(PROCESS_SOURCES src/process.cpp src/idle_process.cpp)

# Architecture-specific assembly files
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    list(APPEND PROCESS_SOURCES src/arch/arm64/context_switch.S)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    list(APPEND PROCESS_SOURCES src/arch/x86_64/context_switch.S)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    list(APPEND PROCESS_SOURCES src/arch/riscv/context_switch.S)
endif()

target_sources(
    moss_process
    PUBLIC FILE_SET
           CXX_MODULES
           FILES
           src/process.cppm
           src/process-types.cppm
           src/process-scheduler.cppm
           src/process-load_balancer.cppm
    PRIVATE ${PROCESS_SOURCES}
)

target_link_libraries(
    moss_process
    PRIVATE moss_core
            moss_abi
            moss_aal
            moss_containers
            moss_mm
            moss_drivers
            moss_interrupts
            moss_ipc
            moss_timer
            moss_hal_timer
            moss_logging
            moss_platform
)

message(STATUS "进程模块配置完成")
