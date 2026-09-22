# Native CMake port of the pinned static Moss profile. Upstream Meson is reference only.
include(moss-sources.cmake)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(__MLIBC_POSIX_OPTION 1)
set(__MLIBC_GLIBC_OPTION 1)
set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS mlibc-config.h.in
)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/mlibc-config.h.in" config)
string(REPLACE "#mesondefine " "#cmakedefine01 " config "${config}")
file(
    CONFIGURE
    OUTPUT
    "${MLIBC_SYSROOT}/usr/include/mlibc-config.h"
    CONTENT
    "${config}"
    @ONLY
)
configure_file(moss-internal-config.h.in "${MLIBC_GENERATED}/internal-config.h" @ONLY)
foreach(header IN LISTS MLIBC_PUBLIC_HEADERS)
    string(REGEX REPLACE "^.*include/" "" destination "${header}")
    set(header_source "${CMAKE_CURRENT_SOURCE_DIR}/${header}")
    # Git represents mlibc's abi-bits as symlinks.  With core.symlinks=false,
    # Windows checks them out as one-line files containing the link target.
    if(CMAKE_HOST_WIN32 AND header MATCHES "^sysdeps/moss/include/abi-bits/" AND NOT IS_SYMLINK "${header_source}")
        file(READ "${header_source}" link_target LIMIT 512)
        string(STRIP "${link_target}" link_target)
        if(link_target MATCHES "^\\.\\./.*\\.h$")
            get_filename_component(header_directory "${header_source}" DIRECTORY)
            cmake_path(
                ABSOLUTE_PATH
                link_target
                BASE_DIRECTORY
                "${header_directory}"
                NORMALIZE
                OUTPUT_VARIABLE
                header_source
            )
            if(NOT EXISTS "${header_source}")
                message(FATAL_ERROR "Broken mlibc ABI header link: ${header} -> ${link_target}")
            endif()
        endif()
    endif()
    configure_file("${header_source}" "${MLIBC_SYSROOT}/usr/include/${destination}" COPYONLY)
endforeach()
add_library(mlibc-math OBJECT ${MLIBC_MATH_SOURCES})
add_library(mlibc-regex OBJECT ${MLIBC_REGEX_SOURCES})
add_library(mlibc-rtld OBJECT ${MLIBC_RTLD_SOURCES})
add_library(
    mlibc STATIC ${MLIBC_LIBC_SOURCES} $<TARGET_OBJECTS:mlibc-math> $<TARGET_OBJECTS:mlibc-regex>
                 $<TARGET_OBJECTS:mlibc-rtld>
)
set_target_properties(mlibc PROPERTIES OUTPUT_NAME c ARCHIVE_OUTPUT_DIRECTORY "${MLIBC_SYSROOT}/usr/lib")
foreach(target mlibc mlibc-math mlibc-regex mlibc-rtld)
    # Use the vendored freestanding C++ headers without host libc++ wrappers.
    target_compile_options(
        ${target}
        PRIVATE ${MLIBC_ARCH_FLAGS}
                -O2
                -g
                -fPIC
                -ffreestanding
                -fno-builtin
                -fno-stack-protector
                -Wno-unknown-pragmas
                -Wno-unused-function
                "$<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>"
                "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti;-fno-exceptions;-fno-sanitize=function;-Wno-vla-cxx-extension>"
                "-idirafter${MOSS_THIRD_PARTY}/freestnd-c-hdrs/${MLIBC_CPU}/include"
    )
    target_compile_definitions(
        ${target}
        PRIVATE _GNU_SOURCE
                _FILE_OFFSET_BITS=64
                __MLIBC_DEBUG
                __MLIBC_BUILDING_MLIBC
                _GLIBCXX_NO_ASSERTIONS
    )
    target_include_directories(
        ${target}
        PRIVATE "${MLIBC_GENERATED}"
                "${CMAKE_CURRENT_SOURCE_DIR}"
                options/internal/include
                options/internal/${MLIBC_CPU}-include
                options/rtld/include
                options/rtld/${MLIBC_CPU}
                options/elf/include
                options/lsb/include
                sysdeps/generic-helpers/include
                # Prefer the materialized ABI headers in the sysroot. On Unix
                # these are copies of symlink targets; on Windows they replace
                # Git's plain-text symlink placeholders.
                "${MLIBC_SYSROOT}/usr/include"
                sysdeps/moss/include
                options/ansi/include
                options/posix/include
                options/glibc/include
                "${MOSS_THIRD_PARTY}/frigg/include"
                "${MOSS_THIRD_PARTY}/libsmarter/include"
    )
    target_include_directories(${target} SYSTEM PRIVATE "${MOSS_THIRD_PARTY}/freestnd-cxx-hdrs/${MLIBC_CPU}/include")
endforeach()
target_compile_definitions(mlibc PRIVATE MLIBC_STATIC_BUILD FRG_HAVE_LIBC)
target_compile_definitions(mlibc-rtld PRIVATE MLIBC_STATIC_BUILD MLIBC_BUILDING_RTLD)
target_compile_options(mlibc-rtld PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden -Wno-extern-c-compat)
foreach(target mlibc-math mlibc-regex)
    target_compile_options(
        ${target}
        PRIVATE -Wno-unused-parameter
                -Wno-unused-but-set-variable
                -Wno-implicit
                -Wno-parentheses
                -Wno-sign-compare
                -Wno-attributes
    )
endforeach()
# mlibc needs _GNU_SOURCE globally to expose its own extension declarations. Some vendored musl math files define it
# themselves without a guard, which Clang diagnoses although both definitions deliberately enable the same API.
target_compile_options(mlibc-math PRIVATE -Wno-macro-redefined)
add_library(mlibc-crt1 OBJECT sysdeps/moss/crt1.S)
target_compile_options(mlibc-crt1 PRIVATE ${MLIBC_ARCH_FLAGS})
