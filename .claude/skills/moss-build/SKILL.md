---
name: moss-build
description: Use when building the moss kernel, running QEMU tests, debugging with QEMU, or using Docker for builds. Covers cmake presets, build.py, run_qemu.sh, QEMU debug tracing, and Docker compose. Use this skill whenever the user mentions building, compiling, testing, running, QEMU, or Docker in the context of the moss kernel.
---

# Moss Build & Test

## Build Commands

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

## QEMU Testing

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

## QEMU Debug Options

**精确跟踪内核运行：**
```bash
# 方式1: 专用选项
./build/<preset>/run_qemu.sh --debug --qemu-args="-d int,in_asm"

# 方式2: 双破折号分隔
./build/<preset>/run_qemu.sh --debug -- -d int,in_asm

# 复合调试选项
./build/<preset>/run_qemu.sh --qemu-args="-d int,in_asm -D qemu.log -trace enable=virtio*"
```

**汇编代码分析：**
- `moss.dis` - 构建后生成的完整内核反汇编代码
- 位置：`./build/<preset>/moss.dis`
- 用于对照QEMU输出分析执行流程

## Docker QEMU Testing

无需本地安装工具链，在容器内完成构建 + QEMU 运行：

```bash
docker compose -f docker/docker-compose.yaml build                    # 构建镜像
docker compose -f docker/docker-compose.yaml run moss-qemu            # 构建内核 + 运行
docker compose -f docker/docker-compose.yaml run moss-qemu test       # 运行单元测试
docker compose -f docker/docker-compose.yaml run moss-qemu run --arch x86_64  # 指定架构
docker compose -f docker/docker-compose.yaml run moss-qemu shell      # 交互式 shell
```
