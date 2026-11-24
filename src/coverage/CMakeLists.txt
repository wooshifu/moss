add_library(moss_coverage OBJECT coverage_runtime.cpp)
target_include_directories(moss_coverage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
    moss_coverage
    PRIVATE moss_process
            moss_mm
            moss_containers
            moss_core
            moss_hal_uart
)

# The runtime must not instrument itself; otherwise its own bookkeeping would recursively grow the profile it is trying
# to export.
set_source_files_properties(
    coverage_runtime.cpp PROPERTIES COMPILE_OPTIONS "-fno-profile-instr-generate;-fno-coverage-mapping"
)
