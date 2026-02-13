# src/core/CMakeLists.txt

add_library(moss_core OBJECT)
target_sources(moss_core PRIVATE src/core.cpp)

target_include_directories(moss_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# === C++26 Modules ===

moss_add_module(moss_std SOURCES modules/std.cppm)
set_target_properties(moss_std PROPERTIES EXPORT_NAME "moss.std")

moss_add_module(moss_types SOURCES modules/types.cppm DEPENDENCIES moss_std)
set_target_properties(moss_types PROPERTIES EXPORT_NAME "moss.types")

moss_add_module(moss_result SOURCES modules/result.cppm
    DEPENDENCIES moss_std moss_types)
set_target_properties(moss_result PROPERTIES EXPORT_NAME "moss.result")

# Architecture-specific module
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    moss_add_module(moss_arch_arm64 SOURCES modules/arch/arm64.cppm
        DEPENDENCIES moss_std moss_types)
    set_target_properties(moss_arch_arm64 PROPERTIES EXPORT_NAME
        "moss.arch.arm64")
endif()

moss_add_module(moss_arch SOURCES modules/arch.cppm modules/arch_impl.cpp
    DEPENDENCIES moss_std moss_types moss_arch_arm64)
set_target_properties(moss_arch PROPERTIES EXPORT_NAME "moss.arch")

if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    target_link_libraries(moss_arch PUBLIC moss_arch_arm64)
endif()

message(STATUS "核心模块配置完成")
