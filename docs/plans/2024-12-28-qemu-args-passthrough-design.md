# QEMU 参数传递功能设计

**日期**: 2024-12-28
**作者**: Claude
**状态**: 已批准

## 概述

为 `run_qemu.py` 脚本添加额外 QEMU 参数传递功能，支持调试选项如 `-d int,in_asm` 的传递，以实现精确的内核运行跟踪。

## 需求分析

**当前问题**: `run_qemu.py` 不支持传递额外的 QEMU 参数，无法使用 QEMU 调试功能。

**需求**:
- 支持两种语法传递额外参数
- 保持现有脚本功能不变
- 遵循项目的 Python 开发约束

## 技术方案

### 选择方案：Typer Context 增强

**核心思路**: 利用 Typer 框架的 Context 机制和 `allow_extra_args` 特性，支持两种参数传递语法：

1. **专用选项**: `--qemu-args="-d int,in_asm"`
2. **双破折号**: `-- -d int,in_asm`

### 设计细节

#### 1. 应用配置修改
```python
app = typer.Typer(
    help="🖥️ MOSS QEMU 运行工具",
    context_settings={"allow_extra_args": True, "allow_interspersed_args": False}
)
```

#### 2. 主函数扩展
```python
@app.callback(invoke_without_command=True)
def main(
    # ... 现有参数 ...
    qemu_args: Annotated[
        Optional[str],
        typer.Option("--qemu-args", help="额外的QEMU参数（用空格分隔）")
    ] = None,
    ctx: typer.Context,
) -> None:
```

#### 3. 参数收集逻辑
```python
def collect_extra_qemu_args(ctx: typer.Context, qemu_args: Optional[str]) -> list[str]:
    """收集来自两种语法的额外QEMU参数"""
    extra_args = []

    # 方式1：--qemu-args 选项
    if qemu_args:
        extra_args.extend(shlex.split(qemu_args))

    # 方式2：双破折号后的参数
    if ctx.args:
        extra_args.extend(ctx.args)

    return extra_args
```

#### 4. QEMU 命令构建修改
在 `build_qemu_args()` 函数末尾添加额外参数：
```python
def build_qemu_args(
    cfg: QemuConfig,
    kernel_file: Path,
    *,
    use_binary: bool,
    test_mode: bool,
    debug_mode: bool,
    extra_args: list[str] = None,  # 新增参数
) -> list[str]:
    # ... 现有逻辑 ...

    # 添加用户指定的额外参数
    if extra_args:
        args.extend(extra_args)

    return args
```

### 使用示例

#### 调试选项
```bash
# 方式1：专用选项
./build/<preset>/run_qemu.sh --debug --qemu-args="-d int,in_asm"

# 方式2：双破折号
./build/<preset>/run_qemu.sh --debug -- -d int,in_asm

# 复合调试选项
./build/<preset>/run_qemu.sh --qemu-args="-d int,in_asm -D qemu_debug.log"
```

#### 其他 QEMU 选项
```bash
# 跟踪选项
./build/<preset>/run_qemu.sh -- -trace enable=virtio*

# 性能监控
./build/<preset>/run_qemu.sh --qemu-args="-d int -singlestep"
```

## 设计优势

1. **向后兼容**: 现有用法完全不受影响
2. **语法灵活**: 支持两种传参方式，适应不同使用习惯
3. **安全可靠**: 额外参数添加在命令末尾，不影响现有设备加载逻辑
4. **代码简洁**: 最少修改现有代码结构，充分利用 Typer 框架
5. **符合约束**: 遵循项目 Python 脚本开发规范

## 实现要点

- 使用 `shlex.split()` 正确处理带引号的参数
- 保持现有的横幅输出和错误处理逻辑
- 确保额外参数在 QEMU 命令显示中可见
- 添加适当的文档字符串和类型提示

## 测试计划

1. **基础功能**: 验证现有所有模式仍正常工作
2. **新增语法**: 测试两种参数传递语法
3. **边界情况**: 空参数、特殊字符、长参数列表
4. **实际调试**: 验证 `-d int,in_asm` 选项的实际效果