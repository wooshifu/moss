# Moss 内核 Clang 工具链检测模块 只支持 Clang 编译器的统一交叉编译

# Clang 最低版本要求
set(MOSS_MIN_CLANG_VERSION "18.0")
set(MOSS_RECOMMENDED_CLANG_VERSION "21.0")

# 检测和验证 Clang 工具链
function(moss_detect_clang_toolchain)
  message(STATUS "检测 Clang 工具链...")

  # 检查编译器是否为 Clang
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(
      FATAL_ERROR "Moss 内核要求使用 Clang 编译器\n" "当前编译器: ${CMAKE_CXX_COMPILER_ID}\n"
                  "请设置环境变量: export CXX=clang++")
  endif()

  # 检查 Clang 版本
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS ${MOSS_MIN_CLANG_VERSION})
    message(
      FATAL_ERROR
        "Clang 版本过低: ${CMAKE_CXX_COMPILER_VERSION}\n"
        "最低要求版本: ${MOSS_MIN_CLANG_VERSION}\n"
        "推荐版本: ${MOSS_RECOMMENDED_CLANG_VERSION}")
  endif()

  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS ${MOSS_RECOMMENDED_CLANG_VERSION})
    message(WARNING "建议升级 Clang 到 ${MOSS_RECOMMENDED_CLANG_VERSION}+ 以获得最佳体验\n"
                    "当前版本: ${CMAKE_CXX_COMPILER_VERSION}")
  endif()

  # 获取 Clang 安装路径
  get_filename_component(CLANG_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
  set(CLANG_INSTALL_PREFIX
      ${CLANG_BIN_DIR}/..
      PARENT_SCOPE)

  message(
    STATUS "发现 Clang ${CMAKE_CXX_COMPILER_VERSION}: ${CMAKE_CXX_COMPILER}")
  message(STATUS "Clang 安装目录: ${CLANG_BIN_DIR}/..")

  # 检测 LLVM 工具
  moss_detect_llvm_tools(${CLANG_BIN_DIR})
endfunction()

# 检测 LLVM 相关工具
function(moss_detect_llvm_tools clang_bin_dir)
  message(STATUS "检测 LLVM 工具...")

  # 需要的 LLVM 工具列表
  set(LLVM_TOOLS "llvm-objdump" "llvm-objcopy" "llvm-readelf" "llvm-nm" "lld")

  # 检测每个工具
  foreach(tool ${LLVM_TOOLS})
    find_program(${tool}_EXECUTABLE ${tool} HINTS ${clang_bin_dir})
    if(${tool}_EXECUTABLE)
      message(STATUS "发现 ${tool}: ${${tool}_EXECUTABLE}")
    else()
      message(WARNING "未找到 LLVM 工具: ${tool}")
    endif()
  endforeach()

  # 检测链接器
  moss_detect_linker(${clang_bin_dir})
endfunction()

# 检测链接器
function(moss_detect_linker clang_bin_dir)
  # 统一使用 Clang 内置的 LLD 链接器，避免不同架构的兼容性问题
  message(STATUS "配置 Clang 内置链接器...")

  # 优先查找Unix版本的LLD (ld.lld)，包括LLVM安装目录
  find_program(
    LLD_EXECUTABLE ld.lld
    HINTS ${clang_bin_dir} /usr/lib/llvm-21/bin /usr/lib/llvm-20/bin
          /usr/lib/llvm-19/bin /usr/lib/llvm-18/bin
    PATHS /usr/bin /usr/local/bin)
  if(LLD_EXECUTABLE)
    message(STATUS "使用 LLVM LLD 链接器: ${LLD_EXECUTABLE}")
    # 使用标准的 -fuse-ld=lld 方式，让 Clang 处理链接器调用
    set(CMAKE_EXE_LINKER_FLAGS
        "-fuse-ld=lld"
        PARENT_SCOPE)
    return()
  endif()

  # 如果没有找到 LLD，回退到默认配置
  message(STATUS "使用 Clang 内置链接器 (LLD) 处理 ${MOSS_TARGET_ARCH} 架构")
  set(CMAKE_EXE_LINKER_FLAGS
      "-fuse-ld=lld"
      PARENT_SCOPE)
endfunction()

# 设置 Clang 特定的编译器标志
function(moss_set_clang_flags)
  message(STATUS "设置 Clang 特定编译标志...")

  # Clang 特定的优化和诊断选项
  set(CLANG_SPECIFIC_FLAGS
      "-fcolor-diagnostics" # 彩色诊断输出
      "-fdiagnostics-absolute-paths" # 绝对路径错误信息
      "-fno-omit-frame-pointer" # 保留帧指针便于调试
      "-fno-common" # 禁用 common 符号
  )

  # Debug 模式下的额外标志
  set(CLANG_DEBUG_FLAGS "-fno-limit-debug-info" # 完整调试信息
                        "-glldb" # LLDB 调试格式
  )

  # Release 模式下的优化标志
  set(CLANG_RELEASE_FLAGS
      "-flto=thin" # 瘦链接时优化
      "-ffunction-sections" # 函数分段
      "-fdata-sections" # 数据分段
  )

  # 应用标志
  string(JOIN " " CLANG_FLAGS_STR ${CLANG_SPECIFIC_FLAGS})
  set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} ${CLANG_FLAGS_STR}"
      PARENT_SCOPE)

  # Debug 特定标志
  string(JOIN " " CLANG_DEBUG_STR ${CLANG_DEBUG_FLAGS})
  set(CMAKE_CXX_FLAGS_DEBUG
      "${CMAKE_CXX_FLAGS_DEBUG} ${CLANG_DEBUG_STR}"
      PARENT_SCOPE)

  # Release 特定标志
  string(JOIN " " CLANG_RELEASE_STR ${CLANG_RELEASE_FLAGS})
  set(CMAKE_CXX_FLAGS_RELEASE
      "${CMAKE_CXX_FLAGS_RELEASE} ${CLANG_RELEASE_STR}"
      PARENT_SCOPE)

  message(STATUS "Clang 编译标志已配置")
