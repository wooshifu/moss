# src/core/CMakeLists.txt

add_library(moss_core OBJECT)

# === C++26 Modules ===
target_sources(moss_core PUBLIC FILE_SET CXX_MODULES FILES
    src/std.cppm
    src/types.cppm
    src/result.cppm
    src/concepts.cppm
    src/smart_ptr.cppm
    src/arch.cppm)
target_sources(moss_core PRIVATE src/arch_impl.cpp)

if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    target_sources(moss_core PUBLIC FILE_SET CXX_MODULES FILES
        src/arch/arm64.cppm)
endif()

message(STATUS "核心模块配置完成")
