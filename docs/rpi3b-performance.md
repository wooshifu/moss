# Raspberry Pi 3B+ 内核态与用户态性能分析

日期：2026-10-16。基线提交：`b9d7889`。本报告包含首次分析、首轮优化和最终真机验证。构建与验收使用独立工作树 `/private/tmp/moss-performance-20261016`，排除共享工作区内并行开发的 monitor/top 功能。

后续的硬件计数器、温度、逐次尾延迟与受控 CPU 布局实验见 [PMU 真机报告](rpi3b-pmu.md)。以下数字保留为首轮历史基线。

## 首轮优化结果

已实现四项改动：BSP 重调度中断、事件驱动的进程等待、兼容 PID 缓存、内核对齐 copy/zero。最终生产 Release 镜像通过 JTAG 上传到 Pi 3B+ RAM，内核、initramfs 和 DTB 均完整回读比对，BusyBox shell 输入检查通过；没有写 SD。

- 跨核单字节 IPC：约 3.77ms → 104–107µs，墙钟耗时下降约 97%，约快 35–36 倍。
- libc `fork+wait`：约 11.3ms → 1.89–1.90ms，下降约 83%，约快 5.9 倍。
- libc `getpid`：约 131µs → 0.52µs；循环中的身份 IPC 已消除。
- 64KiB 文件读取加 seek：约 372µs → 74µs，下降约 80%，约快 5 倍；4KiB 读写下降约 59%。

这些是整组改动的端到端结果，不是每项改动独占 CPU 成本的分解。**没有解决全部瓶颈**：原始 `fork+wait` 相比两轮基线仍慢约 1–2.5%，yield 慢约 1.6–1.9%；同核 IPC 与 exec 组合最终变化较小，尚未建立长期噪声模型。

中间版本曾出现同核 IPC 约 +14%、exec 组合约 +8–10% 的回退，目标复测也出现过。最终去除了进程服务每个 RPC 的重复注册表扫描，并重新从隔离源码构建；两轮完整测量未复现该幅度回退。内核 binary 与中间版本相同，但 initramfs 不同；不能把这些差异全部归因于单个源码改动、或声称已有独立 A/B 证明。同核 IPC、exec 和原始 fork 均继续保留为回归观察项，完整中间数据未删除。

基线、诊断和 idle 单变量实验见 [基线记录](performance/rpi3b-2026-10-16.json)；最终两轮、被拒绝的测量、全部批次、源码及产物哈希、测试结果见 [优化记录](performance/rpi3b-2026-10-16-optimization.json)。

## 最终两轮完整测量

单位为 µs/op。基线列采用基线第一轮中位数；第二轮基线仍保留在原始 JSON。文件场景包含 seek，fork/exec 场景包含等待退出。

| 场景 | 优化前 | 最终第一轮 | 最终第二轮 |
| --- | ---: | ---: | ---: |
| 原始 clock syscall | 4.251 | 3.924 | 3.950 |
| 原始 getpid syscall | 0.842 | 0.840 | 0.828 |
| libc getpid | 130.944 | 0.520 | 0.520 |
| yield | 6.447 | 6.548 | 6.571 |
| 用户 memcpy，1MiB | 1194.801 | 1101.211 | 1100.919 |
| open + close | 53.511 | 47.377 | 47.439 |
| read + seek，64B | 10.289 | 9.238 | 9.249 |
| read + seek，512B | 12.567 | 9.525 | 9.528 |
| read + seek，4KiB | 32.043 | 13.085 | 13.070 |
| read + seek，64KiB | 372.239 | 73.765 | 73.629 |
| write + seek，4KiB | 32.111 | 13.155 | 13.089 |
| mmap + 每页首写 + munmap，1MiB | 2587.741 | 2491.582 | 2493.021 |
| 原始 fork + wait | 1085.436 | 1112.363 | 1112.851 |
| libc fork + wait | 11287.865 | 1893.893 | 1901.309 |
| libc fork + BusyBox exec + wait | 11583.796 | 11338.034 | 11170.475 |
| 同核 IPC，无 deadline | 120.619 | 116.748 | 120.286 |
| 同核 IPC，5s deadline | 125.546 | 121.608 | 124.616 |
| 跨核 IPC，无 deadline | 3768.199 | 104.032 | 106.993 |
| 跨核 IPC，5s deadline | 3767.418 | 108.611 | 112.479 |

