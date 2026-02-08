# cmake/Modules.cmake
# C++20 Modules Configuration for MOSS

# Enable module compilation caching
set(CMAKE_CXX_MODULE_BMI_CACHE_DIR "${CMAKE_BINARY_DIR}/modules")

# Configure module compilation flags
set(CMAKE_CXX_MODULE_COMPILE_FLAGS
    "-fprebuilt-module-path=${CMAKE_CXX_MODULE_BMI_CACHE_DIR}"
)

# Function to create a module target
function(moss_add_module MODULE_NAME)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES DEPENDENCIES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_library(${MODULE_NAME})
    target_sources(${MODULE_NAME}
        PUBLIC
            FILE_SET CXX_MODULES FILES ${ARG_SOURCES}
    )

    if(ARG_DEPENDENCIES)
        target_link_libraries(${MODULE_NAME} PUBLIC ${ARG_DEPENDENCIES})
    endif()

    # Set freestanding compilation flags
    target_compile_options(${MODULE_NAME} PRIVATE
        -ffreestanding
        -fno-exceptions
        -fno-rtti
        -fno-stack-protector
        -Wall -Wextra -Werror
    )
endfunction()