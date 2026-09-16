cmake_minimum_required(VERSION 3.30)
project(moss_mlibc NONE)

# Standalone sysroot build; kernel/QEMU configuration is deliberately not needed.
include(FetchContent)
find_program(CLANG clang REQUIRED)
find_program(CLANGXX clang++ REQUIRED)
find_program(LLVM_AR llvm-ar REQUIRED)
find_program(LLVM_STRIP llvm-strip REQUIRED)
find_program(UV uv REQUIRED)

function(
    fetch_source
    name
    repo
    revision
    digest
)
    fetchcontent_declare(
        ${name}
        URL https://codeload.github.com/${repo}/tar.gz/${revision}
        URL_HASH SHA256=${digest}
        SOURCE_SUBDIR moss-no-cmake-subproject ${ARGN}
    )
    fetchcontent_makeavailable(${name})
    set(${name}_SOURCE_DIR
        "${${name}_SOURCE_DIR}"
        PARENT_SCOPE
    )
endfunction()

fetch_source(
    mlibc managarm/mlibc 7c2a178142625cc9852e59a1a090468c61a62d3b
    22535f15a789b463bf19abb45b6d4aded20cf5f0b207eb43922b2f95e4507603
)
fetch_source(
    frigg
    managarm/frigg
    b0dbea66bc19f7c5546f0039a3be842feb02678c
    10cbee1dab6e7b0a1ca8d6d59d9eeffab1ed3cf6b0031551fd63a6861cc07a41
    SOURCE_DIR
    ${mlibc_SOURCE_DIR}/subprojects/frigg
)
fetch_source(
    libsmarter
    managarm/libsmarter
    f7d061bc37d485418344452c7ceb28d5df3ba85d
    1426e24f3c3c2a08983fad71d461faa415f0ccc1ca432dd80bebe1dac9cc7bce
    SOURCE_DIR
    ${mlibc_SOURCE_DIR}/subprojects/libsmarter
)
fetch_source(
    freestnd_c
    osdev0/freestnd-c-hdrs
    d33711241b46ecb8f2ad33927fcefdcb3ac0162e
    d3f0c1e0720dec9da97175eebdc51ac4b9bdd776b5687b2a1ccd97f6e861498e
    SOURCE_DIR
    ${mlibc_SOURCE_DIR}/subprojects/freestnd-c-hdrs
)
fetch_source(
    freestnd_cxx
    osdev0/freestnd-cxx-hdrs
    a6b351e0ab3e74e5789b01fa1447e4cd62373da7
    dc4a44daef5d50a6f9aea0cb7bd2164c208f159d273c51f3ff8334b5bd65ef95
    SOURCE_DIR
    ${mlibc_SOURCE_DIR}/subprojects/freestnd-cxx-hdrs
)

# Preserve mlibc's pinned freestanding-header Meson overlays and ABI headers.
foreach(name freestnd-c-hdrs freestnd-cxx-hdrs)
    file(COPY "${mlibc_SOURCE_DIR}/subprojects/packagefiles/${name}/"
         DESTINATION "${mlibc_SOURCE_DIR}/subprojects/${name}"
    )
endforeach()
file(COPY "${mlibc_SOURCE_DIR}/sysdeps/demo/" DESTINATION "${mlibc_SOURCE_DIR}/sysdeps/moss")
file(READ "${mlibc_SOURCE_DIR}/sysdeps/demo/meson.build" port_meson)
string(REPLACE "\t'syscall.cpp',\n" "" port_meson "${port_meson}")
string(REPLACE "'posix': true," "'posix': true,\n\t'glibc': true," port_meson "${port_meson}")
# Reuse mlibc's public device-number header without enabling Linux syscalls or requiring a Linux kernel UAPI tree for
# this non-Linux target.
string(APPEND port_meson
       "\nif not no_headers\n\tinstall_headers('../../options/linux/include/sys/sysmacros.h', subdir: 'sys')\nendif\n"
)
string(REPLACE "'include/abi-bits/auxv.h'," "'include/abi-bits/auxv.h',\n\t\t'include/abi-bits/ioctls.h'," port_meson
               "${port_meson}"
)
file(WRITE "${mlibc_SOURCE_DIR}/sysdeps/moss/meson.build" "${port_meson}")
configure_file(
    "${mlibc_SOURCE_DIR}/abis/linux/ioctls.h" "${mlibc_SOURCE_DIR}/sysdeps/moss/include/abi-bits/ioctls.h" COPYONLY
)
foreach(port_file entry.cpp sysdeps.cpp crt1.S include/mlibc/sysdeps.hpp)
    configure_file(sysdeps/${port_file} "${mlibc_SOURCE_DIR}/sysdeps/moss/${port_file}" COPYONLY)
endforeach()
configure_file(../syscall.h "${mlibc_SOURCE_DIR}/sysdeps/moss/include/moss-syscall.h" COPYONLY)