endfunction()

# 验证 Clang 交叉编译能力
function(moss_verify_clang_cross_compile arch)
  message(STATUS "验证 Clang 交叉编译能力: ${arch}")

  # 获取目标三元组
  if(arch STREQUAL "ARM64")
    set(test_target "aarch64-unknown-elf")
  elseif(arch STREQUAL "X86_64")
    set(test_target "x86_64-elf")
  elseif(arch STREQUAL "RISCV")
    set(test_target "riscv64-unknown-elf")
  elseif(arch STREQUAL "ARM32")
    set(test_target "arm-linux-gnueabi")
  else()
    message(FATAL_ERROR "未知架构: ${arch}")
  endif()

  # 创建测试文件
  set(test_file ${CMAKE_BINARY_DIR}/clang_cross_test.cpp)
  file(
    WRITE ${test_file}
    "#ifdef __cplusplus\n"
    "extern \"C\" {\n"
    "#endif\n"
    "int test_function() { return 42; }\n"
    "#ifdef __cplusplus\n"
    "}\n"
    "#endif\n")

  # 尝试编译测试文件
  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} --target=${test_target} -c ${test_file} -o
            ${CMAKE_BINARY_DIR}/test.o
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_output
    ERROR_VARIABLE compile_error)

  # 清理测试文件
  file(REMOVE ${test_file} ${CMAKE_BINARY_DIR}/test.o)

  if(compile_result EQUAL 0)
    message(STATUS "✓ Clang 支持 ${arch} 交叉编译")
  else()
    message(FATAL_ERROR "✗ Clang 不支持 ${arch} 交叉编译\n" "错误: ${compile_error}")
  endif()
endfunction()

