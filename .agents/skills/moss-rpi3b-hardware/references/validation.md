# 真机功能与性能验证

区分编译成功、回读匹配、shell 交互、某项内核行为正确、稳定性和性能改善。先完成 [runbook.md](runbook.md) 的环境发现与变量设置，命令从所选仓库根目录执行。

## 生产内核：直接验证

先执行 runbook 的 `deploy --check`，检查 `result.json` 与原始 `serial.log`。检查实际核心上线和服务日志；不能仅根据 Pi 有四核就写“四核已上线”。然后通过 UART 验证内容与退出码，使用独立 marker 行排除输入回显：

```sh
# RUN 是 runbook 中的新输出目录；30 秒是主机存活期限，不是性能门槛。
uv run scripts/rpi.py console --port "$PORT" --timeout 30 \
  --log "$RUN/arithmetic.log" \
  --send 'test "$((20 + 22))" = 42 && printf "%s%s\n" MOSS_ ARITH_OK' \
  --expect $'\nMOSS_ARITH_OK\n'

# 非零退出码 7 用于确认实际观察到 child 的退出状态。
uv run scripts/rpi.py console --port "$PORT" --timeout 30 \
  --log "$RUN/child.log" \
  --send '/busybox.elf sh -c "exit 7"; rc=$?; test "$rc" -eq 7 && printf "%s%s\n" MOSS_ CHILD_OK' \
  --expect $'\nMOSS_CHILD_OK\n'
```

`$'...'` 适用于 macOS zsh/bash；42 是算术预期值。`--expect` 仅保证收到该行；连续严格驱动时还应等下一次 `moss$ ` 再发命令。

RAM 文件检查可在 shell 中使用未存在的路径，验证后清理：

```sh
f=/rpi-check-$$
test ! -e "$f" && printf '%s\n' moss-data > "$f" && \
  test "$(cat "$f")" = moss-data && rm "$f" && printf '%s%s\n' MOSS_ FILE_OK
```

若长命令丢字，先保存损坏的回显，不把参数解析失败称为内核性能失败。可复用 `scripts.rpi.write_all` 逐字/小段发送，按实际接收能力选择间隔，发送等待必须在测量外。不能删掉缺失样本或补造通过记录。

## 真机用户态探针

先确认当前 checkout 的 `scripts/rpi_perf.py` / `.c` 存在且接口匹配：

```sh
uv run scripts/rpi_perf.py prepare \
  --manifest "$MANIFEST" --output "$RUN/perf"
# 若板上正在运行 Moss，先执行 runbook 的 reset + 新附加。
uv run --frozen python "$SKILL_DIR/scripts/deploy.py" \
  --repo "$MOSS_REPO" --jtag-khz "$JTAG_KHZ" -- \
  --manifest "$RUN/perf/moss-artifacts.json" \
  --dtb "$DTB_PATH" --port "$PORT" --output "$RUN/perf/deploy" --check
uv run scripts/rpi_perf.py measure --port "$PORT" --output "$RUN/perf"
uv run scripts/rpi_perf.py measure --port "$PORT" --output "$RUN/perf" \
  --workload ipc0 --workload ipc1 --workload fork_libc --workload exec
```

上述 MANIFEST 必须是所选 Release 构建的 manifest。prepare 用生产 ARM64 Release libc 构建探针，追加到新 initramfs，保留原产物。collector 独占串口，期间不另接 console/GDB 或 halt 读取数据。

基础协议是 **2 个预热 + 7 个保留批次**，要求全部 `RPI_PERF` 行、`RPI_PERF_DONE` 与实际 `RPI_COMMAND_STATUS=0`；失败保留部分 JSON 与原始日志。以当前代码的协议为准，不为接纳丢行放宽校验。

基础结果是批次平均 ns/op 的中位数/范围，**不是单次 p95/p99，也不是 PMU**。同/跨核 IPC、seek+read、fork+exec+wait 是组合操作，差值不能直接当作某个函数独占成本。RAM 文件吞吐不等于 SD 吞吐。

## PMU、温度、尾延迟

先检查接收者 checkout 的 `--help`、boot CMake 和源文件是否提供 PMU、逐次计时与温度采集。只有确认实现、启用方式和输出协议后才使用相应选项；能力缺失时明确说明，不从其他工作区借用未发布接口。

