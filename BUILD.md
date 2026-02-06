# Moss 内核多架构构建指南

## 构建脚本 `build.sh`

我们提供了一个智能的构建脚本 `build.sh`，可以方便地构建所有或指定的架构。

### 快速开始

```bash
# 构建默认 debug 版本 (ARM64)
./build.sh --preset debug

# 构建主要架构 (ARM64, x86_64) 的所有版本
./build.sh --main

# 只构建 debug 版本
./build.sh --debug

# 构建指定架构
./build.sh --arch arm64

# 清理后构建所有架构
./build.sh --all --clean
```

### 命令行选项

| 选项 | 描述 |
|------|------|
| `-h, --help` | 显示帮助信息 |
| `-l, --list` | 列出所有可用的 workflow 预设 |
| `-a, --all` | 构建所有架构 (默认) |
| `-m, --main` | 只构建主要架构 (ARM64, x86_64) |
| `-d, --debug` | 只构建 debug 版本 |
| `-r, --release` | 只构建 release 版本 |
| `-c, --clean` | 构建前清理所有构建目录 |
| `-j, --jobs N` | 指定并行作业数 |
| `-v, --verbose` | 显示详细构建输出 |
| `--arch ARCH` | 只构建指定架构 (arm64, x86_64, riscv) |
| `--preset PRESET` | 只构建指定的预设 |
| `--dry-run` | 只显示将要执行的命令，不实际构建 |

### 支持的架构

| 架构 | 状态 | 说明 |
|------|------|------|
| **ARM64** | ✅ 完全支持 | 默认架构，完整功能 |
| **x86_64** | ⚠️ 部分支持 | 配置正确，需要x86_64启动代码 |
| **RISC-V** | ⚠️ 配置支持 | 需要RISC-V工具链 |

### 构建示例

#### 1. 开发工作流

```bash
# 快速开发构建
./build.sh --preset debug --clean

# 包含测试
./build.sh --preset debug-test

# 调试模式
./build.sh --preset debug-gdb
```

#### 2. 多架构验证

```bash
# 验证主要架构
./build.sh --main --debug --verbose

# 检查所有配置
./build.sh --all --dry-run

# 只验证配置不构建
cmake --list-presets all
```

#### 3. 发布构建

```bash
# 构建所有 release 版本
./build.sh --release --clean

# 只构建支持的架构
./build.sh --arch arm64 --release
./build.sh --arch x86_64 --release
```

### 构建结果

构建成功后，你会在以下目录找到结果：

```
build/
├── arm64-qemu-debug/          # ARM64 Debug 构建
│   ├── bin/moss.elf          # 内核二进制文件
│   ├── moss.dis              # 反汇编文件
│   ├── moss.sym              # 符号表
│   ├── compile_commands.json # 编译数据库
│   └── run_qemu.sh           # QEMU 运行脚本
├── arm64-qemu-release/        # ARM64 Release 构建
├── x86_64-qemu-debug/         # x86-64 Debug 构建
└── ...
```

### 故障排除

#### 构建失败

如果构建失败，脚本会提供详细的错误日志：

```bash
# 查看详细构建输出
./build.sh --preset debug --verbose

# 检查日志文件
# 失败时会显示日志文件路径，如：
# /tmp/moss_build_x86_64-qemu-debug_*.log
```

#### 常见问题

1. **缺少工具链**：RISC-V 需要相应的交叉编译工具链
2. **架构特定代码**：当前启动代码是 ARM64 特定的，其他架构需要对应的启动代码
3. **链接器问题**：某些架构可能需要特定的链接器配置

#### 工具链安装

```bash
# Ubuntu/Debian
sudo apt install gcc-aarch64-linux-gnu    # ARM64 (已有)
sudo apt install gcc-riscv64-linux-gnu    # RISC-V

# 或使用 Clang (推荐)
# Clang 21 已支持所有目标架构的交叉编译
```

## 手动构建

如果你偏好手动构建，可以直接使用 CMake：

```bash
# 列出所有预设
cmake --list-presets all

# 构建特定预设
cmake --workflow --preset arm64-qemu-debug
cmake --workflow --preset x86_64-qemu-release
cmake --workflow --preset riscv-qemu-debug

# 分步构建
cmake --preset debug
cmake --build --preset debug

# 运行测试
cmake --build --preset debug-test
```

## 性能基准

基于24核系统的构建性能：

| 架构 | Debug 构建时间 | Release 构建时间 | 二进制大小 |
|------|----------------|------------------|------------|
| ARM64 | ~1s | ~1s | 65KB (debug), 81KB (release) |
| x86_64 | 配置中... | 配置中... | 待测试 |

## 下一步

1. 为 x86_64、RISC-V 创建架构特定的启动代码
2. 实现硬件抽象层 (HAL)
3. 添加平台特定的设备驱动
4. 扩展测试覆盖范围