# Raspberry Pi 3B+：控制台输出期间的接收溢出修复

2026-10-18，以 `0ca7d19` 加本批改动构建。生产内核为 Clang 23.1.2、
ARM64 Release `-g0 -Os -DNDEBUG`、PMU OFF。真机是 Raspberry Pi 3 Model B
Plus Rev 1.3，四核、948 MiB firmware ARM RAM；J-Link `000050120677`、
1 MHz JTAG、115200 8N1 串口。部署仅写 RAM，均先经过 EL2h/cache-off 和完整回读。

## 根因与修复范围

普通 `write(/dev/console)` 经 `do_write -> FileOps::write -> console_write`
逐字复制用户数据并等待 UART TX。syscall 入口屏蔽 IRQ，原实现让这个状态覆盖
整次串口输出；TX 等待期间 CPU0 无法及时服务 RX，PL011 接收 FIFO 会溢出。
`do_write` 已在调用 FileOps 前释放 FD-table 锁并持有稳定 File 引用。

本批只在这个可阻塞的 task-context 输出操作中开放 IRQ，继续复用原有用户复制和
逐字 UART TX guard；完整写、部分写、零进度 EFAULT 均恢复入口 IRQ 状态。
没有增加分配、拷贝缓冲层、系统调用，也没有在任意持锁路径或所有 syscall 中开放 IRQ。
这修复了已确认的普通 VFS 输出路径，不能据此关闭 printk/长字符串 TransmitGuard、
其他 IRQ 屏蔽窗口或整个 MOSS-018。

## 真机红绿对照

旧内核是第四批最终候选 `d22dd3b` 的生产镜像，本批新内核保持相同构建设置。
严格对照使用字节一致的用户归档和探针：原生服务固定 CPU2，负载调用端为 CPU0。

| 文件 | SHA-256 |
| --- | --- |
| 旧生产 kernel | `c1ce5713274ba185d40b0a924b28de00fc227c108d3abf4406f73ab695ea1c4c` |
| 新生产 kernel | `2c6eb12755952f5a95bafcc0a36815ab790e76ce13b455209233e2abba58946f` |
| 共用固定服务归档 | `13109e659f832d03b635a2a4ae6286f8fbcf90c210ee4220e22f7d831d1f03f3` |
| 两次部署的 DTB | `696a2e0c55418362f4893e0efcdafbd9c0cfbb8051646db8542896ee06cd970a` |

旧普通镜像的十次空闲输入和三次 4096 字节输出期间输入先通过，未复现故障。
随后重部署第四批相同的固定服务归档，默认启动检查再次只留下
`echo MOSS_RPI_IN` 的 16 字节前缀，按原 **90 秒**期限失败。
进一步输入前，临时 halt CPU0 读取到 `RSR=0x8`、`FR=0x91`、`IMSC=0x50`、
`MIS=0`，观察前后四核均为 running。PL011 的 UART RSR bit 3 表示 overrun，
依据 [Arm DDI0183G §3.3.2](https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5)。
单独换行执行出截断 marker，限速独立恢复随后通过；原失败没有改记为通过。
保存错误状态后才写 ECR 清除，确认清除后的 RSR 为零。

进一步用本地探针在收到首条 `RPI_PERF` 结果行的前 32 字节、该行尚未发完时
注入完整 shell 命令。旧 `getpid_raw 2048` 和 `exec 4` 都只留下
`echo MOSS_UART_E`，原 **5 秒**独立 marker/下一 prompt 检查失败，随后观察到
`RSR=0x8`。getpid 无需 fork/exec 即复现，故不能把此故障归因于进程创建。

新实现重新上传并全回读后：

- 原默认启动检查为 `shell_input_passed`，未延后发命令或放宽 90 秒检查。
  supervisor 输出仍与回显交错，但完整结果行和下一 prompt 均收到。
- 同版 getpid 探针在同一结果行位置注入，连续 **3/3** 通过；exec 注入 **3/3** 通过。
  各次仍要求九条采样行、`RPI_PERF_DONE`、真实退出状态 0、独立 marker 和下一 prompt。
- 十次空闲输入及三次 4096 字节输出期间输入，合计 **13/13** 通过。
- 重放前后 RSR 均为 **0**；仅在实验前后做 JTAG 观察，输入过程中没有 halt。

旧 exec 首次试验使用初版本地探针，晚期 DONE 检查当时有误，但失败发生在更早的
marker 等待阶段，未到达该断言。严格同版脚本对照取旧/新 getpid；不将后来修正的
脚本源码冒充旧 exec 运行时版本。旧普通镜像未复现的 13 次结果也独立保留。

