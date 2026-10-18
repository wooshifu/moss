# 通用混合内核开发路线

2026-10-18 源码评估以 `6274d11` 为起点，架构目标按
[ADR-0036](adr/0036-develop-a-general-purpose-hybrid-kernel.md) 调整。
这份路线按依赖和风险排序；旧 TODO 中的历史通过记录保留其原始范围，
不按文件数量或代码行数估算“完成百分比”。

## 当前已经接通的路径

| 范围 | 已有实现 | 不能据此声称 |
| --- | --- | --- |
| 启动与平台 | 三 ISA 原生镜像、异常/中断、定时器、SMP/IPI、用户返回；ARM64 使用 DTB，RV64 使用 SBI，x64 使用 PVH/最小 ACPI | 任意机器可启动、UEFI 完整支持或所有设备可用 |
| 内存 | Buddy PFA、heap、页引用、用户页表/VMA、匿名 demand paging、COW、brk、private mmap、安全用户复制与 TLB 协调 | 完整共享映射、部分 munmap、mprotect、文件页缓存、回收/swap/compaction |
| 进程与调度 | fork/exec/wait/exit、CFS/RB 运行队列、亲和性与迁移、信号及定时等待；已有 task_transition_lock_ 和原子取队列节点 | 多线程共享地址空间的完整 exec/销毁事务、所有抢占安全点和扩展寄存器状态已闭合 |
| 应用与文件 | 静态 mlibc、BusyBox ash/文件工具、ramfs/devfs、目录、FD、dup、pipe、基本 POSIX I/O；普通 ELF 与文件路径仍走内核 | 动态链接、完整 POSIX、跨重启持久化或磁盘文件系统 |
| 混合机制 | capabilities、同步 IPC/reply、Memory Objects、native domains、supervisor 和独立文件/进程/Loader 等服务 | 普通 shell FD 已迁至服务、所有权限路径统一或已具备隔离设备驱动 |
| 工程验证 | 独立验证镜像、严格 @@MOSS 协议、版本化 catalog、九个构建预设、QEMU 回归；已有 Pi 性能/PMU 历史证据 | 历史真机报告覆盖当前镜像、所有套件已经在板上运行或 Linux 性能目标已达成 |

可追踪源码入口：`src/kernel/src/syscall_table.cpp`、
`src/process/src/process-scheduler.cppm`、`src/mm/`、`src/vfs/`、
`src/userspace/`、`scripts/kernel_validation.py`。
真实板历史证据见 [rpi3b-performance.md](rpi3b-performance.md) 和
[rpi3b-pmu.md](rpi3b-pmu.md)，与本轮回归分开记录。

## 实施优先级

| 顺序 | 工作 | 验收重点与依赖 |
| --- | --- | --- |
| P0-1 | 建立可重用的 Pi 内核套件 runner | 成套镜像/fixture/symbols 与 provenance；先开 UART、cache-off、上传全回读；严格 ready/catalog/case/end；失败后未执行 case 标 not_run。首批仅接通无需输入注入的 mm、users.frame、users.uaccess、users.ipc |
| P0-2 | 修复 ARM64 fork FP/NEON 继承 | 捕获调用线程实时状态，父子进程不经 exec 检查所有向量及 FPCR/FPSR；保留旧实现红例、新实现绿例和真机证据，不扩大普通 syscall 帧 |
| P0-3 | 完成抢占与调度交接审计 | check_need_resched 当前没有执行调度；沿 IRQ/系统调用返回和 preempt_enable 核实安全点，补确定性交错、锁持有者、affinity/on-CPU 单一所有者验收，避免无条件调用 schedule |
| P0-4 | 关闭生命周期与失败事务的剩余缺口 | 复用已有 fork FD/process/metadata OOM 和 migration_current_owner 用例；补共享 exec/root、并发 unmap/fault/退出、迟到 IPC donor、取消/服务死亡交错，逐项核对资源计数 |
| P1-1 | 存储基础：Pi SD 控制器、块 I/O、缓存和持久文件系统 | 根据实际 DTB 选择 SDHCI/SDHOST 路径；先完成 IRQ/超时/DMA/页寿命与 flush 契约，再接文件系统；验证落盘、重启、故障恢复及读写性能。virtio-blk 可补 QEMU 回归，不能代替 Pi 验收 |
| P1-2 | VM/VFS 的通用映射能力 | 部分 unmap、mprotect、共享/文件后备 mmap、统一页缓存与脏页/writeback；并发截断、权限、OOM 与回收先明确，再开放接口；复用现有 Memory Objects |
| P1-3 | Pi 网络完整路径 | Pi 3B+ 有线网依赖 USB 主机与 LAN7800，先驱动/收发/缓冲区所有权，再 Ethernet/ARP/IPv4/ICMP/UDP、TCP、socket/poll 和 DHCP/DNS 消费者；逐层真机互通、错误包与负载验收 |
| P2-1 | 应用兼容与统一权限模型 | 按实际 CLI 程序补 poll/select、终端/job control、多线程同步、凭据/DAC 和 errno；审计普通 POSIX 与 native capability/代码审批路径，避免两套 FD/权限真值；动态链接按消费者需要引入 |
| P2-2 | 内存压力、资源限制和长期运行 | 将已有统计变为分配约束与可验证回收；覆盖文件缓存压力、进程/FD/capability 配额及饥饿，记录失败后的恢复和长期尾延迟 |
| P3 | 扩展平台、设备与高级策略 | Pi 主路径稳定后，再扩充 x64/RV64 真机、设备隔离、更多启动协议和平台；NUMA、THP、compaction、图形等按明确消费者和测量结果推进 |

