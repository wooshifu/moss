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
file(MAKE_DIRECTORY "${MLIBC_GENERATED}/moss")
configure_file("${MOSS_ROOT}/src/abi/include/moss/syscall_numbers.def"
               "${MLIBC_GENERATED}/moss/syscall_numbers.def" COPYONLY)
configure_file("${MOSS_ROOT}/src/abi/include/moss/domain_spawn.h" "${MLIBC_GENERATED}/moss/domain_spawn.h" COPYONLY)
find_program(LLVM_STRIP llvm-strip REQUIRED)
add_subdirectory("${MOSS_THIRD_PARTY}/mlibc" mlibc)
add_subdirectory("${MOSS_THIRD_PARTY}/compiler-rt" builtins)
set(MLIBC_BUILTINS_TARGET "clang_rt.builtins-${MLIBC_CPU}")
add_custom_target(compiler-rt-builtins DEPENDS ${MLIBC_BUILTINS_TARGET})

function(add_moss_runtime_program TARGET SOURCE OUTPUT)
    add_executable(${TARGET} EXCLUDE_FROM_ALL ${SOURCE} $<TARGET_OBJECTS:mlibc-crt1>)
    target_compile_options(${TARGET} PRIVATE ${MLIBC_ARCH_FLAGS} -O2 -ffreestanding -fno-stack-protector)
    target_include_directories(${TARGET} PRIVATE "${MOSS_ROOT}/src/abi/include")
    target_include_directories(${TARGET} SYSTEM PRIVATE "${MLIBC_SYSROOT}/usr/include")
    target_link_libraries(${TARGET} PRIVATE mlibc ${MLIBC_BUILTINS_TARGET})
    target_link_options(
        ${TARGET}
        PRIVATE
        ${MLIBC_ARCH_FLAGS}
        -nostdlib
        -static
        -fuse-ld=lld
        # Match userspace.ld: 8 GiB keeps code above inherited kernel identity mappings;
        # 4096-byte segment alignment permits distinct permissions on base pages.
        -Wl,--image-base=0x200000000,-z,max-page-size=4096
    )
    set_target_properties(
        ${TARGET}
        PROPERTIES OUTPUT_NAME ${OUTPUT}.unstripped
                   SUFFIX .elf
                   LINKER_LANGUAGE C
                   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_custom_command(
        TARGET ${TARGET}
        POST_BUILD
        COMMAND ${LLVM_STRIP} --strip-debug -o "${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT}.elf" $<TARGET_FILE:${TARGET}>
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT}.elf"
        VERBATIM
    )
endfunction()

add_moss_runtime_program(mlibc-validation ../libc_validation.c libc_validation)
target_compile_definitions(
    mlibc-validation PRIVATE MOSS_TEST_RELEASE="${CMAKE_PROJECT_VERSION}"
                             MOSS_TEST_VERSION="${CMAKE_PROJECT_VERSION}-${CMAKE_BUILD_TYPE}"
)
add_subdirectory("${MOSS_THIRD_PARTY}/busybox" busybox)
add_moss_runtime_program(moss-init ../moss_init.c init)
add_moss_runtime_program(moss-file-service ../moss_file_service.c file-service)
add_moss_runtime_program(moss-namespace-service ../moss_namespace_service.c namespace-service)
add_moss_runtime_program(moss-file ../moss_file.c moss-file)
add_moss_runtime_program(moss-domain ../moss_domain.c moss-domain)
add_moss_runtime_program(moss-process-service ../moss_process_service.c process-service)
add_moss_runtime_program(moss-process ../moss_process.c moss-process)
