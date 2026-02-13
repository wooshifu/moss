# Modern Test Framework Configuration
add_library(moss_modern_tests STATIC)

target_sources(moss_modern_tests PRIVATE
    core/test_main_modern.cpp
)

target_sources(moss_modern_tests PUBLIC
    FILE_SET HEADERS
    BASE_DIRS .
    FILES
        core/test_utilities.hpp
        fixtures/kernel_fixtures.hpp
)

target_link_libraries(moss_modern_tests PRIVATE
    moss_test_framework
    moss_containers
    moss_mm
    moss_process
    moss_interrupts
    moss_ipc
    moss_drivers
)

configure_moss_test_target(moss_modern_tests)