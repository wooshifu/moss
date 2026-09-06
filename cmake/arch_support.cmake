# Moss kernel multi-architecture support module (Clang only)

# Display configuration summary
function(moss_print_config_summary)
    message(STATUS "=== Moss 内核构建配置摘要 ===")
    message(STATUS "架构: ${MOSS_TARGET_ARCH}")
    message(STATUS "编译器: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    message(STATUS "目标: ${CMAKE_CXX_COMPILER_TARGET}")
    message(STATUS "构建类型: ${CMAKE_BUILD_TYPE}")
    message(STATUS "==============================")
endfunction()
