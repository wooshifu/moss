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
| P0-3 | 完成抢占与调度交接审计 | 已接通统一用户返回并完成该路径的局部 Release 成本对照；继续核实内核态 IRQ 返回与最外层抢占恢复，补确定性交错、锁持有者、affinity/on-CPU 单一所有者及负载成本验收，避免在任意 unlock 调度 |
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
- `CONTEXT.md` 和 `todo.md` 的后续清单曾沿用“所有核心子系统必须外迁”的旧约束；
  第五批已将术语和当前任务同步到 ADR-0036，历史实现记录与未完成的安全/恢复验收继续保留。
- `todo.md` 的 ARM64 表曾把真实树莓派整体列为未验收，遗漏了后续有限真机记录；
  MOSS-017 未体现已有原子取任务/迁移实现，MOSS-010 仍把已有 OOM 用例列作全新工作。
- README 的文件服务容量写成 64 KiB，源码当前为 16 MiB，启动归档映射不占该可写内容预算。
- “共享内存未实现”过于笼统：capability Memory Object 已存在，POSIX shm/MAP_SHARED 仍未完成。
- 首次评估时 `check_need_resched()` 原注释暗示已在返回点处理，但函数体不调度；
  第三批接通统一用户返回安全点，其他安全点和完整交接仍属于 P0-3。
  全局任务交接锁等可能限制扩展性，须用锁竞争/等待时间证据再优化。
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
- [ ] 第二批结束时 check_need_resched、IRQ/syscall 返回安全点、完整 on-CPU 交接和性能预算
  仍属于 P0-3；统一用户返回随后见第三批，不因锁保护修复而关闭其余工作。

详见 [本批验收记录](rpi3b-try-lock-validation.md)。

## 第三批实施结果（2026-10-18）

- [x] 统一用户返回在信号检查前消费安全的 need_resched 请求；不安全时保留请求，
  复用 bootstrap 保存/恢复、实际时间记账与自选路径，不在任意 unlock 内调度。
- [x] 新增持锁/状态/一次消费回归和关闭 timer 的真实用户寄存器交接探针；
  旧空实现分别断言失败，新 Pi 同一镜像 users.frame 4/4、scheduler 18/18、users.ipc 22/22。
- [x] 三架构 Debug 构建通过，ARM64/x64 QEMU 各 147/147；RV64 本轮矩阵为
  140 passed、1 个 5 秒超时、6 not_run。相同镜像隔离 users.exec 32/32 通过；
  原始超时/GDB 现场、首次 x64 探针错误和 Pi 回读失败均保留，不能记为一次三架构全绿。
- [ ] RV64 压力回收超时原因、其他内核抢占安全点、完整 on-CPU/迁移协议、
  长期负载和 Release/Linux 同板性能仍需后续批次；MOSS-017 保持开放。

详见 [本批验收记录](rpi3b-return-reschedule-validation.md)。

## 第四批实施结果（2026-10-18）

- [x] 用户返回无请求路径直接读取已取得线程的标志，跳过重复 current 查询；
  有请求时仍由调度函数重新检查安全条件，不更改请求消费与信号语义。
- [x] Pi Release 原候选及最终候选 users.frame 各 4/4；最终三架构 Debug 定向 QEMU
  各 44/44，定向 lint 1 TU 和格式检查通过。
- [x] 同板 PMU-OFF、相同用户态归档的九场景对照，按基线、原候选、最终候选、
  回切基线启动，各测两轮。raw getpid 从原候选约 0.973 µs 降至 0.894–0.902 µs；
  基线两次启动约 0.888–0.891 µs。fork/exec 回切存在漂移，不能归因全部差值。
- [ ] 两次候选启动的默认输入命令均截为同一前缀；独立恢复和后续采集成功不覆盖
  原超时。UART 输出期间的 IRQ 屏蔽、接收和调度交错需要单独定位，见 MOSS-018。
- [ ] Linux 同板对照、单次尾延迟、长期负载、其他安全点和完整 on-CPU 协议仍开放；
  第三批 RV64 users.exec 原超时也不由本批定向回归关闭。

详见 [Release 成本与原始证据](rpi3b-return-reschedule-performance.md)。

## 第五批文档纠偏（2026-10-18）

- [x] `CONTEXT.md` 的内核驻留、普通 POSIX/VM、VFS、pager、网络及 Process/Loader
  服务术语与 ADR-0036 一致；隔离服务角色的定义不表示相应能力已经实现。
- [x] `todo.md` 的两条强制迁移任务改为边界收敛与明确隔离契约，未将实现任务勾为完成。
- 本批仅修改文档，核对相对链接、术语和差异；不新增构建、QEMU 或真机运行证据。

## 第六批实施结果（2026-10-18）

- [x] 复现第四批的 Pi 输入截断并读取到 PL011 RX overrun；根因路径为普通 VFS
  控制台写沿用 syscall IRQ mask。本批在该 task-context 操作中开放 IRQ，
  完整/部分/失败写均恢复入口状态，复用现有用户复制与 UART TX guard。
- [x] 同用户归档的新镜像默认启动检查通过；输出期间输入的 getpid 和 exec
  各 3/3、空闲与长输出期间输入 13/13 通过，重放前后 RSR 为零。
- [x] 新增 IRQ 状态回归旧实现 17 passed、3 failed，新三 ISA 各 20/20；
  Pi Release VFS 12/12，物理 runner 增加 vfs，catalog v12；宿主 181/181。
- [ ] 首次七套件 QEMU 的 ARM64 rename、RV64 allocation_fault 原 5 秒超时保留；
  同镜像独立套件通过不关闭根因。旧启动另有 Loader 缺页后重启，仍待定位。
- [ ] 其他长 IRQ 屏蔽、持续全双工负载、量化输出成本、Linux 对照及完整调度交接
  继续开放，不因这个控制台路径修复关闭 MOSS-017/018。

详见 [本批验收记录](rpi3b-console-write-validation.md)。
