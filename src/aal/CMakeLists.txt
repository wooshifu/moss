# src/aal/CMakeLists.txt — Architecture Abstraction Layer
#
# Provides module moss.arch with inline implementations for all
# supported architectures.  The correct code path is selected at
# compile time via CMake-injected MOSS_ARCH_* macros.

add_library(moss_aal OBJECT)

# The single module file contains all arch-specific inline asm, selected by #ifdef at compile time — no per-arch source
# split needed.
target_sources(
    moss_aal
    PUBLIC FILE_SET CXX_MODULES FILES src/arch.cppm
)

# AAL depends on core for moss.std and moss.types
target_link_libraries(
    moss_aal
    PUBLIC moss_core moss_platform moss_hal_uart
)

message(STATUS "AAL (Architecture Abstraction Layer) configured")
