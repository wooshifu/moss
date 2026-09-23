# Validation uses the exact production object modules and bootstrap.
add_executable(
    moss.test.elf
    validation.cpp
    validation/core.cpp
    validation/core_containers.cpp
    validation/core_scheduler.cpp
    validation/drivers.cpp
    validation/memory.cpp
    validation/memory_transactions.cpp
    validation/memory_pages.cpp
    validation/memory_heap.cpp
    validation/resources.cpp
    validation/isolation.cpp
    validation/uaccess.cpp
    validation/timer_control.cpp
    validation/process_control.cpp
    validation/exec_control.cpp
    validation/signal_control.cpp
    validation/benchmark_cases.cpp
    validation/benchmark_control.cpp
    validation/lifecycle.cpp
    validation/registry.cpp
    validation/smp.cpp
    validation/smp_containers.cpp
    validation/smp_lifetime.cpp
    validation/smp_copy.cpp
    validation/smp_interrupt.cpp
    validation/smp_fault.cpp
    validation/smp_leases.cpp
    validation/smp_tlb.cpp
    validation/smp_join.cpp
    validation/smp_vfs.cpp
    validation/smp_vfs_owner.cpp
    validation/vfs.cpp
    validation/vfs_lifecycle.cpp
)
get_target_property(_kernel_modules moss.elf LINK_LIBRARIES)
target_link_libraries(
    moss.test.elf
    PRIVATE ${_kernel_modules} moss_ipc_validation
)
target_include_directories(moss.test.elf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(moss.test.elf PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set_property(
    TARGET moss.test.elf
    APPEND
    PROPERTY LINK_DEPENDS "${MOSS_LINKER_SCRIPT}" "${CMAKE_BINARY_DIR}/validation-initramfs.cpio"
             "${CMAKE_SOURCE_DIR}/scripts/artifacts.py"
)
add_dependencies(moss.test.elf validation-initramfs)
# Keep ELF symbols from this exact link before applying the Release image policy. --strip-debug overrides the preset's
# --strip-all in LLD; Release still has no DWARF.
target_link_options(moss.test.elf PRIVATE "$<$<CONFIG:Release>:-Wl,--strip-debug>")
add_custom_command(
    TARGET moss.test.elf
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:moss.test.elf> ${CMAKE_BINARY_DIR}/bin/moss.test.symbols.elf
    COMMAND $<$<CONFIG:Release>:${OBJCOPY_CMD}> $<$<CONFIG:Release>:--strip-all>
            $<$<CONFIG:Release>:$<TARGET_FILE:moss.test.elf>>
    BYPRODUCTS ${CMAKE_BINARY_DIR}/bin/moss.test.symbols.elf
    VERBATIM
)
add_custom_command(
    TARGET moss.test.elf
    POST_BUILD
    COMMAND ${OBJCOPY_CMD} -O binary $<TARGET_FILE:moss.test.elf> ${CMAKE_BINARY_DIR}/moss.test.bin
    VERBATIM
)
if(MOSS_TARGET_ARCH STREQUAL "ARM64" OR MOSS_TARGET_ARCH STREQUAL "RISCV64")
    add_custom_command(
        TARGET moss.test.elf
        POST_BUILD
        COMMAND uv run ${CMAKE_SOURCE_DIR}/scripts/verify_linux_image.py ${CMAKE_BINARY_DIR}/moss.test.bin --elf
                $<TARGET_FILE:moss.test.elf>
        VERBATIM
    )
endif()
add_custom_command(
    TARGET moss.test.elf
    POST_BUILD
    COMMAND uv run ${CMAKE_SOURCE_DIR}/scripts/artifacts.py --manifest ${CMAKE_BINARY_DIR}/moss-artifacts.json --source
            ${CMAKE_SOURCE_DIR}
    VERBATIM
)
