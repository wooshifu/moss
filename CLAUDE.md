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

## C++ Modules 组织规范

本项目使用 C++26 named modules（Clang 21 + CMake）。修改或新增模块时，严格遵循以下规则：

### 三种文件角色

| 文件 | 声明方式 | 用途 |
|------|---------|------|
| Primary interface (`xxx.cppm`) | `export module moss.xxx;` | 模块入口，小模块放全部代码，大模块仅做 re-export hub |
| Partition interface (`xxx-part.cppm`) | `export module moss.xxx:part;` | 大模块按逻辑关注点拆分的子接口 |
| Implementation unit (`xxx.cpp`) | `module moss.xxx;` | 非导出的实现代码，自动访问所有 partition |

### 按模块大小选择策略

- **小模块（合计 < 500 行）**：接口 + 实现全部放在单个 `.cppm` 文件中。实现代码放在 `export namespace` 闭合之后的非导出 `namespace` 块中。
- **大模块（接口 > 1000 行）**：用 module partitions 拆分。Primary interface 只做 re-export hub（~60 行）：
  ```cpp
  export module moss.xxx;
  // imports ...
  export import :part_a;
  export import :part_b;
  ```
- **架构特定模块（如 boot）**：`.cppm` 接口 + per-arch `.cpp` 实现文件，不做 partition。

### 命名约定

- Partition 文件命名：`{module}-{partition}.cppm`（如 `mm-core.cppm`、`kernel-elf.cppm`）
- Partition 声明：`export module moss.xxx:partition_name;`（冒号语法，不是点）

### Global Module Fragment (GMF)

`module;` 到 `export module` 之间的区域。只有以下内容可以放在 GMF 中：

- `#include "arch_detect.h"`（架构检测宏）
- `extern "C" { ... }` 声明（链接器符号、汇编函数）
- `#include "libfdt.h"` 等 vendored C 头文件
- 宏定义

每个需要这些内容的 `.cppm` / `.cpp` 文件都必须有自己的 GMF —— 这是 C++ modules 的设计要求，不是重复代码。

### 关键约束

- `import` 语句必须紧跟在 `export module` 声明之后，不能出现在文件中间
- `static` 函数（内部链接）不能在 `export namespace` 块内，会触发编译错误
- `[[noreturn]]` 等属性在声明和定义上必须一致
- Implementation unit（`.cpp`）声明 `module moss.xxx;` 后自动属于整个模块，无需指定 partition

### CMake 注册

所有 `.cppm` 文件（含 partition）必须在 `PUBLIC FILE_SET CXX_MODULES` 中注册：

```cmake
target_sources(moss_xxx
    PUBLIC FILE_SET CXX_MODULES FILES
        src/xxx.cppm
        src/xxx-part_a.cppm
        src/xxx-part_b.cppm
    PRIVATE
        src/impl.cpp)
```

### 现有模块结构参考

| 模块 | 策略 | 文件数 |
|------|------|--------|
| `moss.mm` | 5 partitions (core, page_table, policy, reclaim, interface) | 6 .cppm + 6 .cpp |
| `moss.kernel` | 4 partitions (elf, syscall_table, syscall_arch, main) | 5 .cppm + 4 .cpp |
| `moss.process` | 3 partitions (types, scheduler, load_balancer) | 4 .cppm + 2 .cpp |
| `moss.timer` | 单文件（421 行） | 1 .cppm |
| `moss.fdt` | 单文件（428 行） | 1 .cppm |
| `moss.boot` | 接口 + per-arch 实现 | 1 .cppm + 4 .cpp |
| `moss.containers` | 单文件（1892 行，声明为主） | 1 .cppm |
| core 模块 | 单文件 | 各 1 .cppm |

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

构建完成后，CMake 会根据主机平台在 build 目录下生成对应的 QEMU 运行脚本（QEMU 路径已自动探测注入）：

**Linux / macOS** — `run_qemu.sh`：
```bash
# 构建完成后，wrapper 脚本自动生成（调用 scripts/run_qemu.py）
./build/<preset>/run_qemu.sh              # 运行内核（ELF 模式）
./build/<preset>/run_qemu.sh --test       # 运行单元测试
./build/<preset>/run_qemu.sh --debug      # GDB 调试模式
./build/<preset>/run_qemu.sh --bin        # 使用原始二进制内核
```

**Windows** — `run_qemu.ps1` / `run_qemu.bat`：
```powershell
.\build\<preset>\run_qemu.bat              # 运行内核（ELF 模式）
.\build\<preset>\run_qemu.bat --test       # 运行单元测试
.\build\<preset>\run_qemu.bat --debug      # GDB 调试模式
.\build\<preset>\run_qemu.bat --bin        # 使用原始二进制内核
```

也可通过 CMake 目标运行：
```bash
cmake --build --preset arm64-qemu-debug --target run-qemu
cmake --build --preset arm64-qemu-debug --target debug
cmake --build --preset arm64-qemu-debug --target test-kernel
```
