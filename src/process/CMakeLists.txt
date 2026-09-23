# src/process/CMakeLists.txt

add_library(moss_process OBJECT)

set(PROCESS_SOURCES src/process.cpp src/idle_process.cpp src/signal.cpp src/user_access.cpp)

# Architecture-specific assembly files
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    list(APPEND PROCESS_SOURCES src/arch/arm64/context_switch.S)
elseif(MOSS_TARGET_ARCH STREQUAL "X64")
    list(APPEND PROCESS_SOURCES src/arch/x64/context_switch.S src/arch/x64/user_trampoline.S)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV64")
    list(APPEND PROCESS_SOURCES src/arch/riscv64/context_switch.S src/arch/riscv64/user_trampoline.S)
endif()

target_sources(
    moss_process
    PUBLIC FILE_SET
           CXX_MODULES
           FILES
           src/process.cppm
           src/process-types.cppm
           src/process-signal.cppm
           src/process-uaccess.cppm
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
            moss_capability
            moss_interrupts
            moss_timer
            moss_hal_timer
            moss_logging
            moss_platform
            moss_vfs
)

message(STATUS "进程模块配置完成")
