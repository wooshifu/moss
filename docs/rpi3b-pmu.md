# Raspberry Pi 3B+ PMU、温度与尾延迟

2026-10-17。所有性能与温度数据来自 USB 串口及 J-Link 连接的树莓派 3B+，不是 QEMU 时间。源码基于 `b9d7889` 加首轮性能改动，构建在 `/private/tmp/moss-performance-20261016`，没有混入主工作区并行开发的 monitor/top 等功能。部署只写 RAM，并完整回读 kernel、initramfs 和 DTB。

## PMU 测量的含义

PMU 是 Performance Monitoring Unit，即 CPU 的硬件性能监控单元。本次 Cortex-A53 的六个事件计数器分别记录 EL0 指令、EL1 指令、EL0 周期、EL1 周期、L1D refill、L2D refill，另读周期计数器。事件编号来自 ARM PMUv3 的 `INST_RETIRED=0x08`、`CPU_CYCLES=0x11`、`L1D_CACHE_REFILL=0x03`、`L2D_CACHE_REFILL=0x17`。

计数范围是 **CPU0 上的全部活动**，包含中断和同核其他任务。它不是进程独占计费，也不是四核总量；CPU1 上的 IPC 对端、CPU2 上的服务没有计入 CPU0 的计数。计数器读取本身的指令和周期也计入，报告不做未经验证的开销扣除。

`MOSS_ARM64_PERF_PROBE` 默认关闭。诊断构建在每个核启用只读 EL0 计数器访问和虚拟计时器访问，不安装溢出中断，不增加逐次 syscall 的内核日志。这个访问权限会暴露其他任务的计数，因此普通生产构建不启用它。当前 QEMU 不实现需要的全部事件，启用该开关会被能力检查拒绝；功能回归使用关闭开关的构建。

## 校准与采样

- Generic Timer 为 19.2 MHz，单 tick 约 52.08 ns。每个 workload 用约 20 ms 的区间与内核时钟核对；差异超过 1% 则拒绝该次测量。
- 空操作 p50/p99 约 0.208 µs，PMU 内核指令计数为 0。这个开销对缓存 getpid 等短操作不可忽略。
- 固定汇编循环执行 20,000 条指令，测到约 20,168 条 EL0 指令，额外部分包含分支选择和读取计数器的代码。原始 getpid 约 299 条 EL1 指令，缓存 libc getpid 的 EL1 计数为 0。
- 两个完整 warmup batch 后保留七个 batch。通常每 batch 有 1,024 次操作，共 7,168 次；exec 每 batch 256 次，共 1,792 次。p50/p95/p99/max 根据逐次原始 ticks 计算，采用 nearest-rank，绝不把 batch 均值当作尾延迟。
- 每次操作读取 32 位事件计数器并按模差累计。超过一秒的操作拒绝计数，避免在本次普通频率 A53 环境下多次回绕。单次超时与串口丢样都不能产生通过结果。
- 校准循环的周期／时间差对应约 600 MHz 的有效计数速率。没有读取固件的实时频率或 throttling 标志，不能据此断言 DVFS 策略或历史降频原因。

## 服务 CPU 位置是实际的混杂因素

自由调度的基线中，libc fork 约有 7 次系统切换／操作；首次候选中变成 10 次，exec 则从约 13 次变为 16 次。与此同时，CPU0 的用户态指令显著增加。首次候选虽然降低了原始 fork 和 exec 的耗时，libc fork 的 p99 却从约 1.79–1.84 ms 上升到 2.09 ms。该结果完整保留，不能只报告获益项目。

为隔离内核修改，`prepare --pmu` 为七个原有服务生成很小的启动包装：通过自身的既有 `sched_setaffinity` 接口固定到 CPU2，然后进入原服务 main。服务业务代码和 libc 相同。探针固定 CPU0，跨核 IPC 对端固定 CPU1，温度读取使用 CPU3。`RPI_PLACEMENT` 保存进程 CPU 快照，准备记录保存每个服务 ELF 的 SHA-256。

这种固定布局用于受控实验，不是给普通生产系统新增调度策略。固定布局下的收益不能保证自由调度的每次启动都有同样收益。

## 受控对照结果

下表单位为 µs，均为单次操作 p99。基线与候选使用字节完全相同的固定服务 initramfs 和探针，差异是内核 COW/backing 的复制与清零。基线先测两轮，候选再测两轮，最后切回基线再测一轮；没有把一次启动中的七个 batch 当作七次独立启动。

