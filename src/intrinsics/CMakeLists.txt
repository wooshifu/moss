# MOSS Kernel - Intrinsics Module
#
# Provides type-safe wrappers around Clang compiler intrinsics.
# This is the foundation module with zero dependencies.

add_library(moss_intrinsics OBJECT)

target_sources(
    moss_intrinsics
    PUBLIC FILE_SET
           CXX_MODULES
           FILES
           src/intrinsics.cppm
           src/intrinsics-bitops.cppm
           src/intrinsics-bswap.cppm
           src/intrinsics-memory.cppm
           src/intrinsics-atomic.cppm
           src/intrinsics-source.cppm
           src/intrinsics-traits.cppm
           src/intrinsics-control.cppm
)

target_compile_features(moss_intrinsics PUBLIC cxx_std_26)

# No external dependencies - intrinsics is the foundation layer It only uses compiler builtins and basic types
# (__SIZE_TYPE__ etc.)
