# These upstream generators execute on the host, not on the Moss target.
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -dumpmachine
    OUTPUT_VARIABLE host_triple
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY
)
set(CMAKE_C_COMPILER_TARGET "${host_triple}")
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)
file(MAKE_DIRECTORY "${BUSYBOX_BINARY_DIR}/applets")
add_executable(busybox-applet-tables "${BUSYBOX_SOURCE_DIR}/applets/applet_tables.c")
add_executable(busybox-usage "${BUSYBOX_SOURCE_DIR}/applets/usage.c")
foreach(target busybox-applet-tables busybox-usage)
    add_dependencies(${target} busybox-generated-inputs)
    if(CMAKE_HOST_WIN32)
        target_include_directories(${target} BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/windows")
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        # The target platform is Generic, so CMake does not infer the suffix
        # even though these two executables use the Windows host triple.
        set_target_properties(${target} PROPERTIES SUFFIX ".exe")
    endif()
    target_include_directories(
        ${target} PRIVATE "${BUSYBOX_BINARY_DIR}/include" "${BUSYBOX_BINARY_DIR}/applets"
                          "${BUSYBOX_SOURCE_DIR}/include"
    )
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BUSYBOX_BINARY_DIR}/applets")
endforeach()
set_target_properties(busybox-applet-tables PROPERTIES OUTPUT_NAME applet_tables)
set_target_properties(busybox-usage PROPERTIES OUTPUT_NAME usage)
