cmake_minimum_required(VERSION 3.31)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    project(moss_runtime LANGUAGES C CXX ASM)
endif()

# Isolate userspace from kernel code models, SIMD restrictions and linker scripts.
foreach(lang C CXX ASM)
    set(CMAKE_${lang}_FLAGS "")
    foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set(CMAKE_${lang}_FLAGS_${config} "")
    endforeach()
endforeach()
set(CMAKE_EXE_LINKER_FLAGS "")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_EXE_LINKER_FLAGS_${config} "")
endforeach()
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set_property(DIRECTORY PROPERTY COMPILE_DEFINITIONS "")
get_filename_component(MOSS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(MOSS_THIRD_PARTY "${MOSS_ROOT}/third_party")
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
set(MLIBC_SYSROOT "${CMAKE_CURRENT_BINARY_DIR}/sysroot")
set(MLIBC_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/include")
configure_file(../syscall.h "${MLIBC_GENERATED}/moss-syscall.h" COPYONLY)
find_program(LLVM_STRIP llvm-strip REQUIRED)
add_subdirectory("${MOSS_THIRD_PARTY}/mlibc" mlibc)
add_subdirectory("${MOSS_THIRD_PARTY}/compiler-rt" builtins)
set(MLIBC_BUILTINS_TARGET "clang_rt.builtins-${MLIBC_CPU}")
add_custom_target(compiler-rt-builtins DEPENDS ${MLIBC_BUILTINS_TARGET})

add_executable(mlibc-validation ../libc_validation.c $<TARGET_OBJECTS:mlibc-crt1>)
target_compile_options(mlibc-validation PRIVATE ${MLIBC_ARCH_FLAGS} -O2 -ffreestanding -fno-stack-protector)
target_compile_definitions(
    mlibc-validation PRIVATE MOSS_TEST_RELEASE="${CMAKE_PROJECT_VERSION}"
                             MOSS_TEST_VERSION="${CMAKE_PROJECT_VERSION}-${CMAKE_BUILD_TYPE}"
)
target_include_directories(mlibc-validation SYSTEM PRIVATE "${MLIBC_SYSROOT}/usr/include")
target_link_libraries(
    mlibc-validation
    PRIVATE mlibc ${MLIBC_BUILTINS_TARGET}
)
target_link_options(
    mlibc-validation
    PRIVATE
    ${MLIBC_ARCH_FLAGS}
    -nostdlib
    -static
    -fuse-ld=lld
    -Wl,--image-base=0x200000000,-z,max-page-size=4096
)
set_target_properties(
    mlibc-validation
    PROPERTIES OUTPUT_NAME libc_validation.unstripped
               SUFFIX .elf
               LINKER_LANGUAGE C
               RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_custom_command(
    TARGET mlibc-validation
    POST_BUILD
    COMMAND ${LLVM_STRIP} --strip-debug -o "${CMAKE_CURRENT_BINARY_DIR}/libc_validation.elf"
            $<TARGET_FILE:mlibc-validation>
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/libc_validation.elf"
    VERBATIM
)
add_subdirectory("${MOSS_THIRD_PARTY}/busybox" busybox)
