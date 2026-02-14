# src/mm/CMakeLists.txt

add_library(moss_mm OBJECT)

target_sources(moss_mm
    PUBLIC FILE_SET CXX_MODULES FILES src/mm.cppm
    PRIVATE
        src/page_table.cpp
        src/page_frame_allocator.cpp
        src/runtime_heap_allocator.cpp
        src/kernel_memory.cpp
        src/mm_interface_impl.cpp)

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

target_link_libraries(moss_mm PRIVATE moss_core moss_aal moss_platform moss_hal_mmu moss_hal_uart moss_fdt moss_libfdt_c moss_containers)

message(STATUS "内存管理模块配置完成")