两轮完整测量间还有一次失败尝试：exec 首个预热样本的串口行前缀丢失，尽管 guest 返回 0，主机仍因不足九个完整样本拒绝该场景并停止。失败 JSON 和原始串口日志均保留，没有补造样本或将部分结果算成完整成功运行。

## 内核态：BSP 遗漏重调度 IPI 使远程唤醒等 tick

路径：[BSP 中断初始化](../src/boot/src/arch/arm64/boot_impl.cpp) → [BCM2836 IRQ/mailbox](../src/drivers/irqchip/src/irqchip.cppm) → [IPI handler](../src/kernel/src/kernel_main.cpp) → [调度器](../src/process/src/process-scheduler.cppm)。

基线有 CPU0→CPU1 IPC，但返回时需要 CPU1 唤醒 CPU0。BCM2836 controller 初始化会清空本核 mailbox mask；AP 启动已启用 reschedule IPI 0，BSP 成功初始化后却没有执行对应 enable。现场读取到硬件 mailbox control 和软件 IPI mask 都是 `[0,1,1,1]`，CPU0 mailbox0 仍有 pending 请求。CPU0 因此常需等约 6ms 的 scheduler tick 才继续运行。

先前仅改变 idle 的 IRQ/WFI 顺序没有消除约 3.77ms 延迟，因此没有把该实验改动留在生产中。随后通过临时内存 trace、mailbox 状态和现场启用 CPU0 mask 的实验确认方向；现场修正后跨核 IPC 约 113µs。JTAG 停核和临时 trace 会改变时间线，这些仅用于因果诊断，不混入最终性能统计。临时 trace 曾有一次 shell 异常，失败也保留；最终镜像没有 trace。

生产修复是在 BSP controller 初始化成功后启用 IPI 0。它沿用 AP 的同一个中断源与 HAL，没有缩短 tick 或改变 scheduler/idle 协议。

回归测试将 CPU0 本地 timer 暂停到一秒后，由 CPU1 发真实 SGI/mailbox0，在 200ms 窗口内观察 timer compare 是否被 IPI handler 重编程，防止周期 tick 掩盖缺陷。200ms 是 QEMU 宿主机调度余量，不是产品延迟指标。QEMU `raspi3b` 的启用版本通过全部 8 个断言；仅删去 BSP enable 的对照版本在该断言失败（7 passed、1 failed）。对照测试后源码已恢复，重建的测试 Image 与正向验证产物 SHA256 相同。

剩余工作：测试其他核对、忙接收端、多个客户端与合并 IPI，采集 enqueue→dispatch 分段延迟和尾延迟。运行中任务的 `need_resched` 消费、长 IRQ-off 临界区仍是后续候选，不能由本次空闲/简单 IPC 测量推导为已完成分析。

## 用户态：waitpid 从 10ms 轮询改成退出事件

路径：[mlibc Waitpid](../third_party/mlibc/sysdeps/moss/sysdeps.cpp) → [进程服务](../src/userspace/moss_process_service.c) → [OBJECT_WAIT](../src/kernel/src/syscall_table.cpp) → domain exit / Receiver / Reply 事件。

优化前，进程服务遇到尚未退出的 child 立即返回 RUNNING；libc 固定睡眠 10ms 后重新请求。这解释了短命令等待的台阶，但原始 fork 和兼容 fork 还走不同注册协议，二者差值不能全算成 wait 的独占成本。

现在阻塞 wait 发出一次 `WAIT_BLOCK` 请求。进程服务保留 Reply，继续接收其他客户端请求，并同时观察相关 domain 退出、Reply 取消和 receiver readiness；完成回复提交成功后才回收 child record。`WNOHANG` 保留即时查询。普通 RPC 的五秒超时不能用于无限 POSIX wait，因此阻塞 wait 使用无显式 deadline 的 IPC，信号仍可取消。

