---
name: moss-cpp-modules
description: Use when creating, modifying, or reviewing C++ module files (.cppm, .cpp) in the moss kernel. Covers file roles, partition strategy, naming conventions, Global Module Fragment rules, import constraints, and CMake registration for C++26 named modules with Clang 21. Use this skill whenever working with module declarations, partition files, or adding new modules to the build system.
---

# C++ Modules Organization

本项目使用 C++26 named modules（Clang 21 + CMake）。修改或新增模块时，严格遵循以下规则：

## File Roles

| 文件 | 声明方式 | 用途 |
|------|---------|------|
| Primary interface (`xxx.cppm`) | `export module moss.xxx;` | 模块入口，小模块放全部代码，大模块仅做 re-export hub |
| Partition interface (`xxx-part.cppm`) | `export module moss.xxx:part;` | 大模块按逻辑关注点拆分的子接口 |
| Implementation unit (`xxx.cpp`) | `module moss.xxx;` | 非导出的实现代码，自动访问所有 partition |

## Module Size Strategy

- **小模块（合计 < 500 行）**：接口 + 实现全部放在单个 `.cppm` 文件中。实现代码放在 `export namespace` 闭合之后的非导出 `namespace` 块中。
- **大模块（接口 > 1000 行）**：用 module partitions 拆分。Primary interface 只做 re-export hub（~60 行）：
  ```cpp
  export module moss.xxx;
  // imports ...
  export import :part_a;
  export import :part_b;
  ```
- **架构特定模块（如 boot）**：`.cppm` 接口 + per-arch `.cpp` 实现文件，不做 partition。

## Naming Conventions

- Partition 文件命名：`{module}-{partition}.cppm`（如 `mm-core.cppm`、`kernel-elf.cppm`）
- Partition 声明：`export module moss.xxx:partition_name;`（冒号语法，不是点）

## Global Module Fragment (GMF)

`module;` 到 `export module` 之间的区域。只有以下内容可以放在 GMF 中：

- `extern "C" { ... }` 声明（链接器符号、汇编函数）
- `#include "libfdt.h"` 等 vendored C 头文件
- 宏定义

每个需要这些内容的 `.cppm` / `.cpp` 文件都必须有自己的 GMF —— 这是 C++ modules 的设计要求，不是重复代码。

## Key Constraints

- `import` 语句必须紧跟在 `export module` 声明之后，不能出现在文件中间
- `static` 函数（内部链接）不能在 `export namespace` 块内，会触发编译错误
- `[[noreturn]]` 等属性在声明和定义上必须一致
- Implementation unit（`.cpp`）声明 `module moss.xxx;` 后自动属于整个模块，无需指定 partition

## CMake Registration

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

## Existing Module Reference

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