# Exact, idempotent adaptation of the pinned upstream dispatch table.
file(READ "${mlibc_SOURCE_DIR}/meson.build" meson_source)
string(REPLACE "cpp_compiler.compiles(ifunc_check, name:"
               "cpp_compiler.compiles(ifunc_check, args: ['-Werror=attributes'], name:" meson_source "${meson_source}"
)
string(FIND "${meson_source}" "subdir('sysdeps/moss')" moss_dispatch)
if(moss_dispatch EQUAL -1)
    string(
        REPLACE "elif host_machine.system() == 'demo'"
                "elif host_machine.system() == 'moss'\n\tsubdir('sysdeps/moss')\nelif host_machine.system() == 'demo'"
                meson_source "${meson_source}"
    )
    string(FIND "${meson_source}" "subdir('sysdeps/moss')" moss_dispatch)
    if(moss_dispatch EQUAL -1)
        message(FATAL_ERROR "Pinned mlibc OS-dispatch patch did not apply")
    endif()
endif()
file(WRITE "${mlibc_SOURCE_DIR}/meson.build" "${meson_source}")

# ash keeps its FILE across dup2 redirections. A cached file-like stream may now target a pipe: only seek when buffered
# input actually needs repositioning.
file(READ "${mlibc_SOURCE_DIR}/options/ansi/generic/file-io.cpp" file_io)
set(save_pos_original "\t\tauto seek_offset = (off_t(__offset) - off_t(__io_offset));\n")
set(save_pos_patched "${save_pos_original}\t\tif (!seek_offset)\n\t\t\treturn 0;\n")
string(FIND "${file_io}" "${save_pos_patched}" save_pos_patch)
if(save_pos_patch EQUAL -1)
    string(FIND "${file_io}" "${save_pos_original}" save_pos_patch)
    if(save_pos_patch EQUAL -1)
        message(FATAL_ERROR "Pinned mlibc stdio-position patch did not apply")
    endif()
    string(REPLACE "${save_pos_original}" "${save_pos_patched}" file_io "${file_io}")
    file(WRITE "${mlibc_SOURCE_DIR}/options/ansi/generic/file-io.cpp" "${file_io}")
endif()

# Clang's bare-metal x64 target ignores ifunc; retain upstream generic strcmp.
file(READ "${mlibc_SOURCE_DIR}/options/ansi/x86_64/meson.build" strcmp_meson)
if(NOT strcmp_meson MATCHES "if ifuncs_supported")
    file(WRITE "${mlibc_SOURCE_DIR}/options/ansi/x86_64/meson.build" "if ifuncs_supported\n${strcmp_meson}\nendif\n")
endif()

file(READ "${mlibc_SOURCE_DIR}/options/internal/riscv64/setjmp.S" rv_setjmp)
if(NOT rv_setjmp MATCHES "__riscv_float_abi_double")
    string(REPLACE "\tfsd fs0, 112(a0)" "#if defined(__riscv_float_abi_double)\n\tfsd fs0, 112(a0)" rv_setjmp
                   "${rv_setjmp}"
    )
    string(REPLACE "\tfsd fs11, 200(a0)" "\tfsd fs11, 200(a0)\n#endif" rv_setjmp "${rv_setjmp}")
    string(REPLACE "\tfld fs0,112(a0)" "#if defined(__riscv_float_abi_double)\n\tfld fs0,112(a0)" rv_setjmp
                   "${rv_setjmp}"
    )
    string(REPLACE "\tfld fs11,200(a0)" "\tfld fs11,200(a0)\n#endif" rv_setjmp "${rv_setjmp}")
    file(WRITE "${mlibc_SOURCE_DIR}/options/internal/riscv64/setjmp.S" "${rv_setjmp}")
endif()

if(MOSS_TARGET_ARCH STREQUAL "X64")
    set(MLIBC_CPU x86_64)
    set(MLIBC_ARCH_FLAGS --target=x86_64-unknown-elf -mcmodel=large -fPIC)
elseif(MOSS_TARGET_ARCH STREQUAL "ARM64")
    set(MLIBC_CPU aarch64)
    set(MLIBC_ARCH_FLAGS --target=aarch64-unknown-elf -mno-outline-atomics)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV64")
    set(MLIBC_CPU riscv64)
    set(MLIBC_ARCH_FLAGS --target=riscv64-unknown-elf -march=rv64imac -mabi=lp64 -mcmodel=medany)
else()
    message(FATAL_ERROR "Set MOSS_TARGET_ARCH to ARM64, X64 or RISCV64")
endif()
string(JOIN "', '" MLIBC_FLAGS ${MLIBC_ARCH_FLAGS})
set(MLIBC_FLAGS "'${MLIBC_FLAGS}'")
configure_file(cross.ini.in cross.ini @ONLY)
set(MLIBC_BUILD "${CMAKE_CURRENT_BINARY_DIR}/meson")
set(MLIBC_SYSROOT "${CMAKE_CURRENT_BINARY_DIR}/sysroot")

