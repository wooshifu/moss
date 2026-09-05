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
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    set(USERSPACE_ARCH_FLAGS --target=riscv64-unknown-elf -march=rv64imac -mabi=lp64 -mcmodel=medany)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
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
        DEPENDS ${_target} $<TARGET_OBJECTS:${_target}> ${USERSPACE_LD}
        COMMENT "Linking userspace ${NAME}.elf"
        COMMAND_EXPAND_LISTS VERBATIM
    )
endfunction()

add_userspace_program(hello)
add_userspace_program(shell)
add_userspace_program(top)
add_userspace_program(signal_test)
if(MOSS_BUILD_TESTS)
    add_userspace_program(validation)
    add_userspace_program(validation_child)

    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/validation-initramfs.cpio
        COMMAND
            uv run ${CMAKE_SOURCE_DIR}/scripts/gen_validation_initramfs.py ${CMAKE_BINARY_DIR}/validation-initramfs.cpio
            ${CMAKE_BINARY_DIR}/userspace/validation.elf ${CMAKE_BINARY_DIR}/userspace/validation_child.elf
        DEPENDS ${CMAKE_BINARY_DIR}/userspace/validation.elf ${CMAKE_BINARY_DIR}/userspace/validation_child.elf
                ${CMAKE_SOURCE_DIR}/scripts/gen_validation_initramfs.py ${CMAKE_SOURCE_DIR}/scripts/gen_initramfs.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM
    )
    add_custom_target(validation-initramfs DEPENDS ${CMAKE_BINARY_DIR}/validation-initramfs.cpio)
endif()

# GNU cpio newc format (070701), consumed by the kernel's CPIO parser.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/initramfs.cpio
    COMMAND uv run ${CMAKE_SOURCE_DIR}/scripts/gen_initramfs.py ${CMAKE_BINARY_DIR}/initramfs.cpio
            ${CMAKE_BINARY_DIR}/userspace hello.elf shell.elf top.elf signal_test.elf
    DEPENDS ${CMAKE_BINARY_DIR}/userspace/hello.elf ${CMAKE_BINARY_DIR}/userspace/shell.elf
            ${CMAKE_BINARY_DIR}/userspace/top.elf ${CMAKE_BINARY_DIR}/userspace/signal_test.elf
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating initramfs.cpio"
    VERBATIM
)

add_custom_target(initramfs ALL DEPENDS ${CMAKE_BINARY_DIR}/initramfs.cpio)
message(STATUS "Userspace + initramfs configured (${MOSS_TARGET_ARCH})")
