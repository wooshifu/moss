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

# 编译全部架构
./build.sh -a
```

## 编译要求

必须保证没有编译错误。项目使用 `-Weverything -Werror` 最严格模式。

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