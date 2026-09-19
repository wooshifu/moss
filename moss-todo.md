# Moss 内核设计与实现审计报告及修复清单

> 原始审计：2026-09-05，源码基线：`e0e2bbc920b66f91e18802a46106ca5811e588b1`。
> 状态复核：2026-09-19，提交基线：`caaab1b`。第 3.4～3.20 节保留此前实测及当时状态；最新 VFS/信号 uaccess、PVH/initrd、进程生命周期、brk/VMA、exec 事务、ELF LoadPlan 与九 preset CTest 收口见第 3.21～3.27 节。本文与 [todo.md](todo.md) 同步。
> 第 3.1～3.2 节保留原始运行证据；第 5～9 节未标新日期的“位置与事实”、地址和行号属于原始审计，不能当作当前仍然失败的运行结果。当前状态以第 4 节及各项更新说明为准。

## 1. 当前结论

Moss 已具备三架构真实启动、SMP、用户态与进程执行路径。后续提交 `44dedc2` 建立了生产内核验证和函数基准；`6252484` 实现了同 ISA 通用镜像、运行时硬件发现与独立 runner；`b57422d` 恢复了全部 9 个 CMake workflow 的 CTest 步骤。RISC-V 64/x64 不是启动桩，旧 x86 测试 ELF 装载失败和 initramfs fallback 也不再代表当前正常路径。

但“能启动、能完成一次 fork/exec/wait”仍不等于可靠的多架构研究内核：

1. **隔离缺陷尚未全部闭合。** 已修复内核映射 USER，并收紧三架构最终内核 W^X；syscall 0 原始 UART 指针旁路已删除，用户地址域/VMA 准入与跨 VMA 权限校验已补。`0e88344` 改为原生 TrapFrame、按 ISA 净化信号返回状态；3.20～3.21 补共享用户复制的异常 fixup、VFS/信号迁移与缺页 OOM 回归。恶意信号帧全覆盖、COW OOM 和并发 VM 生命周期仍未完成验收。
2. **部分基础修复已落地，但所有权验收未完成。** `040d773` 已验证 heap/PFA 对齐与释放、保留洞和多 bank 耗尽、当前布局及页表/元数据哨兵。`4cde9b3` 修复指针发布误删可达节点；当前工作区已删除伪 RCU，迁移锁保护的拥有型容器并补持有读者、重入与双 CPU 交错测试。IRQ/驱动/IPC 复合生命周期、页引用并发及完整启动保留集合仍待完成。
3. **跨架构入口已统一，完整进程语义仍待补。** 三 ISA 用单一原生帧指针传递 syscall，fork 不再猜内核栈偏移，信号返回桩由各 ISA 汇编产生；3.24～3.26 已闭合当前单地址空间模型下的 brk、exec 事务和受支持静态 ELF LoadPlan。GP/条件码和基本信号往返通过，不代表全部 FP/TLS 继承、CPU-bound 信号、nanosleep、动态加载、共享 LOAD 页或共享地址空间 VM 并发已经可靠。
4. **旧停滞记录应保留，不能直接当作当前复现。** 退出已改为经 `context_switch` 返回活跃 bootstrap 栈，不再在 C++ 帧中直接修改 SP。第 28 次停滞的原版本对照、1,000 次生命周期与资源稳定性尚无完整关闭证据。
5. **测试已从基础语言检查升级为真实内核检查，覆盖仍有边界。** 三架构已有功能、框架自检和 Release 基准证据；默认套件包含 COW 权限、原生帧、输入/输出缺页 OOM、跨页短 I/O 和信号复制故障回归，但未完成恶意信号帧、全部 uaccess 故障、长循环或确定性调度竞争验收。

保留现有 C++ 模块、AAL/HAL、可用进程/VFS/调度路径；先关闭权限、所有权、等待/退出和失败事务，再扩展高级 IPC、POSIX 或设备栈。内核模型仍是模块化单体，而不是已经实现用户态服务隔离的 hybrid。

本文继续追踪 **32 个审计项**。有些子问题已修复，完整审计项仍按原验收关闭；不能只因相关提交或基础测试通过而整体勾选。

## 2. 证据标准、范围与限制

### 2.1 如何阅读严重性和证据

| 标记 | 含义 |
| --- | --- |
| P0 | 破坏内核/用户隔离、基础内存所有权或关键入口契约；应作为后续功能工作的前置阻断项 |
| P1 | 破坏进程语义、跨架构可用性、并发安全、资源回收或测试可信度 |
| P2 | 能力表达、可维护性、可移植性和观测方面的问题；部分工作需提前配合 P0/P1 |
| 静态确认 | 已沿真实调用路径检查源码，可以指出违反的具体不变量；不等于已执行利用程序 |
| 运行复现 | 本次实际构建、测试或 QEMU 交互出现了对应现象 |
| 待验证风险 | 存在源码风险或定位线索，但尚未通过针对性实验确定触发条件或根因 |

P0 是本项目的实现优先级，不是 CVSS 评级。研究内核也需要隔离错误进程；这里不要求它立即具备生产操作系统的全部安全机制。

原审计行号指 `e0e2bbc`，初次更新说明中的符号和路径按 `b57422d` 核对；后续提交和工作区修复另标。请用“路径 + 符号 + 不变量”定位。每项完整验收仍是关闭要求；只有第 3.3～3.15 节明确列出的覆盖可以算已执行，不能把新增普通测试外推成所有审计验收通过。`[x]` 只关闭该行限定的子任务，`[ ]` 包括待实现、部分实现及待专项验收。

### 2.2 原审计方法与本次复核

- 优先使用 codebase-memory-mcp 的符号搜索、调用关系和源码片段，检查索引覆盖及引用路径；对汇编、链接脚本、部分解析范围和负向结论补充直接源码检查。
- 从启动到用户态、系统调用到缺页、fork/exec 到退出回收追踪实际路径，而非只看接口声明或 `todo.md`。
- 对已有六个 QEMU 构建目录执行增量构建和 CTest，并进行 ARM64/RISC-V 64 用户态交互；对重复执行停滞使用 QEMU 寄存器采样和 `llvm-addr2line` 定位。
- 对页权限、异常返回、RISC-V 64 PTE、PVH 和 RCU 契约参考官方资料。知识图谱覆盖是辅助信号，不是源码完整性证明。

2026-09-06 复核先比较 `e0e2bbc..b57422d`，再对相关符号、调用链和当前源码进行检查。知识图谱使用 Tier 2，初始 generation 为 `2026-09-06T10:43:22Z`；已检查证据路径覆盖，并直接读取汇编、链接脚本及部分解析范围。对于图谱未捕获的全限定 C++ 方法，使用直接源码确认。此次更新不是重新执行全部历史故障实验。

范围涵盖启动、AAL/HAL、物理与虚拟内存、页表与缺页、容器、调度、进程、信号、系统调用、ELF、VFS/pipe、定时器、IPC/驱动框架及测试入口。没有完成真实硬件验收、长时间 SMP 压力、所有错误注入、完整指令集状态覆盖或第三方库逐行审计。

### 2.3 当前架构判断

```text
各 ISA 启动汇编 / BootImpl
  → 早期页表、启动信息、物理页分配器、运行时堆
  → Kernel 初始化、计时器、中断、调度器
  → initramfs、VFS、/dev、初始进程
  → 用户态 trampoline → exec shell
  → 系统调用 / 异常 / IRQ
  → VM、VFS、信号、调度、wait/exit/回收
```

从运行机制看，这是**模块化单体内核**：进程、VFS、驱动和调度器共同运行在内核特权域内。README 的 “hybrid” 可以保留为愿景，但当前没有足够的用户态服务隔离与消息路径支持它成为实现事实。

值得保留的基础包括 C++ 模块边界、架构封装、显式错误结果、现有 ELF/用户态闭环、调度器的红黑树实现以及 libfdt 的复用。主要设计改进应集中在少数深层接口：内存所有权、用户访问、陷阱现场、阻塞/唤醒和进程资源事务。

## 3. 验证记录：历史故障与当前基线

### 3.1 原审计环境与构建（2026-09-05，历史）

本机工具：Homebrew Clang `23.1.0`，QEMU `11.1.1`，项目虚拟环境 Python `3.14`。命令通过 `uv run --frozen` 使用现有项目环境。

对以下每个预设执行：

```sh
uv run --frozen cmake --build build/<preset> --parallel 4
uv run --frozen ctest --test-dir build/<preset> --output-on-failure
```

| 预设 | 增量构建 | CTest | 实际意义 |
| --- | --- | --- | --- |
| arm64-qemu-debug | 退出 0，ninja 无工作 | 通过，约 0.70 s | 独立基础测试可运行 |
| arm64-qemu-release | 退出 0，ninja 无工作 | 通过，约 1.11 s | 同上 |
| riscv64-qemu-debug | 退出 0，ninja 无工作 | 通过，约 0.59 s | 同上，不代表进程/信号正确 |
| riscv64-qemu-release | 退出 0，ninja 无工作 | 通过，约 1.29 s | 同上 |
| x64-qemu-debug | 退出 0，ninja 无工作 | 失败，ctest 退出 8 | QEMU 拒绝测试 ELF |
| x64-qemu-release | 退出 0，ninja 无工作 | 失败，ctest 退出 8 | 同上 |

这六次构建是**已有产物的增量检查，不是清空目录后的完整重建**。不能据此宣称全新机器配置、依赖下载和完整编译矩阵均已验证。CTest 日志位于对应构建目录的 `Testing/Temporary/LastTest.log`，后续运行会覆盖。

整理报告期间，工作区另外出现了 lint/格式化脚本、Python 依赖及说明文档的并行改动；本次没有修改或回退它们。上述运行结果对应审计时的配置与源码基线，不自动覆盖这些后续工具配置变更。最终检查时未发现本文引用的内核源码有并行修改。

x64 两个测试的装载错误：

```text
Error loading uncompressed kernel without PVH ELF Note
```

### 3.2 原审计内核交互（2026-09-05，历史）

基础启动命令形式：

```sh
uv run --frozen qemu.py \
  --config build/arm64-qemu-debug/qemu_config.json \
  --timeout 20 --smp 1
```

替换配置路径测试其他架构；进入 shell 后输入程序名。脚本达到观察期限而返回 `124` 是本次主动终止方式，不能单独作为内核失败证据，应看此前是否达到目标检查点。

| 场景 | 观察结果 | 结论边界 |
| --- | --- | --- |
| ARM64 debug，SMP 1 | shell → hello → shell 成功 | 一次完整基本生命周期可运行 |
| ARM64 debug，SMP 4 / GICv2 | 同上 | 多核启动及一次交互可运行，不证明迁移竞争正确 |
| ARM64 debug，SMP 16 / GICv3 | 同上 | GICv3 配置的基本路径可运行 |
| ARM64 debug / release，signal_test | basic、nested、mask、altstack、ignore 报告通过；SIGCHLD 为 SKIP | 只覆盖正常形状；不能称完整信号验收 |
| RISC-V 64 debug，hello | hello 输出后非法指令，PID 1 被终止，无新提示符 | 退出/调度路径未闭环 |
| RISC-V 64 debug，signal_test | 第一个 basic 测试出现 load access fault | 信号路径无法工作 |
| x64 debug，正常内核 | initramfs magic 错误，0 个条目，shell 不存在，PID 1 退出 | 正常内核能进入初始化，但用户态启动失败 |
| ARM64 debug，连续 hello | 两次均在第 28 个 hello 输出后不再返回提示符 | 明确的重复执行可靠性缺陷，根因仍需专项验证 |

RISC-V 64 信号故障摘录：

```text
scause=0x5 sepc=0x802195d4 stval=0x116
```

`sepc` 在本次 debug ELF 中解析为 `setup_sigframe()`，`src/process/src/signal.cpp:136`，正在读取陷阱帧。随后还出现 `scause=0x2`，PC 对应 `g_idle_tasks` 数据符号附近。单独运行 hello 时也出现非法指令，PC 为 `0x803dabbe`。这些地址只适用于此次构建。

ARM64 重复执行实验在首个提示符后等待约 0.5 s 再发命令，此后按提示符逐次发送，避免启动阶段串口输入被清空。计划发送 90 次，实际发送 28 次，取得 28 次 hello 输出和 28 个总提示符；总提示符包含最初的 shell 提示符，因此缺少第 28 次执行完成后的提示符。两个独立观察期限分别为 20 s 和 12 s，均停在该位置。

停滞后读取 QEMU monitor 寄存器：

```text
PC  = 0x4022bd98
X30 = 0x4022bd6c
SP  = 0x40249a40
PSTATE: EL1h
```

PC 解析为 `src/aal/src/arch.cppm:143` 的 `cpu_idle_once()`；X30 解析到 `src/process/src/process-scheduler.cppm:2139` 的 `schedule_after_exit()`。这证明采样时停在退出后的空闲调度路径，**尚未证明是哪一个父进程状态、队列、计时器或对象生命周期错误导致无任务可运行**。尤其不能把“第 28 次”直接解释为 RCU 回调池或堆容量阈值。

### 3.3 实施前的复核证据（2026-09-06，b57422d）

本轮重新运行 `uv run pytest -q scripts/tests`：**102 passed in 30.91s**。这覆盖宿主工具、协议和构建回归，不是 102 项内核子系统验收。

已读取现有 `build/<preset>/workflow-ctest.log`、CTest 的 `Testing/Temporary/LastTest.log` 和其结果报告；本轮文档更新未重新启动这六个 QEMU workflow：

| 预设 | workflow CTest 结果 | 日志中的总时长 |
| --- | --- | --- |
| arm64-debug | functional / framework：2/2 通过 | 14.45 s |
| riscv64-debug | functional / framework：2/2 通过 | 15.15 s |
| x64-debug | functional / framework：2/2 通过 | 16.18 s |
| arm64-release | functional / framework / benchmark：3/3 通过 | 18.09 s |
| riscv64-release | functional / framework / benchmark：3/3 通过 | 16.73 s |
| x64-release | functional / framework / benchmark：3/3 通过 | 18.23 s |

本次复核到的结果报告如下，目录为 `build/<preset>/validation/<ID>/results.json`；它们是保留的运行记录，不是本轮重新执行的结果。各报告均 finalized，guest 的 expected/observed 一致；故意失败自检的 case 仍保留 failed/error/not_run，不伪装成普通 case 通过。

| 预设 | functional ID | framework ID | benchmark ID |
| --- | --- | --- | --- |
| arm64-debug | 1788691226372005000 | 1788691228631333000 | — |
| riscv64-debug | 1788691163475287000 | 1788691166023132000 | — |
| x64-debug | 1788691183675696000 | 1788691186651464000 | — |
| arm64-release | 1788691294814863000 | 1788691296162780000 | 1788691307751560000 |
| riscv64-release | 1788691231717949000 | 1788691234013457000 | 1788691245874994000 |
| x64-release | 1788691253466864000 | 1788691255947137000 | 1788691268354819000 |

原始报告是 ignored 构建产物，清理后可能不再存在；上表保留本次查验位置。修复后应生成新记录，而不是覆盖旧记录或修改其 provenance。

9 个 workflow 均注册 configure → build → test；RelWithDebInfo 的三项只有预设匹配检查，没有本次完整运行证据。独立 configure/build 不要求 QEMU，只有测试阶段通过 runner 执行 QEMU。当前命令为：

```sh
uv run cmake --workflow --preset arm64-debug
uv run ctest --preset arm64-debug-test
uv run qemu.py --manifest build/arm64-debug/moss-artifacts.json
```

`src/test/CMakeLists.txt` 复用生产内核模块；该提交的功能 catalog 是 resources/mm/vfs/users，共 7 个用例。`src/userspace/validation.c` 实际执行一次 fork → exec child → exit(37) → wait → 拒绝重复 reap。框架自检有真实断言失败、panic、timeout、未运行后续 case 和堆边界检查；`scripts/kernel_validation.py` 用完整 `@@MOSS` 记录判定并由宿主回收 QEMU，不再采用旧模拟器退出设备编码。

这不是信号、COW 权限、坏用户指针、失败回滚、保留页耗尽或 1,000 次资源压力的替代测试。`signal_test.c` 仍可输出 SIGCHLD SKIP，且不属于默认功能 catalog。

[通用启动验收记录](docs/generic-boot-acceptance.md) 另有无 QEMU 构建、三架构正常 shell、同镜像 ARM64 GICv3/合成 raspi4b、RV64 无 Sstc、x86 q35/pc 的历史验收证据；本次仅复核记录，不声称重新执行真机或这些附加配置。保存报告中的 revision/dirty/hash 按实际运行时保留，不能改写成新提交版本。第 3.1～3.2 节旧 `*-qemu-*`/`--config` 命令仅供历史追溯，不能再用于当前构建。

### 3.4 基础堆与 CPU 就绪修复（2026-09-06，工作区）

修复与测试复用生产模块，未增加堆容量、QEMU 内核依赖或替代分配器：

- [x] `RuntimeHeapAllocator` 对齐返回地址，头部记录原块和请求大小；拆分保留 16 字节粒度，释放收回对齐填充。拒绝零/非二次幂对齐、超大尺寸、越界/内部地址、错误大小及重复释放；零大小的 free 仍表示已有 unsized-delete 契约。
- [x] 先按预留区容量检查再取整/相加；初始化不再向下对齐而借用未拥有空间。扩容、分配、释放及统计共用锁，内部扩容不递归加锁。删除未使用的 shrink 声明、假映射/归还 PFA 页面路径和未声明的空 debug 验证。
- [x] 新增 `heap` 功能套件：alignment、invalid_requests、release_contract、reuse、exhaustion。覆盖 1～4096 的若干二次幂对齐、极值请求、4,096 次混合分配/释放（seed `0x5eed`）、全缓冲区模式、非重叠和计数恢复；以 64 KiB 块耗尽预留堆，检查独立 PFA 页哨兵及 free-page 计数不变，释放后复用合并的 1 MiB 块。
- [x] 真实回归先红后绿：旧分配器的 `--workload heap` 在 ARM64 报 alignment 断言失败（11 pass / 1 fail），报告 `build/arm64-debug/validation/1788692989477573000/results.json`；修复后同入口通过，再执行下面六配置矩阵。当前默认功能共 5 个套件、12 个用例。
- [x] 修复验证中暴露的 SMP 就绪误报：ARM64 `wait_for_cpu_state` 用硬件计时器代替循环次数；三个 ISA 的 `wait_for_all_cpus_active` 收敛到 `boot.cpp`，只等待从核实际发布的 online 位，不把主核写入的 Active 当作完成；调度器初始化在 CPU 缺失超时后返回错误。

SMP 原失败：堆修复后的第一次并行 Debug CTest 中，ARM64 的 heap guest 在开始用例前上报 `online_mask=work_mask=7`，而请求 4 CPU；runner 正确报 infrastructure error。原始报告 `build/arm64-debug/validation/1788693766207790000/results.json` 保留，不能被一次重跑通过抹去。修复后重新构建、执行六配置 CTest，并让 ARM64 四核 heap 连续运行 10 次（`build/arm64-debug/smp-ready-repeat.log`），均通过；额外 GICv3 16 核 heap 报告为 `1788694151491872000`。这不等于已覆盖全部 CPU 启动失败注入或调度交错。

命令：对表中每个 preset 执行 `uv run cmake --build --preset <preset> -j4` 和 `uv run ctest --preset <preset>-test --output-on-failure`；Release CTest 串行执行，避免基准 guest 互相竞争。日志分别为 `build/<preset>/smp-ready-build.log`、`smp-ready-ctest.log`；全部 15 个 CTest 入口通过。报告路径仍为 `build/<preset>/validation/<ID>/results.json`，revision 为 `b57422d`、dirty 为 true，镜像 hash 和工具版本保留在报告中。

| 预设 | functional ID | framework ID | benchmark ID |
| --- | --- | --- | --- |
| arm64-debug | 1788694074154549000 | 1788694077820841000 | — |
| riscv64-debug | 1788694075453235000 | 1788694079547809000 | — |
| x64-debug | 1788694076553963000 | 1788694080593837000 | — |
| arm64-release | 1788694154176555000 | 1788694156140490000 | 1788694167811780000 |
| riscv64-release | 1788694170383590000 | 1788694172571935000 | 1788694184478034000 |
| x64-release | 1788694187347547000 | 1788694190433892000 | 1788694202800013000 |

宿主 `uv run pytest -q scripts/tests`：102 passed（38.00 s）；Ruff、受改 C++ 的 clang-format dry-run 和 `git diff --check` 通过。仍未完成页表/PFA 元数据的专项哨兵及全布局不重叠检查、所有保留区耗尽、PFA 错误 order、分配器 SMP 确定性交错、真实硬件或 RelWithDebInfo 验收。因此这里只关闭已验收子项，004/005/013/028/032 整项保持未完成。

### 3.5 PFA 分配归属与固件边界（2026-09-06，工作区）

以下接续 3.4 的历史结果，修复共享分配器及真实启动路径，没有把硬件夹具加入内核或 CMake：

- [x] 复用每页 flags 记录分配头和原 order，不增加 PageMetadata 大小；每个分配页的引用初始化为 1。释放先完整校验地址、头/order、范围、每页标志和共享引用，再修改任何页或自由链。错误 order、内部地址、重复释放明确失败；存在共享尾页引用时返回 PageInUse，失败保持整个分配不变。统计在同一锁内取快照。
- [x] `pfa.release_contract` 遍历 order 0～MAX_ORDER，检查对齐、完整页计数、每页引用、错误释放不改变原数据及状态；`pfa.exhaustion` 按大到小 order 耗尽所管理区域，核对每块属于固件 RAM、不重叠 kernel/initrd/reserved 或其他分配，写验每页首尾模式、保持 initrd 校验和，逆序释放后恢复计数并复用最大 order。
- [x] 初始化验证表容量、RAM/保留区加法溢出；有效保留区末端 UINT64_MAX 必须先裁剪至可用 RAM 再向上取整，不能回绕到零地址安放元数据。RV64 的 PFA/heap 失败和 x86 的 heap 失败不再只告警后继续启动。
- [x] 独立 `scripts/check_pfa_firmware.py` 使用原有 runner 和 dtc 工具修改实际 DTB。ARM64 每配置 4 项，RV64 每配置 5 项，Debug/Release 共 18 项检查；同一次检查的镜像 SHA-256 保持不变。覆盖相交/非对齐保留洞、RV64 非对齐 RAM 起止与两 bank 间洞、覆盖全部可用内存的有效极值保留区、区间回绕、保留表容量溢出。正向检查必须核对 guest ready 的 RAM 大小，不能仅凭测试通过断言夹具生效。

先红后绿证据：旧 PFA 允许 order 2 分配按 order 0 释放，`build/arm64-debug/validation/1788694541105235000/results.json` 中契约断言失败；保留区取整修复前，`build/arm64-debug/pfa-firmware/1788695494578572000/reserved_end_max/results.json` 为 startup_timeout，没有应有的内存初始化失败诊断。修复后以上入口及下面矩阵通过。负向固件 guest 的原始报告仍为 error；检查脚本要求失败阶段诊断准确且没有 ready，绝不将未运行改写为内核测试通过。ARM64 失败后的 panic 关机路径还会触发异常，未在本节修复。

命令：`uv run cmake --build --preset <preset> -j4`、`uv run ctest --preset <preset>-test --output-on-failure`。构建日志为 `build/<preset>/pfa-firmware-build.log`，CTest 日志为 `pfa-final-ctest.log`；15 个 CTest 入口全部通过。默认功能当前为 resources/mm/pfa/heap/vfs/users 六套件、14 用例。报告保留 `b57422d`、dirty=true 及实际 hash，路径为 `build/<preset>/validation/<ID>/results.json`：

| 预设 | functional ID | framework ID | benchmark ID |
| --- | --- | --- | --- |
| arm64-debug | 1788696137021848000 | 1788696144046044000 | — |
| riscv64-debug | 1788696156600819000 | 1788696164022112000 | — |
| x64-debug | 1788696176947198000 | 1788696184039990000 | — |
| arm64-release | 1788696274818062000 | 1788696279255332000 | 1788696290823541000 |
| riscv64-release | 1788696293475764000 | 1788696298257648000 | 1788696310262940000 |
| x64-release | 1788696314241398000 | 1788696319728458000 | 1788696332465375000 |

