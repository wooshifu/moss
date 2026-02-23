# src/core/CMakeLists.txt
#
# Core types and utilities — zero hardware dependency.
# Architecture abstraction (moss.arch) has moved to src/aal/.

add_library(moss_core OBJECT)

# === C++26 Modules ===
target_sources(
    moss_core
    PUBLIC FILE_SET
           CXX_MODULES
           FILES
           src/std.cppm
           src/types.cppm
           src/result.cppm
           src/concepts.cppm
           src/smart_ptr.cppm
)

# === Dependencies ===
# Depend on intrinsics module (zero-dependency foundation)
target_link_libraries(
    moss_core
    PRIVATE moss_intrinsics
)

message(STATUS "Core module configured (arch moved to AAL)")
