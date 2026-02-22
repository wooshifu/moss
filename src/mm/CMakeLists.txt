# src/mm/CMakeLists.txt

add_library(moss_mm OBJECT)

target_sources(moss_mm
    PUBLIC FILE_SET CXX_MODULES FILES
        src/mm.cppm
        src/mm-core.cppm
        src/mm-page_table.cppm
        src/mm-policy.cppm
        src/mm-reclaim.cppm
        src/mm-interface.cppm
    PRIVATE
        src/page_table.cpp
        src/page_fault.cpp
        src/page_frame_allocator.cpp
        src/runtime_heap_allocator.cpp
        src/mm_interface_impl.cpp)

# Architecture-specific memory management settings
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    target_compile_options(moss_mm PRIVATE -mgeneral-regs-only)
endif()

target_link_libraries(moss_mm PRIVATE moss_core moss_abi moss_aal moss_platform moss_hal_mmu moss_hal_uart moss_fdt moss_libfdt_c moss_containers moss_logging)

message(STATUS "内存管理模块配置完成")