- PMU 是 CPU 硬件计数器，区别于 Generic Timer 和温度传感器。A53 有六个可编程事件计数器；配置前实际检查支持数量/事件。
- 退休指令、CPU cycles、L1D/L2D refills 可用于归因；EL0/EL1 过滤不等于进程隔离。CPU0 计数包含该核其他任务/IRQ，不包含 CPU1 服务端执行。
- 当前 OpenOCD 0.12 的 `mrc/mcr` 路径不能直接操作 AArch64 PMU，不要反复尝试 AArch32 CP15 命令。若需内核初始化，使用默认关闭的诊断开关、所需核心初始化和最小 EL0 权限，不开启溢出 IRQ；普通生产配置不启用全核用户可见计数。
- 先校准空操作、固定指令循环和内核调用；记录读数成本、计数宽度和 wrap 限制。无法排除多次溢出时拒绝该数据。
- 从目标读取 `CNTFRQ_EL0` 并校验后用于 Generic Timer 换算，不写死计时频率，也不把它当 CPU MHz。cycles / 时间还受 idle、调度和读取成本影响，不能直接当精确 DVFS 主频。
- p99 必须来自逐次原始耗时，并注明数量、预热、计时开销与算法。直接 EL0 timer 读取要先获得内核授权并验证换算。
- BCM2837 温度控制/状态为 `0x3f212000` / `0x3f212004`（外设基址 `0x3f000000` + TSENS 偏移）。Linux 驱动与 DTS 定义 status bit 10 有效、低 10 位 ADC，毫摄氏度 = `412000 - 538 * ADC`。只读已启用传感器，不随意改控制寄存器。
- JTAG 温度读数可能需 halt CPU0，应在正常计时前/后进行。热压力期间周期 halt 要标明扰动，不同时宣称无扰动吞吐。不得向猜测的“空闲 RAM”写 mailbox buffer。

温度依据 Linux `drivers/thermal/broadcom/bcm2835_thermal.c`、`arch/arm/boot/dts/broadcom/bcm2837.dtsi`；PMU 常量可核对 `include/linux/perf/arm_pmuv3.h` 与 Arm 文档。没有访问总数分母时，不把 refill 计数叫 cache miss rate。

## 物理板内核套件 runner

当前 `scripts/rpi.py validate` 可运行无需输入注入的 `mm`、`scheduler`、`vfs`、`users.frame`、
`users.vm`、`users.uaccess`、`users.ipc`，每次一套件、独立启动。使用新结果目录：

```sh
uv run --frozen python scripts/rpi.py validate \
  --manifest "$MANIFEST" --dtb "$DTB_PATH" --port "$PORT" \
  --jtag-khz "$JTAG_KHZ" --workload users.frame --output "$RUN/frame"
```

先构建 `MOSS_BUILD_TESTS=ON`，停于 U-Boot EL2h。runner 复制匹配的验证产物和
provenance，生成相对路径 manifest，在 prepare_image 的明确 bootargs 输入中选择套件，
并复用 cache-off、全回读和 `kernel_validation.Protocol`。检查 finalized 的
`validation-result.json`、部署 `result.json` 和原始 `serial.log`；失败保留 not_run，
复位并重新附加后再运行下一套件。`--check` 仍只用于生产 shell。

## 扩展其他内核套件时

`kernel_validation.py`、`check_production_boot.py` 当前启动 QEMU；使用 `--machine raspi3b` 或板上 DTB 仍不是物理板测试。QEMU 只能补充功能回归。

未列出的套件不自动视为受支持；扩展时复用现有协议并核实其主机交互：

1. `MOSS_BUILD_TESTS=ON`，使用成套 `validation_kernel` / `validation_initramfs` / `validation_debug_symbols`，保留 provenance。
2. 新建硬件验证 manifest，复制文件到新目录，使用目录内相对路径。`Artifacts` 拒绝绝对路径与越出目录的 `../`；不修改生产 manifest。
3. 按当前 `run_guest()` 协议和实际板配置加入 `moss.validation`、`moss.cpus`、`moss.memory` 等 bootargs，使用 `prepare_image(..., bootargs=...)`；仅修改输入 DTB 的 bootargs 仍会被准备过程覆盖。
4. 复用 cache-off 与完整回读路径，进入 kernel 前打开 UART。验证内核不一定有 shell，不能套用生产 `--check` 判据。
5. 复用 `kernel_validation.Protocol` 的 catalog/ready/case/end 状态机和失败规则；需输入注入的套件移植对应交互。物理板没有 QEMU 进程退出信号，应以完整协议和有界 watchdog 判断，不搜索文字 PASS 代替。
6. panic/assertion/缺项/超时/清理失败保留证据并停止本次套件链，后续标 `not_run`。按既有授权恢复生产镜像或保留调试现场。

## 对比与报告

新实验目录保存：源码 revision + dirty/patch 身份、工具链/preset、probe 源码与 ELF 哈希、kernel/initramfs/DTB 哈希、适配器信息、完整日志/JSON，以及失败和重试。不能用当前源码哈希冒充旧二进制的构建来源。

优化前后同一真机、相同探针/参数/构建设置，记录温度、核亲和性、运行顺序和是否重启，重复测量并保留回退。`status=passed` 表示正确性/采集协议通过，不自动说明性能达标。只把本次实际完成的项目写成“真机通过”。
