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

- 项目统一使用 Python 3.14+ 开发脚本和工具，使用 uv 来管理 python 环境
- 如果没有特殊指定，禁止使用 bash、shell 脚本或其他脚本语言
- 使用 `uv run script.py` 方式运行所有 Python 脚本
- 脚本依赖在 `pyproject.toml` 中统一管理
- python 脚本中尽量使用第三方库，简化代码逻辑，可读性，可维护性是最高优先级，只关注于业务逻辑

## CMake 要求

编写 CMake 代码时要求：

- 简洁明了，避免复杂的嵌套逻辑
- 可读性高，使用清晰的变量名和注释
- 遵循项目现有的 CMake 风格和结构

## C++26 开发

Use latest C++26 standard and cutting-edge C++ features for development:

- use c++ 26 modules first
- freestanding environment (no standard library, exceptions, RTTI)
- concepts and constraints
- module support (import/module)

## C++ 命名风格

遵循以下命名约定：

- 类名：CamelCase（如 `ProcessManager`）
- 函数、变量等：lower_case（如 `get_cpu_id()`）
- 私有成员：使用 `_` 后缀（如 `cpu_count_`）

## Commit Message 格式

**使用英文编写所有提交信息，格式如下：**

```plain
[module][subsystem] brief description of changes

Detailed explanation (optional):
- Explain the reason and impact of changes
- List important technical details
- Reference related issues or discussions
```

**格式说明：**

- **必须使用英文**
- **禁止在提交信息中包含**: Co-Authored-By: Claude <noreply@anthropic.com>
- **module**: 主要模块名（如 smp, boot, mm, process, ipc, driver）
- **subsystem**: 具体组件（如 scheduler, allocator, driver）
- **description**: 使用祈使句，首字母小写

**示例：**

```plain
[smp][scheduler] implement dynamic CPU load balancing

[boot][arm64] fix CPU topology detection using MPIDR register

[mm][allocator] optimize slab allocation for multi-core systems
```

## QEMU 测试

使用 QEMU 进行测试：

```bash
# 构建完成后，wrapper 脚本自动生成（调用 scripts/run_qemu.py）
./build/preset-name/run_qemu.sh            # 运行内核
./build/preset-name/run_qemu.sh --test     # 运行单元测试
./build/preset-name/run_qemu.sh --debug    # GDB 调试
```
