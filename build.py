#!/usr/bin/env python3
"""Moss 内核多架构构建脚本 - 现代化版本"""

import io
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

# Windows 终端默认 GBK 编码无法输出 emoji，强制使用 UTF-8
if sys.platform == "win32":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")

import typer
from rich import print as rprint
from rich.console import Console
from rich.progress import Progress, SpinnerColumn, TextColumn, TimeElapsedColumn
from rich.table import Table

console = Console(force_terminal=True)

# 创建应用
app = typer.Typer(help="🚀 Moss 内核多架构构建工具", rich_markup_mode="rich")


@dataclass
class BuildResult:
    """构建结果数据类"""

    preset: str
    status: str = "pending"
    duration: float = 0.0
    error_msg: str = ""


@dataclass
class BuildStats:
    """构建统计信息"""

    results: list[BuildResult] = field(default_factory=list)
    start_time: float = field(default_factory=time.time)

    @property
    def success_count(self) -> int:
        return sum(1 for r in self.results if r.status == "success")

    @property
    def failed_count(self) -> int:
        return sum(1 for r in self.results if r.status == "failed")

    @property
    def skipped_count(self) -> int:
        return sum(1 for r in self.results if r.status == "skipped")


def get_cmake_presets() -> list[str]:
    """获取所有可用的 CMAKE workflow 预设"""
    try:
        result = subprocess.run(
            ["cmake", "--list-presets", "workflow"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )

        presets = []
        for line in result.stdout.splitlines():
            if '"' in line and not line.strip().startswith("Available"):
                preset = line.split('"')[1]
                presets.append(preset)

        return presets
    except Exception as e:
        console.print(f"[red]❌ 无法获取预设列表: {e}[/red]")
        raise typer.Exit(1) from None


def filter_presets(
    all_presets: list[str],
    arch: str | None = None,
    build_type: str | None = None,
    main_only: bool = False,
    preset: str | None = None,
) -> list[str]:
    """智能过滤预设"""
    if preset:
        return [preset] if preset in all_presets else []

    filtered = all_presets.copy()

    # 架构过滤 - 修正逻辑
    if arch:
        if arch == "arm64":
            filtered = [p for p in filtered if "arm64" in p]
        elif arch == "x86_64":
            filtered = [p for p in filtered if "x86_64" in p]
        elif arch == "riscv":
            filtered = [p for p in filtered if "riscv" in p]

    # 构建类型过滤
    if build_type:
        filtered = [p for p in filtered if build_type in p]

    # 主要架构过滤
    if main_only:
        filtered = [p for p in filtered if any(main_arch in p for main_arch in ["arm64", "x86_64"])]

    return filtered


def execute_build(preset: str, verbose: bool, dry_run: bool) -> BuildResult:
    """执行单个预设构建"""
    result = BuildResult(preset=preset)

    if dry_run:
        console.print(f"[yellow]🔍 [DRY-RUN] cmake --workflow --preset {preset}[/yellow]")
        result.status = "skipped"
        return result

    start_time = time.time()
    cmd = ["cmake", "--workflow", "--preset", preset]

    try:
        if verbose:
            console.print(f"[blue]▶️  执行: {' '.join(cmd)}[/blue]")
            subprocess.run(cmd, check=True)
        else:
            subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=True,
            )

        result.status = "success"
        result.duration = time.time() - start_time
        console.print(f"[green]✅ {preset} 构建成功 ({result.duration:.1f}s)[/green]")

    except subprocess.CalledProcessError as e:
        result.status = "failed"
        result.duration = time.time() - start_time
        result.error_msg = getattr(e, "stderr", "") or str(e)
        console.print(f"[red]❌ {preset} 构建失败 ({result.duration:.1f}s)[/red]")
        if not verbose and result.error_msg:
            console.print(f"[dim red]{result.error_msg.strip()}[/dim red]")

    return result


def clean_directories(dry_run: bool) -> None:
    """清理构建目录"""
    dirs_to_clean = ["build", "install"]

    if dry_run:
        console.print("[yellow]🔍 [DRY-RUN] 将清理目录: " + ", ".join(dirs_to_clean) + "[/yellow]")
        return

    for dir_path in dirs_to_clean:
        path = Path(dir_path)
        if path.exists():
            shutil.rmtree(path)
            console.print(f"[green]🧹 已清理: {dir_path}/[/green]")


