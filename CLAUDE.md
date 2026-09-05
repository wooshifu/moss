# CLAUDE.md

## 编译要求

必须保证没有编译错误。项目使用 `-Weverything -Werror` 最严格模式。

## 脚本开发约束

**所有脚本使用 Python 实现：**

- 项目统一使用 Python 3.14+ 开发脚本和工具，使用 uv 来管理 python 环境
- 如果没有特殊指定，禁止使用 bash、shell 脚本或其他脚本语言
- 使用 `uv run script.py` 方式运行所有 Python 脚本
- 脚本依赖在 `pyproject.toml` 中统一管理
- python 脚本中尽量使用第三方库，简化代码逻辑，可读性，可维护性是最高优先级，只关注于业务逻辑

## CMake 要求

- 简洁明了，避免复杂的嵌套逻辑
- 可读性高，使用清晰的变量名和注释
- 遵循项目现有的 CMake 风格和结构

## C++26 开发

Use latest C++26 standard and cutting-edge C++ features for development:

- use c++ 26 modules first
- freestanding environment (no standard library, exceptions, RTTI)
- concepts and constraints
- module support (import/module)

## C++ Modules 组织规范

详见 `moss-cpp-modules` skill。核心规则：小模块 (<500 行) 单 `.cppm`，大模块 (>1000 行) 用 partitions。

## C++ 命名风格

- 类名：CamelCase（如 `ProcessManager`）
- 函数、变量等：lower_case（如 `get_cpu_id()`）
- 私有成员：使用 `_` 后缀（如 `cpu_count_`）

## 代码质量门禁

提交前必须通过所有检查，零错误零警告：

- **C++ Lint**：`uv run lint.py --check`（系统 clang-tidy，`WarningsAsErrors: '*'`；其他架构用 `--preset <name>`）
- **Python Lint**：`uv run ruff check .`
- **Format**：`uv run scripts/format.py format --check`（clang-format + cmake-format + ruff format）

详见 `moss-quality-gate` skill。

## Commit Message 格式

详见 `moss-commit` skill。核心规则：`[module][subsystem] description`，英文，祈使句，禁止 Co-Authored-By。

## 构建与测试

详见 `moss-build` skill。快速参考：`uv run build.py`（全架构），`cmake --workflow --preset arm64-qemu-debug`（单架构）。
