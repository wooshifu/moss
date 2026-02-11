# Moss 内核 Clang 工具链检测模块 - 重构简化版
# 统一支持 Clang/LLVM 21+ 编译器的交叉编译工具链

# =============================================================================
# 配置管理 - 集中管理所有工具链配置
# =============================================================================

# 内部配置函数 - 集中管理版本要求、路径和工具定义
function(_moss_setup_toolchain_config)
  # 版本要求 - Clang/LLVM 最低版本21
  set(MOSS_MIN_LLVM_VERSION "21.0" PARENT_SCOPE)
  set(MOSS_RECOMMENDED_LLVM_VERSION "21.0" PARENT_SCOPE)

  # LLVM 工具搜索路径 - 按优先级排序
  set(MOSS_LLVM_SEARCH_PATHS
    "/usr/lib/llvm-21/bin"
    "/usr/lib/llvm-20/bin"
    "/usr/lib/llvm-19/bin"
    "/usr/bin"
    "/usr/local/bin"
    PARENT_SCOPE)

  # 统一工具定义 - 格式: "name:description:type"
  set(MOSS_ALL_TOOLS
    "clang:编译器:required"
    "clang++:C++编译器:required"
    "lld:链接器:required"
    "ld.lld:Unix链接器:required"
    "llvm-objdump:对象转储工具:required"
    "llvm-objcopy:对象复制工具:required"
    "llvm-nm:符号表工具:required"
    "llvm-readelf:ELF读取工具:required"
    "llvm-strip:符号剥离工具:optional"
    "llvm-strings:字符串提取工具:optional"
    "llvm-addr2line:地址转换工具:optional"
    "llvm-cxxfilt:符号解析工具:optional"
    "llvm-ar:归档工具:optional"
    "llvm-ranlib:索引生成工具:optional"
    "llvm-size:大小分析工具:optional"
    PARENT_SCOPE)
endfunction()

# =============================================================================
# 核心工具检测函数 - 可复用的工具查找和版本检查
# =============================================================================