P1 的存储与网络可按具体消费者分片推进，TCP 依赖可用的收发和定时器；
复杂页缓存/回收依赖先明确文件后备与页面所有权。不要同时重建另一套
用户态 POSIX/VFS 来阻塞这些主路径。设备 DMA 隔离能力必须实测；
在缺少硬件约束的平台，内核驱动属于受信任计算基的一部分。

## 本轮发现的偏移与问题

- ADR-0008/0015/0018/0019/0020/0024/0025 的默认边界与本次混合目标不一致；
  已由 ADR-0036 明确替代相关要求，保留原决策作为历史，不能把调整目标写成实现完成。
- `todo.md` 的 ARM64 表曾把真实树莓派整体列为未验收，遗漏了后续有限真机记录；
  MOSS-017 未体现已有原子取任务/迁移实现，MOSS-010 仍把已有 OOM 用例列作全新工作。
- README 的文件服务容量写成 64 KiB，源码当前为 16 MiB，启动归档映射不占该可写内容预算。
- “共享内存未实现”过于笼统：capability Memory Object 已存在，POSIX shm/MAP_SHARED 仍未完成。
- `check_need_resched()` 原注释暗示已在返回点处理，但函数体不调度；本轮仅澄清注释，
  功能保留在 P0-3。全局任务交接锁等可能限制扩展性，须用锁竞争/等待时间证据再优化。
- socket/bind/listen/accept 等内核入口返回 ENOSYS，不能把接口占位或旧网络服务 ADR 算成网络实现。
- 代码审批原生域、普通 ELF、凭据字段和实际授权并非同一完成状态；安全启动、
  完整权限控制以及所有执行入口的一致性仍需专项闭合。

## 每个功能的完成门槛

1. 先写明可观察契约与失败行为，复用已有组件；修复应有能击中旧错误的回归。
2. 构建受影响架构/配置，跑实际内核与真实应用消费者，完成相关 lint/format。
3. 在真实 Pi 3B+ 运行相应功能与失败/回收检查，保存源码/镜像身份、DTB、原始串口和结构化结果。
   主机测试与 QEMU 可先完成，缺少真机结果的功能保持待验收。
4. 性能使用同板 Release 基线，记录核亲和性、温度、时钟、参数、预热、逐批或逐次原始样本。
   区分吞吐、CPU 成本、批次平均和尾延迟；定时/串口/JTAG 观测开销须可解释。
5. 对有可比语义的应用负载与同板 Linux 比较；根据测得基线制定预算，再执行优化前后对照。
   不从 QEMU 数据、RAM 文件吞吐或微基准通过推出 SD/网络/整体高性能。
6. 更新 TODO 的精确切片与证据；未覆盖架构、失败分支和性能预算继续保留开放状态。

## 首批实施结果（2026-10-18）

- [x] ADR-0036 与当前路线已确立；README 和两份 TODO 的指定偏移已澄清。
- [x] 物理板套件入口已用旧实现断言失败、新实现完整通过验证，并保留 not_run/原始串口。
- [x] ARM64 fork 基础 FP/NEON 继承已通过 Pi users.frame 3/3；同最终镜像 mm 7/7。
- [x] 三架构 Debug 构建及相关 QEMU 共 152 个 case 通过。
- [ ] Release/Linux 同板性能基线、其余物理套件和 P0-3/4 继续推进；不由本批结果代替。

镜像身份、红绿对照与范围见 [本批验收记录](rpi3b-fork-validation.md)。

## 第二批实施结果（2026-10-18）

- [x] 修复 P0-3 的前置缺陷：try_lock 成功遗漏抢占计数，unlock 会错误消耗外层锁的保护。
  保护在 CAS 前建立，失败回滚，成功保持到解锁；VM 事务、日志和 IRQ 锁共用此修复。
- [x] 新增真实 scheduler_tick 回归，检查两种锁的成功/失败/嵌套计数，以及持外层锁时延后抢占；
  旧 ARM64 QEMU 为 3 个断言通过、2 个失败，新 Pi scheduler 17/17 通过。
- [x] 真机 runner 增加无需主机输入的 scheduler 套件，catalog 升至 v10。
- [ ] check_need_resched、IRQ/syscall 返回安全点、完整 on-CPU 交接和性能预算仍属于 P0-3，
  不因这次锁保护修复而关闭。

详见 [本批验收记录](rpi3b-try-lock-validation.md)。