为复用内核对象事件，增加 `OBJECT_WAIT=176` 和 `IPC_TRY_RECEIVE=177`：前者是 level-triggered、不消费事件的等待；后者解决观察 receiver ready 后 caller 取消的竞态。注册全部队列后 prepare sleep，再重查 readiness，避免丢唤醒。domain、receiver、Reply 分别检查 OBSERVE、RECEIVE、SEND 权限；同一队列别名及等待自身 domain 被拒绝。超时 timer 同步取消，避免回调访问已返回的栈。

进程服务保留最多 16 个阻塞等待，与 endpoint admission 边界一致；等待 handle 集合受 capability table 的 64 项限制。Reply cancellation 在没有新请求或 domain exit 时也必须唤醒服务。未真正 fork 的 child reservation 使用原有五秒 lease 驱动 wait deadline，不再依赖新请求触发清扫。

验证覆盖 receiver/domain 混合等待、NONBLOCK、deadline、空 try-receive、别名与权限拒绝、Reply 取消唤醒；生产探针覆盖 WNOHANG、取消后重试、信号打断后重试、超过五秒的 child 等待且服务继续响应、未 fork reservation 到期。完整服务崩溃恢复脚本也通过。更大并发、长期压力及全部交错时序仍未穷举。

## 用户态：缓存兼容 PID

优化前，每次 libc `getpid()` 都向进程服务查询身份，还要读取时间来构造 RPC deadline。原始 `SYS_GETPID` 约 0.84µs，而这条兼容路径约 131–133µs。

现在首次查询后以 atomic 缓存兼容 PID。fork child 在返回用户代码前写入 PREPARE_CHILD 已分配的 PID，exec 新映像重新从空缓存开始。`getppid()` 不做永久缓存，因为重新收养会改变它。不能直接拿原始内核 PID 替换兼容 PID：它们不是同一个命名空间。

最终循环约 0.52µs/op，低于原始 syscall 是因为缓存命中不进入内核；这仍含探针循环与分支等开销。缓存不改变服务身份协议，生产 fork/exec、父子关系和重启恢复检查均继续通过。

## 内核态：copy/zero 与用户边界复制

普通 libc 文件读写路径是 [syscall](../src/kernel/src/syscall_table.cpp) → [RAM 文件](../src/vfs/src/vfs_init.cpp) → [transfer_user_pages](../src/process/src/user_access.cpp)。这组文件基准没有经过用户态 File Service 协议，也不是 `uaccess.S` 的专门基准。

优化前，Release 反汇编确认内核 runtime memcpy/memset 和页内传输使用逐字节循环。现在 [runtime_support.cpp](../src/kernel/src/runtime_support.cpp) 使用自然对齐的 64-bit may_alias word 路径，保留头尾处理和不匹配对齐时的 byte fallback。memmove 保持重叠方向；向前重叠路径不调用公开 memcpy，避免编译器按不重叠语义处理。普通内存快路径不用于 MMIO。

页内 copy/zero 复用该实现，保留整段权限准入、VM lease、逐页解析、缺页/COW 和 access/dirty 更新。最终反汇编确认使用 `ldr/str xN`，没有引入内核 SIMD 或递归调用 memcpy/memset。

新增真实 runtime 符号测试覆盖全部 64-bit 对齐组合、零长度、word/page 边界、8/16-byte 重叠偏移、相同地址和整缓冲哨兵；uaccess 与用户边界功能套件继续通过。最终 64KiB 读取加 seek 约 74µs，4KiB 读写约 13µs。

没有修改 RAM 文件全局 `namespace_lock`，它仍是覆盖用户复制的 IRQ spinlock，分配/扩容也可能延长持锁。下一步应量化锁等待、持有和 IRQ-off 时长；在锁外复制需要稳定的 inode/backing 生命周期和共享 fd/append 语义，不能简单移动 unlock。可执行映像 backing 在 `page_fault.cpp` 的 byte loop 也尚未优化。

1MiB mmap + 每页首写 + munmap 仍约 2.49ms，它混合映射、256 次首写与拆除，不能除以页数后称为纯 page fault 延迟。页清零、页表分配、TLB 和 COW 需要独立场景。当前 ARM64 切换使用 ASID，不能把“每次全量 flush TLB”当成未经确认的瓶颈。

