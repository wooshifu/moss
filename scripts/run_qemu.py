#!/usr/bin/env python3
"""MOSS QEMU 运行脚本 — 替代原 run_qemu.sh.in bash 模板

从 CMake 生成的 qemu_config.json 读取构建配置，启动对应架构的 QEMU 实例。
"""

import json
import shlex
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
        "cpu": "max",
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
    kernel_image: str  # Linux-compatible Image (ARM64 only)
    cpu_cores: int = 4  # 从 CMake MOSS_CPU_CORES 变量读取，默认 4
    qemu_path: str = ""  # CMake 探测到的 QEMU 可执行文件完整路径

    @classmethod
    def from_json(cls, path: Path) -> "QemuConfig":
        data = json.loads(path.read_text())
        arch = data["arch"]
        # qemu_path: 优先使用 CMake 探测的完整路径，回退到架构默认命令名
        qemu_path = data.get("qemu_path", "") or ARCH_CONFIG.get(arch, {}).get("qemu_system", "")
        return cls(
            build_dir=data["build_dir"],
            arch=arch,
            kernel_elf=data["kernel_elf"],
            test_elf=data["test_elf"],
            kernel_bin=data["kernel_bin"],
            kernel_bin_full=data["kernel_bin_full"],
            kernel_image=data.get("kernel_image", ""),
            cpu_cores=data.get("cpu_cores", 4),
            qemu_path=qemu_path,
        )