# 内部函数 - 查找并检查单个工具
function(_moss_find_and_check_tool tool_name tool_desc tool_type missing_var version_var)
  # 创建安全的变量名
  string(REGEX REPLACE "[^a-zA-Z0-9]" "_" var_name "${tool_name}")

  # 查找工具
  find_program(${var_name}_EXECUTABLE ${tool_name}
    HINTS ${MOSS_LLVM_SEARCH_PATHS}
    PATHS /usr/bin /usr/local/bin)

  if(${var_name}_EXECUTABLE)
    # 工具存在，检查版本
    execute_process(
      COMMAND ${${var_name}_EXECUTABLE} --version
      OUTPUT_VARIABLE tool_version_output
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # 解析版本号
    string(REGEX MATCH "([0-9]+\\.[0-9]+)" tool_version "${tool_version_output}")

    if(tool_version AND tool_version VERSION_LESS ${MOSS_MIN_LLVM_VERSION})
      if(tool_type STREQUAL "required")
        list(APPEND ${version_var} "${tool_name} (${tool_desc}): 版本 ${tool_version} < ${MOSS_MIN_LLVM_VERSION}")
        message(WARNING "⚠️  ${tool_name} 版本过低: ${tool_version}, 要求: ${MOSS_MIN_LLVM_VERSION}+")
      endif()
    else()
      if(tool_version)
        message(STATUS "✓ ${tool_name} (${tool_desc}): 版本 ${tool_version}")
      else()
        message(STATUS "✓ ${tool_name} (${tool_desc}): 已找到")
      endif()
    endif()
  else()
    if(tool_type STREQUAL "required")
      list(APPEND ${missing_var} "${tool_name}")
      message(WARNING "✗ 未找到 ${tool_name} (${tool_desc})")
    else()
      message(STATUS "- ${tool_name} (${tool_desc}): 可选工具，未找到")
    endif()
  endif()

  # 返回结果到父作用域
  set(${missing_var} "${${missing_var}}" PARENT_SCOPE)
  set(${version_var} "${${version_var}}" PARENT_SCOPE)
endfunction()

# 内部函数 - 生成简化的安装指南
function(_moss_generate_install_guide missing_tools)
  message(FATAL_ERROR
    "❌ 缺少必需的 LLVM 工具: ${missing_tools}\n"
    "\n"
    "请安装完整的 LLVM 21 工具链:\n"
    "\n"
    "Ubuntu/Debian: sudo apt install llvm-21 clang-21 lld-21\n"
    "Fedora/RHEL:   sudo dnf install llvm clang lld\n"
    "Arch Linux:    sudo pacman -S llvm clang lld\n"
    "macOS:         brew install llvm\n"
    "\n"
    "配置环境变量: export PATH=\"/usr/lib/llvm-21/bin:$PATH\"\n"
    "或查看项目文档获取详细的 update-alternatives 配置说明。")
endfunction()

# =============================================================================
# 公共接口函数 - 重构简化版
# =============================================================================

# Clang 编译器检测 - 重构版
function(moss_detect_clang_compiler)
  message(STATUS "检测 Clang 编译器...")

  # 检查编译器是否为 Clang
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
      "Moss 内核要求使用 Clang 编译器\n"
      "当前编译器: ${CMAKE_CXX_COMPILER_ID}\n"
      "请设置环境变量: export CXX=clang++")
  endif()

  # 检查 Clang 版本要求
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS ${MOSS_MIN_LLVM_VERSION})
    message(FATAL_ERROR
      "Clang 版本过低: ${CMAKE_CXX_COMPILER_VERSION}\n"
      "最低要求版本: ${MOSS_MIN_LLVM_VERSION}\n"
      "推荐版本: ${MOSS_RECOMMENDED_LLVM_VERSION}+")
  endif()

  # 获取 Clang 安装路径
  get_filename_component(CLANG_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
  set(CLANG_INSTALL_PREFIX ${CLANG_BIN_DIR}/.. PARENT_SCOPE)
  set(CLANG_BIN_DIR ${CLANG_BIN_DIR} PARENT_SCOPE)

  message(STATUS "✓ Clang ${CMAKE_CXX_COMPILER_VERSION}: ${CMAKE_CXX_COMPILER}")
  message(STATUS "  安装目录: ${CLANG_BIN_DIR}/..")
endfunction()

# 统一的 LLVM 工具检测 - 合并重构版
function(moss_detect_llvm_tools)
  message(STATUS "检测 LLVM 工具链...")

  set(missing_tools "")
  set(version_issues "")

  # 检测所有工具
  foreach(tool_spec ${MOSS_ALL_TOOLS})
    # 解析工具规格 "name:description:type"
    string(FIND "${tool_spec}" ":" first_sep)
    string(SUBSTRING "${tool_spec}" 0 ${first_sep} tool_name)

    math(EXPR after_first "${first_sep} + 1")
    string(SUBSTRING "${tool_spec}" ${after_first} -1 remainder)
    string(FIND "${remainder}" ":" second_sep)
    string(SUBSTRING "${remainder}" 0 ${second_sep} tool_desc)

    math(EXPR after_second "${second_sep} + 1")
    string(SUBSTRING "${remainder}" ${after_second} -1 tool_type)

    _moss_find_and_check_tool("${tool_name}" "${tool_desc}" "${tool_type}"
                              missing_tools version_issues)
  endforeach()

  # 统一错误处理
  if(missing_tools)
    _moss_generate_install_guide("${missing_tools}")
  endif()

  if(version_issues)
    message(FATAL_ERROR
      "❌ LLVM 工具版本不符合要求 (需要 ${MOSS_MIN_LLVM_VERSION}+):\n"
      "${version_issues}\n"
      "\n"
      "请升级到 LLVM ${MOSS_MIN_LLVM_VERSION}+ 或配置正确的工具版本。")
  endif()

  message(STATUS "✅ LLVM 工具链检测完成")
endfunction()

# 检测链接器 - 保持不变但简化路径查找
function(moss_detect_linker clang_bin_dir)
  message(STATUS "配置 Clang 内置链接器...")

  # 优先查找 Unix 版本的 LLD
  find_program(LLD_EXECUTABLE ld.lld
    HINTS ${clang_bin_dir} ${MOSS_LLVM_SEARCH_PATHS})

  if(LLD_EXECUTABLE)
    message(STATUS "✓ 使用 LLVM LLD 链接器: ${LLD_EXECUTABLE}")
  else()
    message(STATUS "✓ 使用 Clang 内置链接器 (LLD)")
  endif()

  # 统一设置链接器标志
  set(CMAKE_EXE_LINKER_FLAGS "-fuse-ld=lld" PARENT_SCOPE)
endfunction()

# 验证 Clang 交叉编译能力 - 保持不变
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

  # 创建和编译测试文件
  set(test_file ${CMAKE_BINARY_DIR}/clang_cross_test.cpp)
  file(WRITE ${test_file}
    "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
    "int test_function() { return 42; }\n"
    "#ifdef __cplusplus\n}\n#endif\n")

  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} --target=${test_target} -c ${test_file} -o ${CMAKE_BINARY_DIR}/test.o
    RESULT_VARIABLE compile_result
    ERROR_VARIABLE compile_error
    OUTPUT_QUIET ERROR_QUIET)

  file(REMOVE ${test_file} ${CMAKE_BINARY_DIR}/test.o)

  if(compile_result EQUAL 0)
    message(STATUS "✓ Clang 支持 ${arch} 交叉编译")
  else()
    message(FATAL_ERROR "✗ Clang 不支持 ${arch} 交叉编译\n错误: ${compile_error}")
  endif()
endfunction()

# =============================================================================
# 主入口函数 - 保持向后兼容性
# =============================================================================

# 主要的工具链初始化函数 - 重构版
function(moss_initialize_clang_toolchain arch)
  message(STATUS "初始化 Clang 工具链 (${arch})...")

  # 初始化配置
  _moss_setup_toolchain_config()

  # 按顺序执行检测和配置
  moss_detect_clang_compiler()
  moss_detect_llvm_tools()
  moss_detect_linker(${CLANG_BIN_DIR})
  moss_verify_clang_cross_compile(${arch})

  message(STATUS "✅ Clang 工具链初始化完成")
endfunction()
