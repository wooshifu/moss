# src/boot/CMakeLists.txt - 统一多架构启动系统

# 统一启动入口（所有架构共享）
set(BOOT_UNIFIED_SOURCES
    "boot.cpp"
)

# 架构特定源文件
set(BOOT_ARCH_SOURCES "")

# 根据架构添加对应的实现文件
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    list(APPEND BOOT_ARCH_SOURCES
        "arch/arm64/boot_impl.cpp"
        "arch/arm64/start_arm64.S"
        # "arch/arm64/early_init.cpp"  # 不再需要：功能已整合到boot_impl.cpp中
    )
    message(STATUS "ARM64 启动模块: 使用统一启动接口")

elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    list(APPEND BOOT_ARCH_SOURCES
        "arch/x86_64/start_x86_64.S"
        # TODO: "arch/x86_64/boot_impl.cpp"
    )
    message(STATUS "X86_64 启动模块: 使用存根实现")

elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    list(APPEND BOOT_ARCH_SOURCES
        "arch/riscv/start_riscv.S"
        # TODO: "arch/riscv/boot_impl.cpp"
    )
    message(STATUS "RISC-V 启动模块: 使用存根实现")

else()
    message(FATAL_ERROR "不支持的目标架构: ${MOSS_TARGET_ARCH}")
endif()

# 检查必要的架构文件是否存在
foreach(SOURCE_FILE ${BOOT_ARCH_SOURCES})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}")
        message(WARNING "架构源文件不存在: ${SOURCE_FILE}")
        list(REMOVE_ITEM BOOT_ARCH_SOURCES "${SOURCE_FILE}")
    endif()
endforeach()

# 合并所有源文件
set(ALL_BOOT_SOURCES ${BOOT_UNIFIED_SOURCES} ${BOOT_ARCH_SOURCES})

# 创建启动模块库
if(ALL_BOOT_SOURCES)
    # 创建启动模块 OBJECT 库
    add_library(moss_boot OBJECT)
    target_sources(moss_boot PRIVATE ${ALL_BOOT_SOURCES})

    message(STATUS "启动源文件: ${ALL_BOOT_SOURCES}")
else()
    # 如果没有源文件，创建空接口库
    add_library(moss_boot INTERFACE)
    message(WARNING "没有找到启动源文件，创建空接口库")
endif()

# 设置包含目录
if(ALL_BOOT_SOURCES)
    # 对于OBJECT库使用PRIVATE
    target_include_directories(moss_boot PRIVATE
        ${CMAKE_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    # 架构特定编译定义
    if(MOSS_TARGET_ARCH STREQUAL "ARM64")
        target_compile_definitions(moss_boot PRIVATE
            MOSS_ARCH_ARM64
            MOSS_BOOT_ARM64
            MOSS_UNIFIED_BOOT
        )
    elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
        target_compile_definitions(moss_boot PRIVATE
            MOSS_ARCH_X86_64
            MOSS_BOOT_X86_64
            MOSS_UNIFIED_BOOT
        )
    elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
        target_compile_definitions(moss_boot PRIVATE
            MOSS_ARCH_RISCV
            MOSS_BOOT_RISCV
            MOSS_UNIFIED_BOOT
        )
    endif()

    # 设置C++标准和编译选项
    target_compile_features(moss_boot PRIVATE cxx_std_23)

    # 禁用异常和RTTI（内核环境）
    target_compile_options(moss_boot PRIVATE
        -fno-exceptions
        -fno-rtti
        -fno-stack-protector
        -ffreestanding
        -fno-builtin
    )

else()
    # 对于INTERFACE库使用INTERFACE
    target_include_directories(moss_boot INTERFACE
        ${CMAKE_SOURCE_DIR}/src/include
    )

    # 架构特定编译定义（INTERFACE）
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

# 架构验证
if(MOSS_TARGET_ARCH STREQUAL "ARM64" AND ALL_BOOT_SOURCES)
    message(STATUS "✓ ARM64启动模块：统一接口 + 完整实现")
elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    message(STATUS "⚠ x86_64启动模块：存根实现，需要boot_impl.cpp")
elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    message(STATUS "⚠ RISC-V启动模块：存根实现，需要boot_impl.cpp")
endif()

message(STATUS "统一启动模块配置完成")