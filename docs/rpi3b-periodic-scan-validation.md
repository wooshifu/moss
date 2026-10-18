# Raspberry Pi 3B+：全局周期扫描的 CPU 所有权

2026-10-18，基于 `67a3032` 的工作区补丁，闭合 P0-3 / MOSS-017 中两个
共享周期字段的访问归属。此批不关闭完整运行队列、迁移及 on-CPU 交接验收。

## 问题与修复

三架构的 CPU0 timer 分派全局 HrTimer，其余核直接调用 `scheduler_tick()`。
旧实现每核都修改普通 `tick_count_`，并可能调用读写普通 `last_balance_time_`
的 `periodic_balance()`：存在无同步并发访问，扫描节拍也随核数改变。
已有注释所说的八个 6 ms tick、约 48 ms，并不是旧实现保证的全局周期。

现在只允许 CPU0 更新计数、每八次调度 tick 调用全局扫描；负载均衡入口在
读取 timestamp 前再次拒绝非 CPU0 调用。CPU0 已拥有全局 timer heap，
周期 scheduler timer 在 idle 时仍保留，因此无须新增跨核原子计数或共享锁。
各核的预算补充、CPU 时间记账、RR/CFS 和延后抢占逻辑继续执行。
回调在初始化阶段绑定，运行时由 IRQ-masked timer 路径调用。

保留原八 tick 策略及 balancer 的默认 4 ms 最小间隔，没有新的调优依据。
全局扫描不再因更多 CPU 的 tick 而提前；负载重新分布的延迟仍依赖 CPU0
及时处理 timer，不能据此宣称硬实时或最优吞吐。CPU 热插拔若要移除 CPU0，
必须先实现 owner 交接。现有策略/统计/队列采样的其他并发问题不在此结论中。

审阅 idle 路径时同时更正了 NO_HZ_IDLE 注释：周期 scheduler timer 使全局
heap 通常非空，当前条件不会关闭 timer；这次没有实现动态 tick。
注释调整后重新构建，ARM64 生产和验证产物的全部六项哈希与验收快照一致。

## 确定性回归与真机

新增 catalog v14 `scheduler/periodic_scan_ownership`。真实验证线程通过生产
`migrate_current()` 在所有测试核保存/恢复 continuation，IRQ masked 时调用
私有 scheduler 的生产 tick；没有伪造 CPU 身份，也不 dispatch 无栈的合成线程。
回调只记录调用核与次数，独立 balancer 通过真实队列迁移验证 timestamp gate。

检查 CPU0 前七次不扫描，各从核各调用八次仍不推进全局扫描；回到 CPU0 的
第八次扫描一次，下一周期同样成立。从核直接进入 balancer 不得迁移或消耗
CPU0 的下一个间隔；同 timestamp、到期前一纳秒和到期时分别检查队列变化。
所有核仍须记账并产生 RR 延后抢占请求，最终恢复真实 current、CPU0 affinity
并清空私有队列。

| 同一最终回归 | 旧实现 | 修复后 |
| --- | --- | --- |
| Pi 四核 visited mask | 15 | 15 |
| 触发扫描的 CPU mask | 15 | 1 |
| 本例全局扫描次数 | 5 | 2 |
| 新用例断言 | 7 passed、4 failed | 11 passed、0 failed |
| scheduler 套件 | 9 passed、1 failed、9 not_run | 19/19 |

Pi 3 Model B Plus Rev 1.3 / BCM2837 / Cortex-A53，J-Link `000050120677`，
UART `/dev/cu.usbserial-14440`。旧/新均为 ARM64 Release（`-Os -g0 -DNDEBUG`、
PMU OFF），使用本板 firmware DTB，EL2h bootloader 经 cache-off handoff 后，
500 kHz JTAG 全量上传与回读 kernel、initramfs、DTB，再启动验证。
本轮为 RAM 部署，没有写 SD。镜像、DTB、provenance 与日志身份见机器记录。

首版测试曾错误地检查 RR 的 CFS `sum_exec_runtime`，额外产生一个失败；
已改为真实 `cpu_runtime_ns`。首版 Pi/QEMU 的 6 passed、5 failed 原日志保留，
随后同最终测试在旧 Pi/QEMU 均为 7 passed、4 failed，未修改生产代码制造红例。

## 支持验证与边界

ARM64 Release 及三架构 Debug 构建通过；相关宿主测试 260 passed、1 skipped，
定向 clang-tidy 3 TU、Clang format、Ruff 与差异检查通过。
默认四核 scheduler、timers、users.frame、users.ipc、users.timers 五套 QEMU，
三架构分别 59/59；ARM64 两核 scheduler 19/19。

额外 ARM64 十六核首轮为 9 passed、1 case_timeout、9 not_run，新用例迁移到
CPU5 后达到原 5 秒期限。GDB 现场各核等待 `task_transition_lock_`；保留完整
诊断。相同镜像、相同期限的隔离重跑 19/19，新用例约 0.023 s；这只能证明
另一次运行通过，不能解释首次超时、排除锁竞争或证明十六核长期性能。
第七批 RV64 压力回收及其他历史超时也继续保留。

## 同板 Release 成本

旧/新使用同一生产 initramfs 和探针 ELF，客户端固定 CPU0，跨核 IPC 的服务端
固定 CPU1；内核 `-Os`、PMU OFF，探针复用既有 `-O2` 准备流程。按旧版启动、
两轮采集，再新版启动、两轮采集执行；每场景两次预热、七个保留批次。
温度在计时外通过临时 halt CPU3 读取并立即恢复，两版合计 41.856–43.470°C。
未固定/采集 CPU DVFS 频率，不把 Generic Timer 频率当作 CPU MHz。

| 场景与单位 | 旧镜像两轮批次中位数范围 | 新镜像两轮批次中位数范围 |
| --- | --- | --- |
| clock，µs/op | 4.000–4.005 | 4.028–4.030 |
| raw getpid，µs/op | 0.897–0.904 | 0.897–0.904 |
| yield，µs/op | 6.468–6.469 | 6.461–6.462 |
| fork+exec+wait，ms/op | 10.196–10.301 | 10.279–10.288 |
| CPU0 → CPU1 IPC，µs/op | 92.626–94.400 | 91.436–91.831 |

这只是本次局部成本参考，不是单次 p99、性能预算达标或吞吐改善证据。
缺少回切、持续 SMP 负载和 Linux 对照，不能把 IPC 差值归因于本批修复。
Release 反汇编另外确认从核在全局计数器 load/store 前跳过该路径。

两个性能镜像的初次启动均出现 init 日志与 shell 逐字符交错，初始 prompt
被打散，原 90 秒检查均为 failed。待原 reader 退出后，分别独立发送换行并
完成新探针/下一 prompt（约 0.54 s），再采集上述数据；两轮采集协议均通过。
原失败没有改判，也没有将该输出问题归因于共享周期字段，MOSS-018 继续开放。

结束时重新完整上传/回读普通生产镜像，默认 shell 输入检查和下一 prompt 通过；
四个 AArch64 CPU 经 poll 确认为 running，UART 已释放，OpenOCD 已关闭。
Linux 对照、单次尾延迟、持续负载、其他抢占安全点和完整 on-CPU 协议仍未验收。

[机器记录](validation/rpi3b-periodic-scan-2026-10-18.json) 与
[文字证据包](validation/rpi3b-periodic-scan-2026-10-18-evidence.tar.gz)
保留原始失败、最终红绿、上传回读、串口、结构化结果与源码补丁。
