# 可移植的构建、部署、串口与 GDB 流程

## 发现环境，绑定本次参数

代理应先取得下列实际值，再执行后续命令；不要把占位符原样交给 shell，也不要要求接收者使用发布者的目录结构。

| 变量 | 取值方式 |
| --- | --- |
| `MOSS_REPO` | 用户指定的 checkout，或在工作目录执行 `git rev-parse --show-toplevel` 后核验 `scripts/rpi.py`、`scripts/artifacts.py` 和 CMake 配置 |
| `SKILL_DIR` | 当前实际加载的 `SKILL.md` 所在目录；可位于仓库或任意 skill 安装目录 |
| `PRESET` | 从该仓库 `cmake --list-presets` 和 preset 文件选择，如 ARM64 Debug、Release 或 RelWithDebInfo |
| `BUILD_DIR` | 用户指定或为本任务选择的独立构建目录；使用绝对路径，不能与其他构建进程竞争 |
| `MANIFEST` | 本次构建生成的 manifest，通常为所选 `BUILD_DIR` 中的 `moss-artifacts.json` |
| `DTB_PATH` | 从目标板 firmware 捕获并验证的 DTB 文件，见 [hardware.md](hardware.md) |
| `RUN` | 用户指定或在所选构建目录下新建的唯一结果目录，使用绝对路径 |
| `PORT` | 本次枚举并核实的串口设备路径；只有一个候选时可自动选择 |
| `JLINK_SERIAL` | 本次核实的 J-Link 序列号；只有一个探针时可省略对应 CLI 参数 |
| `JTAG_KHZ` | 根据探针/目标配置选择的速率上限；先低速验证，再按回读结果调整 |

环境变量中不要放字面的 `$MOSS_REPO` 等未展开文本。shell 示例适用于 zsh/bash，始终引用路径变量以支持空格和非 ASCII 路径。从 `MOSS_REPO` 根目录执行仓库命令：

```sh
cd "$MOSS_REPO"
uv run scripts/rpi.py --help
uv run scripts/rpi.py deploy --help
uv run cmake --list-presets
```

附带 wrapper 的 `--repo` 可选：不指定时依次检查当前工作目录及祖先、脚本位置及祖先；显式路径优先且无效时直接报错，不静默回退到另一份 checkout。它不会扫描用户主目录。它将转发给 loader 的相对文件路径解释为仓库根目录下的路径。

## 主机准备

需要 Python ≥3.12、uv、带 J-Link 支持的 OpenOCD、LLVM Clang/objcopy/nm、LLD，以及 dtc 的 fdtget/fdtput。以 `command -v` / `shutil.which` 检查可执行文件和版本，只安装确实缺少的工具。

macOS 上若 Homebrew LLVM 未进入 PATH，用 `brew --prefix llvm` 取得实际安装前缀，将其 bin 目录加入该任务环境；不要写死 Intel/Apple Silicon 的 Homebrew 路径。Linux 用发行版的包管理器或已有工具链，确认 ARM64 cross target 和 OpenOCD J-Link 支持；不能仅凭存在同名命令判定满足需求。

仓库串口代码使用 POSIX `fcntl/termios`，直接运行路径为 macOS/Linux。Windows 使用已实际接通 USB 的 WSL/Linux 环境或 Linux 主机；构建、UART 和此 loader 的 OpenOCD 连接应在一致可达的环境中运行。原生 Windows 需要另行移植串口层，不在此声明支持。缺少权限或 USB 转发时先定位缺失条件，不全局修改权限或默认以管理员运行。

`uv run scripts/rpi.py ...` 是支持的入口。需要使用项目 lockfile 时用 `uv run --frozen python scripts/rpi.py ...`。不要混同 `uv run --frozen scripts/rpi_perf.py ...`：PEP 723 inline script 可能没有自己的 lockfile。

## 构建 kernel 与 initramfs

选择实际存在的 ARM64 preset：Debug 用于源码调试，Release 用于性能实验，RelWithDebInfo 在仓库提供时用于优化加符号。查看 preset 的实际编译选项，不能仅凭名称推断优化级别。

```sh
# ON 同时保留独立的内核测试产物；仅生产启动可选择 OFF。
uv run cmake --preset "$PRESET" -B "$BUILD_DIR" -DMOSS_BUILD_TESTS=ON
uv run cmake --build "$BUILD_DIR"
```

CMake 同时构建用户态并调用 cpio 生成器。以 manifest 为准：生产 kernel 是 ARM64 Linux Image，另有 initramfs 和 ELF，测试模式还有 validation kernel/initramfs。不要把裸 ELF 当 Image、手造空 initramfs，或混用不同构建的 kernel/libc/用户程序。

