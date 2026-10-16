# ps / top 实现方案与验收

## 实现前的路径

生产环境使用 BusyBox ash、mlibc 和 Process Compatibility Service。POSIX PID 由服务分配；内核 Process ID 只是执行域标识，两者不能混用。BusyBox 已有 ps/top，但 Moss 配置未启用；原有 SYS_TOPINFO 返回最多 64 个内核进程，且 CPU 时间使用可能被限幅的 CFS 公平性计数，因此不能直接作为生产命令的数据源。

## 实现步骤

1. 共享固定宽度监控 ABI。新增只读系统摘要和需要 DOMAIN_INSPECT 的执行域统计系统调用，保留旧 TOPINFO ABI。使用实际调度时间、地址空间 VMA/用户页表和终止状态；安全拷贝到用户态。统计是逐项瞬时观察，允许一个调度 tick 的计费延迟。旧 resident_pages 只记录部分预先装入的页，RSS 因此直接遍历已存在的用户页表，覆盖按需分页、fork/COW 和 unmap。
2. 在 exec 提交时保存有界命令摘要，fork 继承，失败 exec 保持原值。摘要仅用于显示，并过滤控制字符。多线程 CPU 时间求和；地址空间和执行域在采样期间保有所有权。
3. Process Service 通过已认证会话提供只读枚举。按单调 PID 游标和起始上限遍历，不设 64 项截断；跳过尚未绑定的 fork 预留项；保留未回收的退出记录。返回服务 PID/PPID/PGID/SID，不返回执行域句柄，也不改变信号权限。
4. 复用 BusyBox ps/top 的参数、排序、刷新和终端恢复。Moss procps 适配器读上述服务；top 的系统摘要显示实际内存、busy/idle、uptime、任务数。不开启依赖未实现 smaps、线程枚举和 Linux 负载均值的功能。

## 范围与接口语义

- ps 默认显示进程列表，支持 -l、-w；top 支持批处理 -b、刷新次数 -n、间隔 -d 和 q 退出。当前整数秒配置下，交互间隔至少 1 秒，-d 0 仅用于批处理。
- 分页接口不另设进程数量上限。当前内核每个执行域只有 64 个能力槽，进程服务需用其中一部分保存后端和在途 Reply，故实际同时存活的 POSIX 进程数量仍受既有能力配额限制；不为了监控命令扩大系统配额。
- 列表是当前 Process Service 的 POSIX 命名空间。独立原生服务不是该命名空间中的 POSIX 进程，系统 CPU 摘要仍包含它们的开销。
- CPU 百分比按全部 CPU 的总容量计算。CPU 时间合并用户态和内核态，不伪造两者拆分。
- 命令摘要上限由 IPC 消息容量决定，包含 argv、不包含环境变量，保证 NUL 结尾。
- 进程退出、exec、fork 可与扫描并发；扫描不是全系统原子事务。游标和上限使并发 fork 下遍历仍可结束。
- 内存的 VSZ 为已授权 VMA 总长，RSS 为当前映射的常驻页，包含各进程对共享页的引用；不是独占物理内存。
- 生产 libc 的标准输入仍走内核控制台，Console Service 的 FD_WAIT 不接收这条输入流。新增可读控制台描述符可用的 TRY_GETC ioctl，复用驱动的非阻塞字节读取。libc 以最多每秒 100 次、每次间隔 10 ms 的睡眠轮询实现刷新期限；不切换共享终端模式。今后控制台支持带期限的 readiness wait 时替换轮询。
- 控制台尚无窗口尺寸协议，沿用 BusyBox 的 LINES/COLUMNS 环境变量及 80×24 默认值。ps 的 STIME 受当前系统时钟限制，是启动相对时钟，并非真实日历时间。

## 验收

- ABI 布局、权限不足、坏输出地址、长度不匹配、实际 CPU 时间递增、按需触页及 unmap 后 RSS 变化、终止状态。
- 生产 shell 中验证 ps PID/PPID 和命令、睡眠/忙循环、僵尸回收、并发子进程完整枚举；top 多帧输出及非零负载。
- 交互 top q 退出、终端恢复；参数错误返回失败；输出重定向可用。
- ARM64、x64、RISC-V 构建及适当的真实 QEMU 回归。主机脚本测试只证明工具逻辑，不替代内核运行。

## 验证记录

2026-10-16，在当前含并发修改的工作区验证，未创建提交：

- ARM64、x64、RISC-V Debug 构建通过。
- 生产回归包含内核权限/坏地址/ABI 长度检查、20 子进程枚举、命令截断与控制字符过滤、失败 exec、睡眠/停止/僵尸/回收、RSS 触页/unmap、ps 默认/长格式/重定向、top 两帧真实负载、q 退出及错误参数后的 shell 可用性。
- ARM64 完整生产回归通过，184 步、100.35 秒。报告：`build/arm64-debug/production-boot/run-monitor-ls58twmw/guest/results.json`。该次显式使用 240 秒排查预算；原 90 秒运行在仍有进展的服务恢复阶段超时，因此普通运行默认改为已有 RISC-V 的 180 秒，CTest 外层保留 15 秒开销余量。
- RISC-V 完整生产回归通过，184 步、124.17 秒，报告：`build/riscv64-debug/production-boot/run-708adnwb/guest/results.json`。
- x64 完整生产回归通过，184 步，报告：`build/x64-debug/production-boot/run-54ihtshe/guest/results.json`。
- RISC-V 调度器 16 项回归通过，报告：`build/riscv64-debug/validation/1792165988799712000/results.json`。首次运行在 `kernel_stack_initialization` 的内存压力清理阶段触及单用例时限；GDB 现场保留在 `1792165910619850000`。以 `--case-timeout 30 --guest-timeout 120` 有界复跑通过。
- 主机验证：生产回归驱动 24 项测试、用户态编号/构建检查 13 项通过；未运行其中 6 项独立冷构建/编译数据库用例。主机测试不替代上述真实内核 QEMU 运行。
- 相关自有 C/C++ 文件格式检查、Python Ruff、CMake 格式及 `git diff --check` 通过。第三方代码沿用各自格式，未整库重排。

