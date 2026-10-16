# 接线、固件与连接诊断

以接收者 checkout 的 `docs/rpi3b.md`、`scripts/rpi.py` 和 `scripts/rpi_boot.S` 核对启动约定；接线规则取决于板型和连接器，不取决于主机用户名或目录。

## 连接关系

```text
Mac / Linux
 ├─ USB → J-Link → ARM 20-pin JTAG → Pi 40-pin GPIO
 ├─ USB → 3.3 V TTL UART → Pi UART
 └─ 读卡器 → microSD（初始准备/持久化更新时）
Pi 3B+ ← 独立、足额的 micro-USB 电源
```

Pi 的 micro-USB 口用于供电，不提供这里的 UART/JTAG。断电接线，先核对连接器 pin 1 标记和视角；ARM 20-pin 不能套用 SWD 10-pin 编号。

| JTAG 信号 | ARM 20-pin | Pi **物理针号** | BCM GPIO / 用途 |
| --- | ---: | ---: | --- |
| VTref | 1 | 1 | 3.3 V 参考 |
| GND | 4 | 6 | 共地 |
| nTRST | 3 | 15 | GPIO22 / ARM_TRST |
| TDI | 5 | 37 | GPIO26 / ARM_TDI |
| TMS | 7 | 13 | GPIO27 / ARM_TMS |
| TCK | 9 | 22 | GPIO25 / ARM_TCK |
| RTCK | 11 | 16 | GPIO23 / ARM_RTCK |
| TDO | 13 | 18 | GPIO24 / ARM_TDO |

JTAG pin 19（5 V 输出）和 pin 15（系统 reset）不接。VTref 是电平参考，不用于供电。现有 OpenOCD 用 `trst_only`；整板 reset 由 watchdog 完成。

| UART 适配器 | Pi 物理针号 | 说明 |
| --- | ---: | --- |
| RX | 8 | Pi TX / GPIO14 |
| TX | 10 | Pi RX / GPIO15 |
| GND | 6 或另一 GND | 共地 |
| VCC | 不接 | 独立供电 |

必须是 **3.3 V TTL**，不是 RS-232 或 5 V 逻辑。参数：**115200、8N1、无流控**。

## SD 与 firmware

先确认当前启动分区与文件，再检查配置：

```ini
arm_64bit=1
enable_jtag_gpio=1
enable_uart=1
dtoverlay=disable-bt
```

还需要适配 Pi 3B+ 的 firmware、DTB/overlay 和实际存在的 64 位 bootloader；`kernel=` 必须指向真实启动文件。以上四行不是完整 SD 镜像。使用 EL2h loader 时需要符合该约定的 bootloader，不能用任意裸机循环代替。

写卡前重新确认外置属性、容量、分区与授权，不沿用旧盘号。只读诊断不格式化。准备后安全弹出再插入 Pi。红灯/绿灯不能单独证明启动状态，以 UART 和 CPU 状态为准。

## 主机只读发现

macOS 示例，不打开串口或发送 JTAG 命令：

```sh
system_profiler SPUSBDataType
diskutil list external physical
python3 - <<'PY'
import glob
print('\n'.join(sorted(glob.glob('/dev/cu.usbserial*') + glob.glob('/dev/cu.usbmodem*'))))
PY
lsof -nP -iTCP:6666 -sTCP:LISTEN
pgrep -fl 'openocd|rpi.py|rpi_perf.py'
```

找到串口后，用 `lsof` 检查所选设备占用。Linux 可用：

```sh
lsusb
ls -l /dev/serial/by-id/ 2>/dev/null
ss -ltnp
python3 - <<'PORTS'
import glob
print('\n'.join(sorted(glob.glob('/dev/ttyUSB*') + glob.glob('/dev/ttyACM*'))))
PORTS
```

有稳定 `/dev/serial/by-id/` 链接时可用它标识串口；macOS 设备名也必须重新枚举。将核实的路径设为 `PORT`，J-Link 序列号设为 `JLINK_SERIAL`；两者独立发现，不能按 USB 枚举顺序猜测。多设备时按序列号、USB 拓扑或设备标签辨认。权限不足时检查所属组/udev 或宿主系统驱动，不默认以 root 运行整个工具。

Windows 原生 Python 没有此 loader 所需的 `fcntl/termios`。可在 WSL/Linux 主机运行，但先确认 J-Link 和串口已转发到同一执行环境，并能在其中枚举；不把宿主上的 COM 端口名称直接当 Linux 设备路径。远程/容器环境也需要实际 USB/串口访问，不能假设聊天客户端所在电脑的 USB 自动可见。

USB 枚举、JTAG 识别 A53、UART firmware 日志、Moss 命令成功是四层不同证据。

## 捕获真实 DTB

firmware DTB 是每块板启动时产生的数据，不能随 skill 复用别人的文件。选择本次捕获文件路径作为 `DTB_PATH`。保存这次启动 UART 中 `Device tree loaded to ... (size ...)` 的地址与大小，在 CPU0 停于 bootloader、相关内存未覆盖时捕获：

```python
import os
from pathlib import Path
from scripts.rpi import OpenOCD, tcl_path

# 代理先从本次日志取得地址/大小，选择输出位置，再设置这些环境变量。
address = int(os.environ['DTB_ADDRESS'], 0)
size = int(os.environ['DTB_SIZE'], 0)
assert address >= 0 and size > 0
destination = Path(os.environ['DTB_PATH'])
destination.parent.mkdir(parents=True, exist_ok=True)
with OpenOCD() as ocd:
    ocd.command('targets bcm2837.cpu0')
    assert ocd.command('bcm2837.cpu0 curstate') == 'halted'
    ocd.command(f'dump_image {tcl_path(destination)} {address:#x} {size}')
```

用 `fdtget` 验证 `/` 的 `model` 是 `Raspberry Pi 3 Model B Plus`，`/memory@0 reg` 为真实 RAM 分配，并检查 `/cpus` release 信息。不能替换为 QEMU DTB。

## 故障分流

| 现象 | 下一步 |
| --- | --- |
| USB 有 J-Link，扫不到 A53 | 核对 VTref、共地、固件 JTAG 配置、pin 1 与各信号；低速附加 |
| UART 无输出 | TX/RX、3.3 V、波特率、占用、SD/firmware；不能只根据 LED 判断 |
| U-Boot USB 枚举时重启 | 保存固件日志和复位原因；如需捕获早期现场，在配置的附加期限内 halt |
| ACK 错误或回读不一致 | 保留失败，检查接线/供电，降低速度；不启动损坏镜像 |
| MEM-AP DRAM 操作失败 | 检查目标与访问方式，复用 loader 的 CPU0 + cache-off 路径，不盲目重复失败的 DRAM 访问 |
| UART busy | 找占用者并协调，不关闭其他任务的 reader |