```sh
uv run --frozen python - "$MANIFEST" <<'PY'
import sys
from pathlib import Path
from scripts.artifacts import Artifacts
cfg = Artifacts.load(Path(sys.argv[1]))
print(cfg.arch, cfg.build['type'])
for name in ('kernel', 'initramfs', 'debug_symbols'):
    print(name, cfg.require(name))
PY
```

## JTAG 与部署

只读发现不要运行 `jtag`，因为它会 halt CPU0。已经授权部署并确认板子归本任务使用后，在持久终端启动 server：

```sh
uv run scripts/rpi.py jtag --jlink "$JLINK_SERIAL"
```

单探针可省略 `--jlink`。检查当前脚本的附加速度、重试期限、bind 地址与端口；常见配置是 localhost Tcl `6666` 和 CPU0..3 GDB `3333..3336`，这些是默认接口约定，不是主机身份。已有其他任务的 server/collector 时先协调，不抢占。

另一终端进行部署：

```sh
mkdir -p "$RUN"
uv run scripts/rpi.py deploy \
  --manifest "$MANIFEST" --dtb "$DTB_PATH" --port "$PORT" \
  --output "$RUN" --check
```

检查 loader 是否在部署过程中重设 adapter speed；若会重设，提前在 server 中降速可能被覆盖。原生 CLI 提供速率选项时优先使用它。否则可调用本 skill 的 wrapper：

```sh
uv run --frozen python "$SKILL_DIR/scripts/deploy.py" \
  --repo "$MOSS_REPO" --jtag-khz "$JTAG_KHZ" -- \
  --manifest "$MANIFEST" --dtb "$DTB_PATH" --port "$PORT" \
  --output "$RUN" --check
```

wrapper 强制要求速度参数，没有来自特定探针的默认值；只对现有 adapter speed 命令加上限，不提高原本更低的速度，不修改仓库、不自动复位。较低速率也不能代替正确接线与完整回读。若当前 loader 不再使用该接口，应采用新的原生接口，不静默跳过限速。

检查实际 loader 的 Image 对齐、运行时大小、stub/initrd/DTB 窗口与 firmware RAM 范围。内存布局由目标协议与实现决定，不沿用另一块板的内存分配；保留不重叠检查。

部署阶段应包含：准备 → 检查 bootloader EL2h → cache-off stub → 上传与完整回读 → 按 Image 协议进入内核。Linux Image 的 DTB 通过 x0 传入，x1..x3 清零。进度按已完成的数据块更新，短时无刷新不代表挂死；观察原执行句柄，不重复部署。

通过证据：所有文件回读一致、完整启动日志、shell prompt、输入 probe 的独立结果行和下一次 prompt。检查该次 `result.json` 的 `boot_check=shell_input_passed` 及上传哈希。期限以当前 CLI 为准，结合实际镜像大小调整有界 host timeout，不把超时自动改记为通过。

## 串口和重复运行

```sh
uv run scripts/rpi.py console --port "$PORT" --log "$RUN/console.log"
```

Ctrl-] 退出主机 console，Ctrl-C 发给 Pi。同一 UART 只能有一个 reader；先结束本任务自己的 console 再测量。日志可能追加，使用新文件区分执行。

`console --expect` 是子串匹配，不能用出现在命令回显里的单词当成功证据。采用 [validation.md](validation.md) 的独立完整结果行；连续严格驱动时等下一次 prompt。

从运行中的 Moss 重部署，先确认本任务拥有板子、已有重启授权且 RAM 文件可丢弃，再顺序运行：

```sh
uv run scripts/rpi.py reset
# reset 关闭旧 OpenOCD；在持久终端重新启动 jtag，然后执行 deploy。
uv run scripts/rpi.py jtag --jlink "$JLINK_SERIAL"
```

watchdog reset 不写 SD，会丢失 RAM 文件与上传镜像。旧 OpenOCD 状态失效，必须新附加。reset 不是每次测量前的无条件步骤。

## GDB

用支持 AArch64 的 GDB，先检查实际 ELF 的 `.symtab` / `.debug_info`。Release 可能被 strip，manifest 字段名 `debug_symbols` 不保证有符号。

根据本次 OpenOCD 配置连接 CPU0，检查 `info registers pc cpsr`、`x/12i $pc` 和 `bt`。读取 ELF section 链接地址和实际加载基址计算重定位偏移，再使用 `add-symbol-file` 加载本次 ELF；不要写死符号路径或把另一映像的地址用于断点。先核对已知指令再设置 `hbreak`。

GDB `load` 不能替代 loader 的 firmware、缓存与回读流程。暂停改变调度/IPC/计时，不边断点边报告正常性能。临时观察结束恢复原本运行状态，除非用户要求保留暂停现场。