| 工作负载 | 基线 1 | 基线 2 | 候选 1 | 候选 2 | 切回基线 |
| --- | ---: | ---: | ---: | ---: | ---: |
| raw fork + wait | 1088.698 | 1090.208 | 1084.948 | 1085.677 | 1104.115 |
| libc fork + wait | 1814.010 | 1813.594 | 1818.281 | 1818.438 | 1853.281 |
| fork + BusyBox exec + wait | 11093.542 | 11091.719 | 9736.354 | 9726.823 | 11246.875 |
| 同核 IPC 往返 | 119.375 | 未重复 | 122.917 | 123.021 | 122.396 |
| 跨核 IPC 往返 | 99.323 | 未重复 | 103.021 | 103.073 | 102.812 |
| memcpy 1 MiB | 1905.677 | 未重复 | 1911.510 | 未重复 | 1977.344 |
| mmap + touch + munmap 1 MiB | 3336.094 | 未重复 | 3332.031 | 未重复 | 3361.406 |

exec 的 p99 降低约 **12.2%**，CPU0 的 EL1 指令从约 517 万／操作降到 393 万，EL1 周期从约 569 万降到 483 万；EL0 指令约 48.9 万／操作基本一致，系统上下文切换约 13 次／操作。这些观测与减少内核 backing 字节复制工作相符。

raw fork p99 相对前两轮基线仅小幅下降；libc fork p99 小幅上升。IPC 的两轮候选较首轮基线慢约 3–4%，但切回基线后也出现了接近候选的延迟和 EL1 指令数：同核约 5.56 万、跨核约 3.23 万／操作。单靠这些轮次无法将最初的 IPC 差异隔离为版本回归，更不能称为 IPC 优化。用户态复制和匿名缺页也没有稳定的已证实收益。这里只报告观测差异，没有建立统计置信区间或任意的“可忽略回归”阈值。

基线前两轮温度约 38.6–40.8°C，候选约 37.0–39.7°C，切回基线时约 42.9–46.2°C。切回基线时 exec 的 p99 回到 11.25 ms，EL1 指令回到约 519 万／操作；该轮 exec 最大值达 17.07 ms，原始数据保留。各轮指令校准仍为约 17.031 µs，不能将它等同于持续频率／供电状态的完整监测。最终原始记录包含每个 workload 的起止温度、全部 ticks、PMU 批次计数和进程位置。

## 温度

读取固件已启用的 BCM2837 温度传感器，不修改传感器控制或 CPU 电压／频率。按 Linux `bcm2835_thermal.c` 和 `bcm2837.dtsi`，状态 bit 10 必须有效，低十位为 ADC：`milli°C = 412000 - 538 × ADC`。

一次约 42 秒、36 GiB 的单核复制负载保存了 20 个温度样本，范围为 **44.546–47.236°C**。它是有限时间、单核负载的观测，不是四核长期散热验收。采样会短暂停住 CPU3，因此该轮不用于吞吐或尾延迟比较。正式延迟测量只在 workload 前后读取温度。

OpenOCD 的物理读取会做耗时的 MMU／缓存处理；直接从运行 EL0 的 CPU0 读取 MMIO 则会失败。最终从空闲 CPU3 的 EL1 内核映射读取，并在 `finally` 中恢复该核。最初的串口丢字、失败的传感器访问和不支持完整事件的 QEMU 尝试均保留在记录中。

## 用户态复制的判断

`mlibc` 的 `memcpy → forward_copy` 已按 64 字节展开；当前 ARM64 Release 反汇编实际使用 `ldp/stp q` 的 NEON 路径，不能按“用户 libc 也在逐字节复制”来优化。1 MiB 复制测得约 16.4 万条 EL0 指令和约 1.5 万次 L2D refill，内核周期只占 CPU0 计数的约 0.1%。这支持继续检查缓存和内存访问，但没有访问次数分母、内存控制器带宽或逐次 PMU 关联，不能直接宣称带宽饱和或把所有慢样本归因于 cache miss。

该用户态场景作为对照保留，本轮没有改写 libc 复制算法。内核则受 `-mgeneral-regs-only` 限制，复用现有标量对齐复制，避免未经上下文保存协议允许就在内核引入 SIMD。

## 修改与回归边界

`src/mm/src/page_fault.cpp` 的 COW 整页复制、可执行 backing 复制和页尾清零复用已有的内核 `moss::memcpy`／`moss::memset`。此前 Release 反汇编仍为 `ldrb/strb` 循环。页的所有权、地址空间锁、VMA 权限、PTE 发布和 TLB 处理的顺序保持原有语义。

