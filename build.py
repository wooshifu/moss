#!/usr/bin/env python3
"""并发执行 Moss 的 CMake workflow 预设。"""

import io
import multiprocessing
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

# Windows 终端默认 GBK 编码无法输出 emoji，强制使用 UTF-8
if sys.platform == "win32":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")

import typer
from rich.console import Console
from rich.progress import Progress, SpinnerColumn, TextColumn, TimeElapsedColumn
from rich.table import Table

console = Console(force_terminal=True)

app = typer.Typer(help="🚀 Moss 内核多架构构建工具", rich_markup_mode="rich")
STATUS_LABELS = {
    "success": "[green]✅ 成功[/green]",
    "failed": "[red]❌ 失败[/red]",
    "skipped": "[yellow]⏭️ 跳过[/yellow]",
}


@dataclass
class BuildResult:
    """构建结果数据类"""

    preset: str
    status: str
    duration: float = 0.0
    output: str = ""


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

        return [line.split('"')[1] for line in result.stdout.splitlines() if line.strip().startswith('"')]
    except OSError as e:
        console.print(f"[red]❌ 无法获取预设列表: {e}[/red]")
        raise typer.Exit(1) from None


def filter_presets(
    all_presets: list[str],
    arch: str | None = None,
    build_type: str | None = None,
    preset: str | None = None,
) -> list[str]:
    """指定预设优先，否则按架构和构建类型筛选。"""
    if preset:
        return [preset] if preset in all_presets else []

    return [p for p in all_presets if (not arch or arch in p) and (not build_type or build_type in p)]


def execute_build(preset: str, verbose: bool, dry_run: bool) -> BuildResult:
    """执行单个预设构建"""
    if dry_run:
        return BuildResult(preset, "skipped")

    start_time = time.monotonic()
    try:
        completed = subprocess.run(
            ["cmake", "--workflow", "--preset", preset],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=True,
        )
        status, output = "success", completed.stdout if verbose else ""
    except (subprocess.CalledProcessError, OSError) as e:
        status, output = "failed", getattr(e, "stdout", None) or str(e)
    return BuildResult(preset, status, time.monotonic() - start_time, output)


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


def show_build_report(results: list[BuildResult], duration: float) -> None:
    """显示构建报告"""
    summary_table = Table(title="📊 构建汇总")
    summary_table.add_column("指标", style="cyan")
    summary_table.add_column("数量", style="bold")

    summary_table.add_row("总时间", f"{duration:.1f}s")
    for status, label in STATUS_LABELS.items():
        summary_table.add_row(label, str(sum(r.status == status for r in results)))

    console.print(summary_table)

    detail_table = Table(title="🔍 详细结果")
    detail_table.add_column("预设", style="cyan")
    detail_table.add_column("状态", justify="center")
    detail_table.add_column("耗时", justify="right")
    for result in results:
        detail_table.add_row(result.preset, STATUS_LABELS[result.status], f"{result.duration:.1f}s")
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
                except OSError:
                    console.print(f"  ??? MB - {item.name}")


@app.command()
def main(
    arch: str | None = typer.Option(None, "--arch", "-a", help="🏗️ 目标架构"),
    build_type: str | None = typer.Option(None, "--build-type", help="🔨 构建类型"),
    preset: str | None = typer.Option(None, "--preset", "-p", help="🎯 指定预设"),
    clean: bool = typer.Option(False, "--clean", "-c", help="🧹 构建前清理"),
    verbose: bool = typer.Option(False, "--verbose", "-v", help="📝 详细输出"),
    dry_run: bool = typer.Option(False, "--dry-run", help="🔍 预演模式"),
    jobs: int | None = typer.Option(None, "--jobs", "-j", min=1, help="同时构建的预设数，默认全部选中预设并发"),
):
    """🚀 Moss 内核多架构构建工具

    示例:

    • uv run build.py                    # 全部预设并发构建

    • uv run build.py --jobs 2           # 同时构建两个预设

    • uv run build.py --build-type debug  # 全部架构 debug 版本

    • uv run build.py --arch arm64       # 仅 ARM64 架构
    """

    # 参数验证
    if arch and arch not in ["arm64", "x64", "riscv64"]:
        console.print(f"[red]❌ 不支持的架构: {arch}，支持的架构: arm64, x64, riscv64[/red]")
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

    console.print("🚀 [bold]Moss 内核多架构构建工具[/bold]")

    if dry_run:
        console.print("[yellow]🔍 预演模式已启用[/yellow]")
    if clean:
        clean_directories(dry_run)

    target_presets = filter_presets(get_cmake_presets(), arch, build_type, preset)

    if not target_presets:
        console.print("[red]❌ 没有匹配的预设可构建[/red]")
        raise typer.Exit(1)

    console.print(f"[blue]🎯 将构建 {len(target_presets)} 个预设: {', '.join(target_presets)}[/blue]")

    start_time = time.monotonic()
    worker_count = min(jobs or len(target_presets), len(target_presets))
    console.print(f"[blue]⚙️  预设并发数: {worker_count}[/blue]")

    with Progress(
        SpinnerColumn(), TextColumn("[progress.description]{task.description}"), TimeElapsedColumn(), console=console
    ) as progress:
        task = progress.add_task(f"构建 0/{len(target_presets)}", total=len(target_presets))
        # spawn 避免继承 Rich 进度线程的锁；子进程仅构建，主进程统一输出。
        with ProcessPoolExecutor(max_workers=worker_count, mp_context=multiprocessing.get_context("spawn")) as pool:
            futures = {pool.submit(execute_build, name, verbose, dry_run): name for name in target_presets}
            results = {}
            for future in as_completed(futures):
                name = futures[future]
                try:
                    result = future.result()
                except Exception as e:
                    result = BuildResult(name, "failed", output=str(e))
                results[name] = result
                if result.status == "skipped":
                    console.print(f"[yellow]🔍 [DRY-RUN] cmake --workflow --preset {name}[/yellow]")
                else:
                    console.print(f"{name} {STATUS_LABELS[result.status]} ({result.duration:.1f}s)")
                    if result.output:
                        console.print(result.output.rstrip(), markup=False, highlight=False)
                progress.update(task, completed=len(results), description=f"构建 {len(results)}/{len(target_presets)}")

    ordered_results = [results[name] for name in target_presets]
    show_build_report(ordered_results, time.monotonic() - start_time)

    if any(result.status == "failed" for result in ordered_results):
        raise typer.Exit(1)


if __name__ == "__main__":
    app()
