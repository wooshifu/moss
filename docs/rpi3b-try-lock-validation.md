# try_lock 抢占保护验收（2026-10-18）

本轮以 `6e145d5` 加本批改动构建，Clang 23.1.2、三架构 Debug。
本批关闭锁的抢占计数配对缺陷；完整抢占安全点、on-CPU 交接和 Release 性能仍开放。

## 根因与最小修复

`TicketSpinLock::lock()` 增加当前线程的 preempt_count，unlock 递减；
try_lock 原先只有 CAS。成功 try-lock 后解锁，会减掉外层锁贡献的计数，
使真实 scheduler_tick 可能在外层锁尚未释放时调度。

修复在 CAS 前调用同一禁止抢占 hook：成功保持到 unlock，失败立即回滚。
仍为一次非阻塞 CAS，不增加等待、分配或全局锁。VM 事务、日志 console lock
及 IrqSpinLock 包装自动使用此修复，调用者无需另建保护策略。

新增 scheduler/try_lock_preemption 复用生产 scheduler_tick 和私有运行队列，
在屏蔽 IRQ 时暂用合成 current，分别检查普通锁和 IRQ 锁。两把锁必须贡献两个计数；
递归 try 失败不得改变计数；内锁释放后仍保留外锁保护；强制 RR 到期只设置
need_resched，不能执行抢占。恢复真实 current/IRQ 后才作断言，避免测试日志影响计数。
旧实现即使错误放行也只会选择自己，不切换到未初始化的栈。

## 验证范围

旧 ARM64 QEMU 新用例为 **3 个断言通过、2 个失败**；前七个 case 通过，
新增 case 失败，后九个正确记为 not_run。保留失败证据后才修复生产锁。

三架构 Debug 构建及各自 scheduler、users.frame、users.uaccess、users.exec
均通过，每架构 **64 个 case**，合计 **192 个 case**；报告 finalized 且无 not_run。
宿主 rpi/验证协议定向回归 **173 passed**；覆盖更长的 scheduler catalog、
错误 workload、断言、panic、缺项、追加记录和超时。首次扩展 PTY 测试时，
错误 workload 的 writer 在拒绝后仍发送完整 catalog 而阻塞；改为只发送被拒绝的
ready 记录后全套通过，生产协议和期限未放宽。

定向 ARM64 clang-tidy 检查锁模块的两个编译单元；改动 C++ 的 clang-format、
Python 的 Ruff 与源码/文档差异空白检查通过。

真机逐项结果、源码和上传镜像身份见
[结构化记录](validation/rpi3b-try-lock-2026-10-18.json)。
原始日志按字节保存，不将串口回显、CRLF 或提示符尾空格格式化。

Raspberry Pi 3 Model B Plus Rev 1.3 的 firmware ARM RAM 为 948 MiB，
J-Link `000050120677` 在 2 MHz 完成 kernel/initramfs/DTB 全量回读后启动。
同一测试镜像 `46c82a3eb2e2` 的 scheduler **17/17**、users.uaccess **12/12**
通过；两次 ready 均确认四核 detected/online/work，worker 固定 CPU0。
随后恢复生产镜像，BusyBox shell 的独立 echo 结果行和下一次提示符均收到；
OpenOCD 脱离后保留正常 Moss 运行。全部部署仅写 RAM。

## 性能与剩余工作

本修复补齐与普通 lock 相同的抢占保护 hook。没有测量其 Release 热路径成本，
不从 Debug 的正确性回归声称吞吐改善或 Linux 对比达标。
check_need_resched 尚不调度，最外层 preempt_enable 仍仅减计数；
完整安全点和任务所有权审计继续保持开放。
