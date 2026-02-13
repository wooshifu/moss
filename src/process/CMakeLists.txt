# src/process/CMakeLists.txt

add_library(moss_process OBJECT)

set(PROCESS_SOURCES
    src/process.cpp
    src/idle_process.cpp)

# Architecture-specific assembly files
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    list(APPEND PROCESS_SOURCES src/context_switch.S)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    list(APPEND PROCESS_SOURCES src/arch/x86_64/context_switch.S)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    list(APPEND PROCESS_SOURCES src/arch/riscv/context_switch.S)
endif()

target_sources(moss_process PRIVATE ${PROCESS_SOURCES})

target_include_directories(moss_process PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_definitions(moss_process PRIVATE MOSS_PROCESS_MODULE)

target_link_libraries(moss_process PRIVATE moss_containers moss_mm moss_drivers moss_interrupts moss_ipc
    PUBLIC moss_kernel_headers)

message(STATUS "进程模块配置完成")
