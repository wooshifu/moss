# Pi 3B+ fork FP/NEON 验收（2026-10-18）

本轮以 `6274d11` 加工作树改动构建，Clang 23.1.2、ARM64 Debug。
这是基础 FP/NEON 继承及物理板验证入口的功能证据，不能外推为完整扩展状态、
Release 性能、Linux 对比或所有内核套件已经真机通过。

## 修复与回归

ARM64 `do_fork()` 原先仅复制 GP/TLS，没有将实时 FPCR、FPSR、q0～q31
放入子线程首次用户返回的 CpuContext。复制父线程上次调度快照也不能保证实时状态。
修复仅在 fork 路径增加保存，普通 syscall 帧不增加向量区。

汇编探针在 syscall 前保存调用方状态，为每个向量的两个半部设置不同值，
并设置非默认 FPCR 舍入模式和 FPSR sticky 状态。父子进程都在任何 C 调用或
exec 前检查，之后恢复调用方状态；同一个探针也覆盖普通 syscall 和信号返回。
catalog v9 明确区分此前仅有 GP 检查的覆盖。

## 物理板结果

板型为 Raspberry Pi 3 Model B Plus Rev 1.3，firmware ARM RAM 为 948 MiB。
每次进入内核前完成 cache-off 和 kernel/initramfs/DTB 的逐字节完整回读。
协议要求四核 detected/online/work 全部一致，worker 固定 CPU0。

| 运行 | 测试镜像 SHA-256 前缀 | 结果 |
| --- | --- | --- |
| 旧 fork + 新探针 | `1621c724405a` | native_frame 通过；fork_registers 断言失败；signal_return 正确记 not_run |
| 首次修复 | `1790dbee6f97` | users.frame 3/3 通过 |
| 格式/lint 收尾后的修复 | `20c0c05249a8` | users.frame 3/3 通过 |
| 同一最终镜像 | `20c0c05249a8` | mm 7/7 通过 |

旧镜像使用 1 MHz JTAG；修复镜像使用经过重复 firmware 读取和整次部署回读检查的
2 MHz。该差异只影响上传阶段，不是内核运行速度对照。watchdog 从 Image 入口后开始，
不将 JTAG 时间计入 case。没有 SD 写入。

原始串口、完整哈希、构建 provenance、case/end 记录见
[结构化验收记录](validation/rpi3b-fork-fp-2026-10-18.json)。其 `serial_log`
指向同目录原始日志；记录中的每份日志已重放 `kernel_validation.Protocol`，
结果与原 runner 一致。各镜像保留自身链接时的 dirty/source 身份，后续纯文档或
主机工具改动不会冒充该二进制的构建来源。

另完成最终正常镜像的 `deploy --check`：实际 BusyBox shell 接收 echo，
返回独立结果行及下一次提示符。验证后保留正常 Moss 运行。
套件完成后的首次复位曾遇 J-Link USB 断开；新连接、复位、完整上传回读和
生产检查均成功。该连接失败保留在本地证据中，不能归因为内核故障。

## 配套回归与限制

三架构 Debug 构建通过。最终 QEMU：ARM64 的 users.frame、users.uaccess、
users.exec、users.signals，以及 x64/RV64 的 users.frame、users.exec，
合计 8 个 workload、152 个 case 通过。旧 ARM64 QEMU 同样在 fork_registers 变红。

宿主 PTY 测试检查完整协议、断言后的 not_run、panic、超时、错误/追加记录、
快照与部署失败留证；另验证原有 rpi_perf 的纯标准库环境不依赖验证器包。
验证相关定向回归为 185 项通过，最终 `test_rpi.py` 为 20 项通过。
全量宿主首轮为 443 passed、1 skipped、1 failed：唯一失败是 ARM64 独立
libc/compiler-rt/BusyBox 冷构建超过 300 秒宿主时限。该节点单独按原时限和
全部断言重跑通过，用时 266.99 秒；首轮超时仍保留，不记为一次全量全绿。
定向 ARM64 clang-tidy、全仓 Ruff/格式检查和差异空白检查通过。

真机 runner 当前只选择无需输入注入的 mm、users.frame、users.uaccess、users.ipc；
本记录只对前两套件提供物理执行证据。

MOSS-014 的完整属性继承、其他扩展状态以及共享 exec 等任务仍开放；
抢占闭环只澄清了当前注释，尚未在本轮实现。
