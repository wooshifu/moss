# Keep the existing standalone userspace flags, independent of kernel flags and build type.
# This directory scope prevents the overrides from affecting kernel or test targets.
set(CMAKE_C_FLAGS "")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_C_FLAGS_${config} "")
endforeach()
set_property(DIRECTORY PROPERTY COMPILE_DEFINITIONS "")

set(USERSPACE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(USERSPACE_LD "${USERSPACE_DIR}/linker/userspace.ld")

# Find the ELF linker beside Clang (macOS ld64.lld cannot link these programs).
get_filename_component(_compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)
find_program(LLD_LINKER ld.lld HINTS "${_compiler_dir}" REQUIRED)

if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    set(USERSPACE_ARCH_FLAGS --target=aarch64-unknown-elf)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV64")
    set(USERSPACE_ARCH_FLAGS --target=riscv64-unknown-elf -march=rv64imac -mabi=lp64 -mcmodel=medany)
elseif(MOSS_TARGET_ARCH STREQUAL "X64")
    set(USERSPACE_ARCH_FLAGS --target=x86_64-unknown-elf -mcmodel=large -fPIC)
endif()

set(USERSPACE_COMPILE_FLAGS
    ${USERSPACE_ARCH_FLAGS}
    -ffreestanding
    -fno-stack-protector
    -O2
    -Wall
)
set(USERSPACE_LINK_FLAGS
    -T
    ${USERSPACE_LD}
    --oformat=elf
    -nostdlib
    -static
)

function(add_userspace_program NAME)
    set(_target moss_userspace_${NAME})
    set(_elf ${CMAKE_BINARY_DIR}/userspace/${NAME}.elf)

    # Real CMake compilation targets export their commands and track header dependencies.
    add_library(${_target} OBJECT ${USERSPACE_DIR}/${NAME}.c)
    target_compile_options(${_target} PRIVATE ${USERSPACE_COMPILE_FLAGS})
    target_include_directories(${_target} PRIVATE ${USERSPACE_DIR})

    # Retain direct ld.lld linking and the existing ELF output locations.
    add_custom_command(
        OUTPUT ${_elf}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/userspace
        COMMAND ${LLD_LINKER} ${USERSPACE_LINK_FLAGS} -o ${_elf} $<TARGET_OBJECTS:${_target}>
        DEPENDS ${_target} $<TARGET_OBJECTS:${_target}> ${USERSPACE_LD} DEPENDS_EXPLICIT_ONLY
        COMMENT "Linking userspace ${NAME}.elf"
        COMMAND_EXPAND_LISTS VERBATIM
    )
endfunction()

add_subdirectory(mlibc)
set(BUSYBOX_ELF ${CMAKE_CURRENT_BINARY_DIR}/mlibc/busybox/busybox)
set(MOSS_INIT_ELF ${CMAKE_CURRENT_BINARY_DIR}/mlibc/init.elf)
set(MOSS_FILE_SERVICE_ELF ${CMAKE_CURRENT_BINARY_DIR}/mlibc/file-service.elf)
set(MOSS_NAMESPACE_SERVICE_ELF ${CMAKE_CURRENT_BINARY_DIR}/mlibc/namespace-service.elf)
set(MOSS_FILE_ELF ${CMAKE_CURRENT_BINARY_DIR}/mlibc/moss-file.elf)
if(MOSS_BUILD_TESTS)
    set(LIBC_VALIDATION_ELF ${CMAKE_CURRENT_BINARY_DIR}/mlibc/libc_validation.elf)
    add_userspace_program(validation)
    target_sources(
        moss_userspace_validation
        PRIVATE validation_frame.S validation/basics.c validation/benchmarks.c validation/exec.c validation/ipc.c
                validation/signals.c validation/timers.c validation/uaccess.c validation/vm.c
    )
    add_userspace_program(validation_child)

    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/validation-initramfs.cpio
        COMMAND
            uv run ${CMAKE_SOURCE_DIR}/scripts/gen_validation_initramfs.py ${CMAKE_BINARY_DIR}/validation-initramfs.cpio
            ${CMAKE_BINARY_DIR}/userspace/validation.elf ${CMAKE_BINARY_DIR}/userspace/validation_child.elf
            ${LIBC_VALIDATION_ELF} ${BUSYBOX_ELF}
        DEPENDS ${CMAKE_BINARY_DIR}/userspace/validation.elf
                ${CMAKE_BINARY_DIR}/userspace/validation_child.elf
                mlibc-validation
                ${LIBC_VALIDATION_ELF}
                busybox-validation
                ${BUSYBOX_ELF}
                ${CMAKE_SOURCE_DIR}/scripts/gen_validation_initramfs.py
                ${CMAKE_SOURCE_DIR}/scripts/gen_initramfs.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM
    )
    add_custom_target(validation-initramfs DEPENDS ${CMAKE_BINARY_DIR}/validation-initramfs.cpio)
endif()

# GNU cpio newc format (070701), consumed by the kernel's CPIO parser.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/initramfs.cpio
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BUSYBOX_ELF} ${CMAKE_BINARY_DIR}/userspace/busybox.elf
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MOSS_INIT_ELF} ${CMAKE_BINARY_DIR}/userspace/init.elf
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MOSS_FILE_SERVICE_ELF} ${CMAKE_BINARY_DIR}/userspace/file-service.elf
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MOSS_NAMESPACE_SERVICE_ELF} ${CMAKE_BINARY_DIR}/userspace/namespace-service.elf
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MOSS_FILE_ELF} ${CMAKE_BINARY_DIR}/userspace/moss-file.elf
    COMMAND uv run ${CMAKE_SOURCE_DIR}/scripts/gen_initramfs.py ${CMAKE_BINARY_DIR}/initramfs.cpio
            ${CMAKE_BINARY_DIR}/userspace init.elf file-service.elf namespace-service.elf moss-file.elf busybox.elf
    DEPENDS moss-init ${MOSS_INIT_ELF} moss-file-service ${MOSS_FILE_SERVICE_ELF}
            moss-namespace-service ${MOSS_NAMESPACE_SERVICE_ELF} moss-file ${MOSS_FILE_ELF}
            busybox-validation ${BUSYBOX_ELF} ${CMAKE_SOURCE_DIR}/scripts/gen_initramfs.py
    BYPRODUCTS ${CMAKE_BINARY_DIR}/userspace/init.elf ${CMAKE_BINARY_DIR}/userspace/file-service.elf
               ${CMAKE_BINARY_DIR}/userspace/namespace-service.elf ${CMAKE_BINARY_DIR}/userspace/moss-file.elf
               ${CMAKE_BINARY_DIR}/userspace/busybox.elf
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating initramfs.cpio"
    VERBATIM
)

add_custom_target(initramfs ALL DEPENDS ${CMAKE_BINARY_DIR}/initramfs.cpio)
message(STATUS "Userspace + initramfs configured (${MOSS_TARGET_ARCH})")
