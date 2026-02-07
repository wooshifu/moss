# src/boot/CMakeLists.txt

# 根据架构添加源文件
set(BOOT_SOURCES "")

if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/start.S")
        list(APPEND BOOT_SOURCES "start.S")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/early_init.cpp")
        list(APPEND BOOT_SOURCES "early_init.cpp")
    endif()
    message(STATUS "ARM64 启动源文件: ${BOOT_SOURCES}")
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    # X86_64 特定的启动文件
    message(STATUS "X86_64 架构启动代码待实现")
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    # RISC-V 特定的启动文件
    message(STATUS "RISC-V 架构启动代码待实现")
endif()

# 只有在有源文件时才创建库
if(BOOT_SOURCES)
    # 创建启动模块 OBJECT 库
    add_library(moss_boot OBJECT)
    target_sources(moss_boot PRIVATE ${BOOT_SOURCES})
else()
    # 创建空的接口库作为占位符
    add_library(moss_boot INTERFACE)
    message(STATUS "创建空的启动模块占位符")
endif()

# 设置包含目录
if(BOOT_SOURCES)
    # OBJECT 库使用 PRIVATE
    target_include_directories(moss_boot PRIVATE
        ${CMAKE_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # 架构特定编译设置
    if(MOSS_TARGET_ARCH STREQUAL "ARM64")
        target_compile_definitions(moss_boot PRIVATE
            MOSS_ARCH_ARM64
            MOSS_BOOT_ARM64
        )
    elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
        target_compile_definitions(moss_boot PRIVATE
            MOSS_ARCH_X86_64
            MOSS_BOOT_X86_64
        )
    elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
        target_compile_definitions(moss_boot PRIVATE
            MOSS_ARCH_RISCV
            MOSS_BOOT_RISCV
        )
    endif()
else()
    # INTERFACE 库使用 INTERFACE
    target_include_directories(moss_boot INTERFACE
        ${CMAKE_SOURCE_DIR}/src/include
    )

    # 架构特定编译设置
    if(MOSS_TARGET_ARCH STREQUAL "X86_64")
        target_compile_definitions(moss_boot INTERFACE
            MOSS_ARCH_X86_64
            MOSS_BOOT_X86_64
        )
    elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
        target_compile_definitions(moss_boot INTERFACE
            MOSS_ARCH_RISCV
            MOSS_BOOT_RISCV
        )
    endif()
endif()

message(STATUS "启动模块配置完成")