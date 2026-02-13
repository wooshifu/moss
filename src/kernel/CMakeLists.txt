# src/kernel/CMakeLists.txt

# Header-only interface for modules that need kernel headers without circular deps
add_library(moss_kernel_headers INTERFACE)
target_include_directories(moss_kernel_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(moss_kernel OBJECT)

set(KERNEL_SOURCES
    src/kernel_main.cpp
    src/runtime_support.cpp
    src/syscall_table.cpp
    src/elf_loader.cpp)

# Architecture-specific assembly files
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    list(APPEND KERNEL_SOURCES ${CMAKE_SOURCE_DIR}/src/include/arch/arm64_syscall.S)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    list(APPEND KERNEL_SOURCES ${CMAKE_SOURCE_DIR}/src/include/arch/x86_64_syscall.S)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    list(APPEND KERNEL_SOURCES ${CMAKE_SOURCE_DIR}/src/include/arch/riscv_syscall.S)
endif()

target_sources(moss_kernel PRIVATE ${KERNEL_SOURCES})

target_include_directories(moss_kernel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_definitions(moss_kernel PRIVATE MOSS_KERNEL_CORE)

target_link_libraries(moss_kernel PRIVATE moss_containers moss_mm moss_drivers moss_interrupts moss_ipc moss_process)

message(STATUS "内核核心模块配置完成")