## 内核与用户态共同的剩余成本

| 优先级 | 候选问题 | 当前证据与下一步 |
| --- | --- | --- |
| P1 | 原始 fork 与 yield 的小幅回退 | 最终两轮仍比基线稍慢；保持同参数复测，分解页表/COW、切换和调度成本，先建立噪声模型 |
| P1 | 同核 IPC 固定成本仍约 117–120µs | call/reply 分配、引用计数、锁、sleep/wakeup 与完整消息复制仍在；先记录各阶段和分配次数 |
| P1 | exec 组合仍约 11ms | 已去掉 wait 轮询但 ELF 装载/映射/启动等仍存在；区分 fork、exec 提交、首次运行与退出，不能用组合差值冒充独立 exec 延迟 |
| P1 | RAM 文件全局锁 | 源码确认覆盖长用户复制；增加四核同/不同文件场景与 IRQ 尾延迟，之后再设计生命周期与锁拆分 |
| P2 | 原生 File Service 的 map/copy/unmap | 每次最多搬一个 4KiB Memory Object；本轮未测该协议，先做端到端基准，再考虑长寿命映射或 buffer 池 |
| P2 | 启动 archive 复制、服务装载和串口 | 源码与基线阶段日志支持；本轮没有优化启动，先分解服务装载与日志时间 |

`sys_ipc_call` 每次创建 PendingCall，并保留 timer 槽，即使没有显式 deadline，因为后续 priority donation 可能带入更早 deadline。不能直接删除保留。消息 ABI 结构为 288 bytes，单字节请求也走完整结构的边界复制；可评估按实际长度复制或对象复用，但必须保留取消、清尾、嵌套 donation 与 capability escrow 语义。

同核接收者已等待时可以研究直接交接，但要保持公平、完整上下文发布和 donation。先记录切换数及各段成本；本轮没有新增这类快路径。原始 clock syscall 约 4µs，也会影响带 deadline 的服务请求；类似 vDSO 的只读时间快路径属于后续候选，现有换算已是乘法和移位，不是每次 128-bit 除法。

基线启动日志中，init 到 services_ready 约 149ms，其中 authority→archive_ready 约 56ms；这些不是完整冷启动时长。内核把整个 boot archive 复制到 Memory Object，可研究不可变 backing 复用。115200 8N1 的理论上限约 11,520 bytes/s，基线首个 prompt 前捕获的 14,792 字节仅线路传输就需约 1.28s；不能将 JTAG 上传或 kernel timer 起点前日志计入调度成本。本轮没有调整启动日志、archive backing 或编译优化级别。

## 环境与计量边界

- Pi 3B+，四核 Cortex-A53；调用端 CPU0，IPC 服务端 CPU0 或 CPU1。
- Clang 23.1.2；内核 Release 是 `-Os -g0 -DNDEBUG`，附加 `-mgeneral-regs-only -mstrict-align`；生产用户程序、mlibc 和探针使用其现有 `-O2` 配置。没有修改编译策略。
- J-Link V11、OpenOCD 0.12、CH340 UART 115200 8N1。4MHz JTAG 曾出现 Invalid ACK，最终部署用 2MHz，完整回读校验。
- Generic Timer 为 19,200,000 Hz，它不是 CPU 主频。没有将 timer ticks 换算成 CPU cycles。
- 每场景 2 个预热、7 个保留批次，表格是各批次 `elapsed_ns / iterations` 的中位数。两轮是同次启动内的独立运行，不是两次冷启动。
- 计时包含真实 syscall、服务调度、探针循环、工作负载分支与返回值检查；串口输出、批次外数据比对和 topinfo 快照在区间外，未扣空循环基线。
- 首轮测量是包含等待的墙钟时间，没有独占的内核/用户 CPU 拆分或每次调用 p95/p99。上下文切换差值是系统范围，不是单线程独占。
- 文件读取 `/busybox.elf`，写入提前填充的 RAM 文件；它们不是 SD、冷缓存或扩容吞吐。用户 memcpy 缓冲已预触碰；fork 探针还预触碰两个 1MiB 缓冲，地址空间/COW 会影响结果。
- exec 场景是 `fork → /busybox.elf sh -c 'exit 0' → wait`，其差值不等于纯 exec 成本。
- 首轮未采集 PMU、动态频率、温度、功耗或持续热负载；后续 PMU/温度数据见上述独立报告。`status=passed` 表示协议、返回值及数据检查通过，没有建立性能回归门槛。

