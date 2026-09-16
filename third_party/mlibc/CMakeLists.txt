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
    configure_file("${header}" "${MLIBC_SYSROOT}/usr/include/${destination}" COPYONLY)
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
                sysdeps/moss/include
                options/ansi/include
                options/posix/include
                options/glibc/include
                "${MLIBC_SYSROOT}/usr/include"
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
add_library(mlibc-crt1 OBJECT sysdeps/moss/crt1.S)
target_compile_options(mlibc-crt1 PRIVATE ${MLIBC_ARCH_FLAGS})
