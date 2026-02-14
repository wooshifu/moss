# src/aal/CMakeLists.txt — Architecture Abstraction Layer
#
# Provides module moss.arch with inline implementations for all
# supported architectures.  The correct code path is selected at
# compile time via arch_detect.h macros.

add_library(moss_aal OBJECT)

# The single module file contains all arch-specific inline asm,
# selected by #ifdef at compile time — no per-arch source split needed.
target_sources(moss_aal PUBLIC FILE_SET CXX_MODULES FILES
    src/arch.cppm)

# Make arch_detect.h findable via #include "arch_detect.h"
target_include_directories(moss_aal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# AAL depends on core for moss.std and moss.types
target_link_libraries(moss_aal PUBLIC moss_core)

message(STATUS "AAL (Architecture Abstraction Layer) configured")
