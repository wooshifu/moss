# Moss compiler cache module
#
# Uses sccache as the C/C++/ASM compiler launcher when it is installed. sccache is the only supported cache: ccache
# cannot cache C++ module interface units (-x c++-module), so most of the kernel would stay uncached. The .S files
# (compiler-rt outline atomics, kernel and userspace entry code) are assembled by clang as well and cached the same way.
#
# Public API:
#   moss_enable_compiler_cache()
#
# Controls:
#   MOSS_ENABLE_SCCACHE=OFF            - never use a compiler cache
#   CMAKE_<LANG>_COMPILER_LAUNCHER     - an explicit launcher (cache variable or environment, as in CI) is kept as is
#
# Call after project(): the environment variables above are only turned into CMake variables by project().

option(MOSS_ENABLE_SCCACHE "Use sccache as the compiler launcher when it is installed" ON)

function(moss_enable_compiler_cache)
    if(NOT MOSS_ENABLE_SCCACHE)
        message(STATUS "Compiler cache disabled (MOSS_ENABLE_SCCACHE=OFF)")
        return()
    endif()

    # NO_CACHE re-detects on every configure, so uninstalling sccache never leaves a stale launcher path behind.
    find_program(_sccache sccache NO_CACHE)
    if(NOT _sccache)
        message(STATUS "sccache not found - building without a compiler cache")
        return()
    endif()

    # Only the languages without an explicit launcher are switched on.
    set(_enabled_languages "")
    foreach(_lang IN ITEMS C CXX ASM)
        if(NOT CMAKE_${_lang}_COMPILER_LAUNCHER)
            set(CMAKE_${_lang}_COMPILER_LAUNCHER "${_sccache}")
            set(CMAKE_${_lang}_COMPILER_LAUNCHER
                "${_sccache}"
                PARENT_SCOPE
            )
            list(APPEND _enabled_languages ${_lang})
        endif()
    endforeach()

    # CMake only hands CMAKE_<LANG>_COMPILER_LAUNCHER to C, CXX and a few other languages, and ignores the
    # ASM_COMPILER_LAUNCHER target property, so the ASM compile rule is prefixed directly. This covers every .S in the
    # tree, including the vendored compiler-rt. The cost is that the launcher also shows up in the ASM entries of
    # compile_commands.json (clang tooling strips a leading sccache); C and C++ entries stay untouched.
    set(CMAKE_ASM_COMPILE_OBJECT
        "${CMAKE_ASM_COMPILER_LAUNCHER} <CMAKE_ASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -o <OBJECT> -c <SOURCE>"
        PARENT_SCOPE
    )

    if(_enabled_languages)
        list(JOIN _enabled_languages ", " _enabled_languages)
        message(STATUS "Compiler cache: ${_sccache} (${_enabled_languages})")
    else()
        message(STATUS "Compiler cache: using the launchers already configured")
    endif()
endfunction()
