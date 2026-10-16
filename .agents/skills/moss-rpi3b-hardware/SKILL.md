---
name: moss-rpi3b-hardware
description: 在 Raspberry Pi 3B+ 真机上构建、通过 J-Link/OpenOCD 的 JTAG 部署、串口运行并验证 Moss 内核和用户态。用于树莓派接线与连接诊断、ARM64 kernel/initramfs 构建、RAM 部署、GDB 调试和真机功能或性能验证；其他板型和通用 Linux SD 镜像安装不直接套用此流程。
---

# Moss：树莓派 3B+ 真机运行与验证

本 skill 可随仓库分享，也可单独安装到任意 skill 目录。它依赖接收者自己的兼容 Moss checkout 和可访问的 Pi 3B+；不包含操作系统源码、firmware、DTB 或编译产物。

以实际源码、产物和这一次的硬件结果为依据。优先复用 Moss 的 `scripts/rpi.py`，不要另写上传器、绕过缓存交接或省略回读。

## 开始时

- 从用户提供的位置、当前工作目录或其 Git 根目录定位 Moss，并确认 `scripts/rpi.py`、`scripts/artifacts.py` 和构建配置存在。取当前加载的 `SKILL.md` 所在目录为 skill 根目录，不假设用户名、仓库位置或 skill 安装层级。
- 按 [runbook.md](references/runbook.md) 动态确定仓库、skill、构建目录、manifest、真实 DTB、输出目录、串口和适配器。只有单个候选时才自动选择硬件；多个候选须辨认目标，不能套用别人的设备号。
- 先识别执行主机系统和 USB 可达性。仓库 UART 实现依赖 POSIX 的 `fcntl/termios`，macOS/Linux 为直接路径；Windows 使用能实际访问 USB 的 Linux 环境，不能宣称原生 Windows 已兼容。
- 区分接线说明、只读诊断、构建、部署和性能实验。已有部署/调试授权继续适用；调用 skill 本身不授权格式化 SD、关闭其他任务或重置其他任务正在使用的板子。
- 查看工作区、构建进程、串口占用和 OpenOCD 端口。复用本任务的现有连接；其他任务占用时协调，不抢占。并发工作使用独立构建目录或已授权的隔离 checkout，不覆盖别人的产物。
- 首次接线、没有启动日志、SD/固件或 DTB 不明：读 [hardware.md](references/hardware.md)。
- 构建、部署、重复运行或 GDB：读 [runbook.md](references/runbook.md)。
- 功能、内核套件、性能/PMU 验证：读 [validation.md](references/validation.md)。

## 执行条件

1. 这是 **Pi 3B+ / BCM2837 / Cortex-A53 / AArch64** 流程。40 针表使用物理针号；UART 是 3.3 V TTL。USB 枚举出适配器不证明 Pi 已正常运行。
2. SD 固件先初始化 DRAM 并进入可附加的 64 位 bootloader。JTAG 将 Moss 写入 **RAM**，不等于 SD 烧录，断电后不会保留。
3. 使用本板 firmware 生成的 DTB，保留内存范围和 CPU release 信息，不能用 `scripts/qemu/` 的 fixture 替代。
4. 当前 loader 要求 CPU0 停在 bootloader 的 **EL2h**。必须完成 `rpi_boot.S` 的 cache clean/invalidate、关闭 EL2 MMU/cache，命中 `cache_off` 后才上传；不直接覆盖正在运行的 Moss。
5. 上传文件完整回读匹配后才进入内核。`--check` 成功只证明本次 shell 命令往返，不代表完整驱动或内核测试通过。
6. 有界重试并保留失败记录。启动失败、回读不一致、ACK 错误先记录日志和状态；已有请求尚在进行时不重复部署、重复发命令或循环复位。
7. 临时 halt 观察要在成功和异常路径恢复本任务原本运行的 CPU；有意暂停的断点现场则保持暂停。调试器失联时，恢复状态标为未确认。

## 复用的脚本

| 仓库文件 | 用途 |
| --- | --- |
| `scripts/rpi.py` | UART 独占连接、JTAG server、RAM 部署、回读、watchdog reset |
| `scripts/rpi_boot.S` | EL2h 到 Linux Image 的缓存与寄存器交接 |
| `scripts/artifacts.py` | `moss-artifacts.json` 解析和产物校验 |
| `scripts/gen_initramfs.py` | 生产 initramfs，通常由 CMake 调用 |
| `scripts/gen_validation_initramfs.py` | 内核测试 initramfs，通常由 CMake 调用 |
| `scripts/rpi_perf.py`、`scripts/rpi_perf.c` | 生产用户态探针与真机采集；先确认当前 checkout 存在 |
| `scripts/kernel_validation.py` | 内核验证协议和 QEMU runner，不是现成真机 runner |
| `scripts/check_production_boot.py` | QEMU 生产启动/服务验证，不作为真机通过证据 |

附带 [deploy.py](scripts/deploy.py) 只为现有 `rpi.py deploy` 加进程内 JTAG 速度上限，不修改仓库、不重写部署、不自动 reset。速度由调用者根据探针、接线和目标实际情况传入，不内置某台设备的经验默认值。如果仓库已有原生速度参数，优先使用原生参数。

## 交付证据

报告板型/适配器、源码或 patch 身份、构建模式、kernel/initramfs/DTB 哈希、JTAG 速度、原始日志、命令与逐项结果。区分本次真机结果、历史记录、源码确认、主机测试和 QEMU。性能优化用同一板子和可比工作负载重复验证，不把批次平均值的分位数标成单次操作 p99。
