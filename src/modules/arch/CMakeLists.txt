# src/modules/arch/CMakeLists.txt

# Only build architecture-specific modules for current target
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    moss_add_module(moss_arch_arm64
        SOURCES arm64.cppm
        DEPENDENCIES moss_std moss_types
    )

    set_target_properties(moss_arch_arm64 PROPERTIES
        EXPORT_NAME "moss.arch.arm64"
    )

    # Link architecture implementation to interface
    target_link_libraries(moss_arch PUBLIC moss_arch_arm64)
endif()