# 确保所有必需的 LLVM 工具存在并检查版本
function(ensure_llvm_tools_exists)
  message(STATUS "检查 LLVM 工具链完整性...")

  set(MOSS_MIN_LLVM_VERSION "21.0")
  set(missing_tools "")
  set(version_issues "")

  # 需要检测的工具列表 - 使用结构化的方法
  set(TOOL_NAMES "clang" "clang++" "llvm-objdump" "llvm-objcopy" "llvm-nm" "llvm-readelf"
                 "llvm-strip" "llvm-strings" "llvm-addr2line" "llvm-cxxfilt" "llvm-ar"
                 "llvm-ranlib" "llvm-size" "lld" "ld.lld")

  set(TOOL_DESCRIPTIONS "编译器" "C++编译器" "对象转储工具" "对象复制工具" "符号表工具" "ELF读取工具"
                       "符号剥离工具" "字符串提取工具" "地址转换工具" "符号解析工具" "归档工具"
                       "索引生成工具" "大小分析工具" "链接器" "Unix链接器")

  list(LENGTH TOOL_NAMES tool_count)
  math(EXPR tool_max_index "${tool_count} - 1")

  # 检测每个工具
  foreach(i RANGE 0 ${tool_max_index})
    list(GET TOOL_NAMES ${i} tool_name)
    list(GET TOOL_DESCRIPTIONS ${i} tool_desc)

    # 创建安全的变量名 - 将特殊字符转换为下划线
    string(REGEX REPLACE "[^a-zA-Z0-9]" "_" var_name "${tool_name}")

    # 查找工具
    find_program(${var_name}_EXECUTABLE ${tool_name}
      HINTS /usr/lib/llvm-21/bin /usr/lib/llvm-20/bin /usr/lib/llvm-19/bin /usr/lib/llvm-18/bin
      PATHS /usr/bin /usr/local/bin)

    if(${var_name}_EXECUTABLE)
      # 工具存在，检查版本
      execute_process(
        COMMAND ${${var_name}_EXECUTABLE} --version
        OUTPUT_VARIABLE tool_version_output
        ERROR_VARIABLE tool_version_error
        RESULT_VARIABLE tool_version_result
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
      )

      if(tool_version_result EQUAL 0)
        # 解析版本号 - 从输出中提取版本信息
        string(REGEX MATCH "([0-9]+\\.[0-9]+)" tool_version "${tool_version_output}")

        if(tool_version)
          if(tool_version VERSION_LESS ${MOSS_MIN_LLVM_VERSION})
            list(APPEND version_issues "${tool_name} (${tool_desc}): 版本 ${tool_version} < ${MOSS_MIN_LLVM_VERSION}")
            message(WARNING "⚠️  ${tool_name} 版本过低: ${tool_version}, 要求: ${MOSS_MIN_LLVM_VERSION}+")
          else()
            message(STATUS "✓ ${tool_name} (${tool_desc}): 版本 ${tool_version}")
          endif()
        else()
          message(STATUS "✓ ${tool_name} (${tool_desc}): 已找到，但无法确定版本")
        endif()
      else()
        message(STATUS "✓ ${tool_name} (${tool_desc}): 已找到，但版本检查失败")
      endif()
    else()
      list(APPEND missing_tools "${tool_name}")
      message(WARNING "✗ 未找到 ${tool_name} (${tool_desc})")
    endif()
  endforeach()

  # 处理缺失的工具
  if(missing_tools)
    message(FATAL_ERROR
      "❌ 缺少必需的 LLVM 工具: ${missing_tools}\n"
      "\n"
      "请使用以下命令安装完整的 LLVM 工具链:\n"
      "\n"
      "Ubuntu/Debian 系统:\n"
      "  # 安装 LLVM 21\n"
      "  sudo apt update\n"
      "  sudo apt install llvm-21 clang-21 lld-21 libc++-21-dev libc++abi-21-dev\n"
      "\n"
      "  # 配置 update-alternatives (推荐)\n"
      "  sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-21 100\n"
      "  sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-21 100\n"
      "  sudo update-alternatives --install /usr/bin/llvm-objdump llvm-objdump /usr/bin/llvm-objdump-21 100\n"
      "  sudo update-alternatives --install /usr/bin/llvm-objcopy llvm-objcopy /usr/bin/llvm-objcopy-21 100\n"
      "  sudo update-alternatives --install /usr/bin/llvm-nm llvm-nm /usr/bin/llvm-nm-21 100\n"
      "  sudo update-alternatives --install /usr/bin/llvm-readelf llvm-readelf /usr/bin/llvm-readelf-21 100\n"
      "  sudo update-alternatives --install /usr/bin/llvm-strip llvm-strip /usr/bin/llvm-strip-21 100\n"
      "  sudo update-alternatives --install /usr/bin/llvm-strings llvm-strings /usr/bin/llvm-strings-21 100\n"
      "  sudo update-alternatives --install /usr/bin/llvm-addr2line llvm-addr2line /usr/bin/llvm-addr2line-21 100\n"
      "  sudo update-alternatives --install /usr/bin/llvm-cxxfilt llvm-cxxfilt /usr/bin/llvm-cxxfilt-21 100\n"
      "  sudo update-alternatives --install /usr/bin/llvm-ar llvm-ar /usr/bin/llvm-ar-21 100\n"
      "  sudo update-alternatives --install /usr/bin/llvm-ranlib llvm-ranlib /usr/bin/llvm-ranlib-21 100\n"
      "  sudo update-alternatives --install /usr/bin/llvm-size llvm-size /usr/bin/llvm-size-21 100\n"
      "  sudo update-alternatives --install /usr/bin/ld.lld ld.lld /usr/bin/ld.lld-21 100\n"
      "\n"
      "Fedora/RHEL 系统:\n"
      "  sudo dnf install llvm clang lld\n"
      "\n"
      "Arch Linux:\n"
      "  sudo pacman -S llvm clang lld\n"
      "\n"
      "macOS (Homebrew):\n"
      "  brew install llvm\n"
      "\n"
      "或者设置环境变量指向已安装的 LLVM 工具:\n"
      "  export PATH=\"/usr/lib/llvm-21/bin:$PATH\"\n"
    )
  endif()

  # 处理版本问题
  if(version_issues)
    message(FATAL_ERROR
      "❌ LLVM 工具版本不符合要求 (需要 ${MOSS_MIN_LLVM_VERSION}+):\n"
      "${version_issues}\n"
      "\n"
      "请升级到 LLVM ${MOSS_MIN_LLVM_VERSION}+ 或使用 update-alternatives 配置更高版本的工具。"
    )
  endif()

  message(STATUS "✅ LLVM 工具链检查完成")
endfunction()

# 主要的工具链初始化函数
function(moss_initialize_clang_toolchain arch)
  message(STATUS "初始化 Clang 工具链 (${arch})...")

  # 首先确保所有 LLVM 工具存在并版本符合要求
  ensure_llvm_tools_exists()

  moss_detect_clang_toolchain()

  # 获取Clang bin目录用于链接器检测
  get_filename_component(CLANG_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
  moss_detect_linker(${CLANG_BIN_DIR})

  moss_set_clang_flags()
  moss_verify_clang_cross_compile(${arch})

  message(STATUS "Clang 工具链初始化完成")
endfunction()