# Build compiler helpers for the same ABI; no host cross-libgcc dependency.
fetchcontent_declare(
    compiler_rt
    URL https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/compiler-rt-20.1.8.src.tar.xz
    URL_HASH SHA256=15277402f6fd63397c0917a5c7171cda82d16d226094b828c1ed0f58f73b9c69
    SOURCE_SUBDIR moss-no-cmake-subproject
)
fetchcontent_makeavailable(compiler_rt)
fetchcontent_declare(
    llvm_cmake
    URL https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/cmake-20.1.8.src.tar.xz
    URL_HASH SHA256=3319203cfd1172bbac50f06fa68e318af84dcb5d65353310c0586354069d6634
    SOURCE_DIR ${compiler_rt_SOURCE_DIR}/../cmake SOURCE_SUBDIR moss-no-cmake-subproject
)
fetchcontent_makeavailable(llvm_cmake)
list(
    SUBLIST
    MLIBC_ARCH_FLAGS
    1
    -1
    BUILTINS_ARCH_FLAGS
)
string(JOIN " " BUILTINS_FLAGS ${BUILTINS_ARCH_FLAGS})
set(BUILTINS_BUILD "${CMAKE_CURRENT_BINARY_DIR}/builtins")
set(MLIBC_BUILTINS "${BUILTINS_BUILD}/lib/generic/libclang_rt.builtins-${MLIBC_CPU}.a")
add_custom_target(
    compiler-rt-builtins
    COMMAND
        ${CMAKE_COMMAND} -S ${compiler_rt_SOURCE_DIR}/lib/builtins -B ${BUILTINS_BUILD} -G ${CMAKE_GENERATOR}
        -DCMAKE_SYSTEM_NAME=Generic -DCMAKE_SYSTEM_PROCESSOR=${MLIBC_CPU} -DCMAKE_C_COMPILER=${CLANG}
        -DCMAKE_ASM_COMPILER=${CLANG} -DCMAKE_C_COMPILER_TARGET=${MLIBC_CPU}-unknown-elf
        -DCMAKE_ASM_COMPILER_TARGET=${MLIBC_CPU}-unknown-elf
        "-DCMAKE_C_FLAGS=${BUILTINS_FLAGS} -ffreestanding -isystem \"${MLIBC_SYSROOT}/usr/include\""
        "-DCMAKE_ASM_FLAGS=${BUILTINS_FLAGS}" -DCMAKE_BUILD_TYPE=Release -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON
        -DCOMPILER_RT_BAREMETAL_BUILD=ON -DCOMPILER_RT_INCLUDE_TESTS=OFF -DLLVM_RUNTIMES_BUILD=ON
    COMMAND ${CMAKE_COMMAND} --build ${BUILTINS_BUILD} -j 4
    BYPRODUCTS ${MLIBC_BUILTINS}
    USES_TERMINAL VERBATIM
)

add_custom_target(
    mlibc ALL
    COMMAND
        ${UV} run --with meson==1.10.2 meson setup --reconfigure --cross-file ${CMAKE_CURRENT_BINARY_DIR}/cross.ini
        --prefix=/usr --libdir=lib --wrap-mode=nodownload -Ddefault_library=static -Dlibgcc_dependency=false
        -Duse_freestnd_hdrs=enabled -Dbuild_tests=false ${MLIBC_BUILD} ${mlibc_SOURCE_DIR}
    COMMAND ${UV} run --with meson==1.10.2 meson compile -C ${MLIBC_BUILD} -j 4
    COMMAND ${UV} run --with meson==1.10.2 meson install -C ${MLIBC_BUILD} --destdir ${MLIBC_SYSROOT}
    USES_TERMINAL VERBATIM
)
add_dependencies(compiler-rt-builtins mlibc)

add_custom_target(
    mlibc-validation
    COMMAND
        ${CLANG} ${MLIBC_ARCH_FLAGS} -O2 -ffreestanding -fno-stack-protector -isystem ${MLIBC_SYSROOT}/usr/include
        -DMOSS_TEST_RELEASE="${CMAKE_PROJECT_VERSION}"
        -DMOSS_TEST_VERSION="${CMAKE_PROJECT_VERSION}-${CMAKE_BUILD_TYPE}"
        ${CMAKE_CURRENT_SOURCE_DIR}/../libc_validation.c ${MLIBC_SYSROOT}/usr/lib/crt1.o ${MLIBC_SYSROOT}/usr/lib/libc.a
        ${MLIBC_BUILTINS} -nostdlib -static -fuse-ld=lld -Wl,--image-base=0x200000000,-z,max-page-size=4096 -o
        ${CMAKE_CURRENT_BINARY_DIR}/libc_validation.unstripped.elf
    # Keep host diagnostics, but do not consume bounded ramfs space with DWARF.
    COMMAND ${LLVM_STRIP} --strip-debug -o ${CMAKE_CURRENT_BINARY_DIR}/libc_validation.elf
            ${CMAKE_CURRENT_BINARY_DIR}/libc_validation.unstripped.elf
    DEPENDS mlibc compiler-rt-builtins
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/libc_validation.elf
               ${CMAKE_CURRENT_BINARY_DIR}/libc_validation.unstripped.elf
    VERBATIM
)

include(../busybox.cmake)
