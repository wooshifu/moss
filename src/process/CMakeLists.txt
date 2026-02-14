# src/process/CMakeLists.txt

# Header-only interface for modules that need process headers without circular deps
add_library(moss_process_headers INTERFACE)
target_include_directories(moss_process_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(moss_process_headers INTERFACE moss_core moss_containers)

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

target_sources(moss_process
    PUBLIC FILE_SET CXX_MODULES FILES src/process.cppm
    PRIVATE ${PROCESS_SOURCES})

target_include_directories(moss_process PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_definitions(moss_process PRIVATE MOSS_PROCESS_MODULE)

target_link_libraries(moss_process PRIVATE moss_core moss_containers moss_mm moss_drivers moss_interrupts moss_ipc
    PUBLIC moss_kernel_headers)

message(STATUS "进程模块配置完成")