以上为主机与 QEMU 记录；物理硬件结果见下节。本次不实现 procfs、smaps、线程列表、Linux load average 或 POSIX poll/termios；终端尺寸和输入轮询限制见上文。

### Raspberry Pi 3B+ 真机验收

2026-10-17（Asia/Shanghai），在 Raspberry Pi 3 Model B Plus Rev 1.3 / Cortex-A53 上完成本次验证。使用 J-Link `50120677`，附加速度 1 MHz、上传与完整回读速度 2 MHz；串口 `/dev/cu.usbserial-14440` 为 115200 8N1。只部署 RAM，未写 SD。启动日志确认 `SMP boot complete: 4 CPUs online` 和 `SMP: 4 CPUs active`，用户态 `nproc` / `nproc --all` 均返回 4。

独立构建目录为 `build/monitor-rpi3b-debug`，preset 为 `arm64-debug`，Clang 23.1.2，`MOSS_BUILD_TESTS=OFF`、`MOSS_ARM64_PERF_PROBE=OFF`。生产 initramfs 自带的监控探针参与验收；未运行独立内核测试镜像。构建基于 `efc58e2423d1222c950c8f2e58fc41bc7d19bd14` 加当时未提交修改，构建前后 7,236 个非 docs 源文件的哈希一致。补丁 SHA-256 为 `1c1ff8af939453e08ae7472f2eae43812c89a7bb653105153d7f4ea9775bf56d`；新增 `monitor.h` 单独保存在证据目录，避免仅有 git diff 遗漏未跟踪源码。

| 本次部署文件 | SHA-256 |
| --- | --- |
| kernel | `bae7e2d1ab3572ecd0cf96aa3a4342e5f89b17174bdd5b4a6a1e60373b54300f` |
| initramfs | `839e2f1d42fc0ff96e5851a8db283255ebbe936c6b6d375cb20cae656c15f8ad` |
| firmware 原始 DTB | `6b3c3d5b1bdb9b1f22c7b00701865cf1f31cac7d0b1b3ed275dae924ec89bbe4` |
| loader 处理后实际部署的 DTB | `b01c5f97ef96aa619714da62fc38c6b3d7c4665df61ecbc192c91235f1fd429a` |

复位后重新附加，确认 EL2h，通过 cache-off 交接，再上传并完整回读 handoff、kernel、initramfs 和 DTB，全部字节匹配。部署检查结果为 `shell_input_passed`。随后运行 14 项独立验收，每项检查实际输出、退出状态和下一次 shell 提示符；命令逐字按 2 ms 间隔发送，复用已有真机 collector 的 500 bytes/s 经验值，避免控制台接收丢字。

| 验收内容 | 本次结果 |
| --- | --- |
| shell 往返、在线与配置 CPU 数量 | 通过；两种 nproc 均为 4 |
| `/moss-process.elf probe` | 通过；含 20 个同时存活子进程完整枚举和 fork/wait 生命周期 |
| `/moss-process.elf monitor-probe` | 通过；权限、空地址、ABI 长度、CPU 时间递增、3 个按需页触页/unmap 的 RSS 变化、sleep/stop/zombie/reap、exec 摘要截断与控制字符过滤、失败 exec、TRY_GETC 参数与描述符权限 |
| `ps`、`ps -lw`、重定向及文件清理 | 通过；shell PID=2、PPID=1，命令 `ash -i`，ps 自身命令参数正确 |
| `/moss-process.elf monitor-top` | 通过；两帧、子进程退出回收正常，忙循环占用分别为 25.0% / 25.2%，接近四核总容量归一化预期的 25% |
| `top -d 1` 的 p/m/t/n/r、q、Ctrl-C、Ctrl-D | 通过；排序键触发新帧，三种退出输入均返回 shell |
| `top -b -n 2 -d 0`、错误 `-n invalid`、交互 `-d 0` | 通过；批处理输出两帧，非法参数返回非零，交互零间隔明确拒绝 |
| 退出后的 shell 文件写入、读取、校验和删除 | 通过 |

证据根目录为 `build/monitor-rpi3b-evidence/`：`source-before.json` / `source-after.json`、`source.patch`、`monitor.h`、`environment.json`、`artifacts.json`、`image/` 保存源码与产物身份；`configure.log`、`build.log`、`reset.log`、`jtag.log`、`deploy.log` 和 `deploy/result.json` / `deploy/serial.log` 保存构建与部署过程。逐项结果及原始串口日志为 `validation-1792172237148123000/results.json` 和同目录 `serial.log`，采集脚本为 `validate.py`，执行汇总为 `validation.log`。

本次为生产镜像的定向功能验收；未运行完整内核套件、长时间压力、性能基准或 GDB 控制台竞态测试。排序键检查证明输入与刷新可用，不是各排序方式的独立数据排序证明。

## 使用

```sh
ps
ps -lw
top
top -b -n 2 -d 1
```

交互模式按 `q` 退出；`p`/`m`/`t`/`n` 沿用 BusyBox 的 CPU/内存/时间/PID 排序，`r` 反转排序。CPU 占用按全部 CPU 总容量显示，四核机器上一个占满单核的进程约为 25%。
