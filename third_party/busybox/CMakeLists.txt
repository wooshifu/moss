# Native build for the resolved Moss profile; no Kbuild or Kconfig execution.
include(moss-sources.cmake)
set(BUSYBOX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(BUSYBOX_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS moss.config
)
file(STRINGS moss.config config_lines)
set(autoconf "/* Resolved Moss profile. Generated from moss.config. */\n#define AUTOCONF_TIMESTAMP \"Moss\"\n")
foreach(line IN LISTS config_lines)
    if(line MATCHES "^# CONFIG_([A-Za-z0-9_]+) is not set$")
        set(name "${CMAKE_MATCH_1}")
        string(
            APPEND
            autoconf
            "#undef CONFIG_${name}\n#define ENABLE_${name} 0\n#define IF_${name}(...)\n#define IF_NOT_${name}(...) __VA_ARGS__\n"
        )
    elseif(line MATCHES "^CONFIG_([A-Za-z0-9_]+)=(.*)$")
        set(name "${CMAKE_MATCH_1}")
        set(value "${CMAKE_MATCH_2}")
        if(value STREQUAL "y")
            set(value 1)
        endif()
        string(
            APPEND
            autoconf
            "#define CONFIG_${name} ${value}\n#define ENABLE_${name} 1\n#ifdef MAKE_SUID\n#define IF_${name}(...) __VA_ARGS__ \"CONFIG_${name}\"\n#else\n#define IF_${name}(...) __VA_ARGS__\n#endif\n#define IF_NOT_${name}(...)\n"
        )
    endif()
endforeach()
file(
    CONFIGURE
    OUTPUT
    "${BUSYBOX_BINARY_DIR}/include/autoconf.h"
    CONTENT
    "${autoconf}"
    @ONLY
)
# The fixed profile does not use the optional BSS-tail allocation scheme.
file(
    CONFIGURE
    OUTPUT
    "${BUSYBOX_BINARY_DIR}/include/common_bufsiz.h"
    CONTENT
    "enum { COMMON_BUFSIZE = 1024 };\nextern char bb_common_bufsiz1[];\n#define setup_common_bufsiz() ((void)0)\n"
    @ONLY
)
# The Moss BusyBox profile has an explicit source list. Keeping the generator dependencies explicit avoids CMake's
# per-build glob verification while still regenerating applet metadata when a supported source or template changes.
set(BUSYBOX_GENERATION_INPUTS ${BUSYBOX_SOURCES} applets/applets.c include/applets.src.h include/usage.src.h)
list(TRANSFORM BUSYBOX_GENERATION_INPUTS PREPEND "${BUSYBOX_SOURCE_DIR}/")
add_custom_command(
    OUTPUT "${BUSYBOX_BINARY_DIR}/include/applets.h" "${BUSYBOX_BINARY_DIR}/include/usage.h"
    COMMAND sh "${BUSYBOX_SOURCE_DIR}/scripts/gen_build_files.sh" "${BUSYBOX_SOURCE_DIR}" "${BUSYBOX_BINARY_DIR}"
    DEPENDS ${BUSYBOX_GENERATION_INPUTS} "${BUSYBOX_SOURCE_DIR}/scripts/gen_build_files.sh"
    VERBATIM
)
add_custom_target(
    busybox-generated-inputs DEPENDS "${BUSYBOX_BINARY_DIR}/include/applets.h" "${BUSYBOX_BINARY_DIR}/include/usage.h"
)
add_subdirectory(moss-host-tools)
add_custom_command(
    OUTPUT "${BUSYBOX_BINARY_DIR}/include/applet_tables.h" "${BUSYBOX_BINARY_DIR}/include/NUM_APPLETS.h"
    COMMAND $<TARGET_FILE:busybox-applet-tables> "${BUSYBOX_BINARY_DIR}/include/applet_tables.h"
            "${BUSYBOX_BINARY_DIR}/include/NUM_APPLETS.h"
    DEPENDS busybox-applet-tables
    VERBATIM
)
add_custom_command(
    OUTPUT "${BUSYBOX_BINARY_DIR}/include/usage_compressed.h"
    COMMAND sh "${BUSYBOX_SOURCE_DIR}/applets/usage_compressed" "${BUSYBOX_BINARY_DIR}/include/usage_compressed.h"
            "${BUSYBOX_BINARY_DIR}/applets"
    DEPENDS busybox-usage applets/usage_compressed
    VERBATIM
)
add_custom_target(
    busybox-generated-headers
    DEPENDS "${BUSYBOX_BINARY_DIR}/include/applet_tables.h" "${BUSYBOX_BINARY_DIR}/include/NUM_APPLETS.h"
            "${BUSYBOX_BINARY_DIR}/include/usage_compressed.h"
)
add_library(busybox-lib STATIC ${BUSYBOX_SOURCES})
add_executable(busybox-validation applets/applets.c $<TARGET_OBJECTS:mlibc-crt1>)
foreach(target busybox-lib busybox-validation)
    add_dependencies(${target} busybox-generated-headers)
    set_target_properties(${target} PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
    target_compile_options(
        ${target}
        PRIVATE ${MLIBC_ARCH_FLAGS}
                -Oz
                -ffreestanding
                -fno-stack-protector
                -fno-builtin-strlen
                -fno-builtin-printf
                -fomit-frame-pointer
                -ffunction-sections
                -fdata-sections
                -funsigned-char
                -fno-unwind-tables
                -fno-asynchronous-unwind-tables
                # BusyBox intentionally uses string-literal offsets to omit fixed prefixes.
                -Wno-string-plus-int
                -include
                "${BUSYBOX_BINARY_DIR}/include/autoconf.h"
                -Wno-constant-conversion
    )
    target_compile_definitions(${target} PRIVATE _GNU_SOURCE NDEBUG __moss__ BB_VER="1.37.0")
    target_include_directories(
        ${target} PRIVATE "${BUSYBOX_BINARY_DIR}/include" "${BUSYBOX_SOURCE_DIR}/include" "${BUSYBOX_SOURCE_DIR}/libbb"
    )
    target_include_directories(${target} SYSTEM PRIVATE "${MLIBC_SYSROOT}/usr/include")
endforeach()
target_link_libraries(
    busybox-validation
    PRIVATE busybox-lib mlibc ${MLIBC_BUILTINS_TARGET}
)
target_link_options(
    busybox-validation
    PRIVATE
    ${MLIBC_ARCH_FLAGS}
    -nostdlib
    -static
    -fuse-ld=lld
    -Wl,--gc-sections,--image-base=0x200000000,-z,max-page-size=4096
)
set_target_properties(
    busybox-validation
    PROPERTIES OUTPUT_NAME busybox_unstripped
               SUFFIX ""
               LINKER_LANGUAGE C
               RUNTIME_OUTPUT_DIRECTORY "${BUSYBOX_BINARY_DIR}"
)
add_custom_command(
    TARGET busybox-validation
    POST_BUILD
    COMMAND ${LLVM_STRIP} --strip-all -o "${BUSYBOX_BINARY_DIR}/busybox" $<TARGET_FILE:busybox-validation>
    BYPRODUCTS "${BUSYBOX_BINARY_DIR}/busybox"
    VERBATIM
)
