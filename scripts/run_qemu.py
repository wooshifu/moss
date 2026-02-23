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
from typing import Annotated

import typer
from rich import print as rprint
from rich.console import Console

import fdt

console = Console()

# 架构 → QEMU 参数映射
ARCH_CONFIG = {
    "ARM64": {
        "qemu_system": "qemu-system-aarch64",
        "machine_base": "virt",  # gic-version appended dynamically
        "cpu": "cortex-a57",
        "extra_args": ["-semihosting-config", "enable=on,target=native"],
    },
    "X86_64": {
        "qemu_system": "qemu-system-x86_64",
        "machine": "q35",
        "cpu": "qemu64",
        "extra_args": ["-device", "isa-debug-exit,iobase=0x501,iosize=2"],
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
    kernel_bin: str  # moss_boot.bin（仅 .text.boot 段）
    kernel_bin_full: str  # moss.bin（完整内核，ARM64 含 Linux Image header）
    initramfs: str = ""  # initramfs.cpio 路径（ARM64 only）
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
            initramfs=data.get("initramfs", ""),
            cpu_cores=data.get("cpu_cores", 4),
            qemu_path=qemu_path,
        )


def resolve_machine(arch: str, *, smp: int, force_gic3: bool) -> str:
    """Resolve the QEMU -machine value for the given architecture.

    For ARM64:
      - smp > 8 or force_gic3 → "virt,gic-version=3" (GICv2 max 8 CPUs)
      - otherwise → "virt,gic-version=2" (explicit for clarity)
    """
    arch_cfg = ARCH_CONFIG[arch]
    base = arch_cfg.get("machine_base", arch_cfg.get("machine", "virt"))
    if arch == "ARM64":
        gic_ver = 3 if (smp > 8 or force_gic3) else 2
        return f"{base},gic-version={gic_ver}"
    return base


def get_qemu_version(qemu_path: str) -> str:
    """查询 QEMU 可执行文件的版本号"""
    try:
        result = subprocess.run([qemu_path, "--version"], capture_output=True, text=True, check=False)
        # 首行格式: "QEMU emulator version X.Y.Z ..."
        first_line = result.stdout.strip().splitlines()[0]
        return first_line.split("version", 1)[1].strip()
    except Exception:
        return "unknown"


def resolve_kernel_file(cfg: QemuConfig, *, use_binary: bool, test_mode: bool, debug_mode: bool) -> tuple[Path, str]:
    """选择内核文件并返回 (路径, 描述)

    启动模式优先级：
      1. --test  → test ELF (moss.test.elf)
      2. --bin   → 完整二进制 (moss.bin)，device loader 直接加载到 RAM
      3. --debug → moss.elf（保留完整 DWARF 符号，QEMU 从 ELF entry point 启动）
      4. 默认    → moss.bin（含 Linux Image header，QEMU 自动传递 DTB）
    """
    if test_mode:
        path = Path(cfg.test_elf)
        if not path.exists():
            console.print(f"[red]错误: 测试 ELF 文件不存在: {path}[/red]")
            console.print("请先运行构建命令生成 moss.test.elf")
            raise typer.Exit(1)
        return path, "Unit Test ELF"

    if use_binary:
        # 使用完整内核二进制 moss.bin（含 Linux Image header，首指令为 branch 跳过 header）
        # 而非 moss_boot.bin（仅 .text.boot 段，缺少内核代码会导致挂死）
        path = Path(cfg.kernel_bin_full)
        if not path.exists():
            console.print(f"[red]错误: 内核二进制文件不存在: {path}[/red]")
            console.print("请先运行构建命令生成 moss.bin")
            raise typer.Exit(1)
        return path, "原始二进制 (moss.bin)"

    if debug_mode:
        # 调试模式：使用 moss.elf（保留 DWARF 调试符号）
        # QEMU -kernel 可直接加载 ELF，从 ENTRY(_start) 开始执行。
        # _start 在 Linux Image Header 之后（偏移 0x40），GDB stopAtEntry
        # 直接停在有完整函数边界和行号信息的代码中，F10/F11 正常工作。
        # 注意：ELF 模式下 QEMU 不会自动通过 x0 传递 DTB，内核使用 RAM 扫描回退。
        path = Path(cfg.kernel_elf)
        if not path.exists():
            console.print(f"[red]错误: 内核 ELF 文件不存在: {path}[/red]")
            console.print("请先运行构建命令生成 moss.elf")
            raise typer.Exit(1)
        return path, "Debug ELF (moss.elf)"

    # 默认模式：使用 moss.bin
    # ARM64 的 moss.bin 含 Linux Image header，QEMU 自动识别并传递 DTB。
    path = Path(cfg.kernel_bin_full)
    if not path.exists():
        console.print(f"[red]错误: 内核文件不存在: {path}[/red]")
        console.print("请先运行构建命令生成 moss.bin")
        raise typer.Exit(1)
    return path, "Linux Image (moss.bin)"


def prepare_dtb(cfg: QemuConfig, *, smp: int, force_gic3: bool = False) -> Path | None:
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
    machine = resolve_machine(cfg.arch, smp=smp, force_gic3=force_gic3)

    # 用 QEMU 自身导出当前机器配置的 DTB
    dump_args = [
        cfg.qemu_path,
        "-machine",
        f"{machine},dumpdtb={dtb_path}",
        "-cpu",
        arch_cfg["cpu"],
        "-smp",
        str(smp),
        "-m",
        "256M",
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


# Initramfs 加载地址（--bin 模式使用 -device loader 而非 -initrd）
# 地址选择原则：在 DTB 地址之前，与内核代码（~2MB）不重叠，64KB 对齐。
INITRD_LOAD_ADDR = {
    "ARM64": 0x44000000,  # RAM 0x40000000 + 64MB，DTB 在 +128MB
    "RISCV": 0x82000000,  # RAM 0x80000000 + 32MB，DTB 在 +64MB
}


def patch_dtb_initrd(dtb_path: Path, initrd_start: int, initrd_size: int) -> None:
    """在 DTB 的 /chosen 节点中写入 linux,initrd-start/end 属性

    --bin 模式无法使用 -initrd（QEMU 要求 -kernel），因此通过 -device loader
    手动加载 initramfs 到固定 RAM 地址，同时在 DTB 中记录地址范围。
    内核 FDT parser 读取这两个属性来定位 initramfs。
    """
    dt = fdt.parse_dtb(dtb_path.read_bytes())
    chosen = dt.get_node("chosen")
    if chosen is None:
        chosen = fdt.Node("chosen")
        dt.root.append(chosen)

    # 使用 PropWords (u32) — 内核 FDT parser 同时支持 4 字节和 8 字节值
    chosen.append(fdt.PropWords("linux,initrd-start", initrd_start))
    chosen.append(fdt.PropWords("linux,initrd-end", initrd_start + initrd_size))
    dtb_path.write_bytes(dt.to_dtb())


def _needs_dtb_loader(*, use_binary: bool, debug_mode: bool) -> bool:
    """判断是否需要手动加载 DTB（通过 -device loader）

    需要手动加载 DTB 的场景：
      - --bin 模式：原始二进制没有 Linux Image header
      - --debug 模式：ELF 文件没有 Linux Image header
    默认的 moss.bin 模式不需要——QEMU 识别 ARM64 Linux Image header 后
    自动通过 x0 寄存器传递 DTB 地址。
    """
    return use_binary or debug_mode


def build_qemu_args(
    cfg: QemuConfig,
    kernel_file: Path,
    *,
    smp: int,
    use_binary: bool,
    test_mode: bool,
    debug_mode: bool,
    force_gic3: bool = False,
    extra_args: list[str] | None = None,
) -> list[str]:
    """构造完整的 QEMU 命令行参数列表"""
    arch_cfg = ARCH_CONFIG[cfg.arch]

    smp = 1 if test_mode else smp
    machine = resolve_machine(cfg.arch, smp=smp, force_gic3=force_gic3)

    # 内核加载方式
    if use_binary:
        load_addr = {"ARM64": "0x40200000", "X86_64": "0x00100000", "RISCV": "0x80200000"}[cfg.arch]
        kernel_args = [
            "-device",
            f"loader,file={kernel_file},addr={load_addr},cpu-num=0,force-raw=on",
        ]
    else:
        # ELF 和 Linux Image 都使用 -kernel；QEMU 自动检测格式。
        # moss.bin (Linux Image): QEMU 识别 ARM64 magic → 自动通过 x0 传递 DTB
        # moss.elf (ELF): QEMU 加载到 entry point → x0 = 0，需手动加载 DTB
        kernel_args = ["-kernel", str(kernel_file)]

    # -nodefaults: 禁止 QEMU 创建默认设备（IDE 磁盘等），避免与
    # -device loader 产生 "drive with bus=0, unit=0 exists" 冲突。
    # 因此需要手动通过 -chardev + -serial + -mon 建立串口输出和监控台。
    #
    # chardev 参数说明：
    #   mux=on   — 复用 stdio，让串口和 monitor 共享同一个终端
    #   signal=off — 禁止 chardev 拦截 Ctrl+C（由 mux 层处理转义序列）
    # 有了 mux=on + -mon，用户可以用 Ctrl+A X 退出 QEMU。
    args = [
        cfg.qemu_path,
        "-nodefaults",
        "-nographic",
        "-chardev",
        "stdio,id=char0,mux=on,signal=off",
        "-serial",
        "chardev:char0",
        "-mon",
        "chardev=char0,mode=readline",
        "-machine",
        machine,
        "-cpu",
        arch_cfg["cpu"],
        "-smp",
        str(smp),
        "-m",
        "256M",
        *kernel_args,
        "-no-reboot",
        *arch_cfg["extra_args"],
    ]

    # Initramfs: pass CPIO archive to QEMU via -initrd
    # QEMU loads it into guest RAM and records the address in DTB /chosen node
    # as linux,initrd-start / linux,initrd-end (read by the kernel FDT parser)
    if cfg.initramfs and not test_mode and not use_binary:
        initrd_path = Path(cfg.initramfs)
        if initrd_path.exists():
            args += ["-initrd", str(initrd_path)]

    # DTB 处理：
    #   - moss.bin（默认）: QEMU 自动识别 Linux Image header，通过 x0 传递 DTB
    #   - --bin / --debug 模式: 无 Linux Image header，需手动导出 DTB 并加载到 RAM
    if _needs_dtb_loader(use_binary=use_binary, debug_mode=debug_mode):
        dtb_path = prepare_dtb(cfg, smp=smp, force_gic3=force_gic3)

        # --bin 模式: initramfs 也需通过 -device loader 加载（-initrd 需要 -kernel）
        # 同时在 DTB /chosen 中写入 linux,initrd-start/end 让内核定位 initramfs
        if use_binary and cfg.initramfs and not test_mode:
            initrd_path = Path(cfg.initramfs)
            if initrd_path.exists() and dtb_path and cfg.arch in INITRD_LOAD_ADDR:
                initrd_addr = INITRD_LOAD_ADDR[cfg.arch]
                patch_dtb_initrd(dtb_path, initrd_addr, initrd_path.stat().st_size)
                args += [
                    "-device",
                    f"loader,file={initrd_path},addr={hex(initrd_addr)},force-raw=on",
                ]

        if dtb_path and cfg.arch in DTB_LOAD_ADDR:
            args += [
                "-device",
                f"loader,file={dtb_path},addr={DTB_LOAD_ADDR[cfg.arch]},force-raw=on",
            ]

    if debug_mode:
        args += ["-s", "-S"]

    # 添加用户指定的额外参数
    if extra_args:
        args.extend(extra_args)

    return args


def print_banner(
    cfg: QemuConfig,
    kernel_file: Path,
    kernel_type: str,
    *,
    use_binary: bool,
    debug_mode: bool,
    test_mode: bool,
    timeout: int | None = None,
) -> None:
    """打印启动横幅"""
    qemu_version = get_qemu_version(cfg.qemu_path)

    if test_mode:
        rprint("[bold]==================================================[/bold]")
        rprint(f"[bold]        MOSS Unit Tests ({cfg.arch})[/bold]")
        rprint("[bold]==================================================[/bold]")
        rprint(f"架构:       {cfg.arch}")
        rprint(f"QEMU:       {qemu_version}")
        rprint(f"测试文件:   {kernel_file}")
        if timeout:
            rprint(f"超时:       {timeout}s")
        rprint("[bold]==================================================[/bold]")
        return

    rprint("[bold]==================================================[/bold]")
    rprint(f"[bold]           Moss {cfg.arch} 内核操作系统[/bold]")
    rprint("[bold]==================================================[/bold]")
    rprint(f"架构:       {cfg.arch}")
    rprint(f"QEMU:       {qemu_version}")
    rprint(f"内核文件:   {kernel_file}")
    rprint(f"内核类型:   {kernel_type}")
    rprint(f"构建目录:   {cfg.build_dir}")

    if use_binary:
        size = kernel_file.stat().st_size
        rprint(f"镜像大小:   {size / 1024:.1f}K")

    # Initramfs 信息
    if cfg.initramfs:
        initrd_path = Path(cfg.initramfs)
        if initrd_path.exists():
            rprint(f"[green]Initramfs:  {initrd_path.name} ({initrd_path.stat().st_size / 1024:.1f}K)[/green]")
        else:
            rprint("[yellow]Initramfs:  configured but not built[/yellow]")

    # DTB 传递方式提示
    if _needs_dtb_loader(use_binary=use_binary, debug_mode=debug_mode):
        rprint("DTB 传递:   -device loader + RAM 扫描")
        if use_binary and cfg.initramfs and cfg.arch in INITRD_LOAD_ADDR:
            rprint(f"Initrd:     -device loader @ {hex(INITRD_LOAD_ADDR[cfg.arch])}")
    else:
        rprint("[green]DTB 传递: 自动 (Linux 启动协议, x0 寄存器)[/green]")

    if timeout:
        rprint(f"超时:       {timeout}s")

    if debug_mode:
        rprint("[yellow]调试模式: 启用 (QEMU 启动 moss.elf, GDB 远程连接)[/yellow]")
        rprint("GDB 连接: target remote localhost:1234")
        rprint("[bold]==================================================[/bold]")
        rprint("VS Code: F5 自动连接 GDB，停在 _start")
        rprint("手动调试:")
        rprint(f"  gdb-multiarch {cfg.kernel_elf}")
        rprint("  (gdb) target remote localhost:1234")
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


def collect_extra_qemu_args(ctx: typer.Context, qemu_args: str | None) -> list[str]:
    """收集来自两种语法的额外QEMU参数

    Args:
        ctx: Typer上下文，包含双破折号后的参数
        qemu_args: --qemu-args选项的值

    Returns:
        额外QEMU参数列表
    """
    extra_args = []

    # 方式1：--qemu-args 选项
    if qemu_args:
        extra_args.extend(shlex.split(qemu_args))

    # 方式2：双破折号后的参数
    if hasattr(ctx, "args") and ctx.args:
        extra_args.extend(ctx.args)

    return extra_args


app = typer.Typer(
    help="🖥️ MOSS QEMU 运行工具", context_settings={"allow_extra_args": True, "allow_interspersed_args": False}
)


@app.callback(invoke_without_command=True)
def main(
    ctx: typer.Context,
    config: Annotated[
        Path | None,
        typer.Option("--config", "-c", help="qemu_config.json 路径"),
    ] = None,
    use_binary: Annotated[bool, typer.Option("--bin", help="使用原始二进制内核")] = False,
    debug_mode: Annotated[bool, typer.Option("--debug", help="启用 GDB 调试")] = False,
    test_mode: Annotated[bool, typer.Option("--test", help="运行单元测试")] = False,
    timeout: Annotated[
        int | None,
        typer.Option("--timeout", "-t", help="QEMU 运行超时时间（秒），超时后自动终止"),
    ] = None,
    smp: Annotated[int, typer.Option("--smp", help="CPU 核心数（smp>8 时自动启用 GICv3）")] = 8,
    extra_qemu_args: Annotated[str | None, typer.Option("--qemu-args", help="额外的QEMU参数（用空格分隔）")] = None,
    force_gic3: Annotated[bool, typer.Option("--gic3", help="强制使用 GICv3（ARM64 only, smp>8 时自动启用）")] = False,
) -> None:
    """启动 QEMU 运行 MOSS 内核

    所有模式均使用 moss.bin 启动（ARM64 含 Linux Image header，QEMU 自动传递 DTB）。
    --debug 模式额外启动 GDB server（-s -S），GDB 通过 "file moss.elf" 加载符号表。

    支持两种方式传递额外的 QEMU 参数：
    1. --qemu-args 选项: --qemu-args="-d int,in_asm"
    2. 双破折号分隔: --debug -- -d int,in_asm

    示例:

    • uv run scripts/run_qemu.py --config build/arm64/qemu_config.json

    • uv run scripts/run_qemu.py --config build/arm64/qemu_config.json --debug

    • uv run scripts/run_qemu.py --config build/arm64/qemu_config.json --timeout 30

    • uv run scripts/run_qemu.py --config build/arm64/qemu_config.json --qemu-args="-d int,in_asm"

    • uv run scripts/run_qemu.py --config build/arm64/qemu_config.json --debug -- -d int,in_asm -trace enable=virtio*
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
        cfg, use_binary=use_binary, test_mode=test_mode, debug_mode=debug_mode
    )

    # 收集额外的 QEMU 参数
    extra_args = collect_extra_qemu_args(ctx, extra_qemu_args)

    # 构造 QEMU 参数
    qemu_cmd_args = build_qemu_args(
        cfg,
        kernel_file,
        smp=smp,
        use_binary=use_binary,
        test_mode=test_mode,
        debug_mode=debug_mode,
        force_gic3=force_gic3,
        extra_args=extra_args,
    )

    # 打印横幅
    print_banner(
        cfg,
        kernel_file,
        kernel_type,
        use_binary=use_binary,
        debug_mode=debug_mode,
        test_mode=test_mode,
        timeout=timeout,
    )

    # 打印完整的 QEMU 命令（可直接复制到终端执行）
    rprint(f"\n[dim]$ {shlex.join(qemu_cmd_args)}[/dim]")
    if timeout:
        rprint(f"[yellow]超时: {timeout}s[/yellow]")
    rprint()

    # 启动 QEMU
    try:
        result = subprocess.run(qemu_cmd_args, check=False, timeout=timeout)
    except subprocess.TimeoutExpired:
        rprint(f"\n[red]⏰ QEMU 运行超时（{timeout}s），已终止进程[/red]")
        sys.exit(124)  # 与 GNU timeout 一致的退出码

    # 打印结果
    print_result(result.returncode, test_mode=test_mode, use_binary=use_binary)
    sys.exit(result.returncode)


if __name__ == "__main__":
    app()
