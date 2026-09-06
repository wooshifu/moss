# Validation uses the exact production object modules and bootstrap.
add_executable(moss.test.elf validation.cpp)
get_target_property(_kernel_modules moss.elf LINK_LIBRARIES)
target_link_libraries(
    moss.test.elf
    PRIVATE ${_kernel_modules}
)
target_include_directories(moss.test.elf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(moss.test.elf PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set_property(
    TARGET moss.test.elf
    APPEND
    PROPERTY LINK_DEPENDS "${MOSS_LINKER_SCRIPT}"
)
add_dependencies(moss.test.elf validation-initramfs)
add_custom_command(
    TARGET moss.test.elf
    POST_BUILD
    COMMAND ${OBJCOPY_CMD} -O binary $<TARGET_FILE:moss.test.elf> ${CMAKE_BINARY_DIR}/moss.test.bin
    VERBATIM
)
if(MOSS_TARGET_ARCH STREQUAL "ARM64" OR MOSS_TARGET_ARCH STREQUAL "RISCV")
    add_custom_command(
        TARGET moss.test.elf
        POST_BUILD
        COMMAND uv run ${CMAKE_SOURCE_DIR}/scripts/verify_linux_image.py ${CMAKE_BINARY_DIR}/moss.test.bin --elf
                $<TARGET_FILE:moss.test.elf>
        VERBATIM
    )
endif()