def show_build_report(stats: BuildStats) -> None:
    """显示构建报告"""
    total_time = time.time() - stats.start_time

    # 创建汇总表格
    summary_table = Table(title="📊 构建汇总")
    summary_table.add_column("指标", style="cyan")
    summary_table.add_column("数量", style="bold")

    summary_table.add_row("总时间", f"{total_time:.1f}s")
    summary_table.add_row("✅ 成功", f"[green]{stats.success_count}[/green]")
    summary_table.add_row("❌ 失败", f"[red]{stats.failed_count}[/red]")
    summary_table.add_row("⏭️ 跳过", f"[yellow]{stats.skipped_count}[/yellow]")

    console.print(summary_table)

    # 详细结果表格
    if stats.results:
        detail_table = Table(title="🔍 详细结果")
        detail_table.add_column("预设", style="cyan")
        detail_table.add_column("状态", justify="center")
        detail_table.add_column("耗时", justify="right")

        for result in stats.results:
            status_map = {
                "success": "[green]✅ 成功[/green]",
                "failed": "[red]❌ 失败[/red]",
                "skipped": "[yellow]⏭️ 跳过[/yellow]",
            }

            detail_table.add_row(result.preset, status_map.get(result.status, result.status), f"{result.duration:.1f}s")

        console.print(detail_table)

    # 构建目录信息
    build_path = Path("build")
    if build_path.exists():
        console.print("[cyan]📁 构建输出:[/cyan]")
        for item in build_path.iterdir():
            if item.is_dir():
                try:
                    size = sum(f.stat().st_size for f in item.rglob("*") if f.is_file())
                    size_mb = size / (1024 * 1024)
                    console.print(f"  {size_mb:.1f}MB - {item.name}")
                except Exception:
                    console.print(f"  ??? MB - {item.name}")


def list_presets():
    """📋 列出所有可用的构建预设"""
    presets = get_cmake_presets()

    table = Table(title="🔧 可用的 CMAKE Workflow 预设")
    table.add_column("预设名称", style="cyan")

    for preset in presets:
        table.add_row(preset)

    console.print(table)


@app.callback(invoke_without_command=True)
def main(
    ctx: typer.Context,
    arch: str | None = typer.Option(None, "--arch", "-a", help="🏗️ 目标架构"),
    build_type: str | None = typer.Option(None, "--build-type", help="🔨 构建类型"),
    preset: str | None = typer.Option(None, "--preset", "-p", help="🎯 指定预设"),
    main_only: bool = typer.Option(False, "--main", "-m", help="⭐ 仅主要架构 (ARM64, x86_64)"),
    clean: bool = typer.Option(False, "--clean", "-c", help="🧹 构建前清理"),
    verbose: bool = typer.Option(False, "--verbose", "-v", help="📝 详细输出"),
    dry_run: bool = typer.Option(False, "--dry-run", help="🔍 预演模式"),
    all_arch: bool = typer.Option(False, "--all", help="🌐 构建所有架构"),
):
    """🚀 Moss 内核多架构构建工具

    示例:

    • uv run build.py --all              # 构建所有架构

    • uv run build.py -m --build-type debug  # 主要架构 debug 版本

    • uv run build.py --arch arm64       # 仅 ARM64 架构
    """

    # 如果有子命令被调用，直接返回
    if ctx.invoked_subcommand is not None:
        return

    # 参数验证
    if arch and arch not in ["arm64", "x86_64", "riscv"]:
        console.print(f"[red]❌ 不支持的架构: {arch}，支持的架构: arm64, x86_64, riscv[/red]")
        raise typer.Exit(1)

    if build_type and build_type not in ["debug", "release", "relwithdebinfo"]:
        console.print(f"[red]❌ 不支持的构建类型: {build_type}，支持的类型: debug, release, relwithdebinfo[/red]")
        raise typer.Exit(1)

    # 环境检查
    if not Path("CMakeLists.txt").exists() or not Path("CMakePresets.json").exists():
        console.print("[red]❌ 请在项目根目录运行此脚本[/red]")
        raise typer.Exit(1)

    if shutil.which("cmake") is None:
        console.print("[red]❌ 未找到 cmake 命令[/red]")
        raise typer.Exit(1)

    # 显示配置信息
    rprint("🚀 [bold]Moss 内核多架构构建工具[/bold]")

    if dry_run:
        rprint("[yellow]🔍 预演模式已启用[/yellow]")
    if clean:
        clean_directories(dry_run)

    # 获取并过滤预设
    all_presets = get_cmake_presets()

    # 如果没有指定任何过滤条件，默认构建所有架构
    if not any([arch, build_type, main_only, preset]) or all_arch:
        target_presets = all_presets
    else:
        target_presets = filter_presets(all_presets, arch, build_type, main_only, preset)

    if not target_presets:
        console.print("[red]❌ 没有匹配的预设可构建[/red]")
        raise typer.Exit(1)

    console.print(f"[blue]🎯 将构建 {len(target_presets)} 个预设: {', '.join(target_presets)}[/blue]")

    # 执行构建
    stats = BuildStats()

    with Progress(
        SpinnerColumn(), TextColumn("[progress.description]{task.description}"), TimeElapsedColumn(), console=console
    ) as progress:
        for preset in target_presets:
            task = progress.add_task(f"构建 {preset}", total=1)
            result = execute_build(preset, verbose, dry_run)
            stats.results.append(result)
            progress.update(task, completed=1)

    # 显示报告
    show_build_report(stats)

    # 退出状态
    if stats.failed_count > 0:
        raise typer.Exit(1)


# 添加 list 子命令
@app.command("list")
def list_command():
    """📋 列出所有可用的构建预设"""
    list_presets()


if __name__ == "__main__":
    app()
