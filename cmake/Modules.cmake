# cmake/Modules.cmake C++26 Modules Configuration for MOSS

# Enable module compilation caching
set(CMAKE_CXX_MODULE_BMI_CACHE_DIR "${CMAKE_BINARY_DIR}/modules")

# Configure module compilation flags
set(CMAKE_CXX_MODULE_COMPILE_FLAGS
    "-fprebuilt-module-path=${CMAKE_CXX_MODULE_BMI_CACHE_DIR}")

# Function to create a module target
function(moss_add_module MODULE_NAME)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs SOURCES IMPL_SOURCES DEPENDENCIES)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})

  add_library(${MODULE_NAME})

  # Separate module interface files (.cppm) from implementation files (.cpp)
  set(MODULE_INTERFACES)
  set(IMPL_FILES)

  foreach(SOURCE ${ARG_SOURCES})
    get_filename_component(EXT ${SOURCE} EXT)
    if(EXT STREQUAL ".cppm")
      list(APPEND MODULE_INTERFACES ${SOURCE})
    else()
      list(APPEND IMPL_FILES ${SOURCE})
    endif()
  endforeach()

  # Add module interfaces if any
  if(MODULE_INTERFACES)
    target_sources(${MODULE_NAME} PUBLIC FILE_SET CXX_MODULES FILES
                                         ${MODULE_INTERFACES})
  endif()

  # Add implementation files if any
  if(IMPL_FILES)
    target_sources(${MODULE_NAME} PRIVATE ${IMPL_FILES})
  endif()

  # Add explicit implementation sources if provided
  if(ARG_IMPL_SOURCES)
    target_sources(${MODULE_NAME} PRIVATE ${ARG_IMPL_SOURCES})
  endif()

  if(ARG_DEPENDENCIES)
    target_link_libraries(${MODULE_NAME} PUBLIC ${ARG_DEPENDENCIES})
  endif()

  # Set freestanding compilation flags
  target_compile_options(
    ${MODULE_NAME}
    PRIVATE -ffreestanding
            -fno-exceptions
            -fno-rtti
            -fno-stack-protector
            -Wall
            -Wextra
            -Werror)
endfunction()
