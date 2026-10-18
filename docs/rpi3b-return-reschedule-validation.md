# 用户返回抢占安全点验收（2026-10-18）

本批以 `cd9abbf` 加本批改动构建，Clang 23.1.2、三架构 Debug。
目标是消费统一用户返回路径的待抢占请求；完整内核抢占、迁移交错、
Release 性能和同板 Linux 对比仍保持开放。

## 行为与边界

三架构的 syscall/IRQ 汇编入口均先屏蔽 IRQ，再调用 user_return_handler；
后者只处理 from_user 的帧。现在在信号检查前调用公开的 check_need_resched，
持有当前借用 TrapFrame 直到原线程恢复，再处理离开 CPU 期间到达的信号。

无请求时立即返回；IRQ 开启、线程非 Running 或 preempt_count 非零时保留请求。
安全时原子消费请求，按真实纳秒补记 sub-tick CPU 时间和 CFS 公平时间，
复用 reschedule_current 与 bootstrap 保存/交接，包含无需切换的自选路径。
真正重新派发时在现有转换锁内清除上次运行的请求；后来的队列/优先级发布仍可
设置新请求，避免旧 tick 标志在用户返回时引起第二次无意义调度。

最外层 unlock/preempt_enable 仍仅减少嵌套计数；没有在任意内核锁释放处调度。
内核态 IRQ 返回仍走原有 tick 机制，后续要逐项审计其他安全点与完整 on-CPU 所有权。

## 击中旧缺陷的回归

`scheduler/reschedule_safe_point` 使用私有队列和合成 current，屏蔽 IRQ 后检查：
持锁/非 Running 时请求不丢失；解除保护后恰消费一次；自选后仍 Running 且没有队列节点。
旧空实现为 **4 个断言通过、2 个失败**，后九项正确记 not_run。

`users.frame/return_reschedule` 使用两个真实进程，均固定 CPU0。子进程以 RT 策略
停在生产 prepare_sleep/commit_sleep；父进程在寄存器探针 syscall 的返回 hook 中
屏蔽本核定时器并唤醒子进程。子进程必须先运行并退出，父进程才可完成该次用户返回。
同核 wakeup 不发 IPI，定时器不能以稍后的 tick 掩盖缺少返回安全点的错误。
父进程随后核对 handoff、IRQ/请求状态和真实回收，清理分支恢复定时器并重装 deadline。
复用汇编探针检查 GP/标志，以及 ARM64 FP/NEON；旧用户返回路径为错误 mask `0x4`。

x64 首次扩展探针曾误用 syscall_number() 识别已返回的调用：保存的 RAX 此时是结果。
该轮 return_reschedule 失败已保留。最终改为由用户程序传入汇编探针的确切 post-syscall PC，
三架构均在这个位置触发。没有据此更改生产 syscall ABI 或放宽断言。

## 实际验证结果与失败记录

三架构 Debug 构建通过。最终 ARM64、x64 QEMU 的 users.frame、scheduler、
users.uaccess、users.exec、users.signals、users.ipc、users.timers、users
各 **147/147 case** 通过，报告 finalized 且无 not_run。
RV64 同一矩阵通过 **140 case**；users.exec/allocation_rollback 在原 **5 秒**期限
下于 **5.016 秒**超时，后六项 not_run，不能将本批写为三架构一次全绿。

超时 GDB 现场位于 PagePressure 析构的 order-10 free_pages 元数据校验循环，
没有 panic；物理 hart 2 是 OpenSBI 本轮 boot hart，对应逻辑 CPU0，其他 hart idle。
相同镜像隔离运行 users.exec **32/32** 通过，allocation_rollback 用时 **1.538 秒**，
期限和断言均未调整。首次失败及寄存器/调用栈保留；根因未确定，
不能从复跑通过推断竞争已消失，也没有证据将其直接归因于宿主负载。

宿主定向回归 **173 passed**；ARM64 定向 clang-tidy 的三个编译单元、
改动 C/C++ 的 clang-format、Python Ruff/格式及源码/文档差异空白检查通过。

Pi 最终 PC 探针第一次在 **2 MHz** 上传时，内核回读有四字节差异：
文件偏移 `0x3f090..0x3f093`、物理地址 `0x23f090` 的 `ret` 应为 `c0035fd6`，
首次返回 `f0c097f5`。runner 在进入内核前停止，全部 case 正确保留 not_run。
保持 cache-off/halted 后，2 MHz 和 1 MHz 的只读复查都取得期望指令；
随后复位、新附加并以 **1 MHz** 重新完整上传/回读，禁止跳过原差异直接启动。
瞬时读取差异的原因未确认，不将其归因于接线或内核功能。

Pi 3B+ Rev 1.3 的 firmware ARM RAM 为 **948 MiB**。最终镜像 `d5c2f67c2be3`
在 1 MHz 各次完整回读后，users.frame **4/4**、scheduler **18/18**、users.ipc **22/22**
均通过，ready 确认四核 detected/online/work，worker 固定 CPU0。
随后恢复匹配的生产镜像，完整回读、独立 echo 结果行和下一次 BusyBox 提示符通过；
OpenOCD 脱离后保留正常 Moss 运行。全部部署仅写 RAM。

结构化结果、各镜像构建身份、上传/串口哈希及原始日志见
[验收记录](validation/rpi3b-return-reschedule-2026-10-18.json)。

## 性能与未完成项

普通无请求返回不读取时钟、不获取队列锁、不分配内存；有请求才进入现有调度路径。
本批没有 Release 成本、竞争、唤醒尾延迟或 Linux 对比数据，不能从正确性通过推导性能达标。
非用户返回的最外层抢占恢复、远程 affinity/on-CPU 发布、完整共享地址空间生命周期及
更广的失败/长期负载仍需后续批次；MOSS-017 保持未关闭。
