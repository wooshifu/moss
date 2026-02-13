# src/boot/CMakeLists.txt

aux_source_directory(src BOOT_SOURCES)
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    list(APPEND BOOT_SOURCES src/arch/arm64/boot_impl.cpp src/arch/arm64/start_arm64.S)
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    list(APPEND BOOT_SOURCES src/arch/x86_64/start_x86_64.S src/arch/x86_64/boot_impl.cpp)
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    list(APPEND BOOT_SOURCES src/arch/riscv/start_riscv.S src/arch/riscv/boot_impl.cpp)
endif()
add_library(moss_boot OBJECT ${BOOT_SOURCES})

target_include_directories(moss_boot PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/mm/include
    ${CMAKE_SOURCE_DIR}/src/containers/include
    ${CMAKE_SOURCE_DIR}/src/process/include)
target_include_directories(moss_boot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/boot)

target_compile_definitions(moss_boot PRIVATE MOSS_BOOT_MODULE MOSS_UNIFIED_BOOT)

message(STATUS "启动模块配置完成")
