# src/mm/CMakeLists.txt

# Header-only interface for modules that need mm headers without circular deps
add_library(moss_mm_headers INTERFACE)
target_include_directories(moss_mm_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(moss_mm_headers INTERFACE moss_core)

add_library(moss_mm OBJECT)

set(MM_SOURCES
    src/page_table.cpp
    src/page_frame_allocator.cpp
    src/runtime_heap_allocator.cpp
    src/kernel_memory.cpp
    src/mm_interface_impl.cpp)
target_sources(moss_mm PRIVATE ${MM_SOURCES})

target_include_directories(moss_mm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_definitions(moss_mm PRIVATE MOSS_MM_MODULE)

# Architecture-specific memory management settings
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    target_compile_definitions(moss_mm PRIVATE MOSS_MM_ARM64)
    target_compile_options(moss_mm PRIVATE -mgeneral-regs-only)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    target_compile_definitions(moss_mm PRIVATE MOSS_MM_X86_64)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    target_compile_definitions(moss_mm PRIVATE MOSS_MM_RISCV)
endif()

target_link_libraries(moss_mm PRIVATE moss_core moss_containers)

message(STATUS "内存管理模块配置完成")
