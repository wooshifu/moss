# src/process/CMakeLists.txt

# 添加源文件
set(PROCESS_SOURCES process.cpp)
set(PROCESS_HEADERS cfs_scheduler.hpp load_balancer.hpp process.hpp)

# 根据架构添加汇编文件
if(MOSS_TARGET_ARCH STREQUAL "ARM64")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/context_switch.S")
    list(APPEND PROCESS_SOURCES "context_switch.S")
  endif()
endif()

# 根据是否有源文件决定库类型
if(PROCESS_SOURCES)
  # 创建进程模块 OBJECT 库
  add_library(moss_process OBJECT)
  target_sources(moss_process PRIVATE ${PROCESS_SOURCES} ${PROCESS_HEADERS})
else()
  # 创建接口库（只有头文件）
  add_library(moss_process INTERFACE)
  message(STATUS "创建进程模块接口库（只有头文件）")
endif()

# 设置包含目录
if(PROCESS_SOURCES)
  # OBJECT 库使用 PRIVATE/PUBLIC
  target_include_directories(moss_process
                             PUBLIC ${CMAKE_SOURCE_DIR}/src/include)
  target_include_directories(moss_process PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  # 链接依赖（进程管理依赖所有其他模块）
  target_link_libraries(
    moss_process PRIVATE moss_containers moss_mm moss_drivers moss_interrupts
                         moss_ipc)

  # 编译器定义
  target_compile_definitions(moss_process PRIVATE MOSS_KERNEL_MODULE
                                                  MOSS_PROCESS_MODULE)

  # 架构特定设置
  if(MOSS_TARGET_ARCH STREQUAL "ARM64")
    target_compile_definitions(moss_process PRIVATE MOSS_ARCH_ARM64)
  elseif(MOSS_TARGET_ARCH STREQUAL "X86_64")
    target_compile_definitions(moss_process PRIVATE MOSS_ARCH_X86_64)
  elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    target_compile_definitions(moss_process PRIVATE MOSS_ARCH_RISCV)
  endif()
else()
  # INTERFACE 库使用 INTERFACE
  target_include_directories(
    moss_process INTERFACE ${CMAKE_SOURCE_DIR}/src/include
                           ${CMAKE_CURRENT_SOURCE_DIR})

  # 链接依赖
  target_link_libraries(
    moss_process INTERFACE moss_containers moss_mm moss_drivers moss_interrupts
                           moss_ipc)

  # 编译器定义
  target_compile_definitions(moss_process INTERFACE MOSS_KERNEL_MODULE
                                                    MOSS_PROCESS_MODULE)

  # 架构特定设置
  if(MOSS_TARGET_ARCH STREQUAL "X86_64")
    target_compile_definitions(moss_process INTERFACE MOSS_ARCH_X86_64)
  elseif(MOSS_TARGET_ARCH STREQUAL "RISCV")
    target_compile_definitions(moss_process INTERFACE MOSS_ARCH_RISCV)
  endif()
endif()

message(STATUS "进程模块配置完成")
