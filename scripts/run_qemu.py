#!/usr/bin/env python3
"""MOSS QEMU 运行脚本 — 替代原 run_qemu.sh.in bash 模板

从 CMake 生成的 qemu_config.json 读取构建配置，启动对应架构的 QEMU 实例。
"""

import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Annotated, Optional

import typer
from rich import print as rprint
from rich.console import Console

console = Console()

# 架构 → QEMU 参数映射
ARCH_CONFIG = {
    "ARM64": {
        "qemu_system": "qemu-system-aarch64",
        "machine": "virt",
        "cpu": "cortex-a57",
        "extra_args": ["-semihosting-config", "enable=on,target=native"],
    },
    "X86_64": {
        "qemu_system": "qemu-system-x86_64",
        "machine": "q35",
        "cpu": "qemu64",
        "extra_args": [],
    },
    "RISCV": {
        "qemu_system": "qemu-system-riscv64",
        "machine": "virt",
        "cpu": "rv64",
        "extra_args": [],
    },
}


@dataclass
class QemuConfig:
    """从 qemu_config.json 加载的构建配置"""

    build_dir: str
    arch: str
    kernel_elf: str
    test_elf: str
    kernel_bin: str
    kernel_bin_full: str

    @classmethod
    def from_json(cls, path: Path) -> "QemuConfig":
        data = json.loads(path.read_text())
        return cls(
            build_dir=data["build_dir"],
            arch=data["arch"],
            kernel_elf=data["kernel_elf"],
            test_elf=data["test_elf"],
            kernel_bin=data["kernel_bin"],
            kernel_bin_full=data["kernel_bin_full"],
        )


def resolve_kernel_file(
    cfg: QemuConfig, *, use_binary: bool, test_mode: bool
) -> tuple[Path, str]:
    """选择内核文件并返回 (路径, 描述)"""
    if test_mode:
        path = Path(cfg.test_elf)
        if not path.exists():
            console.print(f"[red]错误: 测试 ELF 文件不存在: {path}[/red]")
            console.print("请先运行构建命令生成 moss.test.elf")
            raise typer.Exit(1)
        return path, "Unit Test ELF"

    if use_binary:
        path = Path(cfg.kernel_bin)
        if not path.exists():
            console.print(f"[red]错误: 内核二进制文件不存在: {path}[/red]")
            console.print("请先运行构建命令生成 moss.bin")
            raise typer.Exit(1)
        return path, "原始二进制"

    path = Path(cfg.kernel_elf)
    if not path.exists():
        console.print(f"[red]错误: 内核 ELF 文件不存在: {path}[/red]")
        console.print("请先运行 'make' 构建内核")
        raise typer.Exit(1)
    return path, "ELF 可执行文件"


def prepare_dtb(cfg: QemuConfig, *, smp: int) -> Path | None:
    """为 ARM64/RISC-V 生成 DTB 文件，供 -device loader 加载。

    QEMU -kernel 模式对裸 ELF 不通过寄存器传递 DTB 地址，
    需要先 dumpdtb 再手动加载到已知内存地址。
    """
    if cfg.arch not in ("ARM64", "RISCV"):
        return None

    arch_cfg = ARCH_CONFIG[cfg.arch]
    dtb_path = Path(cfg.build_dir) / "qemu_virt.dtb"

    # 用 QEMU 自身导出当前机器配置的 DTB
    dump_args = [
        arch_cfg["qemu_system"],
        "-machine", f"{arch_cfg['machine']},dumpdtb={dtb_path}",
        "-cpu", arch_cfg["cpu"],
        "-smp", str(smp),
        "-m", "256M",
        "-nographic",
    ]
    subprocess.run(dump_args, check=True, capture_output=True)

    if dtb_path.exists():
        return dtb_path
    return None


# DTB 加载地址：必须在内核代码之后、RAM 范围之内
DTB_LOAD_ADDR = {
    "ARM64": "0x48000000",  # 内核从 0x40000000 起，DTB 放 +128MB 处
    "RISCV": "0x84000000",  # 内核从 0x80000000 起，DTB 放 +64MB 处
}


def build_qemu_args(
    cfg: QemuConfig,
    kernel_file: Path,
    *,
    use_binary: bool,
    test_mode: bool,
    debug_mode: bool,
) -> list[str]:
    """构造完整的 QEMU 命令行参数列表"""
    arch_cfg = ARCH_CONFIG[cfg.arch]

    smp = 1 if test_mode else 4

    # 内核加载方式
    if use_binary:
        load_addr = {"ARM64": "0x40000000", "X86_64": "0x00100000", "RISCV": "0x80200000"}[cfg.arch]
        kernel_args = [
            "-device",
            f"loader,file={kernel_file},addr={load_addr},cpu-num=0,force-raw=on",
        ]
    else:
        kernel_args = ["-kernel", str(kernel_file)]

    args = [
        arch_cfg["qemu_system"],
        "-nodefaults",
        "-nographic",
        "-chardev", "stdio,id=char0",
        "-serial", "chardev:char0",
        "-machine", arch_cfg["machine"],
        "-cpu", arch_cfg["cpu"],
        "-smp", str(smp),
        "-m", "256M",
        *kernel_args,
        "-no-reboot",
        *arch_cfg["extra_args"],
    ]

    # 为 ARM64/RISC-V 自动生成并加载 DTB
    dtb_path = prepare_dtb(cfg, smp=smp)
    if dtb_path and cfg.arch in DTB_LOAD_ADDR:
        args += [
            "-device",
            f"loader,file={dtb_path},addr={DTB_LOAD_ADDR[cfg.arch]},force-raw=on",
        ]

    if debug_mode:
        args += ["-s", "-S"]

    return args