最终内核 SHA256：`9ee71b588df3ad776fc4b713b155286701d702d9e5d826757f4335ebcfa9046d`。

测量 initramfs SHA256：`2e93e3532382ef33018f59d7e7b0a666a6aacf44f2bffa2427c9a9adc98c420e`。

探针 ELF SHA256：`2119abcee08dfdc2c4874fb37db7936da935bbb08149c44940567616fca20691`。

## 功能验证与证据范围

- ARM64 Release 构建通过；Ruff、仓库自有改动文件的 clang-format 和 `git diff --check` 通过。第三方 mlibc 保留原有适配风格，按 lint.toml 排除；直接套其上游格式对 HEAD 也会报大量差异。
- 主机工具 pytest：175 passed，覆盖 rpi、rpi_perf、production_boot、kernel_validation。
- QEMU 功能套件：`mm`、`mm.uaccess`、`users.uaccess`、`users.ipc`、`users.libc`、`users.exec`、`users.signals`、`interrupts.smp` 最终均有 passed 记录。
- 首次最终回归有 IPC priority handoff deadline 失败和 libc 5s case timeout，当时宿主机 load average 约 82；libc 现场仍在清零 demand-fault 页面。相同产物、不改代码，外部 watchdog 改为 30s 后，22 项 IPC 与两项 libc 测试全部通过，IPC 内部原有一秒 deadline 未放宽。保留初次失败，不能据此保证相同失败不再发生。
- QEMU raspi3b BSP IPI 正负对照：修复版 passed，去掉 enable 的版本在预期唤醒断言 failed。
- 完整生产启动与服务崩溃恢复：168/168 steps passed，用时 110.43s，采用 180s 宿主机预算；时间仅作为测试环境记录，不是真机性能。
- 最终真实硬件：全部上传文件回读一致，shell 输入通过，两轮各 19 项探针通过。

QEMU 用于功能回归，不替代真机性能；本轮没有验证 x64/RISC-V，也没有验收并行 monitor/top 改动。源码哈希、patch 哈希、完整数据和各结果文件 SHA256 都写入优化 JSON。

## 复现

构建生产内核、initramfs 并追加探针；进行功能验证时保留 `MOSS_BUILD_TESTS=ON`，性能镜像仍使用生产 kernel/initramfs：

```sh
uv run cmake --preset arm64-release -DMOSS_BUILD_TESTS=ON
uv run cmake --build --preset arm64-release
uv run scripts/rpi_perf.py prepare \
  --manifest build/arm64-release/moss-artifacts.json \
  --output build/rpi3b/perf
```

连接见 [硬件与部署说明](rpi3b.md)。一个终端保持 JTAG 服务：

```sh
uv run scripts/rpi.py jtag --jlink 50120677
```

另一个终端部署和测量，串口需独占：

```sh
uv run scripts/rpi.py deploy \
  --manifest build/rpi3b/perf/moss-artifacts.json \
  --dtb build/rpi3b/firmware.dtb \
  --output build/rpi3b/perf/deploy --check
uv run scripts/rpi_perf.py measure --output build/rpi3b/perf
uv run scripts/rpi_perf.py measure --output build/rpi3b/perf

# 目标复测仍包含全部预热和保留批次。
uv run scripts/rpi_perf.py measure --output build/rpi3b/perf \
  --workload ipc0 --workload ipc1 --workload fork_libc --workload exec
```

当前适配器最终验收使用临时 deploy wrapper 将 4MHz 改为 2MHz，未新增生产 CLI 选项。重新部署前先 reset 再重新启动 JTAG 服务。prepare 不修改原生产 manifest/initramfs，失败测量也保存部分结果。以上命令从当前工作区构建会包含工作区其他修改；复现实验需使用优化 JSON 记录的隔离源码与产物身份。