新增 `fault_backing_and_cow_contents`，通过真实的缺页处理函数检查对齐和非对齐 backing、完整页内容、EOF 后三个字节清零，以及逐字节修改 child 后 parent 全页不变。辅助 QEMU 检查覆盖 `mm.permissions`、`mm.uaccess`、`mm.lifetime`、`users.vm`、`users.libc`；生产启动检查完成 168 项。QEMU 结果只证明这些功能路径，不用于硬件性能结论。

## 真机功能验收与证据

优化后的 PMU-OFF 验证内核通过真实 Pi firmware DTB 启动，`mm.permissions` **8/8 通过**，新增 `fault_backing_and_cow_contents` 的 **24,591 个断言通过**。既有 `Protocol` 状态机逐条验证 ready、catalog、worker、case 和 end，确认 `detected_cpus=4`、`online_mask=work_mask=15`、RAM 为 994,050,048 字节。这个结果来自真机串口，不是搜索日志中的 PASS 文本。

最终以 1 MHz 完整上传并回读普通生产 kernel、initramfs 和 DTB，恢复了 PMU-OFF、未固定服务 CPU 的镜像。BusyBox shell 输入检查通过，随后只读查询确认 CPU0–CPU3 均为 `running`。最终内核 SHA-256 为 `322f1739acc413cfcd35dea2003ea886b547befcfd9249de233c2899710090f3`，该镜像包含本轮 backing/COW 优化。

主工作区只合入本任务拥有的代码片段，保留并行开发的修改。合入后 165 项主机测试、Ruff 和涉及文件的 clang-format 检查通过；主机测试用于采集与验证工具的协议正确性。隔离构建的辅助 QEMU 功能检查共 24 个 case，生产启动检查 168 项通过。没有重建或验收主工作区同时新增的 monitor/top 等功能。

永久记录位于：

- [机器可读汇总](performance/rpi3b-2026-10-17-pmu.json)：各轮 p50/p95/p99/max、计数器、温度、验证和部署结果。
- [完整原始证据包](performance/rpi3b-2026-10-17-pmu-evidence.tar.gz)：全部成功／失败采样、原始 ticks 和串口日志、部署回读日志、测试结果、源码快照／patch、参考定义和 SHA-256 清单。镜像本体仍在本地 `build/rpi3b/pmu/`，包内保存其哈希。
- 本次固定布局内核 SHA-256：基线 `cb88f3ed2759521f74056b61e56dbb69d08c1bc1248ee2295d07e4ab82cf4461`，候选 `e24ecaeef9e49a2c375bff6d6dda38bfbf51908b07da37a5200f28903983b465`。
- 两者固定布局 initramfs 均为 `46020991fa079ecf650cb3fd9ec11c1997211adba3449270530ec0ec541ecfb8`，探针 ELF 均为 `d7c71fec9ee8729d5100a41be3f635b068935581bd65ffc51ce145e2cf01f982`。

## 重复测量

```sh
uv run cmake --preset arm64-release -DMOSS_ARM64_PERF_PROBE=ON
uv run cmake --build build/arm64-release -j 4
uv run scripts/rpi_perf.py prepare --pmu \
  --manifest build/arm64-release/moss-artifacts.json \
  --output build/rpi3b/pmu/run
```

使用 `scripts/rpi.py` 的既有 reset、jtag、deploy 流程上传该目录的 manifest，并完成 `--check`。本次主要在 2 MHz 上传及回读；固定布局候选的一次回读发生 Invalid ACK (7)，降到 1 MHz 校验保存的前缀并继续剩余回读，完整字节比对后才启动。保存的部署包装和日志记录了恢复过程。

```sh
uv run scripts/rpi_perf.py measure --pmu \
  --output build/rpi3b/pmu/run \
  --workload empty --workload instructions \
  --workload memcpy --workload fault \
  --workload fork_raw --workload fork_libc --workload exec \
  --workload ipc0 --workload ipc1
```

测量结束后以 `-DMOSS_ARM64_PERF_PROBE=OFF` 构建普通生产内核。保存的诊断镜像仍可用于后续重现。

## 参考定义

- [Linux ARM PMUv3 事件与权限位](https://github.com/torvalds/linux/blob/master/include/linux/perf/arm_pmuv3.h)
- [BCM2835/2837 温度驱动](https://github.com/torvalds/linux/blob/master/drivers/thermal/broadcom/bcm2835_thermal.c)
- [BCM2837 温度换算系数](https://github.com/torvalds/linux/blob/master/arch/arm/boot/dts/broadcom/bcm2837.dtsi)

本次下载的参考源码和 SHA-256 保存在 `build/rpi3b/pmu/references/`，避免将未来 master 分支内容当作本次采样时的版本。