旧启动还出现过 Loader 用户态缺页退出：`addr=0x4061ac2e0 pc=0x20002c6d0`，
随后 supervisor 重启服务。原始串口保留；本批没有建立它与 UART 溢出的因果关系，
没有将其记为已修复。

## 回归与首次失败

新增 `vfs/console_write_irqs` 经真实 `do_write` 和 `InputBuffer` 检查两种入口
IRQ 状态下的零进度、部分和完整写，要求复制时 IRQ 开放、返回值正确、入口状态恢复，
合计 **20 个断言**。旧实现为 **17 passed、3 failed**；后续 case 标为 not_run。
catalog 升至 v12，Pi runner 增加无宿主输入的 `vfs` 套件。

ARM64 Release 和三架构 Debug 构建通过。首次编译新增测试时误用不可用的
initializer_list，修正为 constexpr 数组后才完成最终红绿验证，原编译错误日志保留。
宿主 rpi/验证协议回归 **181/181**；定向 ARM64 clang-tidy **2 TU**、Ruff 和
clang-format 通过。编译数据库对三个无关 TU 的既存覆盖提示仍保留，未宣称全仓 lint。

七个定向套件为 `vfs`、`vfs.smp`、`users.uaccess`、`users.signals`、`users.frame`、
`scheduler`、`users.exec`，每架构 114 case：

| QEMU 架构 | 首次矩阵 | 同镜像独立复查 |
| --- | --- | --- |
| ARM64 Debug | 110 passed、1 timeout、3 not_run | VFS 12/12；原 rename case 为 2.177 秒 |
| x64 Debug | 114/114 | 无需重跑 |
| RV64 Debug | 102 passed、1 timeout、11 not_run | users.uaccess 12/12 |

ARM64 `vfs/rename_lifecycle` 达到原 **5 秒**期限，GDB 显示 CPU0 在
do_open/namespace_lock 的 preempt-disable hook，其余核 idle；这不足以断言死锁或
宿主负载根因。RV64 `users.uaccess/allocation_fault` 达到原 **5 秒**期限，
现场在 PagePressure 析构的 order-10 free_pages 元数据清理，与第三批压力回收超时
形态相近，但未证明同一根因。独立复查不覆盖首次失败；三架构新增用例均为 **20/20**。

Pi Release 的同一 catalog v12 验证镜像在完整回读后，VFS **12/12**、新增断言
**20/20**；ready/worker 确认四核 detected/online/work、worker 固定 CPU0。
沿用物理 runner 的 30 秒 startup、30 秒单 case、60 秒套件期限，未调整期限。
内核测试镜像 SHA-256 为
`006d48a9efafc8fc3a73ed70cc7182515c4d6600af1f9dc82cee5be5ad1e5824`，
其 fixture 和匹配符号身份记录在 finalized 的真机 JSON。

## 证据与剩余工作

最终恢复同一新生产 kernel 和原始未固定服务、PMU OFF 的普通 initramfs，
EL2h/cache-off 后以 1 MHz 全量上传、回读，默认独立 echo 结果行和下一 prompt 通过。
普通归档 SHA-256 为
`839e2f1d42fc0ff96e5851a8db283255ebbe936c6b6d375cb20cae656c15f8ad`。
只读确认 CPU0–CPU3 running 后 shutdown OpenOCD，无 halt 留机、无串口 reader 遗留。
shutdown 的 Tcl 控制回复为 `-600 {}`，通用 command 包装将其报告为异常；另确认
OpenOCD 进程正常退出且端口已关闭，状态与异常记录一并保留。

- [机器可读记录](validation/rpi3b-console-write-2026-10-18.json) 包含镜像身份、
  原始红绿范围、逐 case 状态、首次失败、生产恢复及完整/文字证据包哈希。
- [原始文字证据包](validation/rpi3b-console-write-2026-10-18-evidence.tar.gz) 保存
  串口原始字节、结果 JSON、GDB 现场、构建/lint 日志、源码/patch 与文件哈希索引。
  匹配镜像、归档、DTB 和符号保存在本地完整证据包，身份由同一索引关联。
- 首次生产恢复的本地快照漏掉 validation 字段，loader 在连接目标前拒绝；
  原 manifest 和拒绝日志保留，补全快照后才完成上述部署。

本批不报告吞吐提升或单次尾延迟改善。IRQ 状态切换在每次控制台操作边界执行，
没有新增逐字分配或全局锁；量化输出成本、持续全双工负载、Linux 同板对照以及
其他长 IRQ 屏蔽窗口仍需后续验证。MOSS-017/018 保持部分开放，首次 QEMU 超时和
旧 Loader 故障继续跟踪。