def resolve_kernel_file(
    cfg: QemuConfig, *, use_binary: bool, use_image: bool, test_mode: bool
) -> tuple[Path, str]:
    """选择内核文件并返回 (路径, 描述)"""
    if test_mode:
        path = Path(cfg.test_elf)
        if not path.exists():
            console.print(f"[red]错误: 测试 ELF 文件不存在: {path}[/red]")
            console.print("请先运行构建命令生成 moss.test.elf")
            raise typer.Exit(1)
        return path, "Unit Test ELF"

    if use_image:
        path = Path(cfg.kernel_image) if cfg.kernel_image else Path(cfg.build_dir) / "moss.img"
        if not path.exists():
            console.print(f"[red]错误: Linux Image 文件不存在: {path}[/red]")
            console.print("请先运行构建命令（仅 ARM64 架构生成 moss.img）")
            raise typer.Exit(1)
        return path, "Linux Image"

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
    """为 ARM64/RISC-V 生成 DTB 文件，供 -device loader 加载到 RAM。

    == 为什么需要这一步 ==

    QEMU -kernel 模式仅在识别到 Linux ARM64 Image 格式（前 2 字节为
    "MZ"，偏移 0x38 处有 magic "ARMd"）时才通过 x0 寄存器传递 DTB 地址。
    对于裸 ELF（如 MOSS），QEMU 直接跳转到 ELF entry point，x0 = 0，
    且不会将 DTB 加载到 RAM。

    本函数的工作流程：
      1. 用 QEMU -machine dumpdtb 导出当前机器配置（CPU 数量、内存大小
         等参数全部匹配）生成的 DTB 文件
      2. 返回 DTB 文件路径，由调用方通过 -device loader,addr=<固定地址>
         将其放入 RAM
      3. 内核启动汇编代码在 x0==0 时线性扫描 RAM 查找 DTB magic
         (0xD00DFEED big-endian → 0xEDFE0DD0 as little-endian ldr)

    此方案不修改内核二进制格式，兼容任何直接传递 DTB 的真实 bootloader。
    """
    if cfg.arch not in ("ARM64", "RISCV"):
        return None

    arch_cfg = ARCH_CONFIG[cfg.arch]
    dtb_path = Path(cfg.build_dir) / "qemu_virt.dtb"

    # 用 QEMU 自身导出当前机器配置的 DTB
    dump_args = [
        cfg.qemu_path,
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


# DTB 加载地址：必须满足 (a) 在 RAM 范围内 (b) 不与内核代码/数据重叠。
# 内核汇编会从 RAM 顶部向下扫描 DTB magic，所以地址不需要精确匹配——
# 只要在 RAM 范围内且 64KB 对齐即可被扫描到。
DTB_LOAD_ADDR = {
    "ARM64": "0x48000000",  # QEMU virt RAM 起始 0x40000000，内核约 2MB，DTB 放 +128MB
    "RISCV": "0x84000000",  # QEMU virt RAM 起始 0x80000000，内核约 2MB，DTB 放 +64MB
}


def build_qemu_args(
    cfg: QemuConfig,
    kernel_file: Path,
    *,
    use_binary: bool,
    use_image: bool,
    test_mode: bool,
    debug_mode: bool,
) -> list[str]:
    """构造完整的 QEMU 命令行参数列表"""
    arch_cfg = ARCH_CONFIG[cfg.arch]

    smp = 1 if test_mode else cfg.cpu_cores

    # 内核加载方式
    if use_binary:
        load_addr = {"ARM64": "0x40000000", "X86_64": "0x00100000", "RISCV": "0x80200000"}[cfg.arch]
        kernel_args = [
            "-device",
            f"loader,file={kernel_file},addr={load_addr},cpu-num=0,force-raw=on",
        ]
    else:
        # Both ELF and Linux Image use -kernel; QEMU auto-detects format.
        # For Linux Image (moss.img): QEMU recognizes ARM64 magic → passes DTB via x0.
        # For ELF (moss.elf): QEMU loads at entry point → x0 = 0, DTB via -device loader.
        kernel_args = ["-kernel", str(kernel_file)]

    # -nodefaults: 禁止 QEMU 创建默认设备（IDE 磁盘等），避免与
    # -device loader 产生 "drive with bus=0, unit=0 exists" 冲突。
    # 因此需要手动通过 -chardev + -serial 建立串口输出。
    args = [
        cfg.qemu_path,
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

    # DTB handling depends on boot mode:
    #   - Linux Image (--image): QEMU handles DTB automatically, no loader needed
    #   - ELF/binary: need explicit DTB generation + loader for ARM64/RISC-V
    if not use_image:
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
    use_image: bool,
    debug_mode: bool,
    test_mode: bool,
    timeout: int | None = None,
) -> None:
    """打印启动横幅"""
    if test_mode:
        rprint("[bold]==================================================[/bold]")
        rprint(f"[bold]        MOSS Unit Tests ({cfg.arch})[/bold]")
        rprint("[bold]==================================================[/bold]")
        rprint(f"架构:       {cfg.arch}")
        rprint(f"测试文件:   {kernel_file}")
        if timeout:
            rprint(f"超时:       {timeout}s")
        rprint("[bold]==================================================[/bold]")
        return

    rprint("[bold]==================================================[/bold]")
    rprint(f"[bold]           Moss {cfg.arch} 内核操作系统[/bold]")
    rprint("[bold]==================================================[/bold]")
    rprint(f"架构:       {cfg.arch}")
    rprint(f"内核文件:   {kernel_file}")
    rprint(f"内核类型:   {kernel_type}")
    rprint(f"构建目录:   {cfg.build_dir}")

    if use_binary or use_image:
        size = kernel_file.stat().st_size
        rprint(f"镜像大小:   {size / 1024:.1f}K")

    if use_image:
        rprint("[green]DTB 传递: 自动 (Linux 启动协议, x0 寄存器)[/green]")
    elif not test_mode:
        rprint("DTB 传递: -device loader + RAM 扫描")

    if timeout:
        rprint(f"超时:       {timeout}s")

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
    use_image: Annotated[
        bool,
        typer.Option("--image/--no-image", help="使用 Linux Image 格式 (ARM64, DTB 自动传递)"),
    ] = True,
    debug_mode: Annotated[bool, typer.Option("--debug", help="启用 GDB 调试")] = False,
    test_mode: Annotated[bool, typer.Option("--test", help="运行单元测试")] = False,
    timeout: Annotated[
        Optional[int],
        typer.Option("--timeout", "-t", help="QEMU 运行超时时间（秒），超时后自动终止"),
    ] = None,
) -> None:
    """启动 QEMU 运行 MOSS 内核

    默认使用 Linux Image 格式启动（--image），QEMU 自动传递 DTB。
    使用 --no-image 回退到 ELF 模式。

    示例:

    • uv run scripts/run_qemu.py --config build/arm64/qemu_config.json

    • uv run scripts/run_qemu.py --config build/arm64/qemu_config.json --no-image

    • uv run scripts/run_qemu.py --config build/arm64/qemu_config.json --debug

    • uv run scripts/run_qemu.py --config build/arm64/qemu_config.json --timeout 30
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
    kernel_file, kernel_type = resolve_kernel_file(
        cfg, use_binary=use_binary, use_image=use_image, test_mode=test_mode
    )

    # 构造 QEMU 参数
    qemu_args = build_qemu_args(
        cfg,
        kernel_file,
        use_binary=use_binary,
        use_image=use_image,
        test_mode=test_mode,
        debug_mode=debug_mode,
    )

    # 打印横幅
    print_banner(
        cfg,
        kernel_file,
        kernel_type,
        use_binary=use_binary,
        use_image=use_image,
        debug_mode=debug_mode,
        test_mode=test_mode,
        timeout=timeout,
    )

    # 打印完整的 QEMU 命令（可直接复制到终端执行）
    rprint(f"\n[dim]$ {shlex.join(qemu_args)}[/dim]")
    if timeout:
        rprint(f"[yellow]超时: {timeout}s[/yellow]")
    rprint()

    # 启动 QEMU
    try:
        result = subprocess.run(qemu_args, check=False, timeout=timeout)
    except subprocess.TimeoutExpired:
        rprint(f"\n[red]⏰ QEMU 运行超时（{timeout}s），已终止进程[/red]")
        sys.exit(124)  # 与 GNU timeout 一致的退出码

    # 打印结果
    print_result(result.returncode, test_mode=test_mode, use_binary=use_binary)
    sys.exit(result.returncode)


if __name__ == "__main__":
    app()