def print_banner(
    cfg: QemuConfig,
    kernel_file: Path,
    kernel_type: str,
    *,
    use_binary: bool,
    debug_mode: bool,
    test_mode: bool,
) -> None:
    """打印启动横幅"""
    if test_mode:
        rprint("[bold]==================================================[/bold]")
        rprint(f"[bold]        MOSS Unit Tests ({cfg.arch})[/bold]")
        rprint("[bold]==================================================[/bold]")
        rprint(f"架构:       {cfg.arch}")
        rprint(f"测试文件:   {kernel_file}")
        rprint("[bold]==================================================[/bold]")
        return

    rprint("[bold]==================================================[/bold]")
    rprint(f"[bold]           Moss {cfg.arch} 内核操作系统[/bold]")
    rprint("[bold]==================================================[/bold]")
    rprint(f"架构:       {cfg.arch}")
    rprint(f"内核文件:   {kernel_file}")
    rprint(f"内核类型:   {kernel_type}")
    rprint(f"构建目录:   {cfg.build_dir}")

    if use_binary:
        size = kernel_file.stat().st_size
        rprint(f"二进制大小: {size / 1024:.1f}K")

    if debug_mode:
        rprint("[yellow]调试模式: 启用[/yellow]")
        rprint("GDB 连接: target remote localhost:1234")
        rprint("[bold]==================================================[/bold]")
        rprint("在另一个终端运行:")
        rprint(f"  gdb {cfg.kernel_elf}")
        rprint("  (gdb) target remote localhost:1234")
        rprint("  (gdb) continue")
    else:
        rprint("运行模式: 正常")
        rprint("退出方式: Ctrl+A 然后按 X")

    rprint("[bold]==================================================[/bold]")


def print_result(exit_code: int, *, test_mode: bool, use_binary: bool) -> None:
    """打印执行结果"""
    if test_mode:
        rprint("\n=== 单元测试执行完成 ===")
        rprint(f"QEMU 退出状态: {exit_code}")
        if exit_code == 0:
            rprint("[green]✅ 单元测试全部通过！[/green]")
        else:
            rprint(f"[red]❌ 单元测试失败，退出码: {exit_code}[/red]")
    else:
        mode = "原始二进制模式" if use_binary else "ELF 模式"
        rprint(f"\n=== MOSS 内核执行完成 ({mode}) ===")
        rprint(f"QEMU 退出状态: {exit_code}")
        if exit_code == 0:
            rprint("[green]✅ 内核运行成功完成！[/green]")
        else:
            rprint(f"[red]❌ 内核执行异常，退出码: {exit_code}[/red]")


app = typer.Typer(help="🖥️ MOSS QEMU 运行工具")


@app.callback(invoke_without_command=True)
def main(
    config: Annotated[
        Optional[Path],
        typer.Option("--config", "-c", help="qemu_config.json 路径"),
    ] = None,
    use_binary: Annotated[bool, typer.Option("--bin", help="使用原始二进制内核")] = False,
    debug_mode: Annotated[bool, typer.Option("--debug", help="启用 GDB 调试")] = False,
    test_mode: Annotated[bool, typer.Option("--test", help="运行单元测试")] = False,
) -> None:
    """启动 QEMU 运行 MOSS 内核

    示例:

    • uv run scripts/run_qemu.py --config build/arm64/qemu_config.json

    • uv run scripts/run_qemu.py --config build/arm64/qemu_config.json --test

    • uv run scripts/run_qemu.py --config build/arm64/qemu_config.json --debug
    """
    # 查找配置文件
    if config is None:
        # 尝试当前目录下的 qemu_config.json（CMake 构建目录场景）
        config = Path("qemu_config.json")

    if not config.exists():
        console.print(f"[red]错误: 配置文件不存在: {config}[/red]")
        console.print("请指定 --config 参数或在构建目录中运行")
        raise typer.Exit(1)

    cfg = QemuConfig.from_json(config)

    if cfg.arch not in ARCH_CONFIG:
        console.print(f"[red]错误: 不支持的架构 {cfg.arch}[/red]")
        raise typer.Exit(1)

    # 选择内核文件
    kernel_file, kernel_type = resolve_kernel_file(cfg, use_binary=use_binary, test_mode=test_mode)

    # 构造 QEMU 参数
    qemu_args = build_qemu_args(
        cfg, kernel_file, use_binary=use_binary, test_mode=test_mode, debug_mode=debug_mode
    )

    # 打印横幅
    print_banner(
        cfg,
        kernel_file,
        kernel_type,
        use_binary=use_binary,
        debug_mode=debug_mode,
        test_mode=test_mode,
    )

    qemu_cmd = ARCH_CONFIG[cfg.arch]["qemu_system"]
    rprint(f"\n启动 {qemu_cmd}...")

    # 启动 QEMU
    result = subprocess.run(qemu_args, check=False)

    # 打印结果
    print_result(result.returncode, test_mode=test_mode, use_binary=use_binary)
    sys.exit(result.returncode)


if __name__ == "__main__":
    app()