固件命令：`uv run scripts/check_pfa_firmware.py --manifest build/<preset>/moss-artifacts.json`。结果为 `build/<preset>/pfa-firmware/<ID>/<case>/results.json`；arm64-debug `1788696135083326000`、riscv64-debug `1788696151466299000`、arm64-release `1788696335929927000`、riscv64-release `1788696351084913000`。ARM64 QEMU loader 会重建 `/memory`，所以此脚本明确拒绝 ARM64 ram_hole 夹具，只在 RV64 核验真实 RAM bank 输入；原理见 [QEMU arm_load_dtb](https://github.com/qemu/qemu/blob/master/hw/arm/boot.c)。早期未核对 RAM 大小的 ARM64 “ram_hole 通过”不作为证据。

宿主 pytest：102 passed（38.76 s）；脚本 Ruff 和 `git diff --check` 通过。该轮未验证全布局和页表/PFA 元数据哨兵；当时 PFA 只管理包含 kernel_end 的一个 bank，RV64 两 bank 测试核对管理容量低于首 bank 的 512 MiB，不能据此宣称联合管理（后续扩展见 3.6）。页引用增减本身的并发/下溢、完整 COW 生命周期、x86 PVH 非法表、真实硬件及 RelWithDebInfo 仍待完成。MOSS-013 的所列对齐/释放/耗尽专项验收通过，修复尚未提交；005/028 等整项不关闭。

### 3.6 多 RAM bank 分配（2026-09-06，工作区）

- [x] 共享 PFA 不再在找到包含 kernel_end 的 bank 后停止：排序固件区域，拒绝重叠 usable 条目，先合并相邻条目再向内页对齐；保留实际物理洞。继续复用 MemoryRegion 链与 buddy 自由链，没有新增架构分支、板名选择或分配器接口。
- [x] 元数据按物理 PFN 跨度索引，必须完整放在一个真实且未保留的 RAM 区域；首 bank 无空间时继续查找后续 bank。元数据自身单独从自由页中排除，其前方未保留空间不再跟着整段丢弃；无可用页明确失败。
- [x] 加强 `check_pfa_firmware.py`：正向多 bank 夹具同时验证实际 RAM 大小、管理容量和真实分配至耗尽。RV64 新增乱序 8 段 RAM、首 bank 全保留而元数据放到后续 bank、非页对齐相邻段、9 段 RAM 容量溢出检查；ARM64 仍只执行其 loader 不重写的 4 个保留区夹具。

根因回归：仅将原 ram_hole 的容量断言改为应利用其余 bank 后，命令 `uv run scripts/check_pfa_firmware.py --manifest build/riscv64-debug/moss-artifacts.json --case ram_hole` 在原实现失败。固件报告 2,147,373,047 字节，PFA 仅管理 127,335 页；报告 `build/riscv64-debug/pfa-firmware/1788696910064555000/ram_hole/results.json` 保留，guest 的局部 PFA 套件虽通过，宿主容量断言仍正确失败。修复后同形状 DTB 管理 518,966 页，两个 bank 的分配/释放和洞检查均通过；乱序 8 段管理 518,963 页，首 bank 全保留的后续元数据布局管理 391,131 页。

同时修正一个测试假设：kernel_end 是排他末端，不保证它是保留页。元数据移到别处后，该地址可能已合法分配给运行中的对象，测试不可盲目释放它。原断言失败报告 `build/riscv64-debug/pfa-firmware/1788697163577302000/unaligned_reserved/results.json` 保留；现在用 kernel_end 前的最后一个内核页验证拒绝释放，并让耗尽测试按固件 RAM 的并集检查跨相邻条目的块，不把相邻条目误判成物理洞。

最终重新构建三架构 Debug/Release、执行全部 15 个 CTest 入口，全部通过；报告镜像 hash 已逐项与当前产物核对。命令与 3.5 相同，日志换为 `build/<preset>/pfa-banks-{build,ctest}.log`，报告路径仍为 `build/<preset>/validation/<ID>/results.json`：

| 预设 | functional ID | framework ID | benchmark ID |
| --- | --- | --- | --- |
| arm64-debug | 1788697295243791000 | 1788697301095081000 | — |
| riscv64-debug | 1788697313646863000 | 1788697320813879000 | — |
| x64-debug | 1788697333684843000 | 1788697340440754000 | — |
| arm64-release | 1788697342860846000 | 1788697347048032000 | 1788697358726459000 |
| riscv64-release | 1788697361286381000 | 1788697366014494000 | 1788697377879375000 |
| x64-release | 1788697380684124000 | 1788697385839136000 | 1788697398387624000 |

同镜像固件检查共 26 项（每模式 ARM64 4、RV64 9），全部满足预期；负向条目仍保留 error/no-ready 和对应启动错误，未当作已运行通过。目录 `build/<preset>/pfa-firmware/<ID>/`：arm64-debug `1788697353153873000`、riscv64-debug `1788697239915258000`、arm64-release `1788697401600645000`、riscv64-release `1788697416149442000`。`uv run pytest -q scripts/tests` 为 **102 passed in 34.72s**，Ruff、受改 C++ 的 clang-format dry-run 和 `git diff --check` 通过。

剩余边界：仍只支持低于 4 GiB 的物理映射；稠密元数据包含洞的索引，当前最多约 16 MiB，稀疏/高地址 RAM 后续改为分 bank 元数据。kernel_end 以下继续整体保留，因为 PVH 低地址启动参数和 AP trampoline 等尚未完全进入显式保留集合；不能冒充精确回收所有启动 RAM。全布局/页表与元数据哨兵、页引用并发、PVH 异常内存表、真机和 RelWithDebInfo 仍待完成，MOSS-004/005 整项不关闭。

### 3.7 堆、页表与 PFA 元数据所有权（2026-09-06，工作区）

- [x] 复用 PFA 统计快照补充真实 metadata_start/metadata_size，保持 PageMetadata 私有；启动输出堆、链接页表区和元数据的实际地址/大小。PFA 初始化在任何元数据/自由链写入前比较 BSS、heap、pagetable 和 kernel_end 的顺序，重叠布局明确拒绝。
- [x] `LayoutSnapshot` 复用当前页表访问接口与 MMU 层级信息，检查并记录内核低/高映射、当前用户地址空间的表树，以及整个 early table pool 和链接页表预留区。表页必须属于 RAM、与 heap/metadata 分离；动态表页必须有 PFA 引用。重复共享表去重，超过 128 张表明确断言失败，不静默截断。
- [x] heap 耗尽测试持有并填充 4/64/256 KiB 块，逐次检查真实元数据及页表校验和，再执行完整预留堆耗尽/释放检查；PFA 耗尽逐块排除元数据和已记录页表，释放后复核元数据和表内容恢复。两者仍是现有套件中的用例，不增加平行分配器或测试专用内核实现。

先红后绿：新增 `uv run scripts/check_heap_layout.py --manifest build/arm64-debug/moss-artifacts.json`，复用 Ninja 的实际链接命令，仅把临时镜像 `_heap_end_addr` 定义为 `_kernel_end_addr+4096`。修复前符号表显示真实 heap_end 为 `0xa3f000`，pagetable 为 `0xa3f000..0xa4f000`，却向运行时声明 heap_end_addr=`0xa50000`。旧 heap 套件仍报告通过，宿主的“必须启动拒绝”断言正确失败，原始报告 `build/arm64-debug/heap-layout-pw4fqpo1/report/results.json` 保留。这证明只比较 heap 自报边界和单个远端 PFA 哨兵，不能排除堆与相邻所有权区域重叠。新共享启动检查在 ready 之前报告 `PFA: invalid linker memory layout` 和内存初始化失败。

该脚本只生成临时 ELF/bin、派生 manifest 和报告，验证原镜像 hash 未变，不修改源码、默认产物或 CMake 配置。负向 guest 仍记录 error（ARM64 panic、RV64 提前退出、x86 halt 后 startup_timeout）；脚本要求准确的布局错误和没有 ready，不能只凭超时判定成功。

最终重建三架构 Debug/Release 并执行各自 CTest，全部 15 个入口通过；报告镜像 hash 已与当前产物逐一核对，默认仍为 6 套件/14 用例。日志 `build/<preset>/layout-build.log`、`layout-ctest.log`；报告路径为 `build/<preset>/validation/<ID>/results.json`：

| 预设 | functional ID | framework ID | benchmark ID |
| --- | --- | --- | --- |
| arm64-debug | 1788698806689573000 | 1788698814284499000 | — |
| riscv64-debug | 1788698826905793000 | 1788698834196268000 | — |
| x64-debug | 1788698847245567000 | 1788698854693428000 | — |
| arm64-release | 1788698999637122000 | 1788699005387653000 | 1788699018976215000 |
| riscv64-release | 1788699024688094000 | 1788699033076453000 | 1788699045819398000 |
| x64-release | 1788699049843855000 | 1788699055676250000 | 1788699068170407000 |

当前 Debug 真实布局（全部地址为物理/恒等映射地址）：

| ISA | heap 起止 | 链接页表区起止 | PFA 元数据起始 / 字节数 |
| --- | --- | --- | --- |
| ARM64 | 0x40440000..0x40c40000 | 0x40c40000..0x40c50000 | 0x40c50000 / 8,339,456 |
| RV64 | 0x80497000..0x80c97000 | 0x80c97000..0x80ca7000 | 0x80ca7000 / 8,339,456 |
| x64 | 0x003ba000..0x00bba000 | 0x00bba000..0x00bca000 | 0x00bca000 / 8,343,552 |

early table pool 位于 BSS，与表中链接预留区分开；动态用户页表由 PFA 拥有，不应假定所有表页都在链接预留区。测试从实际根表递归发现它们并逐页核对，而非仅检查上表三个区间。

26 项同镜像固件检查全部符合预期，目录 `build/<preset>/pfa-firmware/<ID>/`：arm64-debug `1788698867301499000`、riscv64-debug `1788698884809182000`、arm64-release `1788698957764323000`、riscv64-release `1788698974637652000`。其中多 bank、后续 bank 元数据布局也执行了新增校验和检查。

6 项坏布局拒绝检查全部符合预期，报告为 `build/<preset>/<目录>/report/results.json`：arm64-debug `heap-layout-84e7bjam`、riscv64-debug `heap-layout-v8n77vyl`、x64-debug `heap-layout-olg3r29b`、arm64-release `heap-layout-bafzsfis`、riscv64-release `heap-layout-uh7v9kx8`、x64-release `heap-layout-kbb90upr`。宿主 pytest **102 passed in 37.84s**；Ruff、受改 C++ 的 clang-format dry-run 和 `git diff --check` 通过。

默认 RV64 为 Sv48；另以同一镜像执行 `uv run scripts/kernel_validation.py run --manifest build/<preset>/moss-artifacts.json --cpu rv64,sv48=false --workload heap --workload pfa`。Debug 报告 `1788699127367993000`、Release 报告 `1788699133296375000` 的两套件均通过，原始串口明确为 `MMU mode: Sv39 (3-level page table)`，镜像 hash 与对应默认配置一致；覆盖三层与四层页表遍历。

范围限制：覆盖当前启动/单 worker 工作负载下的所有活动页表树、early pool、链接页表区及元数据，不外推到多进程创建/销毁交错或页引用 SMP 生命周期。kernel_end 以下精确保留/回收仍属 MOSS-005，不能因本节边界检查通过而关闭。

### 3.8 发布指针不等于放弃节点所有权（2026-09-06，工作区）

这是 MOSS-006 的第一步，不是并发容器验收完成。生产修改集中在现有 `containers.cppm`：删除自动析构旧指针的 RcuPtr，head/next 复用 AtomicPtr；push_front 发布新 head 时保留仍经 next 可达的旧 head，remove 只有在摘除后才由链表显式安排删除。clear 沿用显式摘除和删除；未增加定时 drain、扩大回调池或添加兼容指针包装。

真实红例命令：`uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --workload containers`。在未改生产模块的 `040d773` 上，仅增加验证入口，插入 A/B/C 后执行已有回收入口，串口记录 `2 reachable values destroyed after insertion`，assertion failed。报告 `build/arm64-debug/validation/1788699891158090000/results.json`；重复运行 `1788699918870233000` 同样失败。只增加 RcuReadLock 后，`1788699939492883000` 仍失败，证明读深度/屏障没有阻止该回收。编译失败后误启动旧 catalog 的 `1788699873602934000` 是基础设施错误，不计作缺陷复现。

改用 AtomicPtr 后原用例通过，报告 `1788700317254909000`。新增的 `containers` 套件包含三项真实用例：

- `ownership`：A/B/C 插入后仍全部可达且无析构，销毁恰好析构三个对象。
- `release_reuse`：1,024 次插入不析构可达值；分别删除尾/中/头节点并检查其余数据；clear 超过原 512 个回调槽，再插入/销毁仍正确。
- `map_ownership`：强制所有 key 落入一个 bucket，使用真实 unique_ptr 拥有对象；插入、同 key 替换、重复 remove 和清空按对象计数恰好析构。

每项验证实际 heap allocated_bytes 恢复。扩展检查初次暴露全局计数基线差异：`1788700641829247000` 开始已有 4 个待处理回调，heap 从 105,744 降至 105,360 字节，不能将这 384 字节算作本用例资源。将测试开始前已摘除节点的回收放在计数基线之前后，三项通过（`1788700698067821000`）。这只在单 worker 测试环境清理旧状态，不是给生产代码增加回收循环；临时诊断已删除。

最终重建三架构 Debug/Release；六配置的 functional（各 7 套件/17 用例）及 framework 全部通过，报告已 finalized，镜像 hash 均与当前产物核对一致。全部 CTest 入口为 **14/15 通过**，不是全绿：x64 Release 的 benchmark 因一次时钟校准拒绝失败，详情见下。日志 `build/<preset>/containers-{build,ctest}.log`，原始报告 `build/<preset>/validation/<ID>/results.json`：

| 预设 | functional ID | framework ID | benchmark ID / 结果 |
| --- | --- | --- | --- |
| arm64-debug | 1788700799828016000 | 1788700808754441000 | — |
| riscv64-debug | 1788700834423828000 | 1788700843243004000 | — |
| x64-debug | 1788700867108318000 | 1788700875456561000 | — |
| arm64-release | 1788700916125328000 | 1788700921062133000 | 1788700932786773000 / passed |
| riscv64-release | 1788700935553499000 | 1788700942985985000 | 1788700954978055000 / passed |
| x64-release | 1788700958018677000 | 1788700964124686000 | 1788700976895209000 / error |

宿主 `uv run pytest -q scripts/tests`：**102 passed in 41.47s**；Ruff、clang-format dry-run 及 `git diff --check` 通过。本轮没有重跑上一节的 26 项固件/6 项坏布局输入矩阵，不将旧镜像 hash 当作新镜像验收。

x64 Release 的 bench.allocate 在准备阶段发出 `clock frequency=0, source=invalid` 和 `reason=invalid_clock`，其余四个函数基准通过。沿调用路径确认拒绝来自测试镜像 `discover_clock()`，该 x86 路径自行读取 CPUID 或用 PIT 取样，不读取启动期 HAL 的 frequency 值；不能把这个 0 直接断言为运行时定时器频率为 0，也未证明由本次容器修改导致。该函数有样本区间、三样本离散度、计数器单调性/频率范围等拒绝分支，当前失败记录未保留具体分支和原始校准样本，根因仍待定位（028/029/032）。

保持相同 x64 Release 镜像 SHA-256 `ede1df1622754bd56e2de29294eabbc2e02562a8a57d836f37d9e962aab23517`，串行运行上述 runner 加 `--workload bench.allocate` 共 10 次，均通过；这不改写原失败，也不视作已修复。报告 ID：1788701026818659000、1788701028048282000、1788701029450754000、1788701030695773000、1788701031932169000、1788701033090776000、1788701034119988000、1788701035108861000、1788701036073295000、1788701037006896000。后续应先让拒绝保留准确诊断，再用同镜像重复或受控计时扰动复现，不能增加重试到成功或放松有效性阈值。

尚未关闭的真实契约：回调队列没有宽限期，池满会主动执行；RcuReadLock 不固定 CPU/禁止抢占；find/find_if 可返回已退出借用期的裸指针；list 的多写者 remove 仍无完整串行化。下一步必须迁移拥有型锁容器与 scoped borrowing，并覆盖 AddressSpace/VMA、Process/线程/子进程表、WaitQueue、IRQ 描述符、DeviceManager 和两类 IPC 管理器。已有读者持有对象及确定性并发增删查仍需红绿对照，不能以本节单 worker 结果关闭 MOSS-006/A4。

### 3.9 拥有型锁容器、真实双 CPU 交错与从核 MMU（2026-09-06，工作区）

接续 `4cde9b3`，生产容器改为复用现有 IrqSpinLock 的 LockedList/LockedHashMap；删除 RcuReadLock、回调队列/池和 RCU 容器，不保留同名兼容层。节点由容器拥有，查找返回 Optional 值副本；共享对象使用现有 shared_ptr 保持查找后的生命周期。发布、查找、摘除在锁内，节点析构在锁外；scoped for_each/update_if 不允许阻塞、重入或外逸引用，可能重入的调用者使用锁外 for_each_snapshot。快照为 O(n) 临时复制，不引入新的回收框架。

已迁移 VMA/线程/子进程列表、ProcessManager、WaitQueue、IRQ 表、DeviceManager 和两个 IPC 管理器。VMA 重叠检查与插入合并为 push_front_unless；栈增长使用 update_if，brk 后续在 3.24 改为 update_if_unless，使目标选择、冲突检查、驻留页撤销和端点更新共享一次加锁。Process、IRQ 描述符、IPC 对象及嵌套进程列表使用拥有型查找，get_or_insert 保留并发创建的唯一赢家，extract 认领一次摘除。退出进程的引用转交 per-CPU scheduler 状态，返回活跃 bootstrap 栈后再释放，避免释放仍在执行的内核栈；成功 exec 放弃旧 syscall 栈前释放局部引用。

持有读者红例：`uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --workload containers`。在旧生产容器中持有 RcuReadLock，find 后删除并处理回调，仍出现 `1 values destroyed before reader release`；报告 `1788701992102814000`、重复 `1788702034245676000`。仅把裸借用改成 shared_ptr 值副本的探针 `1788702074071464000` 通过，证明旧 read guard 不保留对象；探针已移除，新 API 直接返回拥有者副本，原 held_reader 用例通过（首次锁容器报告 `1788702982808043000`）。上述 ID 均位于 `build/arm64-debug/validation/<ID>/results.json`。

当前 `containers` 五项：ownership、release_reuse、map_ownership、held_reader、reentry。覆盖 A/B/C 可达、1,024 次清空/复用、强制单 bucket 替换/删除、持有读者、同容器快照回调/析构重入以及实际 heap 计数恢复。map_ownership 现使用 shared_ptr，旧 3.8 的 unique_ptr/回调 drain 描述只适用于当时实现。

新增 `containers.smp.interleaving` 通过真实 fork 继承 CPU1 affinity，主线程恢复 CPU0 affinity。两个线程从用户态进入验证 syscall，acquire/release 屏障强制“CPU1 持有 → CPU0 删除 → CPU1 释放”，以及两个创建者都越过同 key 初次查找，再竞争发布、删除。只有 CPU0 记录断言，检查真实 CPU ID、同一个赢家、恰好一次删除/析构，waitpid 回收子进程后才删除测试对象。没有替代调度器、host 模拟容器或定时 sleep；缺少对端进度由宿主 case deadline 判失败。默认 CTest 功能集合现为 **8 套件/20 用例**；至少需要 2 CPU，单 CPU 必须显式选择单 worker 套件。

该真实从核路径先暴露 ARM64 启动缺陷：`1788704095049991000`、`1788704118068808000` 在 CPU1 首次用户指令 `0x200000224` 发生 instruction abort，随后访问内核直映地址 `0xffff800048039000` panic，未到容器交错阶段。探针报告 `1788704281432256000` 显示三个从核 `SCTLR.M=0`；原 secondary_cpu_entry 没有配置/开启各 CPU 的 MMU，主核的寄存器状态不会共享。现在在从核发布 online 前复用 hal::mmu::enable_mmu 与 arch::setup_kernel_mmu，安装现有 identity/high-half 页表，并修正汇编注释。只加此修复后 ARM64 `1788704346538492000` 通过；RV64 `1788704363943481000`、x64 `1788704385995799000` 同一交错用例通过。临时 `[DEBUG-moss006-smp]` 日志已删除；不是 QEMU 特判，也没有放宽地址权限或改变固件假设。

最终六配置均构建通过；串行 CTest **14/15**，不是全绿。六配置的 containers、containers.smp 及 framework 全部通过，三配置 Release benchmark 全部通过；RV64 Debug 的 pfa.exhaustion 仍超时，其余功能套件通过。日志为 `build/<preset>/locked-final-ctest.log`，报告为 `build/<preset>/validation/<ID>/results.json`，均 finalized、无遗漏的请求套件，镜像 hash 与对应当前产物一致：

| 预设 | functional ID / 结果 | framework ID | benchmark ID |
| --- | --- | --- | --- |
| arm64-debug | 1788704603964592000 / passed | 1788704614995840000 | — |
| riscv64-debug | 1788704628804793000 / pfa.exhaustion timeout | 1788704646016996000 | — |
| x64-debug | 1788704660990548000 / passed | 1788704673307549000 | — |
| arm64-release | 1788704686353442000 / passed | 1788704692133979000 | 1788704703729661000 |
| riscv64-release | 1788704706069904000 / passed | 1788704711287132000 | 1788704723019396000 |
| x64-release | 1788704725409350000 / passed | 1788704731795402000 | 1788704744118072000 |

宿主 `uv run pytest -q scripts/tests` **104 passed in 85.01s**，含新增的单 CPU 拒绝检查；Ruff、受改 C/C++ clang-format dry-run 和 diff whitespace 检查通过。本轮未重跑 26 项固件/6 项坏布局矩阵，不把此前镜像的结果当作当前镜像的完整硬件验收。

本轮保留的非全绿记录：最初 RV64 Debug `1788703299066126000`、x64 Debug `1788703300483634000` 的 pfa.exhaustion 在默认 5 秒 case deadline 超时（当时并行构建 Release）；相同镜像单独执行 pfa 的 `1788703689411365000`、`1788703710300045000` 通过。但 x64 Debug 后续 `1788704051110135000` 又超时；最终串行矩阵 RV64 `1788704628804793000` 仍超时，因此不能仅归因于并行构建负载，也不能凭重跑通过认定修复。未改超时阈值、未增加自动重试；这些报告与 3.8 的 x86 benchmark 校准拒绝均继续保留（028/029/032）。

**MOSS-006/A4 尚不关闭。** 该轮建立容器节点和查找引用契约，不保证 IRQ 注销后外部 context 已停止使用、Driver 裸指针/设备绑定的生命周期、IPC connect/unregister、map/destroy、cleanup/new-entry 等复合事务。共享内存的既有占位实现仍属 030；Process/Thread 状态与资源转换、VMA/PTE 事务、WaitQueue lost wakeup 和跨管理器锁序仍需相应专项验收。双 CPU 容器测试不替代 017/023 的单一运行者与长循环验收，也不证明真实硬件正确。

### 3.10 PFA 耗尽的宿主执行预算与逐用例观测（2026-09-06，工作区）

本节处理 3.9 遗留的 pfa.exhaustion 超时，不修改生产分配器、内存大小、断言、校验和或退出判定。全部诊断使用相同 RV64 Debug 镜像，SHA-256 为 `b59a48cb6f095b8cb5790bb0e135f7d26fa9e6094a6b69629ce4a822ef8e96a8`。单独运行五次均通过（1788705008101108000、1788705012852382000、1788705017586258000、1788705022268892000、1788705027083954000）；后续带逐用例观测的 `1788705318227165000` 显示 exhaustion 为 3.294 s，整套 guest 为 4.059 s。

提高复现率的方法是同时运行四个独立的 4-vCPU/2-GiB guest，保持每个 guest 的工作量和执行路径不变。命令为四个并行的 `uv run scripts/kernel_validation.py run --manifest build/riscv64-debug/moss-artifacts.json --workload pfa`，不是同一内核内增加测试线程。旧 5 s 预算稳定出现 **4/4 case_timeout**；再仅指定 `--case-timeout 20`，四个相同镜像都完成了逐页模式、保留区、元数据、耗尽/释放和计数校验。该实验说明宿主资源压力下正常的全 RAM 工作会超过 5 s，不支持把这些超时直接称为内核死锁；不能据此断言所有历史停滞均有同一原因。

| 固定四并发实验 | pfa.exhaustion 宿主观测耗时 | 结果 | 报告 ID（`build/riscv64-debug/validation/<ID>/results.json`） |
| --- | --- | --- | --- |
| 原 5 s 预算 | 5.002～5.011 s 后终止 | 4/4 timeout | 1788705363893964000、1788705363911709000、1788705363900111000、1788705363923710000 |
| 显式 20 s 诊断窗口 | 14.369～14.443 s | 4/4 passed | 1788705547579098000、1788705547579164000、1788705547579184000、1788705547579166000 |
| 修复后的默认 PFA 30 s | 10.709～10.802 s | 4/4 passed | 1788705720540865000、1788705720545937000、1788705720552271000、1788705720577501000 |

宿主为 16 GiB RAM；`/usr/bin/time -l` 记录单独运行峰值 RSS 约 2.17 GB、约 56 万次 page reclaim，20 s 并发诊断约 93～95 万次 reclaim、1,069～3,052 次 page fault。不同批次宿主压力会变化，上表 **不是 14 s → 10 s 的内核性能改进**，也不是硬件性能测量。

修复在独立 runner：PFA 默认 case 预算为 30 s，其他 workload 保留 5 s；显式 `--case-timeout` 无条件优先。JSON 的每个 guest 记录实际 `case_timeout_seconds`；执行过的 case 记录从宿主观察 case_start 到 case_end/终止决定的 `elapsed_seconds`，未运行的 case 不伪造耗时，超时耗时不含 SIGTERM/SIGKILL 等待。观测有轮询/调度误差，不冒充内核精确计时。CTest 的 functional/framework 外层总时限改为 600 s，覆盖八个最多 60 s 的 guest 及报告/回收；配置后实际 CTest 属性已核验。CMake 不读取机器配置，编译和生产内核不因此依赖 QEMU。

显式短预算仍能失败：新 runner 加 `--case-timeout 0.1` 的 `1788705954378643000` 返回非零及 case_timeout，结果仍记录为 error，不改写成成功。宿主回归先分别暴露缺少逐 case 耗时/实际预算字段，再由修复通过；最终 **109 项宿主测试通过（35.75 s）**，覆盖预算选择、显式覆盖、超时/未运行耗时区分和子进程回收。没有加自动重试，所有旧失败报告保留；MOSS-028 的其他审计覆盖和 MOSS-029 的历史校准拒绝仍未关闭。

重新 configure/build 六配置后，串行 CTest **15/15 通过**，每个 functional 仍是 8 套件/20 用例。日志 `build/<preset>/deadline-{build,ctest}.log`；以下报告均 finalized、无缺失的请求套件，实际 deadline 为 PFA 30 s/其他 5 s，镜像 hash 与对应当前产物一致：

| 预设 | functional ID | framework ID | benchmark ID |
| --- | --- | --- | --- |
| arm64-debug | 1788706012771337000 | 1788706020779256000 | — |
| riscv64-debug | 1788706033163678000 | 1788706043183702000 | — |
| x64-debug | 1788706056063223000 | 1788706064949004000 | — |
| arm64-release | 1788706077903344000 | 1788706083059769000 | 1788706094791591000 |
| riscv64-release | 1788706097147162000 | 1788706102679866000 | 1788706114682616000 |
| x64-release | 1788706117504457000 | 1788706123770470000 | 1788706136268248000 |

报告路径为 `build/<preset>/validation/<ID>/results.json`。这关闭的是本节可复现的 PFA 执行预算不足，不是并发分配器、所有平台输入、真实硬件或全部审计项的验收；3.8/3.9 的旧失败记录没有被重写。

### 3.11 内核映射默认 supervisor-only（2026-09-06，工作区）

本节关闭 B1 的 U/S 构造与最终页表结构检查，不关闭整个 MOSS-001。x86 的 normal/device 内核块不再设置 USER；`PageTableEntry::set_table()` 默认构造内核专用上级表，只有 `create_user_address_space`、`map_user_page` 和 `clone_user_page_tables` 显式启用用户权限。用户低地址根中共享的内核叶子仍为 supervisor-only。`map_page` 修改内核 PGD，现直接拒绝 USER 属性；没有替代页表、QEMU 特判或向用户态暴露内核指针的验证接口。

真实结构红例：仅加入页表检查、尚未修改生产构造时，运行 `uv run scripts/kernel_validation.py run --manifest build/x64-debug/moss-artifacts.json --workload mm.permissions`，`kernel_mappings` 记录 **15 个失败断言**，后续用例为 not_run，报告 `1788707394008747000`。修复后的同一入口与 users、containers.smp 联合通过（`1788707497164923000`）。两份报告均在 `build/x64-debug/validation/<ID>/results.json`；旧失败不改写。

默认 functional 新增 `mm.permissions`，现为 **9 套件/23 用例**。新增三项复用真实页表和生产用户线程：table_defaults 检查安全默认/显式用户分支与内核 API 拒绝；kernel_mappings 遍历 kernel identity/direct-map；active_user_mappings 从 CR3/TTBR0/satp 读取实际根，与当前进程核对后检查继承的内核叶子和 x86 用户完整 U/S 权限链，ARM64 另核对活动 TTBR1。检查涵盖当前 0-4 GiB identity/direct-map 契约，不是任意物理布局证明。

重建全部六配置，串行 CTest **15/15 通过**；宿主 `uv run pytest -q scripts/tests` **109 passed in 45.72s**，Ruff、受改测试区间 clang-format dry-run 与 `git diff --check` 通过。日志 `build/<preset>/permissions-{final-build,ctest}.log`，所有报告 finalized、没有缺失的请求套件、镜像 hash 与当前产物一致：

| 预设 | functional ID | framework ID | benchmark ID |
| --- | --- | --- | --- |
| arm64-debug | 1788707600770441000 | 1788707615847250000 | — |
| riscv64-debug | 1788707636220283000 | 1788707649586226000 | — |
| x64-debug | 1788707668171058000 | 1788707677878692000 | — |
| arm64-release | 1788707704099898000 | 1788707712156786000 | 1788707724226817000 |
| riscv64-release | 1788707737422859000 | 1788707747453394000 | 1788707760308970000 |
| x64-release | 1788707763721405000 | 1788707777699426000 | 1788707790687286000 |

默认 RV64 为 Sv48。保持各自镜像不变，增加运行参数 `--cpu rv64,sv48=false --workload mm.permissions --workload users --workload containers.smp`，Debug（`1788707833244672000`）和 Release（`1788707836364967000`）的 Sv39 三层页表及合法用户路径也通过。以上报告均位于 `build/<preset>/validation/<ID>/results.json`。

**本阶段保留边界：** 当时未收紧 text/rodata 的 RO、data/MMIO 的 NX 或直映别名 W^X，也未移除共享内核映射对 block/table 形状的依赖；后续修复见 3.12。本阶段没有重跑固件/坏布局矩阵或实机。这不是用户违规访存/异常终止隔离的完整验收，B 阶段整体退出条件、MOSS-001/002 仍未满足；3.8 的历史基准校准拒绝继续保留待定位。

### 3.12 最终内核 W^X 与共享页表归属（2026-09-06，工作区）

- [x] `PageTableManager::create_user_page_tables` 统一构造用户根表，process 层不再复制架构页表。create/clone/free 按内核 VA 范围判断共享归属，不再把 block 形状当所有权；四层模式保留每进程私有的混合低 PUD，Sv39 直接借用内核根项。用户映射入口拒绝内核根、内核 VA、非对齐地址和缺少 USER 的权限。
- [x] 三架构最终 identity/direct-map 在固件 RAM 和链接权限边界按需拆分 1 GiB → 2 MiB → 4 KiB。包含 boot text 的代码范围 RX，rodata 为 RO/NX，其余 RAM/device 为 RW/NX；直映全部 NX，text/rodata 的直映别名也只读。删除旧宽权限 normal/device block helper，复用现有 PTE 构造和 64 页 early pool，不增加板型开关。
- [x] x86 开启 CR0.WP；AP 只在离开低地址 trampoline 前使用现有 PVH 临时页表，进入 C++ 入口后安装最终页表并设置 WP，再发布 online。最终映射不为 trampoline 保留 RWX 例外。
- [x] `mm.permissions` 增加 kernel_wx 和 address_space_ownership：遍历生产页表，检查叶子权限与直映别名；实际创建两个地址空间、映射用户页、clone 后核对引用、销毁并核对 PFA 计数与共享表快照。默认 functional 现为 **9 套件/25 用例**，不是仅测静态权限常量。

W^X 的真实结构红例：只增加检查、尚未收紧生产映射时，ARM64 Debug `1788708649766301000` 的 kernel_wx 有 **26 个失败断言**。这不运行越权用户程序，也没有新增内核指针暴露接口。报告保存在 `build/arm64-debug/validation/<ID>/results.json`。

拆页后 x86 Debug 的 PFA/heap 耗尽又暴露快照误报：`1788709086648885000` 分别有 2/3 个页表 hash 断言失败；探针 `1788709372722382000` 保留失败断言，并证明三次变化忽略硬件 A/D 位后 hash 完全一致。快照现只过滤架构 Accessed/Dirty 状态，仍包含地址、USER、读写、执行和 COW 位；self.cleanup_guards 用未安装的本地表验证这些位的变化仍能被发现。PFA 元数据和链接预留区仍按原字节校验，不改分配器、不重试、不扩大预算。删除临时探针后 `1788709589819331000` 的 permissions/pfa/heap/self 全部通过。以上报告在 `build/x64-debug/validation/<ID>/results.json`，原失败保留。

最终重建六配置，串行 CTest **15/15 通过**；每个 functional 为 9/25，framework 为 4/8，Release benchmark 为 5/5。日志 `build/<preset>/wx-{final-build,ctest}.log`，下列报告全部 finalized、not_run 为空，镜像 SHA-256 与本轮最终产物一致：

| 预设 | functional ID | framework ID | benchmark ID |
| --- | --- | --- | --- |
| arm64-debug | 1788709701428902000 | 1788709710394146000 | — |
| riscv64-debug | 1788709737323140000 | 1788709753128364000 | — |
| x64-debug | 1788709771770096000 | 1788709786682531000 | — |
| arm64-release | 1788709819877226000 | 1788709828168390000 | 1788709840645049000 |
| riscv64-release | 1788709861533332000 | 1788709874086716000 | 1788709890014452000 |
| x64-release | 1788709916783675000 | 1788709926529837000 | 1788709939838580000 |

所有报告位于 `build/<preset>/validation/<ID>/results.json`。宿主 `uv run pytest -q scripts/tests` 为 **109 passed in 44.53s**（`build/wx-host-tests.log`）；Ruff 和 `git diff --check` 通过。

保持各自最终镜像不变，RV64 增加 `--cpu rv64,sv48=false --workload mm.permissions --workload users --workload containers.smp`，Debug `1788710167407561000`、Release `1788710171026805000` 均通过；串口确认为 Sv39，报告 hash 与各自默认 Sv48 镜像一致。

`uv run scripts/check_pfa_firmware.py --manifest build/<preset>/moss-artifacts.json` 重跑 ARM64/RV64 Debug/Release 共 **26 项**：ARM64 各 4 项，RV64 各 9 项。非对齐保留区、多 bank/洞、乱序八段、后续 bank 元数据和相邻段实际耗尽通过；保留区极值/回绕、容量溢出则匹配指定启动错误且不发布 ready。所有报告 hash 与各自最终镜像一致。`check_heap_layout.py` 六配置临时重链接坏 heap_end，全部在 ready 前报告 `PFA: invalid linker memory layout` 与内存初始化失败；原始产物未修改。负向报告中的 panic/missing_completion/startup_timeout 是 runner 的终止分类，脚本必须另核对上述具体错误，不能仅凭超时视为通过。

| 预设 | pfa-firmware 目录 ID | 坏布局目录 |
| --- | --- | --- |
| arm64-debug | 1788710173614904000 | heap-layout-8spmxm64 |
| riscv64-debug | 1788710189217031000 | heap-layout-gt2y8f5f |
| x64-debug | — | heap-layout-99yek664 |
| arm64-release | 1788710214706234000 | heap-layout-cf9i8nzi |
| riscv64-release | 1788710229165539000 | heap-layout-qlkx3emn |
| x64-release | — | heap-layout-ltfvqss2 |

固件原始报告：`build/<preset>/pfa-firmware/<ID>/<case>/results.json`；坏布局报告：`build/<preset>/<坏布局目录>/report/results.json`。汇总日志为 `build/<preset>/wx-{firmware,heap-layout}.log`。

**保留边界：** 当前只覆盖 0–4 GiB identity/direct-map、64 页 early pool 与已声明固件布局；不证明任意 SoC、真实硬件或 RelWithDebInfo。用户违规访问的受控异常/终止隔离、uaccess、信号返回、用户 COW 权限和 clone OOM 回滚均未因此完成；MOSS-001 整项及 B 阶段退出条件保持打开。3.8 的历史校准失败不因本轮 benchmark 通过而关闭。

### 3.13 用户地址域与跨 VMA 访问策略（2026-09-07，工作区）

- [x] 删除 `system_call_handler` 分发前的原始 puts；syscall 0 只经过现有有界字符串处理函数，不再输出两次或先访问后校验。未增加兼容旁路。
- [x] `PageTableManager::is_user_range` 用减法检查完整区间，统一排除低 4 GiB 内核 identity 区和 active USER_MAX 以外的非规范/内核地址；页表入口、VMA 查询/准入和用户复制检查复用该接口，RV64 上界随 Sv39/Sv48 确定。
- [x] `AddressSpace::valid_vma_range/add_vma` 拒绝倒置、非页对齐、越界、未知权限及普通空区间；3.24 后仅固定 `HEAP_START` 的唯一 HEAP 可为空。sigreturn 页有独立 VmaType，只有精确范围的只读可执行 stub VMA 可覆盖。mmap/munmap 限制完整范围；mmap 明确只支持 ANONYMOUS|PRIVATE、fd=-1、offset=0，拒绝 FIXED/未知 flags/prot，合法冲突 hint 可尝试单调 cursor 而不替换原映射。堆增长辅助与 brk/栈增长补地址域检查；当时尚未完成的 VM 事务后续见 3.24。
- [x] `AddressSpace::allows_user_access` 检查完整跨 VMA 范围，缺口或任一权限不符则失败；字符串逐字节复用现有 copy-in 策略，未在上限内遇到 NUL 返回 ENAMETOOLONG，不再静默截断 pathname。无新 VM 管理器或测试专用复制实现。

真实结构红例：仅加入 `mm.permissions.vma_boundaries`、尚未修改生产准入时，ARM64 Debug 报告 `1788710845613280000` 有 **9 个失败断言**，对应 9 类非法 VMA 均被接受。用例只登记/删除元数据，不访问非法地址。修复后增加相邻 VMA、跨段只读/可写、缺口、未知 flags 和保留 stub 检查，仍通过生产地址空间创建/回收路径。

`users.user_ranges` 使用三架构真实 syscall 入口：拒绝坏 debug 指针、内核/越界/reserved mmap、FIXED/未知权限与只读/PROT_NONE 输出；实际创建相邻匿名页，跨 VMA 执行 affinity copy-in、时钟 copy-out 和字符串 open，移除第二 VMA 后要求 EFAULT，并核对冲突 hint 不覆盖原映射、最后 munmap 成功。默认 functional 现为 **9 套件/27 用例**，其中 permissions 6 项、users 3 项；不是 27 组完整安全验收。

首轮 users 报告 `1788711165303037000` 失败掩码为 `0x400004`。核对生产 ABI 后确认测试假设有误：Moss clock_gettime 输出单个 u64 纳秒值，原 offset=4088 并未跨页；只把 offset 改为 4092 后，`1788711273193033000` 仅剩 `0x4`，证明移除 VMA 的拒绝已通过。最后按当前 read/write 对零长度返回 EINVAL 的约定修正期望，没有改 ABI 或删除该检查。ARM64 Debug `1788711360810565000` 的 permissions/users/containers.smp 全部通过。以上报告均在 `build/arm64-debug/validation/<ID>/results.json`，失败记录保留。

该阶段宿主 `uv run pytest -q scripts/tests` **109 passed in 35.87s**（`build/user-range-host-tests.log`）；Ruff 与 `git diff --check` 通过。首轮六配置 CTest **13/15**：ARM64/RV64 通过，x86 Debug/Release 的 users 在编译器生成的 `movaps` 处 #UD，报告分别为 `1788711542287028000` / `1788711651067986000`。这是继续执行用例后暴露的 CPU 状态缺口，修复及后续验证见 3.14；不以早期 ARM64 单架构绿例冒充完整矩阵。

**保留边界：** MOSS-002 整项仍打开。当前复制和 VFS 缓冲区访问仍是普通 load/store，没有架构异常 fixup、页固定或完整 VM 并发事务；合法 VMA 内驻留/权限故障及 OOM 仍可能终止进程或 panic，不能宣称 fault-safe uaccess。fork 元数据复制与 rollback 仍属于 010/014～016；brk 空堆、缩堆和增长重叠在本节当时尚未闭合，后续修复与验收见 3.24。信号复制/返回净化、错误进程故障隔离、真实硬件与 RelWithDebInfo 未验收。

### 3.14 x86 基础 FP 状态、入口栈和 userspace 段布局（2026-09-07，工作区）

- [x] BSP/AP 按 CPU 能力启用 x87/FXSR/SSE2，清 EM/TS，配置 OSFXSR/OSXMMEXCPT；不启用尚无保存协议的 AVX/XSAVE。复用 CpuContext 内嵌的 16 字节对齐 FXSAVE64 状态，每次切换保存/恢复；fork 复制调用者的实时 FP 状态，exec 重置默认状态，没有 lazy-owner 管理器或 QEMU 特判。
- [x] AP 汇编通过 C 调用进入 `x86_secondary_entry`。新 ELF 按 Moss 现有 `_start(argc, argv)` C ABI 准备 x86 栈（RSP mod 16 = 8），不改 fork 恢复 SP 或内嵌汇编 init 的入口约定。
- [x] 公共 userspace 链接脚本收拢 `.ltext/.lrodata/.ldata/.lbss`、small-data 和 GOT，并按页分离 RX/R/RW。自有 ELF 不再因代码/常量/数据共页触发加载器权限 OR；这不修复任意外来 ELF 的重叠/加载事务（015/016 仍打开）。
- [x] x86 users 检查 x87 数据/控制字、MXCSR、XMM15 经 yield/fork 的继承、子修改不污染父进程、exec 默认状态与 argc/argv；SMP 检查真实 CPU1 子进程的 FP 继承。用户 #MF 走已有进程终止路径，父进程 wait 后继续且 FP 状态不变。
- [ ] SIMD #XM 隔离的真实验收、全扩展状态/信号帧、ARM64/RV64 扩展状态与硬件覆盖；不因 legacy x86 子检查通过关闭 001/007/014。

逐步保留的 x86 Debug 证据（`build/x64-debug/validation/<ID>/results.json`）：

| 报告 ID | 实测结果与处理 |
| --- | --- |
| 1788712115790861000 | FP 初始化加入后启动提前退出；CPU trace 在 AP `initialize_fpu` 的 `fxrstor64` 处 #GP，缓冲区地址仅 8 字节对齐，继而三重故障。修 AP 跳转的 C 栈约定，不取消 FP 指令。 |
| 1788712540532469000 | AP 修正后 SMP/permissions 通过，users 的 `movaps` 写栈 #GP；修 exec 新 C 入口栈。 |
| 1788712615521632000 | users 能继续，掩码 `0x20`：只读 literal 所在页实际被合并为 RWX；readelf 验证大模型段共页，修公共链接布局。 |
| 1788712702927877000 | users/containers.smp/mm.permissions 全部通过，readelf 显示 RX/R/RW 页分离。 |
| 1788712770776836000、1788712870013673000 | 新增浮点异常验收失败；细化阶段掩码为 `0x8`，仅 SIMD 异常项失败，不能记为完整通过。 |
| 1788713116723580000 | 默认 users/containers.smp/mm.permissions 通过，含 FP 状态、x87 异常和 argv 检查。 |
| 1788713120635633000 | 显式 `users.simd_fault` 保留为 failed/assertion，不转换为 expected pass。 |

`build/x64-debug/fpu-fault-cpu.log` 的 CPU 记录只有受控 #MF，没有 #XM；本机 QEMU 11.1.1 TCG 在未屏蔽的 `divss 0/0` 后继续执行。该观察与 [QEMU 上游对状态位和陷阱支持的区分](https://github.com/qemu/qemu/commit/418b0f93d12a1589d5031405de857844f32e9ccc) 一致，但不是实机内核 #XM 已通过的证据。完整断言留在非默认 `users.simd_fault` 专项；kernel 不检测 emulator，不合成软件异常，不禁用 SIMD。

提交前单独执行全部 9 个 preset 的 configure/build，再逐一执行 `cmake --build --preset <preset> --clean-first`，ARM64/x64/RV64 的 Debug、Release、RelWithDebInfo 全部编译通过，未复现新的编译错误。日志为 `build/<preset>/commit-{configure,build,clean-build}.log`。这是编译验证，RelWithDebInfo 运行时尚未由本轮独立验收。

宿主测试 **111 passed in 44.95s**（`build/fpu-host-tests.log`），Ruff、修改行格式检查及 `git diff --check` 通过。六配置 Debug/Release 的两轮 CTest 均为 **14/15**，不能报告整体验收通过：

- 首轮 `build/<preset>/fpu-matrix-ctest.log`：ARM64 Release benchmark 入口失败，报告 `1788713325136420000` 的 `bench.read` 在执行基准前因 CPU3 未上线而 panic，其余测试通过。无并发构建后同镜像连续 20 次 `resources` 启动通过（`fpu-smp-boot-probe.log`），不是启动故障已修复的证据。
- 无并发构建的隔离复测 `build/<preset>/fpu-isolated-ctest.log`：ARM64 Debug functional 失败，报告 `1788713649870304000` 的 `containers.smp` 完成容器交错、子进程记录 Zombie/exit 37 后发生 `case_timeout`；其余 14 个 CTest 入口通过。保留 5 秒用例期限，不通过加时或重试把该失败变绿。启动协调和 wait/调度竞争的根因尚未确认，MOSS-017/018 仍打开。

同镜像附加 `mm.permissions/users/containers.smp` 全部通过：RV64 Sv39 Debug/Release 报告 `1788713808806661000` / `1788713827343812000`；x86 `pc`、16 CPU、1 GiB Debug/Release 报告 `1788713819212658000` / `1788713835773807000`。默认与附加配置使用各自相同构建产物；不外推到实机。

**保留边界：** 默认集合仍为 9 套件/27 用例；额外 SIMD 专项单独计数且当前失败。MOSS-002 的 fault-safe copy、014 的全部状态继承、015/016 的 exec/ELF 事务、上述 SMP 间歇失败及真实硬件验收未完成。

### 3.15 ARM64 启动事件发布与停机现场（2026-09-10，工作区）

- [x] `activate_secondary_cpus` 在发布 `Active` 后、`SEV` 前执行 `DSB SY`，不再把 release store 当作事件发送前的完成屏障。ARM64 Debug 构建和修改行格式检查通过，实际指令为 `mark_cpu_active → dsb sy → sev`。
- [ ] 从核启动偶发挂起的完整根因与回归验收；以下现场定位不能独自证明补屏障已解决 QEMU 上的全部挂起。
- [ ] `containers.smp` 子进程 Zombie 后的 wait/调度超时仍单独跟踪，MOSS-017/018 不关闭。

未修改 `a5ff24f` 内核时，ARM64 Debug `containers.smp` 连续运行前 24 次通过，第 25 次在测试开始前发生 `unexpected_kernel_panic`；报告 `build/arm64-debug/validation/1789049114268692000/results.json` 的 ready 为空、用例为 not_run。不能把它归因于 waitpid，因为用户测试还没有运行。

缩小到默认四核、2 GiB、GICv2 的 `resources` 启动后，60 次标准 runner 检查通过；加 GDB socket 的首批 100 次启动完成，下一批在第 484 次抓到同样的启动失败。调试脚本只以协议 end 作为正常停止点，其 completed 数不是完整功能验收。原始串口与停机现场保留在 `build/arm64-debug/smp-gdb-anyhz67j/484.{serial,gdb}.log`：

```text
CPU0: kernel_panic("Kernel initialization failed", Timeout)
CPU1/CPU2: cpu_idle_once / cpu_startup_entry
CPU3: secondary_cpu_entry, PC=0x4020b438, halted at activation WFE
g_cpu_topology: online_cpus=3, cpu_states={Online,Online,Online,Active,...}
early_uart_lock=0
```

因此已排除本次失败卡在 UART 锁或从核 MMU/GIC 初始化内部：CPU3 尚未越过激活等待。原符号 ELF SHA-256 为 `36fc35d481cdf1cc6fdd82f1f7ac1f32b59e71b555485e30e9fc64cc1e65c21c`。`DSB` 补齐的是独立可确认的架构契约：release 对内存访问排序，不保证非访存 `SEV` 等到写入完成；依据 [Armv8-A Memory Systems 第 16 页](https://documentation-service.arm.com/static/62d6777531ea212bb6627683?token=)。这不是 QEMU 检测或机器特判，也没有加超时、重试或替代调度器。

**反证保留：** 只补 `DSB SY` 后，相同调试循环的前 646 次完成，第 647 次仍失败；`build/arm64-debug/smp-gdb-df2j_8kb/647.{serial,gdb}.log` 显示同一 CPU3 PC、Active 状态和空闲 UART 锁。因此“仅缺少屏障即可解释本机偶发启动挂起”的假设已被反例否定；屏障修正不能记为该故障修复。此候选 validation image SHA-256 为 `fe94a26bc87807c9752bb58bd67f71034dd5debb1d2280d76618d10e06f738da`，符号 ELF 为 `078942a5f8f1d4c85fcd61ca8cefde3cf02ab2c822ad22b82e4d622fe8c33753`。没有把前 646 次完成或一次普通回归通过作为启动可靠性的证明。

**独立模拟器诊断，不计入内核功能覆盖：** 临时 `build/arm64-debug/event-probe.{S,py}` 不加载 Moss，仅用 CPU0/CPU3、acquire/release 序号、发送侧 `DSB SY; SEV` 和等待侧 `DSB SY; WFE` 握手，无 MMU、定时中断、分配器、调度器或 UART 锁。相同 428 字节镜像 SHA-256 `efa58102c5abab1fc037a01b515cfadcea74c522426e815a00ac6936e875c2df`，连续三次多线程 TCG 分别在 request/ack 为 324/323、42/41、71/70 时超时；单线程 TCG 三次均完成 100,000 次握手。日志 `event-probe-{multi,single}-{1,2,3}.log`；这不是把失败重试成成功，而是只改变执行模式的独立对照。

LLDB 检查本机 QEMU 11.1.1 进程时，CPU3 为 `halted=1, halt_reason=HALT_WFE(3), event_register=1`，对应宿主线程仍在 `qemu_process_cpu_events → qemu_cond_wait_impl → pthread_cond_wait`。不修改 guest 状态，仅调试调用一次该 CPU 的 `qemu_cpu_kick` 后，CPU3 消费了待处理 request，ack 追上 request；见 `event-probe-host-debug.log`、`event-probe-host-kick{-2,}.log`。宿主字段偏移按本机二进制反汇编核对，不假定任意 QEMU 版本 ABI 相同。该独立复现确认本机模拟器存在事件已发布但宿主等待线程未被唤醒的问题；Moss 的 WFE 现场与之相符，但还没有用修正后的模拟器完成原始长循环对照，不能关闭启动验收。

本机反汇编确认 `helper_sev` 设置事件并广播，但没有与宿主 idle 检查/条件等待使用同一锁；这也与上游 [SEV helper](https://github.com/qemu/qemu/blob/master/target/arm/tcg/op_helper.c) 和 [CPU 事件等待](https://github.com/qemu/qemu/blob/master/system/cpus.c) 的结构一致。未改动安装的 QEMU，未把正式 runner 改成单线程，未在内核加入 emulator 分支、重复 SEV 或轮询绕过；独立诊断保留在明确的 build 调试目录，不进入正常构建与 CTest。

**本轮普通回归：** ARM64 Debug/Release/RelWithDebInfo 三配置构建通过（`build/<preset>/smp-event-build.log`）；Debug/Release CTest **5/5 通过**（`smp-event-ctest.log`）。原始报告均 finalized、not_run 为空，hash 与当前产物一致：

| preset | functional | framework | benchmark |
| --- | --- | --- | --- |
| arm64-debug | 1789050194173601000 | 1789050204916237000 | 不适用 |
| arm64-release | 1789050217496590000 | 1789050223438748000 | 1789050235251722000 |

报告路径为 `build/<preset>/validation/<ID>/results.json`。Release image SHA-256 为 `41bc35a71e26f15f4d26611cbb8dd45cd7e139b8587fbc525c12e0119479f5aa`；Debug 同上述反例镜像，说明普通 CTest 通过并不能消除偶发失败。修改行 clang-format 与 `git diff --check` 通过；本轮没有改动 x86/RV64，也没有重跑它们的完整矩阵或实机验证。下一步内核等待/调度修复仍按 MOSS-017/018 的真实生产路径执行，不能把模拟器问题当作所有 wait 超时的解释。

### 3.16 COW 权限与缺页分类（2026-09-10，部分修复）

`clone_user_page_tables` 现在只给原本可写的用户叶子新增 COW，保留真正只读页及多代 fork 已有的 COW 标志。当前 mmap 仅支持私有映射。`try_cow_fault` 重新检查用户地址域、可写 VMA、用户只读 COW PTE 和非零页引用；替换物理页通过 `PageTableEntry::set_page` 编码，避免把 RISC-V 64 PPN 当作直接物理地址字段。替换并失效本地翻译后才释放旧引用。

三个 ISA 的故障入口区分读、写、执行访问；demand 路径拒绝已有有效叶子，并在分配前检查对应 VMA 权限。RISC-V 64 先处理合法 COW 写故障，避免把驻留页重新填充；x86 保留位错误不进入 demand/COW。映射失败时释放尚未交给页表的新数据页，不代表中间表页已经具有完整回滚。

**先失败再修复的证据：** ARM64 Debug 报告 `1789050943430875000` 的新 `cow_clone_permissions` 用例为 17 个断言通过、6 个失败，后续用例未运行。只修 clone 后，ARM64 报告 `1789051244969892000` 的页表检查通过，但 `users.vm.access_permissions` 仍以 `mask=0x2` 失败（PROT_NONE 读取被允许）；RISC-V 64 报告 `1789051245994490000` 的 `users.vm.private_cow` 以 `mask=0x1` 失败，后两项未运行。这些原始失败报告保留，不用成功重跑覆盖。

补齐 fault 修复后，ARM64/RISC-V 64 Debug 的 `mm.permissions` 与 `users.vm` 均通过，报告分别为 `1789051673139223000`、`1789051674378797000`。新增测试使用真实页表/PFA 和真实用户 fork、mmap、访问异常、wait、munmap：检查三代页权限、引用与释放计数，多页数据隔离，最后引用的 COW 写入，驻留 text/rodata 写入拒绝，以及 PROT_NONE 读写和 NX 执行拒绝。`users.vm` 已加入默认 functional 集合；默认共 10 个 suite、31 个 case。

**构建和普通回归：** `cmake --workflow --preset <preset>` 的九配置全部完成 configure/build/test，CTest 合计 **21/21**；格式收尾后的最终产物再次完成九配置 workflow，仍为 **21/21**。两轮日志分别为 `build/<preset>/compile-fix-workflow.log` 和 `compile-fix-final-workflow.log`。宿主 `uv run pytest -q` **111/111**，Ruff、clang-format 与 diff 格式检查通过。本次未复现新的 C/C++ 编译错误，收尾修正了新增测试的格式及 Python 超长行。下表为最终报告，均 finalized、not_run 为空，且报告中的 image SHA-256 与对应 manifest 指向的当前测试镜像逐项一致。

| preset | functional | framework | benchmark |
| --- | --- | --- | --- |
| arm64-debug | 1789051963930521000 | 1789051973639017000 | 不适用 |
| arm64-release | 1789051990335243000 | 1789051996832263000 | 1789052008456881000 |
| arm64-relwithdebinfo | 1789052014689405000 | 1789052020290759000 | 不适用 |
| riscv64-debug | 1789051965173596000 | 1789051976560858000 | 不适用 |
| riscv64-release | 1789051994096733000 | 1789052000906683000 | 1789052012893911000 |
| riscv64-relwithdebinfo | 1789052019698245000 | 1789052026522714000 | 不适用 |
| x64-debug | 1789051966308835000 | 1789051977866715000 | 不适用 |
| x64-release | 1789051995767787000 | 1789052003693917000 | 1789052016156046000 |
| x64-relwithdebinfo | 1789052024576747000 | 1789052033219927000 | 不适用 |

报告路径为 `build/<preset>/validation/<ID>/results.json`。另以同一 RISC-V 64 Debug 镜像运行 `--cpu rv64,sv48=false --workload mm.permissions --workload users.vm`，Sv39 专项通过，最终报告 `1789051963203443000`；镜像 SHA-256 为 `a686d3b5708e1b15a9672d91eab657ff2dba87e215caac516da0a68b2156b580`。

**仍不关闭 MOSS-008/009：** 尚无统一 VMA/PTE/ref/TLB 事务锁、同时写故障与 fork/unmap 交错验收、OOM 逐点注入及完整回滚。`map_user_page` 的有效叶子覆盖与 block/table 冲突仍归 MOSS-010；本次 demand 的驻留检查不是并发防覆盖协议。用户异常测试证明访问被拒绝，但未直接计量拒绝路径没有额外分配，也未单独强制构造 VMA 不可写而 PTE 遗留 COW 的故障。fork 前的独立 text/rodata 写入、多物理地址 PTE 编解码专项及实机验证仍待补。普通矩阵通过不取消 3.15 的偶发启动失败，也不替代显式 `users.simd_fault` 或完整 SMP 验收。

### 3.17 页表 map/clone 分配失败事务（2026-09-10，工作区）

本轮继续 MOSS-010，保留页表模块的职责：调用者不管理分配日志或手工回滚。新增内部 `TableReserve`，用尚未发布的表页自身保存分配链，不增加 heap 依赖或固定地址空间容量上限。

- `map_user_page` 拒绝覆盖有效叶子，遇到中间 block 返回 `NotSupported`。缺失路径的所有表页先分配并初始化，成功后才发布第一个连接；失败释放暂存表页，原树与调用者的数据页引用不变。
- `clone_user_page_tables` 改为显式 `VoidResult`。先检查目标无用户叶子、源描述符受支持，并分配所需表页；这些步骤不改父子 PTE 或数据页引用。之后进入不再分配的提交阶段，保留只读/COW 语义及借用的内核分支。`sys_fork` 检查结果，页表 OOM 返回 ENOMEM，不继续绑定或调度部分克隆。
- PTE 保持 8 字节大小并改为自然对齐，新描述符通过 release 原子存储发布；ARM64 Debug 反汇编确认 `publish_entry` 使用 `stlr x8, [x9]`。这是发布顺序的局部修正，不是完整的并发页表/TLB 协议。通用内存顺序动机参考 [Linux memory barriers](https://docs.kernel.org/core-api/wrappers/memory-barriers.html)。

**反例保留：** 同一命令 `uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --workload mm.transactions` 逐步验证真实实现：

| 报告 ID | 当时改动 | 结果 |
| --- | --- | --- |
| 1789052486817825000 | 仅添加已有映射保护用例 | map 覆盖已有叶子，断言失败 |
| 1789052641489739000 | 只补 map 占用与 block 检查 | 已有映射用例通过 |
| 1789052743297182000 | 添加真实 PFA 耗尽用例 | map 分配失败仍留下中间表，4 个断言失败 |
| 1789052880007688000 | map 离线准备后发布 | map 事务、权限及 users.vm 通过 |
| 1789052954564607000 | 添加 clone 逐点耗尽用例 | clone 改动父 PTE/引用/子树后失败，44 个断言失败 |
| 1789053200083831000 | clone 先准备后提交并返回结果 | 事务、权限、users.vm 与 users 通过 |

`mm.transactions` 的五项默认回归覆盖已有叶子/非空目标/源目标别名拒绝、各级 block 拒绝、map 每次表页分配处的 OOM、跨多个表级/分支的 clone 每次表页分配处 OOM，以及释放后再次成功。`PagePressure` 持有真实 PFA 页块，再逐个放回预算页；没有 allocator mock、返回假失败的 hook 或替代 fork。clone 用例检查失败前后原父页权限/物理内容/引用、目标根与混合低地址 PUD，以及空闲页计数；恰好足够的预算能成功，最终释放恢复基线。测试里的 block 描述符只放入自有、未激活的真实页表中，用于验证软件 walker 拒绝，不代表支持用户大页。

**表级修复首轮矩阵：** 三架构 Debug/Release/RelWithDebInfo 九配置 configure/build 全部通过；默认 functional 为 **11 个 suite、36 个 case**，九配置全部通过。CTest 合计 **20/21**，不是全绿；宿主 pytest **111/111**，Ruff、clang-format 与 diff 检查通过。日志 `build/<preset>/vm-transaction-workflow.log`，报告 `build/<preset>/validation/<ID>/results.json`：

| preset | functional | framework | benchmark |
| --- | --- | --- | --- |
| arm64-debug | 1789053472180227000 | 1789053488576058000 | 不适用 |
| arm64-release | 1789053520114647000 | **1789053530620345000，启动失败** | 1789053572642324000 |
| arm64-relwithdebinfo | 1789053653778603000 | 1789053659725116000 | 不适用 |
| riscv64-debug | 1789053482209345000 | 1789053502105309000 | 不适用 |
| riscv64-release | 1789053535175189000 | 1789053543011577000 | 1789053554967769000 |
| riscv64-relwithdebinfo | 1789053576105731000 | 1789053583289946000 | 不适用 |
| x64-debug | 1789053483246643000 | 1789053501791029000 | 不适用 |
| x64-release | 1789053534781109000 | 1789053543786539000 | 1789053556268329000 |
| x64-relwithdebinfo | 1789053577773257000 | 1789053585959764000 | 不适用 |

首轮 21 份报告均 finalized，报告层 not_run 为空，image SHA-256 已与该轮对应产物逐项核对。失败报告的 `self` guest 尚未 ready，四个用例均 not_run：串口显示 CPU1/2 进入调度循环，CPU3 未打印激活后的标记，8 秒后 `SMP startup timed out: 3 of 4 CPUs online`，随后初始化 panic；其余三个 framework guest 达到各自预期结果。本次没有抓 CPU PC，不能仅凭症状将根因归于 3.15 的 QEMU 问题，也没有覆盖该失败报告。

首轮同一 RISC-V 64 Debug 镜像还通过 `--cpu rv64,sv48=false --workload mm.transactions --workload mm.permissions --workload users.vm`，报告 `1789053613334273000`，SHA-256 `420e49589a30d3b3f5f6586fc033b4648a45889879cf1c36a5cf5ca0b56b8db1`。这验证了 Sv39 与默认 Sv48 的同镜像路径，不是另一个板级构建。

**调用方收尾与最终矩阵：** 继续核对调用者后，补齐 `kernel-main.cppm::create_init_process` 中 x86 原始程序路径 map 失败时的数据页释放，并保留具体错误码；该原始启动分支没有定点 OOM 运行证据，不能算作表级测试已经覆盖。此修改后重建并运行全部九配置，构建全部通过，CTest 仍为 **20/21**，但失败位置不同；不能用后一轮 ARM64 通过抵消首轮的失败。最终 `mm.transactions` 五项在九配置均通过。

| preset | functional | framework | benchmark |
| --- | --- | --- | --- |
| arm64-debug | 1789054087674031000 | 1789054099456243000 | 不适用 |
| arm64-release | 1789054123333753000 | 1789054131311932000 | 1789054143167372000 |
| arm64-relwithdebinfo | 1789054156785267000 | 1789054164878569000 | 不适用 |
| riscv64-debug | **1789054088760333000，users.vm 启动失败** | 1789054163951238000 | 不适用 |
| riscv64-release | 1789054249091880000 | 1789054255886792000 | 1789054268353760000 |
| riscv64-relwithdebinfo | 1789054281927732000 | 1789054289449191000 | 不适用 |
| x64-debug | 1789054089639000000 | 1789054105013620000 | 不适用 |
| x64-release | 1789054129001330000 | 1789054138518509000 | 1789054151584601000 |
| x64-relwithdebinfo | 1789054166991979000 | 1789054176481668000 | 不适用 |

最终日志为 `build/<preset>/vm-transaction-caller-workflow.log`；报告均 finalized，报告层 not_run 为空，image SHA-256 与最终 manifest 对应产物逐项一致。RISC-V 64 失败 guest 已 ready、四核上线，但没有 worker 事件，三个 VM case 均 not_run；串口显示首次调度 PID 1 后 `scause=0xc, addr=0, pc=0`，进程以 -11 退出成 Zombie，最终被 host 记为 `guest_timeout`。本次没有保留现场寄存器，根因尚未确定；应继续排查首次用户执行/陷阱返回/抢占路径，不能误称 COW 用例断言失败或直接归因于模拟器。两轮失败均保留。

最终 RISC-V 64 Debug 镜像的 Sv39 专项也通过，报告 `1789054394589676000`，SHA-256 `17b86cc71619491d9d1870d9d305fa27c91cffa529fe2be44c3fc4eb6afc4388`，与最终默认 Sv48 报告使用同一产物。该不同 MMU 模式的成功不消除默认模式那次首次取指失败。

宿主 111/111 的全量结果发生在并行 `README.md`、`build.py`、`scripts/tests/test_build.py` 修改出现之前；收尾重新运行本轮相关的 `scripts/tests/test_kernel_validation.py` 为 **57/57**，不代替并行构建脚本改动的验收。

**提交前复核（2026-09-10，基线 `204914e`）：** `uv run build.py --jobs 3` 的九配置 configure/build 全部通过，未复现编译器或链接器错误；完整 workflow 为 **8/9**，CTest 为 **19/21**。`mm.transactions` 五项在九配置均通过。x64 Release 的两个失败均发生在运行阶段：functional 报告 `1789055109377273000` 中，VFS guest 在 ready 前因 `PIT/TSC/APIC clock calibration failed` 启动失败，尚未执行 VFS 用例；benchmark 报告 `1789055177434585000` 中，`bench.release` 发出 `frequency=0, source=invalid` 并以 `invalid_clock` 结束。报告位于 `build/x64-release/validation/<ID>/results.json`，保留原始串口记录，未通过重试、放宽超时或跳过 CTest 将其改判成功。它们不证明 VFS 或页表事务断言失败，也不能直接归因于 QEMU。当前运行时失败仍待排查；相关宿主测试再次通过 **57/57**。

另外对 `arm64-relwithdebinfo`、`x64-release`、`riscv64-debug` 分别运行 `cmake --build --preset <preset> --clean-first --parallel 4`，三者干净编译均通过。重新生成的镜像各自通过 `mm.transactions`、`mm.permissions`、`users.vm` 专项，报告依次为 `1789055337050159000`、`1789055338287025000`、`1789055339719556000`。这些专项不覆盖上述时钟失败，不是完整 CTest 转绿的证据。

**MOSS-010 仍为部分完成：** 当前测试证明页表函数在稳定源、自有未激活目标条件下的分配失败契约。接口尚依赖调用方排除并发修改，统一 VMA/PTE/ref/TLB 锁与跨 CPU 生命周期仍属 MOSS-008/011/028。`sys_fork` 的页表错误传播已实现，但完整系统调用的逐点 OOM 专项尚未运行；页表 clone 成功后的 kernel-stack、VMA、FD 等后续失败仍需纳入整次 fork 的事务。没有以这些表级用例关闭整个 fork 或完整内核目标，实机与长期 SMP 验收仍待补。

### 3.18 首次用户调度的 IRQ 临界区（2026-09-11，工作区）

继续 MOSS-007/017，确认并修复一个会损坏首次用户返回现场的真实交错，不以普通启动偶然通过作为修复依据。

**根因与修复：** `CfsScheduler::context_switch_to_task` 原来先发布 current task、准备首次上下文、切换地址空间和内核入口栈，最后才屏蔽 IRQ。RV64 的 `set_user_kernel_stack` 写入非零 `sscratch` 后，如果此时从 S-mode 进入 IRQ，统一异常入口会将其当作用户来源栈进行交换。待恢复的初始 `CpuContext` 与新陷阱帧占用同一栈区，`sd tp, 256(sp)` 将用户 PC 覆盖成当前 hart ID。hart 0 会产生 `pc=0`；hart 2 对照产生 `pc=2`。这不是 ELF 入口值最初为零，也不是 COW 断言失败。

现在在发布 current task 之前屏蔽本 CPU 的 IRQ；保护范围覆盖上下文、地址空间、入口栈和汇编切换。返回时仅恢复调用者原有的 IRQ 使能状态，IRQ 调用者不能在陷阱帧恢复前被无条件重新开中断。改动在三 ISA 共用的调度路径中，RV64 栈设置接口补充该前置条件；`sys_execve` 的调用点已有显式关中断。没有新增板名、QEMU 地址或模拟器依赖。

**真实写入现场：** `build/riscv64-debug/diagnostics/first-user-7v4ry6vu/0001/gdb.log` 保留硬件观察点：用户 PC 从 `0x200000000` 变成 `2`，写入者为 `syscall_entry_point` 的 `sd tp, 256(sp)`，`sstatus.SPP=1`，`sepc` 指向仍在 S-mode 的栈发布函数；随后用户在地址 2 取指失败。此前不带断点的自然失败仍保留在 `first-user-f9tfl9ga/0027`（前 26 次到达 worker，第 27 次 `PC=0`）。最初报告 `1789054088760333000` 没有寄存器现场，因此不能声称本路径已证明是每一次历史 `PC=0` 的唯一原因。

新增独立回归 `scripts/check_riscv64_dispatch.py`：GDB 在生产栈发布函数的返回点设置真实 SSIP pending/enable，不修改内核代码、PC、栈数据或调度器实现；验证初始 PC 未改变、切换期间 IRQ 关闭，以及 SSIP 仅在首次 SRET 后以正确用户 PC/SP 进入真实异常入口，最后必须完成全部 `users.vm` 用例。软件 IRQ 的 pending/enable 语义依据 [RISC-V 64 Supervisor ISA](riscv64_SUPERVISOR_DOC)。脚本使用已有 artifact reader、QEMU runner 和严格串口协议解析，冻结 kernel/initramfs/symbols 并记录 SHA-256；每次 fresh guest，第一次失败即停止并保留报告。它是需要 RISC-V 64 GDB 的专项入口，尚未加入默认 CTest，不把它计入默认测试数量。

```sh
uv run python scripts/check_riscv64_dispatch.py \
  --manifest build/riscv64-debug/moss-artifacts.json \
  --symbols build/riscv64-debug/bin/moss.test.elf --cpus 4 --runs 10
# 同一镜像再用 --cpus 1，或 --cpu rv64,sv48=false
```

环境为 QEMU 11.1.1 TCG、GDB 17.2、2 GiB RAM；`virt`/OpenSBI 装载地址仅属于上述专项 runner fixture。报告路径为 `build/riscv64-debug/dispatch-irq/<目录>/results.json`，各 guest 的串口及 GDB 现场在其编号子目录：

| 镜像与条件 | 报告目录 | 结果 |
| --- | --- | --- |
| 修复前，4 CPU，默认 Sv48 | `run-_m9opl74` | 首次 PC 被改成 2，专项失败 |
| 修复前，1 CPU，默认 Sv48 | `run-2o526_hw` | 首次 PC 被改成 0，专项失败 |
| 修复后，4 CPU，默认 Sv48 | `run-7fg12n67` | 10/10，包含 IRQ 时序检查及完整 users.vm |
| 修复后，1 CPU，默认 Sv48 | `run-q_qukbyk` | 10/10，包含 IRQ 时序检查及完整 users.vm |
| 修复后，4 CPU，Sv39 | `run-vig8w16h` | 3/3，同一镜像，包含 IRQ 时序检查及完整 users.vm |

修复前 RV64 Debug image SHA-256 为 `17b86cc71619491d9d1870d9d305fa27c91cffa529fe2be44c3fc4eb6afc4388`，修复后为 `b19256bbf48e53f356f0b70af71b3408b688508b9ec6716366c0d22eeb3a92ad`。不带调试器的后置启动循环 `build/riscv64-debug/diagnostics/first-user-hv92vdc0/results.json` 为 100/100 到达 worker；这个循环只证明到达用户执行，不能算作 100 次完整功能套件。旧镜像带“未处理缺页分支”断点的 100 次循环也未复现自然失败，说明断点会影响时序；修复依据是上述确定性交错的前后对照，不是重试到绿。

**三架构回归：** `uv run build.py --jobs 3` 的九配置 configure/build 全通过，完整 workflow **8/9**、CTest **20/21**。三架构九配置的 `users`、`users.vm` 均通过。宿主 pytest **121/121**，新增脚本 Ruff 与格式检查、改动行 clang-format 及 diff 检查通过。各报告为 `build/<preset>/validation/<ID>/results.json`：

| preset | functional | framework | benchmark |
| --- | --- | --- | --- |
| arm64-debug | 1789058556614572000 | 1789058571568943000 | 不适用 |
| arm64-release | 1789058557674388000 | 1789058566914500000 | 1789058578854512000 |
| arm64-relwithdebinfo | 1789058559449795000 | 1789058568421515000 | 不适用 |
| x64-debug | 1789058605929820000 | 1789058627285295000 | 不适用 |
| x64-release | 1789058609642577000 | 1789058622808073000 | 1789058635901872000 |
| x64-relwithdebinfo | 1789058617151552000 | 1789058627790634000 | 不适用 |
| riscv64-debug | **1789058648884619000，mm.transactions 超时** | 1789058680175489000 | 不适用 |
| riscv64-release | 1789058673907584000 | 1789058685608673000 | 1789058698620711000 |
| riscv64-relwithdebinfo | 1789058676734790000 | 1789058688003835000 | 不适用 |

失败 guest 已 ready 且启动 worker，`map_preserves_existing` 通过，随后 `map_allocation_rollback` 在 5.012 秒超时，其后三例 not_run；没有捕获 PC 或断言失败。该矩阵与部分诊断循环并行执行，宿主压力、页分配耗尽速度或内核交错均未完成因果对照，不能擅自归为环境问题。同一镜像另一次定向 `mm.transactions` 报告 `1789058733949855000` 通过，但不抵消原失败，也没有放宽原期限。

- [x] 限定关闭：首次用户上下文在入口栈发布与汇编切换之间被本 CPU IRQ 覆盖的路径；真实 IRQ 注入前后对照已完成。
- [ ] 完整 TrapFrame/syscall/信号返回仍属 MOSS-007；跨 CPU 的原子取任务、on-CPU 交接、迁移与等待所有权仍属 MOSS-017/018，本地 IRQ 屏蔽不能替代它们。
- [ ] 上述 `mm.transactions` 超时、历史 ARM64 启动/wait 超时及 x86 时钟校准失败仍待独立查因；真机、全异常入口交错及长期 SMP 验收未完成。

### 3.19 原生 TrapFrame、fork 与信号返回（2026-09-14，`0e88344`）

- [x] `moss.abi:trap_frame` 定义各 ISA 的原生帧、GP/参数/返回值访问和用户状态白名单；所有字段偏移及总大小与汇编共享常量逐项静态断言。C 入口只接收有效帧指针，线程只在 syscall 或用户返回检查点借用它。
- [x] syscall 先写回结果，再在 syscall/IRQ/可返回用户异常出口检查信号；不再由汇编覆盖 handler 参数。RV64 保存完整 GP，x86 SYSCALL 构造与 IDT 一致的 IRETQ 帧，ARM64 删除不兼容的第二套 syscall 保存路径。
- [x] fork 从实际用户帧复制 GP；ARM64 分离完整用户快照与内核调度跳板，修复 x30 被跳板地址覆盖；x86 首次返回保留用户条件码，不再固定丢弃 CF。
- [x] 移除 init/exec 中共用的 ARM64 指令字节；`src/abi/src/sigreturn.S` 按 ISA 生成 RX 返回桩。信号帧 V2 序列化原生 GP，检查 PC/SP 用户域、状态白名单和 x86 MXCSR，保存/恢复 x86 legacy FP；仍不把普通复制称为 fault-safe uaccess。
- [x] 默认新增真实用户态 `users.frame`：有效帧/全部六参数、GP/条件码 fork、信号 handler 内嵌 syscall 后的 GP/返回值恢复；x86 另检查 handler 改变 FP 后恢复原状态。suite 容量由 16 改为 32，溢出自检随常量调整，拒绝保护保留。

**失败证据保留：** 旧 RV64 `native_frame` 报告 `1789392990073436000` 捕获 `frame=0x1ff`，不是内核栈；ARM64 `1789393808264973000` 的子退出码指向 x30；x86 `1789394012207023000` 的增强用例捕获 fork 丢 CF。原生返回桩修复前 RV64 `1789393643823636000`、x86 `1789393643851972000` 在 signal_return 超时，串口显示执行共用 ARM64 字节后的错误指令/地址。早期 x86 报告因 suite_capacity 在 ready 前停止，只算框架容量失败，不算 TrapFrame 运行证据。ARM64 `1789393931460233000` 使用了编译失败后残留的旧镜像，不作为修复后验收。

**提交前矩阵：** `uv run build.py --jobs 3`：九个 configure/build/test workflow 全通过（162.1 s），CTest **21/21**；宿主 `uv run pytest -q scripts/tests` **121/121**，Ruff 与 diff 检查通过。原始报告的 revision 保持运行时 `4fce85a`、dirty=true，不回写成提交后的 SHA；对应源码随后提交为 `0e88344`。各报告路径为 `build/<preset>/validation/<ID>/results.json`：

| preset | functional | framework | benchmark |
| --- | --- | --- | --- |
| arm64-debug | 1789394530915195000 | 1789394556420772000 | 不适用 |
| arm64-release | 1789394555145071000 | 1789394565341261000 | 1789394577090541000 |
| arm64-relwithdebinfo | 1789394556723693000 | 1789394563451235000 | 不适用 |
| x64-debug | 1789394574398580000 | 1789394601647936000 | 不适用 |
| x64-release | 1789394604935876000 | 1789394614483541000 | 1789394627074884000 |
| x64-relwithdebinfo | 1789394611011849000 | 1789394619524257000 | 不适用 |
| riscv64-debug | 1789394620691560000 | 1789394636139993000 | 不适用 |
| riscv64-release | 1789394662261567000 | 1789394669778572000 | 1789394681757791000 |
| riscv64-relwithdebinfo | 1789394664061012000 | 1789394671552789000 | 不适用 |

- [ ] 全异常/抢占交错、全部 FP/TLS fork 继承、恶意/嵌套/备用信号帧、可恢复复制、CPU-bound 投递及 STOP/CONT/SIGCHLD 仍待实现或专项验收；MOSS-002/003/007/014/020 整项不关闭。
- [ ] 本轮全绿不取消 3.18 的 mm.transactions 超时、历史 ARM64 启动/wait 及 x86 校准失败，也不等于重新完成 IRQ 注入、真机或长期 SMP 验收。

### 3.20 用户复制异常恢复（2026-09-14，基于 `0e88344`）

- [x] 新增 `moss.process:uaccess` 共享接口，统一完整区间的用户域/VMA 读写准入；调用期间持有 Process 所有者。底层按 ISA 执行复制，返回未复制字节数；输入复制失败时清零内核缓冲区未复制尾部。
- [x] `src/abi/src/uaccess.S` 为用户操作数的实际 load/store 登记只读、自相对异常表；三架构缺页入口传递原生 TrapFrame，仅匹配的内核复制 PC 和用户地址可以转到恢复点。内核缓冲区操作数没有登记恢复点。
- [x] demand/COW 分配失败交回异常入口处理，允许共享复制返回失败并由 syscall 转为 `EFAULT`；普通用户不可恢复异常仍终止进程。原有 syscall 复制调用已接入共享接口。
- [x] 新增默认 `users.uaccess.allocation_fault`：真实 mmap 保留合法但未驻留的可写页，耗尽真实 PFA，再调用 clock_gettime 写出。检查 `EFAULT`、进程存活、PTE 仍未安装、释放压力后的页数恢复及同一地址重试成功；不 mock 分配器或故障 PC。

**修复前后证据：** 以下报告均在 `build/<preset>/validation/<ID>/results.json`。修复前串口记录内核用户复制遇到 OOM 后终止 PID 1，case 随后超时；修复后相同用例通过。报告保持实际运行时 revision/dirty/image hash，不改写为提交后的版本。

| preset | 修复前（case_timeout） | 修复后（passed） |
| --- | --- | --- |
| arm64-debug | 1789395097579673000 | 1789396191884195000 |
| x64-debug | 1789395098712892000 | 1789396191912766000 |
| riscv64-debug | 1789395079843119000 | 1789395851917794000 |

**提交前回归：** `NO_COLOR=1 TERM=dumb uv run build.py --jobs 3` 九个 configure/build/test workflow 全通过（158.0 s），CTest **21/21**；每个配置的默认功能包含 `users.uaccess`。宿主 `uv run pytest -q scripts/tests` **121/121**；全仓格式检查、新增模块文件的 clang-format 检查、runner Ruff 检查和 diff 检查通过。原始报告均基于 `0e88344`、dirty=true；本轮通过不取消 3.18 的间歇失败记录。

| preset | functional | framework | benchmark |
| --- | --- | --- | --- |
| arm64-debug | 1789396307743705000 | 1789396322976302000 | 不适用 |
| arm64-release | 1789396308602584000 | 1789396317458047000 | 1789396329267466000 |
| arm64-relwithdebinfo | 1789396309807560000 | 1789396318523582000 | 不适用 |
| x64-debug | 1789396350173698000 | 1789396366978318000 | 不适用 |
| x64-release | 1789396352782328000 | 1789396362910295000 | 1789396375573153000 |
| x64-relwithdebinfo | 1789396358435391000 | 1789396367506679000 | 不适用 |
| riscv64-debug | 1789396407444576000 | 1789396425003863000 | 不适用 |
| riscv64-release | 1789396409285437000 | 1789396418079184000 | 1789396430373237000 |
| riscv64-relwithdebinfo | 1789396410819718000 | 1789396419489860000 | 不适用 |

- [ ] VFS read/write 和信号帧仍需迁移；输入复制/COW 的 OOM、部分跨页复制、恶意/嵌套信号帧尚无本轮专项验收。
- [ ] 复制允许部分前缀成功，不是事务；持有 Process 也不等于锁定 AddressSpace 或页表，不能替代并发 exec/unmap/COW 的 VM 生命周期协议。MOSS-002/003 整项不关闭。

### 3.21 VFS/信号安全复制与部分 I/O（2026-09-14，基于 `dd6a099`）

- [x] `moss.process:uaccess` 对外返回未复制字节数，零代表完整成功；syscall 的 errno 转换保留在调用侧。输入失败仍清零未复制的内核缓冲区尾部。
- [x] 新增 `moss.vfs:buffer` 的有界 `InputBuffer`/`OutputBuffer`。内核调用者提供稳定内核存储，syscall 注入共享用户复制策略；FileOps 不再接收裸用户指针，VFS 不引入对 process 的依赖，也不增加堆分配或任意短 I/O 上限。
- [x] ramfs、pipefs、devfs 的现有 read/write 回调迁移到视图。ramfs offset、管道读写位置和可用字节只按实际复制前缀推进；首字节失败返回 EFAULT，部分成功返回已复制字节数。保留现有 EOF、只读 ramfs 和非阻塞管道行为。
- [x] 删除 `signal_copy` 的普通用户地址循环。信号帧与 x86 返回地址写出使用 `copy_to_user`，sigreturn 读回使用 `copy_from_user`；完整复制及帧检查成功后才提交恢复现场。未改变信号检查点的 `128 + signo` 退出策略。
- [x] 默认 `users.uaccess` 扩为九项：allocation_fault、write_fault、read_fault、partial_read、partial_write、partial_pipe_read、sigframe_fault、sigreturn_fault、devices。真实 PFA 耗尽覆盖输入/输出失败、4/8 字节跨页短复制、offset/管道剩余数据、输入尾部清零、备用栈写出失败及 sigreturn 读回失败；释放压力后检查可重试。devfs 检查 zero 的正常/部分/失败读取、console 写出失败，以及 null 不触碰合法未驻留缓冲区的行为。

**缺陷红灯证据：** 报告路径为 `build/<preset>/validation/<ID>/results.json`。用例先落地，再修生产实现。三架构故障 PC 均已符号化：ramfs_read、pipe_write 分别直接访问用户输出/输入；signal_copy 的写、读循环分别在 signal.cpp 原第 141、143 行触发故障。下面的信号读回红灯来自写出迁移后的中间阶段，不伪称为同一版镜像。

| preset | ramfs read 红灯 | pipe write 红灯 | 信号帧写出红灯 | sigreturn 读回红灯 |
| --- | --- | --- | --- | --- |
| arm64-debug | 1789397160755843000 | 1789397479168774000 | 1789399751019719000 | 1789400279490114000 |
| x64-debug | 1789397160788187000 | 1789397478854227000 | 1789400171632712000 | 1789400278428979000 |
| riscv64-debug | 1789397160736720000 | 1789397479229565000 | 1789399751011447000 | 1789400436091370000 |

**测试前置条件与未关闭失败：**

- x86 报告 `1789399751048613000` 在合并跨页测试中触发用户代码页取指缺页（PC=0x200002000），不是信号路径证据。userspace linker 增加 text 区间符号，压力测试开始前读取每个代码页；目标匿名数据页仍由内核检查为未驻留，不硬编码测试函数地址。
- ARM64 首次信号帧测试的退出码预期也曾误写为负信号码，已按现有检查点实现改为 `128 + SIGUSR1`。红灯依据是实际 signal_copy 故障 PC，而非错误退出码断言本身；内核退出策略未为测试改变。
- RV64 Debug 报告 `1789400280670714000` 的合并 partial_io 在 5 s 超时，同轮单次耗尽用例为 2.0～2.7 s；将三次耗尽拆成独立跨页用例，没有放宽预算。之后报告 `1789400574263859000` 又在首个 allocation_fault 超时，新用例均未运行，且没有停机 PC 证据，根因仍未定位。后续矩阵通过不取消这两份失败，也不把它们直接归因于宿主负载或 QEMU。

**完整回归：** `NO_COLOR=1 TERM=dumb uv run build.py --jobs 3` 九个 configure/build/test workflow 通过（177.2 s），CTest **21/21**。默认功能为 **13 套件/48 用例**，每个配置的 users.uaccess 九项均通过。宿主 `uv run pytest -q scripts/tests` **121/121**；全仓格式检查、新模块 clang-format、runner Ruff 及 diff 检查通过。下面 21 份原始报告均 finalized，revision=`dd6a099`、dirty=true，未改写运行时 provenance/image hash。

| preset | functional | framework | benchmark |
| --- | --- | --- | --- |
| arm64-debug | 1789400691668343000 | 1789400720701503000 | 不适用 |
| arm64-release | 1789400702316033000 | 1789400716065800000 | 1789400729351502000 |
| arm64-relwithdebinfo | 1789400703142982000 | 1789400716068330000 | 不适用 |
| x64-debug | 1789400737610084000 | 1789400782316832000 | 不适用 |
| x64-release | 1789400759557543000 | 1789400775876334000 | 1789400789428980000 |
| x64-relwithdebinfo | 1789400762485254000 | 1789400776974079000 | 不适用 |
| riscv64-debug | 1789400798321803000 | 1789400847297282000 | 不适用 |
| riscv64-release | 1789400800985703000 | 1789400822744553000 | 1789400836409062000 |
| riscv64-relwithdebinfo | 1789400823508709000 | 1789400835455385000 | 不适用 |

- [ ] COW OOM、并发 exec/unmap/COW、AddressSpace/PTE/页引用生命周期仍未闭合；持有 Process 和借用缓冲区视图不等于锁定页或 VM。
- [ ] 信号特权字段攻击、全部嵌套/备用栈边界、console_read 真实串口输入故障未专项验收；本轮不关闭 MOSS-002/003，也不宣称所有用户访问路径均已穷尽验证。
- [ ] File/FD/管道的并发、访问模式、阻塞/EOF 和失败回滚仍归 MOSS-024～026；部分复制不是事务，不保证 PIPE_BUF 原子写入。历史间歇失败及 x86 SIMD 专项限制继续保留。

### 3.22 x64 PVH initrd 契约与启动失败闭合（2026-09-19，基于 `caaab1b`，工作区）

- [x] x64 启动现在拒绝非零 PVH 保留字段、越过 4 GiB 早期恒等映射的完整模块表、空模块、非零模块保留字段，以及未被可用 RAM bank 连续覆盖的 initrd。相邻可用 bank 可以共同覆盖模块，RAM 洞、MMIO/保留区和溢出范围不能。
- [x] `initramfs` 的 newc 解析器校验全部十三个十六进制字段、文件名 NUL/边界、数据和 padding 边界、固定索引容量及必需 `TRAILER!!!`；失败会清空部分索引并保持未初始化，空但结构合法的 archive 交给启动策略报告缺少 init。
- [x] initramfs、`/validation.elf` 或 `/busybox.elf`、VFS 和初始进程建立前不再发布 `[boot] MOSS kernel boot completed`。缺失 initrd、格式错误、缺少必需 init 或初始进程创建失败均在完成标记前停止。
- [x] 新增 x64-only `moss-pvh-initrd` CTest。runner 冻结并散列同一生产 kernel/initramfs，依次执行七个真实 QEMU 场景；非法模块场景只在 QEMU 到达真实 ELF 入口后把 PVH 模块大小改为零，再让未修改内核继续运行，不使用测试专用内核或生产绕过。

**真实 PVH 区间证据：** 512 MiB 基线 archive 为 1,110,744 字节，QEMU 报告 `0x1fec8000..0x1ffd72d8`；追加 1 MiB 合法尾部 padding 后为 2,159,320 字节，区间移动为 `0x1fdc8000..0x1ffd72d8`；同一基线 archive 在 768 MiB RAM 下移动为 `0x2fec8000..0x2ffd72d8`。runner 同时检查日志大小等于实际文件大小、`end - start == size`、三个要求变化的区间确实不同，且运行前后源 artifact SHA-256 不变。

| 模式 | 报告 | 七场景 | 区间/源 hash 检查 |
| --- | --- | --- | --- |
| x64-debug | `build/x64-debug/pvh-initrd/1789799435146586000/results.json` | 全部 passed | true / unchanged |
| x64-release | `build/x64-release/pvh-initrd/1789799435135892000/results.json` | 全部 passed | true / unchanged |

**负向结果：** 无 `-initrd` 精确得到 `BOOT ERROR: PVH initrd module is required`；零长度 PVH 描述符精确得到 `BOOT ERROR: invalid PVH initrd module`；坏 newc magic 得到 `Error: Invalid initramfs archive`；结构正确但没有必需 init 得到 `Error: Required init executable is missing`。四项都记录 `boot_completed=false`；前两项先在早期硬件初始化输出精确 BOOT ERROR，后两项由内核启动策略明确 panic。生产 CPIO 源提取回归还覆盖非十六进制未使用字段、未终止文件名、截断数据和缺 trailer；生产 PVH helper 源提取回归覆盖模块表越界、模块保留字段、零长度、RAM 洞及 RAM 外范围。

**限定回归：** 九个 preset 的当前工作区均完成 configure/build；x64 Debug/Release 的 `moss-production-boot` 与 `moss-pvh-initrd` 各 **2/2**，x64 RelWithDebInfo 的新 gate **1/1**，ARM64/RV64 Debug/Release 的生产 boot 各 **1/1**。`moss-production-boot` 使用真实 BusyBox ash，实际 exec、管道/文件操作、子进程回收后命令和返回 shell 均通过。专项宿主测试 **3/3**。

不把额外全量结果伪报为全绿：`uv run pytest -q scripts/tests` 为 **353 passed / 8 failed**；四项是 macOS `AF_UNIX` 路径过长，四项依赖本机不存在的 Linux `os.sched_setaffinity`，均位于未修改测试。额外九 preset runner 的 ARM64 Debug 生产 boot 通过，但同时保留了 `vfs.smp` panic、`users.applications` 无进展和缺少 `gdb-multiarch` 三个非本项失败；其余超长 applications 流程在不再能为 MOSS-027 提供新证据后停止。因此本节关闭 MOSS-027/C1b，不扩大为 MOSS-028 或全仓回归通过。

### 3.23 ASID、信号状态与退出 FD 生命周期（2026-09-19，工作区）

本节重新沿当前源码核对了原审计的 MOSS-011、MOSS-021、MOSS-022。三项生产实现已在此前提交中落地，而本节更新前第 4 节仍保留原始“未修复”状态；本轮没有重写生产路径，只强化用户态回归并以当前 dirty 源码重新构建、运行。下面的关闭结论只覆盖对应不变量，不外推到 MOSS-012 的 VMA/PTE 事务、MOSS-017 的完整迁移协议或 MOSS-025 的一般 pipe 阻塞语义。

**实现复核：**

- MOSS-011：`user_space::allocate_asid()` 以 IRQ spin lock 保护 256 位活跃表，标签 0 保留给内核，1…255 是用户租约；耗尽返回 `ResourceExhausted`，不再回绕覆盖活跃所有者。`AddressSpace` 析构释放标签；ARM64 在归还前广播按 ASID 的 TLB 失效，RV64 每次 SATP 切换都失效，x64 当前未启用 PCID。锁使分配/释放线性化；这不是无锁 generation 方案。
- MOSS-021：信号状态由 `Process` 拥有，不再按绝对 PID 查询固定数组。fork 复制进程 disposition、线程 mask 与 altstack；exec 保留忽略动作、重置其他 disposition 和 altstack；退出随 `Process` 生命周期回收。
- MOSS-022：`do_exit()` 在拆地址空间和发布 Zombie 前调用 `Process::cleanup_files()`；该操作先摘下 FD table 再 `close_all()`/删除，析构仅作兜底，避免重复关闭。exec 只执行 close-on-exec 规则，不误关普通继承描述符。

**强化验证：**

- `mm.transactions/asid_leases` 在当前进程之外保持 254 个地址空间存活，验证所有 255 个用户标签唯一、下一次分配显式耗尽、隔项释放后只复用已归还标签，最终物理页计数回到基线；每次运行 511 项断言。
- `users.signals/pid_lifecycle` 仍顺序运行 300 个子进程，跨越 255 个用户 ASID 与旧 256 槽信号表边界；每个子进程在同一用户 VA 写入逐次唯一的非零模式，交替设置 CPU0/CPU1 affinity，睡眠后核对实际 CPU，并在八次重新调度中检查内容。父进程同址值必须始终为零，同时保留信号安装、投递与返回检查。
- `users.lifecycle/core_paths_recovery` 的 warmup 在 `waitpid` 之前读取 pipe 最终 EOF，直接证明子进程退出已关闭最后一个继承 writer；随后 1,000 个资源周期保留原有 wait-before-EOF 顺序，以免每周期额外的阻塞/唤醒掩盖 FD 回收测量。每 100 次精确比较 heap、页、进程/线程、描述符、文件引用及 VFS 池占用。

六个受影响 Debug/Release 构建均成功。下表报告均 finalized，取证时共同记录 revision=`caaab1bce5259ddf2c23437b8f13bb12c233a9c7`、dirty=true 与 source hash `af2d1442f941a79e901a0e3cbf5f5542cf93ba4f1df126d4e89c9c6aa2470531`；随后只更新了本节等交付文档，因此该哈希不是文档完成后的工作区身份：

| preset | `mm.transactions/asid_leases` 与 `users.signals/pid_lifecycle` | 默认 `users.lifecycle/core_paths_recovery` |
| --- | --- | --- |
| arm64-debug | `1789801308711624000`，通过 | 同报告，1,000 次 / 3,026 断言通过 |
| arm64-release | `1789801347726477000`，通过 | 同报告，1,000 次 / 3,026 断言通过 |
| riscv64-debug | `1789801463490433000`，通过 | `1789801038486023000`，30 秒预算在第 600 次稳定检查点超时 |
| riscv64-release | `1789801373375143000`，通过 | 同报告，1,000 次 / 3,026 断言通过 |
| x64-debug | `1789801398825575000`，通过 | 同报告，1,000 次 / 3,029 断言通过 |
| x64-release | `1789801435917554000`，通过 | 同报告，1,000 次 / 3,026 断言通过 |

RV64 Debug 的最终默认报告在 guest elapsed 29.644042 秒时已到第 600 次检查点，所有资源仍与基线相等，没有 EOF 死锁或泄漏证据。同一源码的显式 60 秒诊断报告 `1789801163232597000` 在 guest elapsed 40.080208 秒完成 1,000 次、3,026 项断言且资源完全恢复。该诊断说明当前障碍是默认时限下的吞吐，不替代默认 gate；仓库的 30 秒预算没有放宽。

宿主侧 `scripts/tests/test_kernel_validation.py` **124/124** 通过；userspace 变更的 clang-format dry-run 与全工作区 `git diff --check` 通过。这些宿主检查验证报告/runner 契约和代码形状，不替代上述真实 QEMU 结果。

因此 MOSS-011 与 MOSS-021 关闭：生产不变量、边界/耗尽检查和当前六配置真实用户态复用均有证据。MOSS-022 只记“实现已修复、验收部分”：五个默认配置和 RV64 Debug 加长诊断证明功能/回收，但原定默认六配置矩阵仍缺一项。原始失败报告和后续其他长循环任务继续保留。

### 3.24 brk/VMA/PTE 生命周期事务（2026-09-19，工作区）

本节关闭 MOSS-012 的限定契约：所有声称成功的 brk、匿名 mmap 和整段 munmap 都必须让 VMA 授权与实际 PTE 可达性一致；部分 munmap 继续明确拒绝。PROT_NONE/只读/NX 的 demand-fault 拒绝已在 3.16 落地，本轮没有复制另一套权限逻辑，重点补齐空堆、缩堆撤销、重新增长清零和增长冲突回滚。

**生产路径：**

- init 与 exec 现在都安装固定 `HEAP_START` 的空 HEAP VMA，`brk_base == brk_current == HEAP_START`；`HEAP_INIT` 只保留为 ELF 与堆之间的 64 KiB 排除窗口，不再提前授权 16 个页。VMA 准入拒绝其他起点的 HEAP 和第二个 HEAP，fork 仍复制唯一空/非空堆元数据。
- 新的 `LockedList::update_if_unless` 在同一次 IRQ spin-lock 持有期内选择精确旧 VMA、扫描新范围冲突并执行更新回调。`AddressSpace::resize_vma` 复用它；增长撞到 mmap/stack 时不执行回调、不改变 VMA 或 `brk_current`。
- 跨整页缩小时，回调在缩短 VMA 前逐页调用 `unmap_user_page`。该原语先清叶 PTE、执行架构 TLB 失效，再递减 COW 页引用、在最后引用时归还物理页，并剪除空用户页表。页内缩小明确保留所在硬件页及其尾部内容；这是当前 ABI，而不是伪造字节粒度撤销。
- 无调用者的 `allocate_user_heap` 辅助也改为同一空堆/resize 契约，避免未来重新引入重叠 VMA。当前地址空间模型没有共享地址空间的多 CPU 同时执行；page-fault lookup 到 map 的通用并发事务与跨 CPU shootdown 仍属于 MOSS-008～010，不能由本节单进程 brk 结果外推。

**红绿回归：** 新增 `users.vm/brk_lifecycle`，先确认初始 break 不可访问，再增长三页并写入模式；缩到一页加 37 字节后，完整离开的第三页必须故障，重新增长必须为零。随后在第四页放置匿名映射，尝试把 break 增长穿过第五页必须返回旧 break，保留映射内容且不授权中间缺口；缩回 base 后第一页故障，最终重新增长仍为零。固定数字都在用例旁说明页边界或冲突布局来源。

只加入用例、尚未修改生产路径时，ARM64 Debug 报告 `1789803020518526000`（source hash `cd9ae34486837d39e261b125ae9ec152639183e42cc2733026b75795618abd10`）保留为红例：前三个 `users.vm` case 通过，`brk_lifecycle` 以掩码 `0x14b52` 失败。置位项证明旧实现提前授权空堆、缩堆后页和旧内容仍可见、冲突增长被部分提交、间隙被授权，以及缩回/再增长仍看到旧字节；不是先写修复再构造“红例”。

修复后六个 Debug/Release 构建全部成功。3.26 的 ELF LoadPlan 验收落地后又以最终生产源码重跑：每个 preset 的 finalized 报告都包含 `users.vm` 四项、`mm.permissions` 七项、`mm.transactions` 九项、`users.exec` 二十八项及 libc/BusyBox 消费者，共 **6 套件 / 73 case**，全部通过。报告在执行时共同记录 revision=`caaab1bce5259ddf2c23437b8f13bb12c233a9c7`、dirty=true 和 source hash `c936891a8035d9aaaaead297cd85f5ab002b3dc07cd865edfad75804503cfe8b`；交付文档随后更新，因此该哈希是被执行源码身份，不是文档完成后的工作区哈希。

| preset | 最终 73-case 报告 |
| --- | --- |
| arm64-debug | `1789814154497644000` |
| arm64-release | `1789814177535478000` |
| riscv64-debug | `1789814194506681000` |
| riscv64-release | `1789814222503529000` |
| x64-debug | `1789814238346898000` |
| x64-release | `1789814264526648000` |

`mm.permissions/vma_boundaries` 还验证空堆只允许固定起点、重复堆拒绝、无冲突增长、冲突失败不调用更新回调、缩回空哨兵及精确移除。`mm.transactions/unmap_reclaims_tables` 在真实页表上检查清叶后页引用归零、相邻映射保持、重复 unmap 无害和 PFA 空闲页恢复。`users.libc`、`users.exec` 与真实 BusyBox ash 覆盖从空 break 启动后的分配、exec 及 shell 消费者，不以单一 syscall case 代替集成路径。

并行启动三个 Debug runner 的早期报告 `1789803823431979000`、`1789803823427498000`、`1789803823427213000` 出现默认 case/guest 超时；随后按 preset 串行、保持原 5/30/60 秒预算的同源码报告通过。RV64 Debug 串行报告 `1789803979141898000` 的 exec OOM rollback 也曾在 5.010 秒越界，单独默认预算重跑 `1789804114118442000` 通过。这里保留这些失败并只把串行成功作为功能证据；没有放宽仓库阈值，也不声称精确定位了宿主负载波动。

宿主 runner 测试 `scripts/tests/test_kernel_validation.py` **124/124** 通过。MOSS-012 因此按上述当前模型关闭；不包含部分 munmap、同地址空间多线程并发 fault/resize、一般 COW/fork OOM 原子性、真实硬件或 RelWithDebInfo。

### 3.25 exec 准备/提交与逐级失败回滚（2026-09-19，工作区）

本节关闭 MOSS-015 的当前契约。原审计所述“先释放旧页表、提前改名”已经与实现不符：`sys_execve` 先复制全部用户输入并构造独立映像，直到没有可失败准备工作后才提交。后续 MOSS-016 的通用 ELF LoadPlan 与边界页验收另见 3.26；两项仍以各自证据关闭。

**生产路径：**

- 路径、argv 和 envp 在旧地址空间仍活动时完整复制；128 个合计字符串和含 NUL 的 16 KiB 是显式 Moss ABI，坏指针、缺少终止符与超限分别返回 EFAULT/ENAMETOOLONG/E2BIG。参数快照不再使用 OOM 会 panic 的普通 `new`，而是通过 `RuntimeHeapAllocator::allocate_aligned` 加 placement construction 建立可失败 `unique_ptr`。
- immutable CPIO ELF 借用只读 backing；可写 ramfs ELF 在 namespace lock 内一次性复制到 `ExecutableImage`。其对象和 `SharedPtr` control block 都通过 `try_make` 独立失败，映像字节也显式检查分配结果；control block 失败会由临时 `unique_ptr` 释放已构造对象。
- header/segment 检查后创建独立 `AddressSpace`，逐项加入 LOAD、sigreturn、stack 与固定空 HEAP VMA，再分配并通过物理映射填充完整启动栈。任一失败只析构 prepared ownership；旧 AS、用户栈、进程名、信号和 FD table 尚未改变。
- commit 先屏蔽 IRQ 并切换 TTBR0/SATP/CR3，再把非空 prepared owner 交给 `Process`；旧页表此后才析构。随后执行 close-on-exec、改名、信号/altstack 重置和不可失败的上下文赋值并直接 `switch_to_user`。当前 commit 段没有可恢复失败点，因而没有用“半提交后杀进程”替代错误返回。

**逐级真实压力：** `users.exec/allocation_rollback` 保留根页、stack leaf 和各级中间页表的真实 PFA 预算；夹具现在只耗尽 PFA 一次，再逐轮多释放一页，仍给出完全相同的 0…N 页失败点，最终一次释放并与完整资源基线比较。它还显式检查不存在文件、损坏入口、旧 AS 指针/root hash/name、栈 canary、FD offset，以及压力解除后的成功 fork/exec。

新增 `mutable_snapshot_rollback` 先把真实 `/validation_child.elf` 复制成可写 ramfs `/exec-copy.elf`，再对六个独立 heap 边界逐一施加真实耗尽：参数块、映像对象、共享 control block、映像字节、地址空间，以及至少已有一个节点的部分 VMA 列表。每次必须返回 ENOMEM，保持旧 AS 身份/root hash/name、canary 与连续 FD offset，并在释放探针后让 heap、物理页、进程/线程、用户/栈页、描述符/文件引用和 VFS pool 精确回到基线。64 KiB 起逐级减半的 heap 压力先消耗大块再封住目标大小碎片；它改变夹具成本，不替换目标分配结果或放宽 5 秒阈值。

成功语义由当前矩阵中的 `users.libc/static_runtime` 继续覆盖：普通 FD 跨 exec 保留，FD_CLOEXEC 只在成功提交时关闭；失败的 missing exec 不改变它们。可写 ELF 成功启动后会截断并删除自己的 backing，证明执行映像由独立快照持有。3.23 的生命周期循环还在重复成功 exec/exit/reap 后精确比较页和对象资源；生产 `set_address_space` 的所有权顺序与这些回归共同覆盖旧地址空间回收。

**红绿证据：** 第一个新 heap 阶段在修复前真实触发 fatal `operator new`，ARM64 Debug 报告 `1789807628141897000`（source hash `3005e7829f49bcc54ee09945f0bcce53eb8dfc8aac4bab2fe182d44659cffcb0`）记录 `mutable_snapshot_rollback` 的 `unexpected_kernel_panic`，回溯精确落在 `make_unique<Arguments>`。改成可失败参数分配后，报告 `1789807781399619000` 进入共享 control-block 阶段，但旧夹具用 16 字节块填满并逐块释放整个 8 MiB heap，在 5.017 秒达到默认超时；这是保留的夹具成本失败，不是将超时改长。分层压力后，隔离默认预算报告 `1789807979527823000` 的 13/13 exec case 全通过，其中页表回滚 0.931 秒、六级 heap 回滚 0.069 秒。

3.26 落地后的最终六个 Debug/Release 构建均成功；与 3.24 共用的最终报告各含 **6 套件 / 73 case**，全部通过并共同记录执行时 source hash `c936891a8035d9aaaaead297cd85f5ab002b3dc07cd865edfad75804503cfe8b`：

| preset | 最终报告 |
| --- | --- |
| arm64-debug | `1789814154497644000` |
| arm64-release | `1789814177535478000` |
| riscv64-debug | `1789814194506681000` |
| riscv64-release | `1789814222503529000` |
| x64-debug | `1789814238346898000` |
| x64-release | `1789814264526648000` |

最终宿主 `scripts/tests/test_kernel_validation.py` 与 `scripts/tests/test_userspace_build.py` 合计 **134/134** 通过。MOSS-015 因此按当前单线程进程/独立地址空间模型关闭；多线程共享地址空间的 exec/fault/unmap 互斥仍留给 MOSS-002/008～010。静态 ELF 子集、动态段拒绝和边界页语义由 3.26 单独验收。

### 3.26 不可变 ELF LoadPlan 与边界页语义（2026-09-19，工作区）

本节关闭 MOSS-016 的当前静态 ELF 子集。`build_load_plan` 在创建地址空间前一次性验证 ELF64 header 和最多 64 个 program header，将每个 PT_LOAD 固化为页范围、文件 backing 范围和最终权限；后续 VMA 构造只读消费该计划，不再重复 program-header 算术。文件大小、用户范围、页圆整和 file-prefix 均使用减法式上界检查；入口必须位于可执行段。W+X、保留的 sigreturn/stack/heap 区、LOAD 共享页、PT_INTERP/PT_DYNAMIC 和未知 flag 均返回 ENOEXEC。

PT_TLS 不单独产生 VMA。纯 NOBITS TLS 只保留为静态 runtime 元数据；存在文件模板时，其内存区间、文件区间和 VA/file 平移必须完全落在同一已验证 PT_LOAD 内。线程指针和完整 TLS 继承不是本项承诺，仍属于 userspace runtime 与 MOSS-014；这里保证 loader 不把孤立 file-backed TLS 当成已映射数据。

正向 `/boundary_load.elf` 在真实 child 后追加两个与原映像绝对平移不同的非页对齐 PT_LOAD。RX 段的文件内容跨页，RW 段包含 BSS 并跨页；child 核对页前缀、文件尾、BSS 和圆整页尾的实际字节，执行目标 ISA 的 `return 42` 指令，直接写 RW BSS，并分别要求 RX 写入和 RW 执行在 fork child 中以 SIGSEGV 终止。这样同时验证 demand fault 的 backing offset/长度、零填充和最终 PTE 权限，不以 exec 成功代替内容检查。

负向集合共有 17 个独立映像：无效入口、错误 phentsize、`filesz > memsz`、截断 header/table、文件回绕、内核地址、地址回绕、页内 offset 不一致、非二次幂对齐、保留区、LOAD 页重叠、65 个 program header、RWX、PT_DYNAMIC、PT_INTERP 和孤立 file-backed TLS。拒绝 probe 使用 `must-reject` argv；若 loader 错误接受，新的 child 会以启动错误码退出，不能再与“ENOEXEC 后旧进程退出 37”混为成功。

该 oracle 修复实际暴露了此前被隐藏的接受路径。ARM64 Debug `1789813438676220000` 在前 16 个拒绝 case 通过后单独失败于 `rejects_tls`。粗暴拒绝全部 PT_TLS 的诊断 `1789813531418854000` 又使真实 mlibc 的 `exact_combined_count` 失败，证明合法 file-backed TLS 不能一刀切。随后 `1789813725768099000` 暴露 containment 先减后判导致的无符号下溢；六份 `1789813880514573000`～`1789813919429130000` 则证明把纯 NOBITS TLS 也要求落入 LOAD 会破坏真实 BusyBox。最终策略只约束 file-backed TLS，并保留这些红报告而不把它们改写成通过。

最终六个 Debug/Release 构建均成功。以下串行报告各含 **6 套件 / 73 case**：`users.exec` 28 项、`users.vm` 4 项、`mm.permissions` 7 项、`mm.transactions` 9 项、`users.libc` 2 项和 `users.busybox` 23 项；全部通过、无 not_run，并共同记录 revision=`caaab1bce5259ddf2c23437b8f13bb12c233a9c7`、dirty=true、source hash `c936891a8035d9aaaaead297cd85f5ab002b3dc07cd865edfad75804503cfe8b`：

| preset | 最终 73-case 报告 |
| --- | --- |
| arm64-debug | `1789814154497644000` |
| arm64-release | `1789814177535478000` |
| riscv64-debug | `1789814194506681000` |
| riscv64-release | `1789814222503529000` |
| x64-debug | `1789814238346898000` |
| x64-release | `1789814264526648000` |

宿主 runner、fixture 打包与三 ISA vendored userspace 构建测试合计 **134/134** 通过。一次较早的 ARM64 Release 全矩阵报告 `1789811660833369000` 在进入 `mm.permissions` 前因 4 个从核仅 3 个 online 而启动 panic；同配置专项 `1789811773578266000` 和上述最终完整报告均通过，因此保留为 MOSS-017/018 范围的 SMP 启动诊断，不归因于 LoadPlan。

当前契约仍只接受固定地址 ET_EXEC 静态子集；动态解释器、动态重定位、共享 LOAD 页和内核负责的 TLS 初始化被拒绝或不在范围内。本节的专项六报告未覆盖 RelWithDebInfo；后续 3.27 的九 preset 普通 functional 矩阵补齐了该构建类型，但真实硬件和多线程共享地址空间并发 exec/fault/unmap 仍未覆盖。关闭 MOSS-016 表示上述有限子集及明确拒绝策略已验收，不表示任意外部 ELF 均可执行。

### 3.27 九 preset CTest 错误修复与收口（2026-09-19，工作区）

MOSS-016 完成后串行执行全部九个 CTest preset。首个 ARM64 Debug 运行保留了三个红项，而不是直接重跑覆盖：`moss-applications` 报告 `1789815040270100000` 的 0/10 周期检查点资源完全相等，但 runner 在最终串口回放时漏传本次 `lifecycle_cycles=10`，错误按默认 1,000 周期拒绝；`moss-framework` 报告 `1789815056415011000` 的 `self.panic` 尚未进入 validation ready，第四个从核停在启动期 WFE，CPU0 以初始化超时 panic；`moss-console-input` 的 `run-k0tdvahz` 在启动 QEMU 前因 CMake 硬编码本机不存在的 `gdb-multiarch` 失败。

三项分别修复其真实原因。串口回放现在复用当前 guest 已计算的 lifecycle 周期数；ARM64 从核在定时器和 IRQ 尚不可用的 Active 握手阶段用 `cpu_yield()` 轮询，不再依赖一次可能无法唤醒 QEMU 11.1.1 多线程 TCG 宿主线程的 SEV/WFE；发送侧只保留与 acquire 轮询配对的 release 状态发布，旧 DSB/SEV 已没有睡眠消费者并被删除。CTest 配置在 configure 时要求并解析 `gdb-multiarch` 或 `gdb`，当前工作区选择 `/usr/local/bin/gdb`，不再硬编码 Linux 包名。没有关闭测试、放宽 deadline、改成单线程 TCG 或把启动 panic 当成预期 self-test panic。

三个红项先单项通过，随后九个配置重新 configure/build 并串行完成 **43/43 CTest**：

| preset | CTest | 总时长 | 最终 functional 报告（171 case） |
| --- | ---: | ---: | --- |
| arm64-debug | 5/5 | 98.76 s | `1789817091021342000` |
| arm64-release | 5/5 | 71.34 s | `1789817203567493000` |
| arm64-relwithdebinfo | 4/4 | 63.75 s | `1789817294018303000` |
| x64-debug | 5/5 | 105.71 s | `1789815900880040000` |
| x64-release | 6/6 | 79.80 s | `1789816020589431000` |
| x64-relwithdebinfo | 5/5 | 69.46 s | `1789816137722556000` |
| riscv64-debug | 4/4 | 117.06 s | `1789816220220585000` |
| riscv64-release | 5/5 | 72.12 s | `1789816361313349000` |
| riscv64-relwithdebinfo | 4/4 | 64.80 s | `1789816456994123000` |

每份 functional 报告均为 23 套件、171 case 全部通过且无失败 guest；Release 的 13 项 benchmark、x64 的 PVH/initrd、普通 production boot 和 ARM64 Debug 受控首读也全部通过。最终受控首读证据为 `run-3p24rz28`，11 个 shell/exec/wait 步骤完成。修改后的宿主回归为 runner **126/126**、硬件与 production probe **19/19**；Ruff、修改行 ClangFormat 和 `git diff --check` 通过。

启动轮询只关闭此次可复现的 CTest 前进性错误，不代替 ARM 实机覆盖、启动压力循环或 MOSS-017/018 的完整调度/等待验收。九 preset 的普通通过也不满足 30 分钟稳定性、性能门禁、网络或持久存储范围。

## 4. 问题总表与当前状态

| 编号 | 优先级 | 审计主题 | 当前状态与下一步 |
| --- | --- | --- | --- |
| MOSS-001 | P0 | 内核映射 USER / W^X | 部分修复（工作区）；U/S、三架构最终内核 W^X/RO/NX、直映别名与共享页表归属已修复并加入默认检查。用户违规访问的受控异常隔离验收仍未完成，见 3.11～3.12。 |
| MOSS-002 | P0 | 用户域 / uaccess / 调试旁路 | 部分修复（3.13、3.20～3.21）；统一准入、共享异常 fixup、VFS/信号迁移及输入/输出/跨页 OOM 回归已补；COW OOM 和页/VM 生命周期未完成。 |
| MOSS-003 | P0 | 信号帧与特权状态恢复 | 部分修复（3.19、3.21）；原生帧、地址/状态净化、安全复制及写出/读回 OOM 已补；恶意帧/altstack 全验收未完成。 |
| MOSS-004 | P0 | 堆与页表/PFA 所有权重叠 | 已关闭（`040d773`）；当前布局、活动页表树/早期表池及元数据哨兵、耗尽、坏布局启动拒绝专项验收通过，见 3.7。 |
| MOSS-005 | P0 | 启动保留区未排除 | 多 bank、保留洞、非对齐/重叠、容量/溢出及耗尽已验证；低地址启动区仍保守保留，PVH 异常表/完整保留集合待补，见 3.5～3.6。 |
| MOSS-006 | P0 | 容器与使用者的所有权 | 部分修复（工作区）；伪 RCU 已替换为 LockedList/LockedHashMap，查找复制拥有者，锁外析构/快照回调；持有读者与双 CPU 交错有实测，IRQ/驱动/IPC 复合协议未闭合，见 3.9。 |
| MOSS-007 | P0 | 跨 ISA TrapFrame / syscall 参数 | 部分实现（3.18～3.19）；有效原生帧、布局断言、六参数/GP/条件码与 native sigreturn 已通过九配置；全异常交错及完整扩展状态仍待验收。 |
| MOSS-008 | P0 | 只读页被 COW 放宽权限 | 部分修复；clone 保留真实只读页，fault 检查可写 VMA，多代 COW/用户异常回归通过；并发事务与 OOM 验收待补，见 3.16。 |
| MOSS-009 | P1 | RV64 fault 分类 / PPN | 部分修复；访问类型与驻留检查、COW 优先和 HAL PPN 编码已补，Sv39/Sv48 回归通过；分配/引用失败专项仍待补，见 3.16。 |
| MOSS-010 | P1 | 页表 clone/map 回滚 | 部分修复（3.17）；map/clone 表页准备失败不改原树和数据页引用，clone 返回结果并由 fork 检查，三架构逐点 PFA 耗尽通过；完整 fork 后续失败事务与并发锁待补。 |
| MOSS-011 | P1 | 活跃 ASID 回绕重用 | 已关闭（工作区，3.23）；加锁的 1…255 活跃租约、耗尽错误、析构归还和 ARM64 广播失效已核对，内核边界测试及同 VA/跨 CPU 的 300 次用户复用在六配置通过。 |
| MOSS-012 | P1 | brk/VMA/PTE 权限生命周期 | 已关闭（工作区，3.24）；空堆、缩堆 PTE/ref/TLB 撤销、重新增长清零、增长冲突回滚及权限拒绝已在六配置通过；部分 munmap 与共享地址空间并发仍明确留在范围外。 |
| MOSS-013 | P1 | 对齐 / buddy / 释放契约 | 已关闭（`040d773`）；heap/PFA 对齐、释放归属、保留洞分段/耗尽专项验收通过；页引用并发不在此结论内，见 3.4～3.5。 |
| MOSS-014 | P1 | fork 用户现场与继承状态 | 部分实现；三 ISA 实际用户帧 GP 复制、ARM64 x30 和 x86 CF 修复有不立即 exec 的回归（3.19）；VM/凭据/全部 FP/TLS 继承及失败验收仍缺。 |
| MOSS-015 | P1 | exec 原子替换 | 已关闭（工作区，3.25）；全部可失败准备均在旧地址空间仍有效时完成，提交后才回收旧所有权；PFA 与六级 heap 失败、可变 ELF 快照及 FD 语义已在六配置通过。 |
| MOSS-016 | P1 | ELF 校验与分段策略 | 已关闭（工作区，3.26）；不可变 LoadPlan、checked arithmetic、严格拒绝 oracle、17 个畸形映像，以及非页对齐 RX/RW 的实际字节/零填充/最终权限已在六配置通过。 |
| MOSS-017 | P1 | 运行队列 / 迁移 / on-CPU | 部分修复（工作区）；三 ISA yield 真切换、退出通知共享原子唤醒已回归；pick/dequeue、迁移及 on-CPU/check_need_resched 仍未闭合，见当前验收记录。 |
| MOSS-018 | P1 | wait/console 丢失唤醒 | 部分修复（工作区）；wait 的 status EFAULT 可重试，选项/PID 边界已回归；条件检查与等待登记仍分离，丢失唤醒和信号中断未关闭。 |
| MOSS-019 | P1 | nanosleep / timer 生命周期 | 部分修复（工作区）；三 ISA 切换、先 Sleeping 后 arm、容量失败及同步取消已回归；跨 CPU 交接与信号中断仍未闭合。 |
| MOSS-020 | P1 | 信号投递 / STOP/CONT/SIGCHLD | 部分修复（3.19）；统一用户返回检查点、结果/handler 参数写回顺序及终止信号编号已补；CPU-bound、STOP/CONT/SIGCHLD 和阻塞中断仍待闭合。 |
| MOSS-021 | P1 | 信号状态生命周期 | 已关闭（工作区，3.23）；状态由 `Process` 拥有，fork/exec/exit 规则已核对，继承/重置及跨旧 256 槽边界的 300 次生命周期在六配置通过。 |
| MOSS-022 | P1 | 退出 FD 关闭 | 实现已修复、验收部分（工作区，3.23）；退出在 Zombie 前 close-all、析构兜底，EOF-before-wait 与 1,000 次资源恢复通过五个默认配置；RV64 Debug 默认 30 秒仍超时，60 秒诊断通过，故不关闭。 |
| MOSS-023 | P1 | 退出换栈 / 连续执行停滞 | 部分实现；已复用 context_switch 返回 bootstrap 栈；旧停滞根因对照及 1,000 次验收仍缺。 |
| MOSS-024 | P1 | VFS 访问模式 / 引用 / offset | 未修复；do_read/do_write 直接调用 f_ops，共享 File/池并发契约未补。 |
| MOSS-025 | P1 | pipe 阻塞与 EOF | 未修复；空但有 writer 的 read、满 write 仍返回 0，断端仍 InvalidArg。 |
| MOSS-026 | P1 | 固定池回收与 pipe 回滚 | 未修复；inode 单调计数、端点失败清理及用户输出失败 FD 回滚仍需处理。 |
| MOSS-027 | P1 | x86 PVH / initramfs | 已关闭（工作区，3.22）；真实模块表/可用 RAM 范围、严格 newc、完成标记顺序及七场景生产 QEMU gate 已通过 Debug/Release。 |
| MOSS-028 | P1 | 真实内核测试与失败传播 | 框架已替换，覆盖待补；生产模块、串口协议及三架构自检已落地，T01～T12 不能整体关闭。 |
| MOSS-029 | P2 | 计时源和 ISA 能力 | 部分实现；x86 双频率校准、RV64 SBI TIME 已落地；3.8 新暴露验证镜像一次校准拒绝，具体分支未定位，异常能力与时钟误差验收仍待补。 |
| MOSS-030 | P2 | 空成功 / 固定地址 / 假统计 | 未修复；`mm_interface_impl.cpp` 与 `ipc.cppm` 仍有固定结果/空成功及提前 Ready。 |
| MOSS-031 | P2 | 核心边界与 ABI | 部分边界改善；启动/硬件/runner 已拆分，uaccess/TrapFrame/进程事务和共用 ABI 仍待收敛。 |
| MOSS-032 | P2 | 文档 / 状态 / 统计一致性 | 文档部分已更新；能力矩阵及历史/当前证据已分开，启动假成功已随 3.22 修复，假统计和完整验收仍未关闭。 |

004/013 已按所列专项验收和修复提交关闭，011/021 已按 3.23 的当前实现复核和六配置专项验收关闭，012 已按 3.24 的红绿回归与六配置专项验收关闭，015 已按 3.25 的准备/提交事务、逐级真实压力与六配置验收关闭，016 已按 3.26 的 LoadPlan、畸形输入和边界页语义验收关闭，027 已按 3.22 的工作区修复和专项验收关闭；其余包含未完成验收的整项仍保持打开，有限子任务在第 11 节单独勾选。以下保留原始问题细节，避免修复后丢失回归依据。

## 5. 隔离与基础内存

### MOSS-001 · x64 内核页表允许用户态访问内核映射

**当前状态：** U/S 构造、三架构最终内核 W^X/RO/NX、直映别名及共享页表归属已修复，生产结构/生命周期检查见 3.11～3.12；整个 MOSS-001 尚未关闭，受控用户异常隔离的完整验收仍待完成。MOSS-002 的用户域/uaccess 旁路不因本项修复而关闭。

**原始审计证据（修复前）：** `src/hal/mmu/src/mmu_hal.cppm:392` 的 device block 和 `:410` 附近的 normal block 构造包含 `USER | WRITABLE | HUGE_PAGE`；`src/mm/src/mm-page_table.cppm:76` 的 x86 表项也传播 USER/WRITABLE。`src/process/src/process.cpp:276` 创建用户页表时复制低地址的内核 identity blocks。检查到的内核 PUD 块值包括 `0xe7`，上级表项也允许用户访问。

**原始影响：** 硬件权限链允许 CPL3 访问同一映射中的内核 RAM、代码/数据、页表或 MMIO。这里的问题是页表 U/S 权限错误，不是只有侧信道威胁模型下才需要讨论的 KPTI。原审计时 x86 用户态启动还有 MOSS-027 阻塞；当时没有执行 CPL3 攻击程序。

**修复：**

- 内核叶子映射必须为 supervisor-only；审计所有上级表和叶子，不要只改启动期临时页表。
- 将内核 text、rodata、data、MMIO 权限分别表达；可执行代码 RX，数据 RW 且 NX，避免整个大块可写可执行。
- 在页表构造接口区分用户映射与内核映射，调用者不能用默认属性意外获得 USER。

原 `make_normal_block()` 的 ARM64 大块未按 text/data 拆分权限，RISC-V 64 同时设置 READ/WRITE/EXECUTE；该 helper 已删除。3.12 先把用户页表创建/clone/free 改为按 VA 范围借用内核映射，再拆分最终页表；早期临时宽权限不再成为最终运行权限。用户 huge page 生命周期、COW 权限和分配失败事务仍属待办，不因共享内核子树的创建/回收通过而关闭。

**验收：** x86 用户态恢复后，用户程序读取/写入内核 text、data、页表和 MMIO 地址均产生可控用户异常，只终止该进程；合法用户页仍可读写。增加最终页表的 U/S、RW、NX 结构检查。权限语义参见 [Intel SDM Volume 3A](https://cdrdv2-public.intel.com/874249/253668-090-sdm-vol-3a.pdf)。

### MOSS-002 · 用户地址合法性不等于“找到一个 VMA”

**当前状态：** 第 3.13 节已补地址域、VMA 准入/跨段权限检查和字符串长度错误，删除 syscall 0 先 puts 后分发的旁路；3.20～3.21 补共享复制异常 fixup、VFS/信号迁移、输入/输出缺页 OOM 和跨页部分 I/O 回归。COW OOM、全部故障覆盖与 VM 生命周期未完成，整项不关闭。

**原始位置与事实（准入修复前）：** `src/kernel/src/syscall_table.cpp:56` 的 `validate_user_range()` 主要检查溢出及一个 VMA 的覆盖/标志；`:89` 和 `:104` 的复制为直接访存。`:1657` 的 `sys_mmap()` 接受用户给出的地址，但没有完整限制到架构用户地址域。`src/process/src/process-types.cppm:288` 的 `add_vma()` 没有承担地址域、有效区间等统一约束。

更直接的旁路在 `src/kernel/src/kernel_main.cpp:133`：系统调用 0 在正常 dispatcher 校验前，使用参数指针调用 UART `puts()`。因此，即使修复 `sys_debug_print()` 的普通 handler，这条入口仍可读取内核地址或因坏指针故障。

**影响：** 用户可以使一个内核可访问区间被软件登记成“用户 VMA”，随后通过内核态 copy 路径访问它；这不要求对应页对 EL0/CPL3 直接开放。已有内核 identity mapping 与任意 mmap hint 的组合尤其危险。跨两个合法相邻 VMA 的缓冲区又可能被当前单 VMA 检查错误拒绝。

**修复：**

1. 定义每个架构的 `UserAddressRange`：含规范地址约束、上下界、保留洞、内核 identity 区和 trampoline 保留区；所有创建/修改 VMA 的入口调用同一检查。
2. 对加法、对齐、长度取整做溢出检查；拒绝不支持的 flags，明确 hint 与 fixed 的语义。
3. 删除入口的未校验调试输出旁路；所有用户字符串采用有上限的复制。
4. 建立可恢复的 `copy_from_user` / `copy_to_user`，覆盖完整跨页/跨 VMA 区间，使用异常 fixup 返回 EFAULT。不能用“可能杀进程或 panic 的普通 load/store”冒充可失败复制。
5. 将页表是否存在、VMA 是否允许、地址是否属于用户域区分开：合法尚未驻留的用户页可以按策略缺页；已有 PTE 不得绕过用户地址域和权限策略。

**验收：** 对空指针、非规范地址、内核地址、末端溢出、跨页未映射、只读输出缓冲区、合法跨 VMA 缓冲区逐项验证。尝试先 mmap 内核地址再 copy 必须失败；错误用户指针不能导致全局 panic。

### MOSS-003 · 信号帧是用户输入，不能直接恢复为特权现场

**2026-09-14 更新：** 原生帧、地址/状态净化及基本信号返回已实现（3.19），安全复制和帧写出/读回 OOM 已补（3.21）；下述原始风险不再等于当前仍能直接恢复任意特权状态。恶意/备用帧完整验收仍待完成。

**位置与事实：** `src/process/src/signal.cpp:128` 的 `setup_sigframe()` 用原始 `u64*` 解释陷阱帧，取 `[33]` 为用户 SP，直接 `memcpy` 写信号帧；备用栈地址加法、下减帧大小和可写性未完整校验。`:208` 的 `do_sigreturn()` 直接读取用户帧，只校验 magic，就恢复 PC、SPSR、SP 和通用寄存器。`src/boot/src/arch/arm64/start_arm64.S:894` 至返回段将这些值装入 ELR_EL1/SPSR_EL1 后 ERET。`src/kernel/src/syscall_table.cpp:1454` 的 `sigaltstack` 检查最小尺寸，未充分验证地址域和溢出。

**影响：** 用户可修改公开布局中的返回状态。magic 只能帮助识别格式，不能证明内容可信；用户可提供指向内核可写区的备用栈，或构造非 EL0 的返回状态。是否能直接执行用户页还受 PXN 等控制，但这不能阻止选择已有内核可执行地址，所以不能据此认为特权返回已安全。本次未执行提权利用。

**修复：** 先用 MOSS-002 的 uaccess 将帧完整复制到内核临时对象，检查后再一次性提交：PC/SP 属于有效用户域、对齐合规、返回模式强制 EL0、特权/中断控制位只能采用内核允许的状态；恢复掩码清除不可阻塞信号。备用栈的完整区间、溢出、帧空间和嵌套状态也必须验证。每个 ISA 的允许状态由该 ISA 实现，禁止共用 ARM64 数组下标。

**验收：** 伪造特权状态、越界 PC/SP、内核地址备用栈、只读/未映射栈、整数溢出、错误 magic 均只能导致明确错误或终止当前进程；内核哨兵内存不改变；合法嵌套和备用栈往返保留现场。ERET 恢复异常级别和状态的依据见 [Arm ERET 文档](https://developer.arm.com/documentation/100069/0606/General-Instructions/ERET?lang=en)。

### MOSS-004 · 运行时堆占用超过真正预留的内存

**2026-09-06 更新：已关闭，修复提交 `040d773`。** `44dedc2` 将三个 `src/linker/kernel_*.ld` 的 heap 改为 8 MiB NOLOAD 预留；`RuntimeHeapAllocator::initialize_heap/expand_heap` 以 linker `heap_end()` 为上限，检查越界/回绕。现有 256 KiB 起始 arena 在预留区内，扩容不再吞并 PFA 页面。第 3.7 节补充区域间启动约束、真实元数据位置、活动页表树/early pool/链接表区的校验和以及重叠堆末端的负向镜像；六配置和多 bank 固件输入均已验证。原 4 KiB/256 KiB 重叠和下列地址仅属历史；大数 size/alignment 的专项结果见 MOSS-013。

**后续工作区修复：** 第 3.4 节覆盖预留堆耗尽、块内模式、独立 PFA 页哨兵和页计数；3.7 补全当前单 worker 下的区域布局、页表/元数据哨兵和失败启动检查。不将这些结果推广为多进程/SMP 引用正确性；完整启动保留集合仍属 MOSS-005。

**位置与事实：** 三个 BootImpl 分别在 `src/boot/src/arch/arm64/boot_impl.cpp:682`、`src/boot/src/arch/riscv64/boot_impl.cpp:260`、`src/boot/src/arch/x64/boot_impl.cpp:330` 用 `_heap_start` 初始化 256 KiB 堆。ARM64 链接脚本 `src/linker/kernel_arm64.ld:187` 只为 heap 保留 4 KiB，之后紧邻页表预留区。`src/mm/src/page_frame_allocator.cpp:178` 把 PFA 元数据放到 `_kernel_end` 后。

本次 ARM64 debug ELF 的实际符号：

```text
_heap_start                     0x403ec000
_heap_end / _pagetable_start    0x403ed000
_kernel_end / _pagetable_end    0x403fd000
运行时初始堆声称的结束地址       0x4042c000
```

因此，运行时堆声称拥有的 `[0x403ec000, 0x4042c000)` 确定覆盖链接保留的页表区域，并继续跨入 `_kernel_end` 后的 PFA 元数据范围。这证明所有权重叠，不表示这些字节在启动瞬间已全部被覆盖。

`src/mm/src/runtime_heap_allocator.cpp:22` 还设置连续增长上限；`:154` 的扩容调用 `:225` 的 `map_heap_pages()`，后者主要检查地址低于 4 GiB 和对齐便返回成功，没有从 PFA 取得页面，也没有建立相应的独占映射。

**修复：**

- 第一步先使链接脚本、BootInfo 保留区、PFA 初始化和初始 heap 大小一致。最小安全版本可以使用明确预留、不可扩容的静态 arena，并在耗尽时正确失败。
- 后续动态扩容只能取得有所有权的 PFA 页面，再映射到专用内核堆虚拟区；不得直接把后续物理地址视为可用堆。
- 页分配失败、映射失败均回滚；释放只能归还堆真正取得的页；把内存布局检查加入启动诊断。

**验收：** 初始堆、所有页表、PFA 元数据、可分配物理页区间两两不重叠；堆跨过当前 4 KiB、64 KiB、256 KiB 边界时页表/PFA 哨兵保持不变；耗尽返回失败而不是吞并邻接区域。修复此项前，长时间压力结果可能被基础内存破坏污染。

### MOSS-005 · 物理内存需要保留区集合，不能只用 kernel_end 切一刀

**2026-09-06 更新：部分验收。** `44dedc2` / `6252484` 已在 `platform.cppm::PlatformInfo`、`fdt.cppm`、PVH 解析和 `PageFrameAllocator::parse_memory_layout/initialize_free_lists` 传递实际 RAM、DTB/固件保留信息；metadata 放置及自由块发布会绕过 initrd/reserved ranges。第 3.5 节工作区补齐真实耗尽、保留洞/非对齐/重叠、initrd 校验和与极值区间边界；3.6 进一步联合管理 kernel_end 以上的合格 RAM bank，覆盖乱序多段、相邻合并和后续 bank 安放元数据，当前布局与元数据/页表保护检查见 3.7。低地址启动数据尚未全部显式保留，暂不回收 kernel_end 以下；完整保留集合、回收及 x86 PVH 异常表仍待补。

**位置与事实：** `src/mm/src/page_frame_allocator.cpp:178` 至初始化区域构造路径以 RAM 起止、内核末端和元数据计算主要可分配区，没有统一扣除 initrd、DTB、固件/OpenSBI 以及其他 reserved ranges。现有 initramfs 文件还依赖启动镜像中的后备数据。

**影响：** 在分配压力下，仍被使用的启动数据可能进入普通页分配池；零拷贝 ELF/文件后备页可能被其他内核对象或用户页覆盖。单次 shell 演示不一定触及这些物理页，因此不能反证正确。

**修复：** BootInfo 提供标准化的 usable/reserved 区间集合；合并、裁剪后从 usable 中扣除 kernel、初始堆、页表、PFA 元数据、DTB、initrd 和固件区域。多段 RAM 和洞必须被保留，不得用总大小推导成一个连续区。initrd 只有在所有后备引用解除或内容复制完成后才能归还。

**验收：** 构造多段内存、区间重叠、非页对齐、RAM 内 initrd 和保留洞；枚举分配至耗尽，任何保留页都不得被返回；运行加载程序时给其他页写模式，文件后备数据保持不变。

### MOSS-006 · 当前 RCU 容器既没有安全退休语义，也没有宽限期

**2026-09-06 更新：部分修复，工作区。** `4cde9b3` 的指针发布修复见 3.8；当前已删除 RcuList/RcuHashMap、读锁和回调池，使用 IRQ-safe LockedList/LockedHashMap，查找返回值副本或 shared_ptr。VMA、进程、WaitQueue、IRQ、驱动和 IPC 调用者已迁移；析构与可能重入的快照回调在解锁后执行，3.9 保留持有读者的红绿对照及真实双 CPU 交错证据。下列旧 RCU 源码是原始审计事实，不再代表当前实现；所有使用者的复合生命周期尚未验收，本项和 A4 仍不关闭。

**位置与事实：** `src/containers/src/containers.cppm:1075` 的 `RcuPtr::store_rcu/compare_exchange_rcu` 对替换下来的指针自动排队删除。`:1129` 的 `RcuList::push_front()` 把旧 head 接到新节点 next 后替换 head，于是**仍通过 next 可达的旧 head 被安排删除**。CAS 重试中对 next 的替换也混淆了发布与所有权释放。

`:937` 的固定回调池耗尽后直接处理回调，处理路径不等待读侧宽限期，极端情况下还同步执行删除。`:1007` 的读锁主要维护每 CPU 深度/屏障，没有证明所有旧读者退出的机制；查找还可能在 guard 析构后返回裸指针。检查到的运行路径没有正常的按宽限期回收闭环。

**影响：** 可达节点被释放、读者访问已释放对象、多个指针位置对同一对象重复承担删除责任；扩大回调池或增加定时 drain 只能改变触发时间。**不能先加周期性 drain 来“补齐 RCU”**，这会更早执行错误退休。

**修复：** 当前推荐使用锁保护的拥有型链表/映射，明确容器拥有节点、查找者借用的有效期和删除位置。原子指针只负责发布，不自动拥有被替换对象。如果未来确需 RCU，再单独实现 unlink → 等待已存在读者退出 → reclaim，并定义写者串行化、CPU 迁移及引用外逸策略。参考 [Linux RCU 基本契约](https://cdn.kernel.org/doc/html/latest/RCU/whatisRCU.html)。

**验收：** 连续插入 A/B/C 后全部可达且无删除；删除一次只析构一次；读者持有期间不可回收；超过 512 次发布/退休仍正确；并发增删查有可重复交错测试。修复需覆盖所有使用该基础容器的拥有者，不能只修改一个调用点。

### MOSS-007 · 陷阱帧新增参数没有贯穿三个架构

**2026-09-14 更新：** `0e88344` 已将三 ISA 改为原生帧单指针入口并完成 3.19 的寄存器/信号正常路径验收；下面的第八参数/ARM64 共用数组描述仅保留为历史根因。

**2026-09-06 更新：部分实现，入口契约仍未完成。** x86 已显式压入第八参数，但值是 `$0`，不是 TrapFrame；RV64 入口恢复 CPU 身份/用户 gp/tp 的代码已补，调用 dispatcher 前仍仅安排 a0～a6。`signal.cpp` 没有改成按 ISA 的帧访问。基础 syscall 和 fork/exec 通过不能关闭信号/完整寄存器返回任务；不要再把“x86 少压一个参数”当唯一剩余修复。

**位置与事实：** `src/abi/src/abi.cppm:84` 和 `src/kernel/src/kernel_main.cpp:133` 的系统调用处理入口已有第八个 `trap_frame` 参数。ARM64 入口 `src/boot/src/arch/arm64/start_arm64.S:876` 传入当前 SP；RISC-V 64 的 `src/kernel/src/arch/riscv64_syscall.S:417` 只安排 a0–a6，没有把 a7 改成帧指针；x64 的 `src/kernel/src/arch/x64_syscall.S:91` 只安排第七个栈参数，缺少第八个参数。

与此同时 `src/process/src/signal.cpp:136` 一律把该参数解释为 ARM64 的 34 个槽位。RISC-V 64 信号测试在读取这里时出现 `stval=0x116`，与错误参数被当成指针的路径吻合。

**修复：**

- 为每个 ISA 定义自己的 `TrapFrame` 和编译期偏移校验；汇编入口保存完整用户状态，准确满足 C 调用约定、栈对齐和全部参数传递。
- `SwitchContext` 仅表示内核调度切换现场，不能代替用户异常现场。
- 共同代码通过 `user_pc()`、`user_sp()`、`set_syscall_result()`、`sanitize_user_return()` 等小接口操作，布局转换由架构层承担。
- 信号 trampoline、sigreturn 以及 fork 用户返回都按 ISA 实现。当前 exec 中写入的 ARM 指令序列不能在 RISC-V 64/x86 被当作通用信号返回桩。

**验收：** 每架构用全部参数和哨兵寄存器往返系统调用；触发信号前后验证通用寄存器、PC/SP、返回值、合法状态位；RISC-V 64 basic signal 不再读取低地址假指针；x86 缺失栈参数有结构性回归检查。

## 6. 虚拟内存、分配器与加载

### MOSS-008 · COW 不能把原本只读的页变成可写

**位置与事实：** `src/mm/src/page_table.cpp:196` 的 `clone_user_page_tables()` 对有效叶子统一设置 COW 并清写权限，没有先区分原本只读与可写私有页。`src/mm/src/page_fault.cpp:346` 的 `try_cow_fault()` 主要看 COW 位，成功后给页恢复可写，没有先检查 VMA 是否允许写。

**影响：** fork 后向代码或只读数据写入可能被解释为合法 COW，从而绕过原本的只读保护。这与“COW 是否节约内存”无关，是权限提升问题。

**修复：** 仅对原本可写且允许私有复制的映射建立 COW；原本只读页保持只读共享。故障入口携带读/写/执行类型，COW 仅接受合法私有 VMA 的写保护故障；页引用计数、PTE 更新和 TLB 失效作为同一受锁保护的状态变更。

**验收：** fork 前后写 text/rodata 都失败；写私有数据只改变写入方；多代 fork、引用计数为 1 的快速路径、同时写故障及分配失败均不放宽只读权限。

### MOSS-009 · RISC-V 64 缺页必须区分“没有页”和“已有页但权限不符”

**位置与事实：** `src/mm/src/page_fault.cpp:604` 的 RISC-V 64 路径先尝试 demand paging，再尝试 COW。demand 路径并不以“叶子不存在”为充分前置条件，可能重新分配/填充页面；`src/mm/src/page_table.cpp:458` 的 `map_user_page()` 又允许覆盖已有项。`:346` 的共用 COW 路径在替换物理地址时直接把 `new_pa & PTE_ADDR_MASK` 写入 raw PTE，但 RISC-V 64 PPN 编码与 ARM/x86 的地址字段不同。

**影响：** COW 写故障可能先被当作首次映射，丢失已修改的栈/数据并漏掉旧页引用；错误 PPN 会指向错误物理地址。执行权限故障如果被重复当作 demand fault，还可能反复分配而不消除真正原因。

**修复：** 根据异常原因和页表 walk 结果生成明确的 `FaultInfo`：访问类型、来源特权级、页不存在或权限错误。仅缺页进入 demand；合法 COW 写故障进入 COW；其他权限错误终止用户访问。所有 PTE 编解码调用对应 HAL 构造函数，禁止在共用代码手工拼某一 ISA 的地址位。

**验收：** 已修改匿名页 fork 后分别写入仍保留原内容；执行 NX、写 RO、读 PROT_NONE 不新增替代页；PTE 编解码对多个物理地址双向一致；旧页引用在覆盖/失败路径正确变化。编码及访问检查依据见 [RISC-V 64 Supervisor 规范](riscv64_SUPERVISOR_DOC)。

### MOSS-010 · 页表克隆失败不能以“部分成功”发布子进程

**位置与事实：** `src/mm/src/page_table.cpp:196` 的克隆接口返回 void，分配失败只能提前退出当前路径；调用者无法可靠获知未复制完整的子页表。过程中还已经修改父 PTE/COW 和引用计数。`src/mm/src/page_table.cpp:458` 的映射路径也没有完整地区分中间 block、table、空叶子和替换叶子。

**影响：** fork 可以成功返回一个缺失映射的子进程；半初始化页表、错误引用和父页权限修改难以回滚。把有效 block 当 table 继续走还可能把普通映射内存解释成页表。

**修复：** `clone_address_space()` 返回显式 Result 和尚未发布的地址空间；记录新分配表页、取得的页引用及父权限改动，失败统一回滚。walk 返回类型化结果，遇到 block 时显式拒绝或拆分；映射默认要求空叶子，替换必须使用承担旧页释放/TLB 责任的专用操作。

**验收：** 在第 1…N 次页分配逐点注入失败，fork 返回 ENOMEM，父进程仍可读写原数据，页/引用计数回到基线；不出现半初始化可运行子进程。对 block/table 冲突有拒绝或正确拆分用例。

### MOSS-011 · ASID 是地址空间租约，不是可以直接回绕的计数器

**2026-09-19 更新：已关闭（工作区 3.23）。** 当前实现已改为加锁的活跃租约表、显式耗尽、地址空间析构归还和 ARM64 广播失效。`asid_leases` 的 255 个活跃标签边界以及用户态 300 次同 VA/CPU0—CPU1 复用在当前三架构 Debug/Release 均通过。下面是原始审计事实与修复要求，不能再当作当前实现描述。

**原始位置与事实：** `src/process/src/process.cpp:256` 用 1…255 分配 ARM64 ASID，回绕执行本地 `tlbi vmalle1` 后从 1 重用。它不证明旧 ASID 的拥有者已经退出；其他 CPU 的 TLB 也不由该本地失效覆盖。

**影响：** 两个仍存活地址空间可得到相同 ASID；旧进程之后仍能重新填充该标签的 TLB。即使回绕瞬间全局 flush，也不能单独解决存活拥有者冲突；并发回绕还需额外序列化。

**修复：** 最小实现使用有锁的活跃 ASID 分配/释放表，直到地址空间销毁才归还，并在重用前完成所需 CPU 的失效；资源耗尽明确失败。若需要超过硬件标签容量的并发地址空间，再采用 generation 与每 CPU 激活协议，不要只放大计数器。

**验收：** 保持多个进程存活，累计创建超过 255 个地址空间；同一 VA 放不同模式并反复切换、迁移，内容不能串扰；销毁/重用及并发分配无重复活跃租约。

### MOSS-012 · VMA 修改必须同步改变页表和访问能力

**原审计位置与事实（修复前）：** `src/kernel/src/syscall_table.cpp:1782` 的 brk 缩小主要修改 VMA 端点，没有同步解除已驻留 PTE、引用和 TLB；增长没有完整的相邻映射冲突事务。demand fault 对读/执行限制的表达也不完整，例如 flags 为零的 PROT_NONE 区仍可能得到可读映射。

**影响：** 缩小后已映射页仍可访问，重新增长可能看到旧内容；VMA 与硬件实际可访问范围不一致。VMA 是授权策略，不应成为与页表互不约束的记录。

**修复：** 集中实现 VMA 与页表的 map/unmap/resize 操作，检查相邻区间、页边界和访问类型。brk 释放完整离开有效范围的页，更新引用并完成 TLB 失效；部分页按已定义的 ABI 处理。PROT_NONE 不可通过 demand paging 获得可读页。

**边界：** 当前 munmap 只处理完整匹配 VMA 可以作为明确声明的研究内核限制；如果尚不支持部分拆分，应明确拒绝，不必为追求 POSIX 名称一次实现复杂合并树。但任何声称成功的操作必须与实际访问能力一致。

**验收：** 缩小跨页后访问释放页故障，重新增长按约定初始化；增长撞到 mmap/stack 被拒绝；NONE/RO/RW/RX 的读写执行组合逐项验证；失败不部分改变 VMA。

**2026-09-19 工作区更新：已按当前模型关闭。** 3.16 已让 demand fault 在分配前按读/写/执行检查 VMA，覆盖只读 COW、PROT_NONE 与 NX；3.24 又把固定空堆、整页缩堆的 PTE/TLB/引用撤销、重新增长清零及 mmap 冲突无部分提交做成红绿回归，并在三架构 Debug/Release 通过。部分 munmap 仍明确拒绝，页内 brk 缩小保留所在页；共享地址空间并发事务不在本项关闭范围内。

### MOSS-013 · 分配器对齐与伙伴系统边界需要成为显式不变量

**2026-09-06 更新：已关闭，修复提交 `040d773`。** `PageFrameAllocator::initialize_free_lists` 循环选择符合物理地址对齐、剩余长度和保留区约束的 order。heap 修复及五项用例见 3.4；PFA 原分配头/order、所有页的引用/标志、完整范围检查及两项真实套件见 3.5。六配置 CTest 与 ARM64/RV64 固件边界检查已通过本项所列对齐、错误释放和保留洞后的分段/耗尽验收。此结论不覆盖页引用 API 自身的并发/下溢或完整 COW 生命周期；那些仍需 008～010/028 验收。

**位置与事实：** `src/mm/src/runtime_heap_allocator.cpp:57` 的 aligned allocation 主要对尺寸取整，却返回 header 后的地址，不保证返回指针满足大于 header 对齐的请求，也未完整检查对齐是否为 2 的幂和算术溢出。`src/mm/src/page_frame_allocator.cpp:238` 初始化空闲块遇到不对齐时只降一次 order，不能保证任意起始 PFN 对该 order 对齐；`:120` 的释放检查不足以证明地址、order 与原分配完全匹配。

**影响：** 页表、SIMD 或其他对齐对象可获得错误地址；伙伴 XOR/合并逻辑建立在无效块边界上；错误释放可破坏相邻分配。

**修复：** 对齐的是用户返回地址，保存可恢复原块的元数据；所有 size/align 运算检查溢出。初始化空闲区时循环选择满足起始对齐与剩余长度的最大 order；释放验证分配头、order、范围和状态。内部可信快速路径也应有 debug 断言和清楚的调用契约。

**验收：** 覆盖 8/16/64/4096 等对齐，非法对齐与大数溢出；非大块对齐的 RAM 起始、保留洞后的分段、拆分/合并至耗尽；重复释放和错误 order 被检测，不能污染空闲链。

## 7. 进程、调度、阻塞与信号

### MOSS-014 · fork 应复制用户执行状态，不应复制再清空内核切换现场

**2026-09-07 工作区更新：** x86 legacy FP 的实时 fork 快照、切换恢复和 exec 默认化已补，限定状态及红绿证据见 3.14。完整继承表、其他 ISA 扩展状态、信号及 OOM 回滚仍未完成。

**2026-09-06 更新：部分实现。** `44dedc2` 的 `sys_fork` 已提取 x86 PC/SP、RV64/x86 GP 寄存器；首次运行先在新内核栈保存完整 CpuContext，再经架构 trampoline 返回用户态。三架构 `validation.c` 的一次 fork/exec/exit/wait 已通过。尚需 fork 不立即 exec 的快照验证、VM 元数据/凭据/信号继承、FP/SIMD 状态与资源失败回滚；共用 syscall 仍手写架构槽位。

**位置与事实：** `src/kernel/src/syscall_table.cpp:238` 的 fork 初始化用户 PC/SP 为 0；ARM64、RISC-V 64 有部分提取，x86 路径没有补齐用户 PC/SP。RISC-V 64/x86 的部分处理复制内核保存上下文，但 `src/process/src/process-scheduler.cppm:1874` 的首次运行路径重新清空上下文并只重建部分参数寄存器。这不是对父用户寄存器现场的完整克隆。

fork 对 VMA 有复制，但未完整继承 `brk_base/brk_current/mmap_next` 等 VM 状态，以及凭据、信号动作/掩码/备用栈。ARM64 的通用寄存器路径更完整，也仍需核对 FP/SIMD 等扩展状态的复制与首次使用语义。

**影响：** 子进程的 PC/SP、调用保存寄存器或堆状态可能不符合父进程观察到的 fork 语义；x86 子进程可能从 0 地址开始。当前 shell 的“fork 后立即 exec”会掩盖许多寄存器和进程属性继承错误。

**修复：** 在 MOSS-007 统一入口后，使用架构 `clone_user_frame()` 复制完整用户现场，只改子返回值为 0，父返回值为 PID；为子任务准备独立的内核首次返回桩。制定进程属性继承表：哪些复制、哪些共享引用、哪些重置，并据此复制凭据、VM 元数据、FD 引用及信号状态。

**验收：** 子进程不立即 exec，直接验证栈局部变量、全部约定寄存器、堆当前位置、映射游标、父子返回值、FD 共享偏移和信号动作；扩展状态按支持能力做独立用例。只有 hello 成功不能关闭此项。

### MOSS-015 · exec 必须先准备新地址空间，再提交替换

**2026-09-19 工作区更新：已按当前单线程进程/独立地址空间模型关闭。** `sys_execve` 现在先快照用户输入与可变 ELF，构造独立地址空间、VMA、页表和用户栈；只有全部可失败步骤成功后才切换页表并转交所有权，随后执行 close-on-exec、进程名和信号状态提交。PFA 各级与参数、可变映像对象/控制块/字节、地址空间、部分 VMA 六级 heap 失败均有真实压力回归，旧 AS/root hash/name、栈 canary、FD offset 和完整资源基线保持，详见 3.25。以下位置与影响保留为修复前审计证据。

**原始位置与事实（修复前）：** `src/kernel/src/syscall_table.cpp:441` 的 exec 在所有新资源确认可用前就释放旧页表；进程名等状态也在加载完全成功之前发生变化。部分失败分支直接把线程标为 Terminated、出队、调度，没有统一走进程 Zombie、父等待者唤醒和资源关闭路径。

**影响：** 本来应返回错误的 exec 失败会毁掉仍可运行的进程；父进程可能等不到一致的退出事件；不同失败阶段遗留不同资源。失败处理若只是继续追加 `delete/free`，仍难保证闭环。

**修复：**

1. 在旧地址空间仍有效时复制路径、argv 等用户输入，进行完整大小检查；超限明确返回 E2BIG。
2. 准备 `ExecImage`：验证后的 ELF、独立新地址空间、栈/参数、架构返回桩及所需文件引用。
3. 所有可失败工作结束后，在受控点一次性交换地址空间和用户现场，再释放旧资源。
4. fork/exec/exit 的资源转换用少数明确接口完成；不可恢复失败必须进入统一退出路径。

**验收：** 不存在文件、损坏 ELF、参数过大、每一级分配失败都保留原进程继续运行能力；成功 exec 后旧页全部回收，继承 FD 按已声明语义保留。不要用“exec 一律关闭全部 FD”修补退出泄漏。

### MOSS-016 · ELF 映射需要按文件区间和页权限生成加载计划

**2026-09-19 工作区更新：已按受支持的固定地址静态 ELF 子集关闭。** `build_load_plan` 一次完成 header/program-header、文件/用户范围、加法回绕、标志、对齐、保留区、LOAD 页重叠、入口及 file-backed TLS containment 校验；VMA 只读消费计划。17 个畸形映像使用严格拒绝 oracle，独立非页对齐 RX/RW 映像核对实际前缀/文件/BSS/页尾字节及 RX/RW 最终权限，六配置报告均为 28/28 exec case 通过，详见 3.26。PT_INTERP/PT_DYNAMIC、共享 LOAD 页和内核 TLS 初始化不属于已支持能力。

**2026-09-07 工作区更新：** 自有 userspace ELF 已按页分离 RX/R/RW，包含 x86 large-model 段；外来 ELF 的以下加载器问题仍未修复，不以约束构建产物替代输入验证。

**位置与事实：** `src/kernel/src/kernel-elf.cppm:81` 检查 ELF 基本标识和部分 program header 范围，但没有完整覆盖 `e_phentsize`、所有加乘溢出、每段文件范围、`filesz <= memsz`、用户地址域、对齐和入口可执行性。`src/kernel/src/syscall_table.cpp:441` 的加载还存在最多若干段的固定处理和重叠时聚合策略：用总区间及合并权限表示多个段，隐含文件偏移与虚拟地址具有相同平移关系。

**影响：** 合法但布局不同的 ELF 可能映射错误；BSS、非页对齐段的页内内容和跨段空洞不能由单一 backing offset 正确表示；简单 OR 权限可能把较大区域变成 RWX。忽略 `add_vma()` 的失败还可能让日志与实际映射不一致。

**修复：** 生成不可变的 `LoadPlan`，对每个段进行 checked arithmetic 和文件/用户域校验。对边界页明确来自文件的字节区间、零填充区间及最终权限；重叠规则不支持时返回 ENOEXEC。允许暂时只支持有限 ELF 子集，但必须验证并明确拒绝其余情况，不能静默截断或错误聚合。入口必须落在允许执行的有效用户映射内。

**验收：** 用独立构造的小 ELF 覆盖 header 截断、错误 entry size、整数溢出、filesz 大于 memsz、文件越界、内核地址、非页对齐、不同 VA/offset 平移、BSS、边界页重叠、过多段和 RWX 冲突。检查实际字节及最终页权限，而不只检查“exec 返回成功”。

### MOSS-017 · 调度器缺少从运行、入队到迁移的统一所有权协议

**位置与事实：** `src/process/src/process-scheduler.cppm:407` 的入队会初始化节点，缺少足够的重复入队防护；`:452` 选中任务后返回裸指针并解锁，之后另行出队。`:1156` 的公共入队路径在队列操作后再更新部分线程状态。`:1630` 的 tick 在旧任务现场完全切换保存前将其重新放回可被选择的集合。

`src/process/src/process-load_balancer.cppm:124`、`:260` 的迁移/窃取分开进行选择、出队、入队，没有一个同时保护来源、目标和任务拥有者的转移事务；迁移路径也没有完整落实目标 CPU affinity 检查。

**影响：** 竞争时可重复出队、双 CPU 选择同一任务、破坏红黑树节点或把尚在旧 CPU 执行的任务交给新 CPU。现有一次 SMP hello 成功不能覆盖这些交错。

另外 `preempt_enable()` 的计数下降与调度请求处理没有完整闭环，`src/process/src/process-scheduler.cppm:2081` 的 `check_need_resched()` 为空。设置 `need_resched` 不等于返回用户态前一定执行抢占。

**修复：**

- 定义任务状态与所有权：Sleeping/Stopped 不在运行队列；Runnable 只在一个队列；Running 只由一个 CPU 拥有；切换保存完成之前不能被其他 CPU 运行。
- 提供在队列锁内选择并摘取的 `take_next()`；迁移按固定 CPU 锁顺序进行，并再次检查 affinity、状态和拥有者。
- 明确 `on_cpu` 清除及切换完成的发布时机；将 CPU、队列归属和状态一起发布。
- 在安全的 IRQ/syscall 返回路径及最外层抢占恢复点处理调度请求，禁止持有不允许调度的锁时切换。

**验收：** 以可控 hook 强制两个 CPU 同时选择/唤醒/迁移同一任务，检查每个任务最多一个运行者；持续检查队列节点、计数与 RB 不变量；绑核任务永不越界；高优先任务的唤醒延迟有上界。现有插入/删除红黑树平衡实现应保留，不应按旧 TODO 再写一套。

### MOSS-018 · 检查条件、登记等待和进入睡眠必须是一个协议

**位置与事实：** `src/kernel/src/syscall_table.cpp:988` 的 wait4 先扫描 Zombie，再登记等待和改变状态；子进程可在这段窗口退出，使唤醒早于有效等待登记。`:2695` 的 console 阻塞路径也把缓冲区检查、状态/队列修改和单个 `blocked_reader` 注册分开；另一个 CPU 的 RX 可穿过窗口，且单指针无法支持多个等待者。

原先 wait4 先 reap 后复制 status 的问题已在工作区修复：现在先 copyout，成功后才提交回收。`users/wait_status_rollback` 的旧实现失败及 16 次错误重试/回收回归见[当前验收记录](docs/kernel-validation.md#continued-acceptance-on-2026-09-15)。这不关闭原子等待协议；信号唤醒后若只重新循环睡眠，仍没有明确的 EINTR/重启语义。

**修复：** 为条件变量对应的等待队列定义原子协议：持锁检查条件 → 登记等待者和状态 → 安全释放并调度；唤醒方在同一协议下发布条件。醒来后重新检查条件并识别退出、数据、信号或超时原因。console 使用多等待者队列。wait 先保证输出可交付或保留可重试的退出结果，再完成不可逆 reap。

**验收：** 在检查条件与登记等待之间强制 child exit/RX；每次都能结束等待。覆盖“事件已经发生后才调用 wait”、多个读者、信号中断、坏 status 指针后再次 wait。不能用固定 sleep 避开竞争窗口作为修复。

### MOSS-019 · nanosleep 和定时器缺少可失败、可取消的生命周期

**2026-09-15 工作区进展：** 下述原始缺陷中的三 ISA 实际切换、先 Sleeping 后 arm、队列容量 Result 和 in-flight callback 同步取消已修复并回归；三 CPU 持有回调的负向注入能检出提前返回。跨 CPU 上下文交接与信号中断仍待闭合，不能整体关闭。证据见[当前验收记录](docs/kernel-validation.md#continued-acceptance-on-2026-09-15)及该节之前的定时器记录。

**位置与事实：** `src/kernel/src/syscall_table.cpp:2225` 在任务进入 Sleeping 前启动定时器，若提前到期，回调可能看不到应唤醒状态。实际切换代码仅在 ARM64 条件分支；x86/RISC-V 64 可在已改为 Sleeping 的情况下取消定时器并返回。

`src/timer/src/timer.cppm:373` 的固定队列满时直接返回，但启动接口没有向调用者报告失败，定时器可能已被标为 active；`:420` 的到期处理从锁内取出回调、锁外执行，取消并不自动等待已取出的回调结束。nanosleep 使用局部定时器及当前任务指针，返回与正在执行的回调需要额外生命周期保证。

**影响：** 无法被唤醒的睡眠、立即返回却保留错误任务状态、信号中断与定时器回调交错造成的悬空访问。取消链表节点不等于取消正在执行的回调。

**修复：** 与 MOSS-018 共用阻塞协议；让各架构调度切换通过统一接口发生。`start_timer()` 返回 Result，容量满和 deadline 溢出必须可见；采用同步取消或受引用保护的回调上下文，明确不能同步取消的中断上下文限制。无需立刻改成复杂定时器树，先保证当前有界队列行为正确。

**验收：** 零/极短 sleep、最大队列容量及第 257 个请求、取消与触发交错、信号中断、CPU 迁移；三架构测得真实阻塞及恢复，线程状态与队列一致，栈上回调对象离开作用域后无访问。

### MOSS-020 · 信号正常演示通过，仍不代表投递和返回语义正确

**位置与事实：** `src/process/src/signal.cpp:128` 写 `frame[0] = signo`，但 ARM64 系统调用返回段 `src/boot/src/arch/arm64/start_arm64.S:879` 随后把 C 返回值写回相同槽位，覆盖 handler 参数；建立信号帧时也需要先确定被中断系统调用的最终返回值。当前用户测试 handler 不充分断言该参数，无法检测这个错误。

信号 checkpoint 主要在系统调用路径；纯用户计算循环不一定经过投递点。STOP 改状态/出队后若继续返回用户态，不能完成停止；CONT 对 Stopped 的恢复不能只依赖“线程再次执行 checkpoint”。`src/process/src/process.cpp:437` 的退出路径没有形成完整 SIGCHLD 投递闭环，现有测试明确跳过该项。

需要区分：`src/kernel/src/syscall_table.cpp:1112` 的 kill 已包含对 Sleeping 任务的唤醒，不能声称实现完全没有信号唤醒；缺失的是对所有状态、返回点和等待结果的一致协议。

**修复：** 在共同返回用户态路径处理 pending signal：先提交 syscall 结果，再允许投递修改返回现场，汇编不得覆盖已编辑帧。把 IRQ 返回纳入投递/重调度检查。显式实现 STOP/CONT 状态转换和 SIGCHLD 产生；为可中断阻塞定义 EINTR 或已声明的重启子集；sigreturn 使用专门的“现场已恢复”结果，避免普通返回值覆盖。

**验收：** handler 收到正确 signo；被中断 syscall 的返回值在信号返回后正确；CPU-bound 进程能收到信号；STOP 后无用户进展、CONT 后恢复；wait/console/sleep 有一致中断行为；SIGCHLD 从 SKIP 改为真实断言。

### MOSS-021 · 信号状态不能以绝对 PID 作为固定数组下标

**2026-09-19 更新：已关闭（工作区 3.23）。** 当前 `Process` 直接拥有信号状态，fork、exec、exit 规则已沿生产调用链核对；现有继承/exec-reset 用例与强化后的 300 次生命周期在当前六配置通过。下面保留原始审计事实解释修复动机。

**原始位置与事实：** `src/process/src/signal.cpp:95` 使用固定大小 256 的进程信号状态数组，按 PID 直接取项；PID 达到该范围后取不到状态。状态没有完整随进程创建、fork、exec、退出回收管理。

**影响：** 即使同时存活的进程很少，顺序创建超过阈值后也会丧失信号能力；PID 复用还可能继承历史状态。增大数组只会推迟问题。

**修复：** 将信号状态作为 Process 拥有的资源，或由具有 generation 的进程句柄映射管理；定义 fork 复制、exec 重置及 exit 释放规则。区分进程共享动作与线程级掩码/备用栈，当前不支持多线程的部分也要明确边界。

**验收：** 顺序创建并销毁超过 256 个进程后信号仍正常；PID/槽位复用不继承旧 handler、mask、altstack；fork 和 exec 分别满足继承表。

### MOSS-022 · Zombie 只应保留等待结果，不应继续占有打开文件

**2026-09-19 更新：实现已修复，验收部分（工作区 3.23）。** 当前 `do_exit()` 在发布 Zombie 前调用幂等的 `cleanup_files()`，析构兜底；warmup 已在 wait/reap 前观测 EOF，1,000 次资源恢复在五个默认配置通过。RV64 Debug 当前源码仍超过默认 30 秒，只有显式 60 秒诊断完成，故本项保持未关闭。下面是原始审计事实。

**原始位置与事实：** `src/process/src/process-types.cppm:501` 用 `void*` 保存 FD table，析构 `:529` 主要清理线程；`src/process/src/process.cpp:437` 的退出没有关闭 FD table。`src/vfs/src/vfs-file.cppm:125` 有 `close_all()`，但检查退出调用链未形成调用。`src/vfs/src/vfs_init.cpp:274` 的 fork 克隆会分配新表并增加文件引用。

**影响：** 每次 fork/退出可遗留 FD table 和文件引用；pipe 的最后一个写端实际上不归零，读者收不到正确 EOF；固定对象池迟早耗尽。不能等待父进程 reap 才释放所有打开文件，因为 Zombie 可能长期存在。

**修复：** 由 Process 的资源对象拥有 FD table；进入 Zombie 前关闭全部 FD，统一触发 file release 和 pipe 端点引用变化，随后只保留 PID、退出状态、统计和亲属关系所需信息。析构作为最终兜底，并保证关闭恰好一次；fork/创建失败也走同一拥有关系回滚。

**验收：** 反复 fork/exit/wait 后 fd/file/table/page 计数回到稳定基线；子写端退出而父读端未关闭时获得 EOF；父暂不 wait 时 FD 已释放；exec 保留应继承 FD，不因本修复误关。

### MOSS-023 · 退出换栈需要汇编边界；连续执行停滞需要单独关闭

**2026-09-06 更新：危险换栈实现已替换，故障项尚未关闭。** `CfsScheduler::schedule_after_exit` 现在关中断、清 current，以 `context_switch(&discarded, &bootstrap_contexts_.get_cpu(cpu))` 返回已有调度栈；没有继续使用 C++ inline asm 改 SP，也无需新增平行 exit 栈框架。三架构基本生命周期通过，但没有旧第 28 次失败的前后对照或 1,000 次/多 CPU/资源计数验收。下面的采样 PC 是旧实现，不能用于定位新实现。

**位置与事实：** `src/process/src/process-scheduler.cppm:2097` 的 `schedule_after_exit()` 在普通 C++ 函数内部直接用 inline asm 修改 SP/RSP，然后继续执行 C++ 循环和函数调用。编译器已经建立的帧、局部变量和寄存器溢出位置不能依赖这种换栈后仍正确；仅添加 `memory` clobber 不会建立新的 C++ 调用帧契约。

**运行事实：** 第 3 节的 ARM64 重复执行停滞采样就在此函数调用的 `cpu_idle_once()`。RISC-V 64 hello 之后也不能返回 shell。换栈方式是应修复的独立风险，但本次没有证明它就是这些运行故障的唯一根因。

**修复：**

- 已通过现有汇编 `context_switch` 返回活跃 bootstrap 栈；后续验证该边界的栈对齐与生命周期，不再要求新增另一套 noreturn exit 栈入口。
- 旧任务的内核栈必须在任何 CPU 和保存上下文都不再引用之后才回收；审计 bootstrap context 与每 CPU idle context 的复用关系。
- 为停滞专项记录 parent/child 状态、runqueue 内容、`on_cpu`、等待队列、timer pending/enable 状态和 wakeup 原因；在第 27–29 次执行附近保存事件环形缓冲区。
- 检查 idle 的“无任务判定—关闭 timer—进入 WFI”与新任务发布/IPI 是否有原子握手；不能以永久开 timer 的方式掩盖丢失唤醒而关闭问题。

**验收：** Debug/Release 均连续 fork/exec/wait 至少 1,000 次，且在 1/4/16 CPU 的已支持配置下完成；每次恰有退出和父唤醒，资源稳定。第 28 次停滞的回归必须先能在原版本失败，再由对应修复消除。修复内存与容器基础后应重新定位，避免把多个独立问题混为一个。

## 8. VFS、管道与资源池

### MOSS-024 · FD 访问模式和共享文件对象需要独立保护

**位置与事实：** `src/vfs/src/vfs_syscall.cpp:73` 的 read/write 路径未完整执行 O_ACCMODE 检查。`src/vfs/src/vfs-file.cppm` 的 FD table、共享 File 的位置/引用及 `src/vfs/src/vfs_init.cpp` 的全局 file pool 没有足够的并发保护。

**影响：** 只读打开的对象可能被写；fork 共享 File 后，文件偏移竞争；多 CPU 分配/关闭同一池可能出现重复占用、引用丢失或 use-after-free。进程当前是否支持多个用户线程不消除全局池和 fork 共享对象的多进程竞争。

**修复：** read/write 前检查访问模式；FD table 锁保护安装/摘除和引用取得；IO 期间持有稳定 File 引用；全局池独立保护分配标志；共享偏移在 File 层串行更新。避免跨可能阻塞的设备 IO 或用户缺页长期持自旋锁，先取得稳定引用再进入相应睡眠锁/操作协议。

**验收：** 只读 FD 写与只写 FD 读返回 EBADF；fork 后对共享偏移的并发读有明确序列；并发 open/close 不重复分配槽位；close 与 read 交错无悬空引用。

### MOSS-025 · 管道的“暂时为空”与 EOF 是不同事件

**位置与事实：** `src/vfs/src/vfs_init.cpp:360` 的 pipe 读在空但仍有 writer 时返回 0；`:426` 的满管道写返回 0，断开的读端错误为 EINVAL；读写共享缓冲区也没有完整等待和互斥协议。

**影响：** 常规用户程序把 0 长度读解释为 EOF，会提前结束消费；生产者在满缓冲区时收到无法表达阻塞原因的结果；fork/close/exit 后端点计数又受 MOSS-022 影响。

**修复：** 空且有 writer 时阻塞，空且无 writer 才 EOF；满且仍有 reader 时阻塞，只有明确支持的 nonblocking 模式才返回 EAGAIN；无 reader 的写按已声明 ABI 返回 EPIPE，并在支持时产生 SIGPIPE。读写使用一致的环形缓冲区锁和等待队列，处理部分读写与小写入原子性。

**验收：** 读者先读、写者稍后写不会得到假 EOF；写超过缓冲容量能随消费继续；关闭最后一端、进程退出、信号中断和多读写者交错有明确结果。若声明 PIPE_BUF 语义，多个不超过其大小的写入不可交错。

### MOSS-026 · 固定容量可以接受，不回收和失败泄漏不能接受

**位置与事实：** `src/vfs/src/vfs_init.cpp:163` 的 inode 分配使用单调增长计数，固定容量 256；pipe 创建消耗 inode，关闭未形成可重用槽位协议。`src/vfs/src/vfs_syscall.cpp:169` 的 do_pipe 在部分失败分支直接 free File，绕过其 release 所承担的 PipeState/inode 释放。`src/kernel/src/syscall_table.cpp:1627` 的 sys_pipe 创建成功后，如果结果数组 `copy_to_user` 失败，直接返回 EFAULT，没有关闭已安装的两个 FD。

**影响：** 顺序创建/关闭也会耗尽对象池；用户提供坏输出指针即可反复消耗 FD/pipe 资源；失败分支和正常关闭产生不同引用结果。

**修复：** 池槽位在最终引用归零后可重用；分配过程使用局部拥有对象，两个端点、File、inode、FD 安装和用户结果交付任一步失败都统一回滚。保留固定池是可行的最小实现，但容量满必须可预测地返回错误且不破坏已有对象。

**验收：** 创建/关闭至少 1,000 次 pipe 后容量不降低；分别注入第一/第二 File、inode、第一/第二 FD 和用户结果复制失败；每次前后资源计数一致，已成功安装的 FD 不残留。

## 9. 启动、测试与架构能力

### MOSS-027 · x64 启动应消费真实 PVH 信息，不能猜 initramfs

**2026-09-19 更新：已关闭。** `X86BootImpl::hardware_early_init` 校验 PVH start info/memory map、完整模块表及模块在可用 RAM 中的连续覆盖，直接消费真实 initrd 地址/大小；扫描和固定 fallback 已移除。3.22 的同一生产镜像 gate 已覆盖大小/位置变化、缺失/非法模块、坏 archive、必需 init 缺失和完成标记顺序，Debug/Release 全部通过。旧独立测试入口已删除，MOSS-028 继续使用生产启动路径；MOSS-028 的其他覆盖缺口不因本项关闭。

**原始位置与事实（修复前）：** `src/boot/src/arch/x64/boot_impl.cpp:233` 的 initramfs 发现扫描低内存 magic，失败后退到固定 `0x1000000` 和 `102400` 字节。实际运行打印该 fallback，但当时镜像约 46.2 KiB，随后出现 `bad magic at offset 0x0`、解析 0 条目、`/shell.elf` 不存在。

启动汇编 `src/boot/src/arch/x64/start_x64.S:61` 已有保存 PVH EBX 指针的尝试。因此不能简单断言“EBX 从未保存”；需要修复的是从 PVH 入口、跨模式转换、早期数据生命周期到 BootInfo 消费的完整传递链。

**修复：** 严格按 PVH 启动信息的真实指针和模块表取得 initrd 地址/长度，校验 magic、结构范围与模块范围；不要扫描候选地址代替协议，也不要把猜测的镜像区间当成功。无 initrd、格式错误或缺少必需 init 进程时给出明确启动失败，不能继续显示完整初始化成功。协议依据见 [Xen PVH boot ABI](https://xenbits.xenproject.org/docs/unstable/misc/pvh.html)。

**验收：** 改变 initrd 大小和装载地址后都能找到真实模块；非法/缺失模块有准确错误；x86 shell → fork/exec hello → wait → shell 完整运行。测试 ELF 的 PVH 缺失是 MOSS-028 的独立问题，修好正常内核不会自动修好独立测试入口。

### MOSS-028 · 测试必须测试真实内核，并证明失败能传到 CTest

**2026-09-06 更新：框架已重写，整项仍待覆盖验收。** 原 `test_main.cpp`、`minimal_boot_support.cpp`、`moss_ut.hpp` 和两份基础 case 已删除；`src/test/CMakeLists.txt` 链接生产模块，`validation.cpp` 使用 `ut_kernel.hpp` 与真实 userspace。`kernel_validation.py::Protocol` 区分完整成功、assert、panic、timeout、protocol failure 与未运行；host 测试覆盖进程提前退出/回收和坏协议。结束依据改为串口协议，宿主负责终止 QEMU，不能按原 ISA magic exit code 方案再修一次。剩余是审计用例、SIGCHLD skip、各架构故障路径及 CI 持续门槛，见第 3.3/12 节。

**位置与事实：** `src/test/CMakeLists.txt:3` 的独立测试主要链接 `moss_core`；`src/test/cases/kernel_modern_validation.cpp:19` 的 12 个测试、63 个断言主要覆盖算术、指针、循环、常量、局部缓冲区等，不能证明真实 PFA、页表、调度器、VFS 和生命周期正确。`src/test/test_main.cpp:35` 的独立 `_start` 没有普通 x86 内核入口所需的完整 PVH 启动转换，本次两个 x86 CTest 均未运行到断言。

`src/test/framework/moss_ut.hpp:74` 的 x86 `isa-debug-exit` 成功值会被 QEMU 编码成非零退出状态，而 `qemu.py:591` 的普通返回处理直接传播进程状态；这会在装载问题修复后继续产生假失败。不能简单把所有退出 1 当成功，因为装载失败也可返回 1。

同一测试框架 `:87` 的 RISC-V 64 SBI reset 路径没有根据测试失败设置不同失败 reason，存在失败仍退出成功的风险，需要故意失败用例确认端到端效果。`src/userspace/signal_test.c` 的 SIGCHLD 为 SKIP，末尾仍输出 ALL TESTS PASSED，也夸大覆盖。

**修复：**

1. 已用生产启动入口替代独立 boot 桩并输出结构化记录；继续完善 skipped/not_run 的覆盖与独立 signal_test 结果表达。
2. 以完整串口协议及宿主记录的实际终止原因为准，保留装载错误、panic、断言、timeout 的负向验证；不恢复旧架构退出设备或 magic exit code。
3. 已有三架构故意失败、panic、timeout 自检；继续验证普通非预期失败必然令 runner/CTest 失败，不能把自检“预期失败匹配”用于忽略实际错误。
4. 保留当前真实内核/userspace 套件，补齐 T01～T12 和宿主可运行的生产算法检查；不复制另一套容易测试却未用于内核的算法。

**验收：** 三架构 Debug/Release 的通过、断言失败、panic、超时、镜像装载失败、跳过场景均被正确区分；完整进程/内存/信号用例纳入 CI，不能把 63 个基础断言当作这些子系统的覆盖证明。

### MOSS-029 · 时间与指令能力不能依赖碰巧匹配的 QEMU 常量

**2026-09-06 更新：主要平台假设已移除，验收未齐。** `timer_hal.cppm::calibrate` 用 PIT 分别测 TSC 和 LAPIC，`set_compare` 使用独立频率换算；RV64 改为 SBI TIME，DTB 提供 timebase，已有 `sstc=false` 记录。仍需验证缺失/失败的固件计时能力、SBI 错误返回、多个间隔误差和 CPU 能力变化，不把“无 Sstc 可启动”扩大成任意计时硬件已支持。

**位置与事实：** `src/hal/timer/src/timer_hal.cppm:31` 的 x86 时间换算使用平台假定频率，LAPIC 事件设置又以固定关系换算 TSC delta。RISC-V 64 路径使用 stimecmp，但没有完整的 Sstc 能力检查和不支持时的 SBI fallback 协议。

**影响：** clock_gettime/nanosleep/调度时间在频率不同或扩展缺失的平台上失真或异常。现有 QEMU 配置可运行不能证明硬件能力探测正确；本次未做真实硬件计时误差测量。

**修复：** 将 clocksource 的频率和 clockevent 的编程单位分开；使用平台/固件提供的可信频率或经验证的校准，记录来源。对 Sstc、定时器和中断控制器显式探测；提供已实现的 fallback，否则启动时准确报告不支持。对各架构分别声明 SMP、用户态、信号等能力，不要把单个全局“支持架构”标志扩大解释。

**验收：** 与 QEMU/宿主观察时间比较多个间隔，设定并记录误差容限；改变虚拟 CPU 能力和频率来源后仍有正确结果或明确拒绝；不支持扩展的配置不能在首次 sleep 才非法指令。

### MOSS-030 · 空实现返回成功使上层无法建立可靠契约

**位置与事实：** `src/mm/src/mm_interface_impl.cpp:22` 一些初始化/压缩、watermark、压力和统计为固定结果；`:125` 的大小查询固定返回页大小，prefault、reclaim、hugepage promotion 等路径含成功占位；性能统计还包含固定成功率。`:51` 的初始化状态发布顺序也先标记 initialized 再发布实例，若被并发调用会暴露未完成状态。

`src/ipc/src/ipc.cppm:304` 附近的共享内存相关辅助路径返回固定物理/虚拟地址，映射操作为空成功。这些接口并非都已接到当前用户态 syscall，故应记录为**潜在错误能力**，而不是声称现有 shell 已通过它们分配共享内存。

**修复：** 未实现的操作返回 NotSupported，并在能力表中标为未实现；已实现接口只能报告真实状态。初始化采用状态机或有序 once 发布，所有依赖就绪后再标 Ready。需要统计时维护真实计数，不需要时删除虚假指标。共享内存等功能等基础 VM 稳定后按真实页面/映射所有权实现。

**验收：** 每个公开接口都有明确的 implemented/unsupported 状态，Unsupported 无副作用；查询值能与实际对象核对；并发初始化不会看到空实例；固定地址不会作为成功分配结果流到外部调用者。

### MOSS-031 · 需要收紧少数核心接口，而不是再增加一层通用框架

**2026-09-06 更新：部分边界已改善。** ADR-0005 的架构启动协议→运行时硬件资源、构建产物→独立 runner 已落地，不需要再引入板级工厂。下表中的 uaccess、TrapFrame、AddressSpace 和进程资源/ELF 事务仍需实现；当前 ABI 仍不能仅凭 syscall 名称宣称 Linux/POSIX 兼容。

**位置与事实：** `src/kernel/src/syscall_table.cpp` 同时承担用户复制、ELF 加载、进程状态构造、页表操作、等待和设备 IO；`src/process/src/process-types.cppm:501` 的 void FD table 及原始陷阱帧指针使资源与布局契约越过模块边界。`src/kernel/src/syscall_table.cpp:2198` 的 clock_gettime 忽略 clock_id 并写入一个 u64 纳秒值，nanosleep 也采用自定义参数；用户封装在 `src/userspace/syscall.h`，不能仅凭函数名宣称 Linux/POSIX ABI 兼容。

**修复设计：**

| 边界 | 对外负责什么 | 必须隐藏什么 |
| --- | --- | --- |
| BootMemoryMap / PhysicalAllocator | 取得/归还具有所有权的物理页 | 保留区裁剪、buddy 元数据与 order 校验 |
| AddressSpace | 合法 map/unmap/fault/clone/activate | PTE 编码、ASID、TLB、页引用及回滚 |
| UserAccess | 有界、可失败的用户复制 | 架构地址域与异常 fixup |
| ArchTrapFrame | 用户状态读取、克隆、安全恢复 | ISA 布局、特权位、返回指令 |
| Scheduler / WaitQueue | 状态转换、原子阻塞、唤醒与迁移 | 运行队列锁、on_cpu、上下文交接 |
| ProcessResources | fork 继承、exec 提交、exit 关闭 | FD/信号/地址空间生命周期 |
| ExecLoader | 从文件生成已验证加载计划 | ELF 边界字节、重叠段和栈布局 |

这些是职责边界，不要求逐项引入虚函数、依赖注入容器或新文件层次。先通过当前修复形成最小 API，再按需要移动代码。优先复用已有 HAL、错误类型、libfdt；不要重新发明设备树解析或再做平行的内存框架。

**ABI 决策：** 先明确并版本化 Moss ABI，维护内核和 userspace 共用的系统调用号、定长结构、错误码和布局断言；支持的行为有测试，未支持行为明确拒绝。是否迁移 POSIX timespec、完整 envp、部分 munmap 等属于后续兼容性决策，不应混入隔离修复而无声破坏现有程序。

**验收：** syscall handler 不再手工解释其他 ISA 的寄存器槽位，也不手工管理多阶段资源回滚；同一 ABI 定义在三架构具有明确布局；所有接口既能说明成功后保证什么，也能说明失败后保留什么。

### MOSS-032 · 文档、日志和统计需要如实表达当前能力

**2026-09-06 更新：清单与能力矩阵已同步，整项未关闭。** `todo.md` 现区分已有实现、通过配置和待验收；不再要求重做 RB 平衡或从零实现 RV64/x86，workflow 也已注明 configure/build/test。本文保留旧故障与新基线，不将历史 timeout 当当前失败。`mm_interface_impl.cpp` 的固定统计、部分启动阶段继续成功及独立 signal_test 的 SKIP 表达仍需修复；文档更新不能替代这些实现。

**事实：** 原审计时的 `todo.md` 中“x86/RISC-V 64 只有桩”“红黑树平衡未实现”等表述不准确：调度器 `src/process/src/process-scheduler.cppm:882` 有插入平衡，`:1011` 有删除平衡；三个架构也都有大量真实代码。反过来，凭据结构存在不代表完整权限策略，信号正常例子通过不代表完整信号语义，存在 validate helper 不代表所有用户指针都安全。

DeviceManager 的框架存在、运行时设备计数为 0 与 UART 实际通过 HAL 工作可以同时成立；不能据计数断言“没有驱动”，也不能据框架声明断言动态设备管理已经完整可用。启动阶段打印成功、固定统计和末尾 ALL TESTS PASSED 同样需要与真实条件绑定。

**修复：** 维护按架构划分的能力矩阵，至少区分 declared/implemented/tested/unsupported；启动关键依赖失败就停在准确失败阶段；测试结果分别统计 pass/fail/skip。旧 TODO 作为历史保留或加迁移说明，新任务以本文的源码证据和验收为依据，关闭条目时附修复提交与实测证据。

**验收：** 每个“完成/支持/通过”均能指向实际调用路径及对应测试；没有实现的 IPC、hugepage、pressure 等不会报成功；文档不再要求重做已有能力，也不掩盖已复现的故障。

## 10. 修复后必须成立的核心不变量

后续实现评审优先检查以下不变量。它们比“使用某种模式”“新增某个 Manager”更能判断设计是否真正改善。

| 编号 | 不变量 | 主要关联项 |
| --- | --- | --- |
| I-01 | 任意物理页在一个时刻只有明确的拥有者；共享通过显式引用表达，保留页不进入普通空闲池 | 004、005、010、013 |
| I-02 | 用户无法通过硬件权限、伪造 VMA、系统调用指针或恢复现场获得内核访问权限 | 001、002、003、008 |
| I-03 | VMA 策略、PTE 权限和 TLB 中可观察权限一致；权限放宽必须有合法策略依据 | 008–012 |
| I-04 | 发布给其他 CPU/进程的对象已经完整初始化；最后一个读者/引用退出后才释放 | 006、010、017、022、024 |
| I-05 | 一个线程最多由一个 CPU 执行、最多属于一个运行队列；保存完成前不转交运行权 | 017、023 |
| I-06 | 条件成立与等待登记之间不会丢失事件；取消后不会访问已结束生命周期的回调上下文 | 018、019、025 |
| I-07 | fork 复制的是用户执行快照；exec 失败保留旧映像；exit 一次性释放运行资源 | 014–016、021、022 |
| I-08 | 每个用户返回路径只接受该 ISA 合法的用户态现场，并统一处理结果、信号与重调度 | 003、007、020 |
| I-09 | 对外返回成功就兑现全部后置条件；失败保持旧状态或执行明确、完整的回滚 | 010、015、026、030 |
| I-10 | 测试失败必定使自动化失败；没有执行、被跳过和通过是三种不同结果 | 028、032 |

## 11. 实施顺序与可交付阶段

下面是建议依赖顺序，不是估算工期。基础错误会污染上层症状，不能同时改十几个模块后只看 shell 是否恢复。每一步保存可比较的资源计数和失败用例，完成一个边界再推进下一层。

### 阶段 A：建立可观察基线，并修复基础内存所有权

- [x] A1a：建立生产内核功能套件、框架失败/panic/timeout 自检、串口协议和宿主报告；六配置已有通过记录（028，`44dedc2` / `6252484`）。
- [ ] A1b：补 ARM64 原重复失败的可重放脚本、未覆盖装载/skip 负向用例和审计 T01～T12，不把当前 14 个功能用例当完整覆盖（028）。
- [x] A1c：修复 Active/Online 混淆和循环次数超时造成的 CPU 就绪误报；统一三架构等待，六配置 CTest、ARM64 四核 10 次与 16 核 heap 通过（3.4）。
- [x] A1d：逐 case 宿主耗时与实际 deadline 可观测；PFA 全 RAM 耗尽预算按真实工作量调整，四并发红绿对照和显式短超时负向检查通过，不减少断言或忽略失败（028，工作区 3.10）。
- [x] A2a：三个 linker 预留 8 MiB heap，起始 arena/扩容不得越过 heap_end；加入并通过 heap_bounds（004，`44dedc2`）。
- [x] A2b：PFA 选择真实 RAM bank，metadata 和自由块跳过 initrd/固件保留区（005，`44dedc2` / `6252484`）；这里只标实现，不标穷尽验收。
- [ ] A2c：完成堆边界/耗尽哨兵与保留区多段/洞/重叠/容量/算术验收（004、005）。
  - [x] 堆耗尽返回失败、块内数据与独立 PFA 页哨兵不变、分配计数恢复及大块合并复用（3.4）。
  - [x] 所选 RAM bank 分配至耗尽、保留洞/重叠/非对齐、initrd 校验和及 DTB 极值/容量边界检查（3.5）。
  - [x] 联合管理 kernel_end 以上多 bank；RV64 同镜像两段/乱序八段、后续 bank 元数据、相邻合并及容量溢出验收（3.6）。
  - [ ] 完整显式启动保留集合、kernel_end 以下可用页回收及 PVH 异常内存表。
  - [x] 当前 heap/活动页表树/early pool/链接表区/PFA 元数据布局、增长与耗尽校验和，以及坏布局启动拒绝（004，3.7）；多进程/SMP 引用交错仍待专项验收。
- [x] A3a：初始自由块按物理对齐、长度和保留区循环降 order；真实 PFA orders/reuse/free-page 检查已接通（013）。
- [x] A3b：修复 heap 返回地址对齐/溢出与错误释放 order 契约，补全页/对象计数及边界验收（013，工作区 3.4～3.5）。
  - [x] 共享 RuntimeHeapAllocator 的对齐、溢出、错误释放与计数/复用回归（3.4）。
  - [x] PFA 原分配头/order/范围校验及保留洞后的分段、拆分/合并至耗尽（3.5）。
- [ ] A4：替换错误的自动退休容器；先用明确锁与拥有关系闭合生命周期（006）。
  - [x] 指针发布不自动析构仍通过 next 可达的旧节点（`4cde9b3`，3.8）。
  - [x] 删除伪 RCU，迁移 LockedList/LockedHashMap；查找复制值/拥有者，摘除与发布受锁保护，析构/快照回调移到锁外（工作区，3.9）。
  - [x] 持有读者的真实红绿对照、同容器回调/析构重入、双 CPU 查找—删除与同 key 创建/删除交错（工作区，3.9）。
  - [ ] 闭合所有使用者的复合操作、外部对象生命周期及跨锁顺序：IRQ context、驱动绑定、IPC 创建/销毁/清理等；补针对这些协议的交错验收。

**退出条件：** 分配至边界/耗尽和失败注入不越界；对象插入/删除压力不释放可达节点；已有 ARM64 基础路径保持可运行。不要为了通过这一步增加 heap/回调池大小。

### 阶段 B：封闭权限边界与架构异常入口

- [x] B1：x86 内核页权限改为 supervisor-only，补最终页表结构检查（001，工作区 3.11）；不代表 W^X 或用户异常隔离完整验收。
- [x] B1a：三架构最终内核 W^X/RO/NX，全部直映 NX 且 text/rodata 别名只读，用户页表按 VA 归属共享内核子树；生产结构与创建/clone/回收检查、六配置 CTest 通过（001，工作区 3.12）。
- [ ] B1b：受控用户违规访问/异常终止隔离验收，不以结构检查替代。
- [ ] B2：统一用户地址域与可恢复 uaccess，移除 syscall 0 调试旁路（002）。
  - [x] 统一 MM 用户地址域和 VMA 准入/跨段权限策略；删除入口原始 puts，字符串有界且不静默截断（工作区，3.13）。
  - [x] 三架构共享复制异常 fixup；合法未驻留 VMA 在 PFA 耗尽时输出复制返回 EFAULT，并能在释放压力后重试（3.20）。
  - [x] VFS/信号迁移，输入/输出缺页 OOM、跨页短 I/O、offset/管道数据保留及输入尾部清零，九配置通过（3.21）。
  - [ ] 页/VM 生命周期、COW OOM 及并发 exec/unmap/COW 专项验收。
- [x] B3a：RV64/x86 首次用户返回与基本 fork 用户 GP 现场恢复已补，三架构真实 users 套件通过（007、014，`44dedc2`）。
- [ ] B3b：三个 ISA 有效 TrapFrame 传参、布局断言、完整用户现场、信号桩及安全返回仍待统一（007）。
  - [x] 单一原生帧入口、全部布局断言、GP/六参数/条件码往返、fork 和 native sigreturn，九配置通过（`0e88344`，3.19）。
  - [ ] 完整 FP/TLS、恶意返回状态和全异常/抢占交错验收。
- [x] B3c：修复首次上下文/入口栈发布的本地 IRQ 覆盖；RV64 Sv48 的 1/4 CPU 注入前后对照、Sv39 的 4 CPU 后置回归及 users.vm 通过（007/017，工作区 3.18）。
- [ ] B4：信号帧复制与返回状态净化，修复结果/handler 参数写回顺序（003、020 的入口部分）。
  - [x] 结果先写回、再保存信号现场与设置 handler 参数；三 ISA 的真实 GP/返回值信号往返通过（`0e88344`，3.19）。
  - [x] 信号帧复用可恢复复制，合法未驻留备用栈写出失败及 sigreturn 读回 OOM，九配置通过（3.21）。
  - [ ] 恶意/嵌套/备用帧完整边界与 IRQ CPU-bound 信号专项。

**退出条件：** 各架构静态页表/布局检查通过；能运行用户态的架构上，坏指针、坏栈、特权状态和只读映射攻击只影响调用进程。x86 正常用户态已接通，可直接补隔离测试；普通 users 套件没有执行这些攻击，不能勾为通过。

### 阶段 C：建立三架构真实用户态闭环与 VM 事务

- [x] C1a：正常 PVH 模块发现、用户态基本闭环及复用生产启动的验证镜像已接通（027、028，`44dedc2` / `6252484`）。
- [x] C1b：initrd 大小/位置变化、缺失/非法模块、坏 archive、关键 init 失败及无假完成标记的生产 QEMU 验收通过（027，工作区 3.22）。
- [ ] C2：统一 fault 分类，修复 COW 权限、RISC-V 64 PPN、页表 walk 和克隆回滚（008–010）。
- [x] C3a：实现并验证 ASID 活跃租约、耗尽与跨 CPU 重用隔离（011，工作区 3.23）。
- [x] C3b：实现 VMA/PTE 一致的 brk/map/整段 unmap（012，工作区 3.24）；部分 munmap 仍明确拒绝。
- [ ] C4：完成 fork 用户现场/继承状态、ELF LoadPlan 与 exec 替换事务（014–016）；当前仅 MOSS-014 仍阻止整项关闭。
  - [x] C4a：exec 的独立准备、单点提交、逐级 PFA/heap 回滚与成功 FD/可变 backing 语义已通过六配置验收（015，工作区 3.25）。
  - [x] C4b：外来 ELF 的不可变 LoadPlan、checked arithmetic、严格拒绝 oracle、边界页实际字节和最终权限已通过六配置验收（016，工作区 3.26）。
  - [ ] C4c：补全 fork 继承/扩展状态及失败验收（014）。

**退出条件：** 三架构 Debug/Release 均完成 shell → fork/exec hello → wait → shell；只读/COW/PROT_NONE 用例通过；每个分配失败点可回滚；fork 不立即 exec 的状态验证通过。

### 阶段 D：闭合调度、等待和退出生命周期

- [ ] D1：运行队列原子取任务、on_cpu 交接、迁移锁序及 affinity（017）。
- [ ] D2：共同 WaitQueue 和唤醒原因，修复 wait/console/nanosleep 及 timer cancel（018、019）。
- [x] D3a：删除 C++ inline 换栈，复用已有 context_switch 返回活跃 bootstrap 调度上下文（023，`44dedc2`）。
- [ ] D3b：验证旧栈/上下文生命周期；原失败对照及至少 1,000 次、多 CPU、资源稳定性实验后才关闭旧停滞（023）。
- [x] D4a：`Process` 拥有信号状态，fork 继承、exec 重置、exit 回收及跨旧固定槽边界验收通过（021，工作区 3.23）。
- [ ] D4b：FD 所有权和 Zombie 前关闭已实现，EOF-before-wait 与资源恢复已验证；RV64 Debug 默认 30 秒矩阵仍需闭合（022，工作区 3.23）。
- [ ] D5：统一 IRQ/syscall 返回处的信号和抢占检查，补 STOP/CONT/SIGCHLD（020）。

**退出条件：** 连续 1,000 次进程生命周期和可控并发交错通过；CPU-bound 信号可达；阻塞、取消、退出都无丢失唤醒；运行者/队列/资源计数始终满足不变量。一次性启动成功不满足此阶段。

### 阶段 E：修复 VFS 语义并收敛对外能力

- [ ] E1：FD 模式检查、File 引用/偏移保护、可回收池和所有失败回滚（024、026）。
- [ ] E2：pipe 的正确阻塞、EOF、端点关闭和写原子性（025，依赖 D2/D4）。
- [x] E3a：x86 TSC/LAPIC 独立 PIT 校准，RV64 DTB timebase + SBI TIME；无 Sstc 配置已有验收（029，`6252484`）。
- [ ] E3b：缺失/失败的计时能力、SBI 返回、误差和 CPU 变化验收；所有空实现改为 Unsupported（029、030）。
- [x] E4a：按 ADR-0005 拆分内核/构建/runner；本次同步两份清单和能力矩阵，保留历史证据（031、032）。
- [ ] E4b：核心资源/异常接口和 Moss ABI 仍待收敛；启动状态、统计、skip 与真实能力一致，不能仅关闭文档部分（031、032）。

**退出条件：** 被声明支持的能力都有真实内核测试；未实现能力不会伪装成功；错误输入/容量耗尽不会消耗永久资源。新增驱动、共享内存、高级回收和更广 POSIX 兼容再依据明确需求单独立项。

### 如何拆分每个修复提交

建议遵循“一个不变量 + 一个能在旧版本失败的验证 + 一个最小修复”。涉及底层 ABI 的变更允许同时修改对应 C++、汇编和 userspace 定义，因为只提交一侧会产生不可运行中间状态。不要把文档清理、目录搬迁、算法替换和语义修复捆绑在一起。

基础内存/调度修复前后的行为可能变化；保留原失败记录，然后重新定位余下故障。某个症状消失后，只有关联验收通过才能关闭对应问题，不能据此推断其他独立缺陷也被修好。

## 12. 后续验证矩阵与关闭规则

### 12.1 分层测试

| 层级 | 适合验证的对象 | 最低要求 |
| --- | --- | --- |
| 宿主纯逻辑测试 | 保留区裁剪、checked arithmetic、ELF LoadPlan、buddy/RB/容器不变量 | 使用生产实现或可提取的同一核心；可随机生成并记录 seed |
| 架构结构测试 | TrapFrame 偏移、PTE 编解码、页权限、ABI 布局 | 三个 ISA 分别断言；不能把 ARM64 常量作为所有平台预期 |
| 真实内核单核集成 | usercopy、权限、fault/COW、fork/exec/wait、signal、pipe | 内核启动后实际从用户态触发；失败必须传回 runner |
| 可控并发测试 | wake/block、迁移、timer cancel、FD close/read、COW 同页写 | 使用 hook/barrier 强制交错；不能仅靠概率压力 |
| 长循环/资源测试 | 进程、ASID、信号 PID、pipe/inode、堆和页回收 | 超过当前阈值；记录开始、峰值和结束资源数量 |
| 平台验收 | QEMU CPU/中断控制器配置、频率、扩展缺失 | 先声明支持矩阵；不支持应清晰失败 |

### 12.2 必须能变红的验收用例

| 用例组 | 关键触发 | 成功条件 |
| --- | --- | --- |
| T01 隔离 | kernel VA、伪造 mmap VMA、坏用户指针、RO 输出页 | EFAULT/受控用户异常，无全局 panic 或内核写入 |
| T02 信号返回 | 非用户返回模式、越界 PC/SP、恶意 altstack、signo 检查 | 拒绝非法帧，合法现场和参数保持 |
| T03 内存所有权 | 堆跨预留边界、分配至 initrd/保留洞附近、非法 free | 保留数据不变，所有权无重叠 |
| T04 COW/权限 | 只读页 fork 后写、已修改页 fork、NONE/NX 故障 | 只复制合法可写页，数据保留，无错误重映射 |
| T05 失败事务 | fork/exec/pipe 在每个资源获取点失败 | 旧进程可继续，计数恢复，无半发布对象 |
| T06 ASID/PID | 累计超过 255/256 次并保留部分活跃对象 | 地址不串扰，信号状态不丢失/串用 |
| T07 调度 | 双 CPU 抢同一任务、迁移时仍运行、affinity 限制 | 单一拥有者，队列/RB 一致，绑核有效 |
| T08 等待 | child exit/RX 正好发生在登记窗口、signal 中断 | 无丢唤醒，返回原因正确 |
| T09 定时器 | 第 257 个 timer、到期与 cancel/exit 同时发生 | 明确容量错误，无永久睡眠/悬空回调 |
| T10 生命周期 | 连续 hello、fork 不 exec、父暂不 wait、退出写端 | 每次返回，FD/页回收，EOF 正确 |
| T11 VFS | 只读写入、共享 offset、多读写者、反复创建 pipe | 访问模式正确，结果有序，池可重用 |
| T12 测试协议 | 人工断言失败、装载失败、panic、timeout、skip | CTest 准确区分，失败不能绿，成功不能误红 |

阈值 255、256、512 来自对应有界实现，第 28 次来自原审计观测，只用于确保回归越过现有边界。修复不能以调整阈值替代不变量证明；1,000 次循环是最低验收建议，也不是可靠性的数学证明。

### 12.3 执行矩阵

- 三架构均验证 Debug 与 Release，避免调试版栈布局偶然掩盖汇编/C++ 边界问题。
- ARM64 的完整生命周期目标仍覆盖 SMP 1、4（GICv2）和 16（GICv3）；现有 16 核结果仅 resources，不是长循环。x86/RV64 已有默认 4 核功能基线，应在现有 SMP 路径补交错和资源验证，而不是重做启动；未支持配置不能静默降为 1 核。
- 每次运行记录源码提交、配置、QEMU/编译器版本、随机种子、timeout、pass/fail/skip 和原始输出。
- 结构/逻辑测试按修改范围运行；改变页分配、调度或异常入口时必须运行受影响的真实内核用例。纯文档更新不要求重新跑全部压力。
- 后续记录过独立构建目录六配置验收（`docs/kernel-validation-acceptance.md`，历史预设）和无 QEMU ARM64 新目录构建（`docs/generic-boot-acceptance.md`）。阶段 C 关闭仍需用当前预设和阶段 C 修复后的源码重跑六配置，保存明确的新目录/构建过程证据；不能只引用旧增量日志。

### 12.4 一个问题可以关闭的条件

1. 指向具体修复提交，描述最终保证的不变量。
2. 至少一个验收能在修复前触发真实错误或结构断言失败；修复后通过。
3. 覆盖相关失败回滚；涉及共享对象时覆盖生命周期，涉及 SMP 时覆盖关键交错。
4. 标明验证架构与配置，未验证的平台保留待验收状态。
5. 若只消除了症状而没有解释根因，保留“待定位”，不能标为已修复。

## 13. 尚未解决的定位问题与本次未作的断言

以下问题仍需后续专项实验，本文没有把它们编造成确定答案：

- 原 ARM64 第 28 次停滞的父/子任务与等待队列快照仍缺。当前 exit 已更换实现，需以旧版本重放和新版本长循环确认症状/根因，不能沿用旧 idle PC 推断新失败。
- 原 RV64 hello 后非法指令已有用户返回/退出路径修复；原生 TrapFrame、基本信号返回和信号复制故障已有 3.19～3.21 的专项证据，但未完成全部扩展状态、COW、连续退出和旧故障因果对照，不能宣称整体跨架构语义已完成。
- 3.21 两份 RV64 Debug 用户复制超时继续保留；特别是首个 allocation_fault 的超时没有停机 PC，不能仅凭后续矩阵通过认定根因已经消除。
- 调度迁移和 timer cancel 的具体交错触发概率、影响 CPU 数量，以及页表/TLB 跨 CPU 回收是否还有遗漏，需要在基础内存和所有权修复后用确定性交错验证。
- 未执行特权信号帧利用、x86 用户态内核写入攻击或任意用户指针内核写攻击；隔离问题的结论来自源码和架构权限契约。
- 未完成真实硬件、长期负载、全部 FP/SIMD/向量扩展、完整文件系统格式攻击面、第三方代码、所有锁顺序和每条系统调用的穷尽检查。因此“本文列出的问题全部修复”不等于“内核再无缺陷”。

这些限制不妨碍开始已经有充分证据的修复。应先消除确定的所有权和权限错误，再用更可信的运行状态定位剩余故障。

## 14. 最终建议

Moss 当前最需要的是建立一条可信的工程闭环：**正确拥有内存 → 正确隔离用户态 → 正确保存/恢复现场 → 原子调度与唤醒 → 完整退出回收 → 测试能真实判错**。其中堆预留、退出切换和测试基础已有明确进展，剩余权限、RCU、VM/资源事务和并发验收仍有独立缺口；任何单个演示通过都不足以替代它们。

在此基础上，现有代码完全可以继续演进为可靠的多架构研究内核。优先修复本文 P0/P1，按阶段验收；暂缓新增复杂功能，保留简单、可解释、有界且失败行为明确的实现。后续方案若改变本文判断，应附新的源码或运行证据，并更新对应条目的事实状态与验收结果，而不是只修改完成标记。
