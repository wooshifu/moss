# CLAUDE.md

## 构建命令

使用这些预设构建：
```bash
# 单个架构构建
cmake --workflow --preset arm64-qemu-debug
cmake --workflow --preset arm64-qemu-release
cmake --workflow --preset x86_64-qemu-debug
cmake --workflow --preset x86_64-qemu-release
cmake --workflow --preset riscv-qemu-debug
cmake --workflow --preset riscv-qemu-release

# 编译全部架构（默认行为）
uv run build.py

# 其他常用选项
uv run build.py list                    # 列出所有预设
uv run build.py --arch arm64            # 仅ARM64架构
uv run build.py -m --build-type debug   # 主要架构debug版本
uv run build.py --dry-run               # 预览模式
uv run build.py --all                   # 显式构建所有架构
```

## 编译要求

必须保证没有编译错误。项目使用 `-Weverything -Werror` 最严格模式。

## 脚本开发约束

**所有脚本使用 Python 实现：**
- 项目统一使用 Python 3.12+ 开发脚本和工具
- 禁止使用 bash、shell 脚本或其他脚本语言
- 使用 `uv run script.py` 方式运行所有 Python 脚本
- 脚本依赖在 `pyproject.toml` 中统一管理

## CMake 要求

编写 CMake 代码时要求：
- 简洁明了，避免复杂的嵌套逻辑
- 可读性高，使用清晰的变量名和注释
- 遵循项目现有的 CMake 风格和结构

## C++26 开发

Use latest C++26 standard and cutting-edge C++ features for development:
- freestanding environment (no standard library, exceptions, RTTI)
- concepts and constraints
- module support (import/module)

## C++ 命名风格

遵循以下命名约定：
- 类名：CamelCase（如 `ProcessManager`）
- 函数、变量等：lower_case（如 `get_cpu_id()`）
- 私有成员：使用 `_` 后缀（如 `cpu_count_`）

## QEMU 测试

使用 QEMU 进行测试：
```bash
# 构建完成后，运行脚本自动生成
./build/preset-name/run_qemu.sh
```