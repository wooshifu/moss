# MOSS 内核能力与待办

> 源码复核基线：2026-09-23，`abeba591`（当次未重跑 QEMU）。之后的当前九预设运行验收见 [moss-todo.md](moss-todo.md) 第 3.59～3.75 节。历史实现与实测保留原日期；源码与旧报告的对应关系见第 3.41 节。
> ADR 迁移复核基线：2026-09-24，`c970c13`；新增能力和服务的状态见下文“ADR 迁移”。上面的 44/44 九预设记录早于该基线；IPC 收敛后的 **53/53 CTest** 见 [3.76](moss-todo.md#376-旧-ipc-与生产-capability-ipc-分界2026-09-24)。
> 启动驱动边界复核：2026-09-24，`290339e`；九预设 **53/53 CTest**、关闭测试的独立生产构建及符号检查见 [3.77](moss-todo.md#377-启动机制与旧设备框架分界2026-09-24隔离工作树)。
> IPC 优先级继承复核：2026-09-24，`ded02f6` 加本次工作树；九预设 **53/53 CTest**，细节与剩余交错验收见 [3.78](moss-todo.md#378-同步-ipc-的传递式优先级继承2026-09-24)。
> IPC 取消与服务死亡复核：2026-09-24，九预设 **53/53 CTest**，两个已认领请求交错见 [3.79](moss-todo.md#379-已认领-ipc-的取消与服务死亡交错2026-09-24)。
> IPC 已认领超时复核：2026-09-24，九预设 **53/53 CTest**；调用方及迟到 reply 的 `ETIMEDOUT` 顺序见 [3.80](moss-todo.md#380-已认领-ipc-的-deadline-与迟到-reply2026-09-24)。
> IPC reply 与取消竞争复核：2026-09-24，九预设 **53/53 CTest**、x64 Debug 重复五次；胜者一致性见 [3.81](moss-todo.md#381-同步-reply-与信号取消的胜者一致性2026-09-24)。
> IPC 优先级倒置时延复核：2026-09-24，九预设 **53/53 CTest**，全部 `users.ipc/priority_latency` 通过；关闭优先级捐赠的 x64 反向对照超时，见 [3.86](moss-todo.md#386-同步-ipc-优先级倒置时延验收2026-09-24)。
> 代码批准实例撤销复核：2026-09-24，九预设最终 **53/53 CTest**，`users.ipc/code_revocation` 全部通过；关闭撤销状态变更的 x64 反向对照失败。并发矩阵中两次 x64 `vfs` 超时已在串行重跑通过，见 [3.87](moss-todo.md#387-原生域代码批准实例与撤销2026-09-24)。
> 批准服务死亡后权能存续复核：2026-09-24，九预设 **53/53 CTest**，`users.ipc/code_service_survival` 全部通过；服务进程退出后的已发行实例准入、保留撤销权及替代进程的显式重新委托见 [3.88](moss-todo.md#388-代码批准服务进程退出后的权能存续2026-09-24)。
> 本文取代旧清单中“完成即可靠”“x86/RISC-V 64 仅为启动桩”的描述。
> 审计问题的原始证据、当前状态及完整验收条件见 [moss-todo.md](moss-todo.md)；MOSS-001～032 沿用原编号，不重新编号。

## 状态口径

- `[x]`：这一行限定的实现或验证已完成，不代表整个子系统已可靠。
- `[ ]`：仍有实现或验收工作；“部分实现”“待专项验收”不算关闭。
- “已有代码”与“实测通过”分别列出。单次 fork/exec、四核启动和宿主测试通过，不能代替隔离、并发、失败回滚或长循环验收。
- 带日期的“工作区”沿用当时验证记录的措辞，不表示当前 `abeba591` 仍有未提交实现；3.40 的运行结果也不表示本次重新测试。
- 原清单重复列出的 fork/exec/wait、信号、内存等能力已按下面的能力表和审计任务归并；不再维护估算行数、过时源码行号及手绘模块依赖图。

## 当前架构与构建边界

MOSS 当前是 C++26 freestanding 内核，三架构已有实际启动、中断、调度及用户态执行路径。内核仍承担 POSIX 进程、ELF、VFS 和启动驱动，但已经有独立的用户态 supervisor、进程兼容服务、文件与命名空间服务、capability 控制 IPC，以及受工厂 capability 授权的原生域构造和不可变代码版本机制；文件服务已有 `/scratch` 易失性平坦目录和内部描述符探针，普通 shell 文件路径仍未迁移。

按 [ADR-0005](docs/adr/0005-generic-kernels-and-independent-runners.md)，同一源码树生成三个 ISA 各自的原生镜像；**同一 ISA 的镜像在满足已支持启动协议和设备契约的机器间复用，不是一个二进制跨三个 ISA 运行**。

| 架构 | 当前启动和硬件发现 | 已有运行证据 | 尚不能声称 |
| --- | --- | --- | --- |
| ARM64 | Linux Image + DTB；PL011/16550；GICv2/v3；PSCI 或 spin-table | Debug/Release 默认四核真实内核测试；GICv3 16 核资源测试；同镜像 QEMU `raspi4b` 合成 DTB 测试 | 任意 SoC、真实树莓派、完整信号投递/阻塞语义 |
| RV64 | Linux Image + DTB/SBI；Sv39/Sv48；16550、PLIC、SBI TIME/IPI/HSM | Debug/Release 默认四核真实内核测试；同镜像 `sstc=false` 双核配置 | 完整信号投递/阻塞语义、并发 COW/VM、AIA/IMSIC/APLIC 支持 |
| x64 | PVH + 内存表、最小 ACPI MADT/SPCR/BDA 发现；IDT、xAPIC/I/O APIC、PIT 校准、SYSCALL、AP 启动 | Debug/Release 默认四核真实内核测试；同镜像 `q35`/`pc`；正常镜像 shell；三个构建配置的页权限和用户违规访存隔离验收（3.30） | 完整信号投递/阻塞语义、并发 VM 隔离、UEFI/x2APIC/完整 ACPI |

表中运行证据来自 [通用启动验收记录](docs/generic-boot-acceptance.md)，有配置与范围限制，不自动推广到所有机器。当前最多 16 CPU、8 个固件 RAM 区域，早期物理映射低于 4 GiB；PFA 已联合管理 kernel_end 以上的合格 RAM bank，保留物理洞。kernel_end 以下仍整体保留，尚不能声称精确回收全部启动内存。

### 已完成的构建与运行拆分

- [x] 架构预设共 9 个：`{arm64,riscv64,x64}-{debug,release,relwithdebinfo}`；每个 workflow 为 configure → build → matching CTest preset（`0a7f1d7c`）。3.40 的九 preset **43/43 CTest** 及 16 CPU/Sv39/GICv3 同镜像补验是当时记录；本轮补 console RX 排队、双读者、跨 CPU IRQ/登记回归、限定 SA_RESTART 重试、CPU-bound 信号、基本 STOP/CONT、SA_NOCLDSTOP、SA_NOCLDWAIT/显式 SIG_IGN 自动回收、信号 action 更新交错、进程组 wait 筛选和成员变更唤醒与 waitpid 停止/继续及信号致死状态验收后，九预设通过 **44/44 CTest**，见 3.62～3.74。3.34～3.40 的初始失败、默认超时与受控变异失败仍保留。
- [x] 独立 configure/build 不查找、不启动 QEMU；workflow 的 test 阶段通过独立 runner 使用 QEMU。
- [x] CMake 产出版本化、相对路径的 `moss-artifacts.json`，只描述架构、构建与产物；机器、CPU、RAM、SMP、固件选项由 runner 决定。
- [x] 删除旧 `*-qemu-*` 预设、生成的 QEMU wrapper/config 和内核平台默认地址；以启动信息填充 `platform::hardware`。
- [x] ARM64/RV64 Image 启动重定位及静态头/重定位校验；x64 使用 PVH ELF。
- [x] 独立验证镜像复用生产内核模块与启动路径，使用 `@@MOSS` 串口协议；宿主负责终止、回收 QEMU，不使用 guest 端模拟器退出设备。
- [x] userspace 是真实 CMake 编译目标，进入编译数据库；正常/验证 initramfs 分离。

入口：`CMakeLists.txt`、`cmake/presets/arch/`、`build.py`、`scripts/artifacts.py`、`qemu.py`、`scripts/kernel_validation.py`、`src/userspace/CMakeLists.txt`。

```sh
# 仅编译，不要求安装 QEMU
uv run cmake --preset arm64-debug
uv run cmake --build --preset arm64-debug

# 完整 workflow，包括 CTest；测试阶段需要 QEMU
uv run cmake --workflow --preset arm64-debug

# 单独测试 / 启动已有正常内核
uv run ctest --preset arm64-debug-test
uv run qemu.py --manifest build/arm64-debug/moss-artifacts.json
```

仅需正常内核时，在 configure 加 `-DMOSS_BUILD_TESTS=OFF`，随后使用独立 build；不要将此配置的“无测试”视为验收通过。当前产物：ARM64/RV64 `moss.bin`，x64 `bin/moss.elf`。完整用法和限制见 [generic-boot.md](docs/generic-boot.md)。

## 已接通的功能，不等于可靠性任务已关闭

| 子系统 | 已有实现 | 剩余可靠性任务 |
| --- | --- | --- |
| Boot / AAL / HAL | 三架构启动、CPU 身份、per-CPU 栈与上下文、异常/IRQ、用户返回、UART；DTB 或 PVH/ACPI 资源发现、SMP/IPI、硬件定时器 | 007、023、029；001 的当前页权限隔离已关闭，真机和未知设备仍需适配/验收 |
| 物理内存与堆 | Buddy PFA、页引用、order 分配/释放、统计；链接预留 8 MiB NOLOAD 堆，256 KiB 起始 arena，扩容限制在预留内；SlabCache/SlabAllocator 代码 | 005 及 008～010/028 的页引用生命周期；004/013 已按限定验收关闭，不能据此声称 COW 或 Slab 并发可靠 |
| 虚拟内存 | 动态页表、用户地址空间、TTBR/CR3/satp 切换、VMA、demand paging、COW、匿名 private mmap、整段匹配 munmap、brk、栈增长与故障诊断 | 002、008～010；共享 uaccess、受控 COW/clone OOM 已有回归，完整 fork 失败与共享地址空间并发事务未闭合 |
| 进程与调度 | ProcessManager、PID/PPID、每线程内核栈、CFS vruntime/权重、内嵌 RB 节点、插入/删除旋转与着色、idle、负载均衡和 affinity；fork/exec/wait/exit/Zombie | 014、017～020、022～023；015/016 已按当前单线程进程和静态 ELF 子集关闭，有 RB 算法不代表调度队列所有权已正确 |
| 信号与系统调用 | syscall dispatcher、kill/sigaction/sigprocmask/sigaltstack/sigreturn、三 ISA 原生帧及基本信号返回、共享用户复制及异常 fixup、clock_gettime/clock_getres/nanosleep；恶意帧和 `clock_getres` 已有九预设通过记录 | 002、007、019～020、031；003 的原生帧输入验收已限定关闭，完整信号投递、阻塞中断和并发 VM 仍待补 |
| ELF / userspace / initramfs | ELF64 checked LoadPlan、逐段 PT_LOAD/VMA 后备、按 ISA 的 trampoline 和 syscall wrapper；静态 mlibc、BusyBox ash、独立 validation 映像、CPIO newc 与 VFS exec | 014；015/016 的事务与受支持静态 ELF 子集已关闭；动态加载、共享 LOAD 页和完整进程继承不是已支持能力 |
| VFS | inode/dentry/File/FdTable、路径/mount/dcache、ramfs、devfs(console/null/zero)、stdio、open/close/read/write/lseek/fstat/dup/dup2/pipe、匿名 pipefs 与有界 I/O 视图；FD 模式检查、稳定引用及 pipe 阻塞/EOF 已实现 | 022、024～026；共享 offset/close 并发、管道多端交错和分配失败注入仍待专项验收 |
| 核心与同步 | C++ 模块、freestanding types/std/concepts、Result、unique_ptr/shared_ptr、klog；ticket/IRQ spinlock、RAII guard、atomics、PerCpuData/计数/队列、MPSC、拥有型锁容器、WaitQueue | 尤其 006、017、018；容器节点/查找引用安全不等于使用者的复合生命周期或调度协议安全 |
| Native IPC 与服务 | 进程局部带权限 capability 表、同步控制调用/一次性 reply、单页共享内存对象；`moss-init` 启动并监护文件/命名空间服务，`/moss-file.elf` 通过它们访问内存中的 `/scratch`；同步 IPC 已按等待依赖传递 RT/CFS 有效优先级，绑核竞争的用户态延迟回归已通过 | ADR-0012 的回复句柄交接、CPU 预算、deadline 传播和死锁策略，0024 的执行域/兼容服务拆分、0018 的实际 VFS 迁移等仍未完成；旧 PID/全局 ID IPC 模块尚保留测试代码 |
| 启动机制与扩展框架 | 中断控制器、计时器及可用串口控制台保留必要的内核启动机制，实际硬件操作仍经 HAL；旧 `DeviceManager`/`Driver` 仅在验证镜像测试生命周期算法；NUMA/hugepage/reclaim/compaction/共享映射等未实现接口显式返回 Unsupported；Process 已有 uid/gid/euid/egid 字段 | ADR-0015 的设备资源 capability、隔离驱动、动态发现和 DMA 限制尚未实现；006、031～032 及其他未实现能力继续追踪 |

主要实现分别位于 `src/boot/`、`src/aal/`、`src/hal/`、`src/drivers/`、`src/mm/`、`src/containers/`、`src/process/`、`src/kernel/`、`src/vfs/`、`src/userspace/` 和 `third_party/mlibc/`。下面以稳定审计编号追踪未完成工作，详细源码符号见 [审计状态表](moss-todo.md#4-问题总表与当前状态)。

## ADR 迁移

ADR-0008～0033 是已接受的目标边界，并非当前实现的完成声明。下面仅勾选行内明确限定的切片；旧 MOSS-001～032 审计项仍按原验收标准追踪。

- [x] ADR-0009/0010/0011 的第一条可运行路径：进程局部 capability、同步控制调用和单页共享内存已供独立服务使用；不代表通用权限撤销或完整数据面已完成。
- [x] ADR-0014/0018 的第一条服务路径：`moss-init` 监护文件与命名空间服务，后者只解析 `/scratch`，服务死亡后的重连由用户态显式处理；内核 VFS 仍处理普通文件。
- [x] 生产内核不再创建或链接旧的 PID/全局服务 ID IPC 管理器，也不再把它的消息计数展示为 native IPC 统计；旧模块仅留验证镜像专项回归，不作为新服务 ABI。九预设 53/53 CTest 及生产 ELF 符号检查见 3.76。
- [x] 生产启动不再创建 `DeviceManager` 或通过 `BootDriver` 重新包装已启用的中断控制器、计时器和控制台；旧匹配/回调框架仅链接验证镜像。九预设 53/53 CTest、关闭测试的独立构建及符号检查通过；内核仍保留上述启动机制，资源 capability 和隔离驱动服务仍未实现（ADR-0015，见 3.77）。
- [x] 同步 IPC 的等待依赖参与内核 RT/CFS 有效优先级计算；嵌套调用、多调用者、基础 nice 变更、服务线程死亡和恢复有内核回归，另有实际三进程嵌套 IPC 往返（ADR-0012/0023，见 3.78）。
- [x] 已认领 IPC 请求收到信号后取消调用，过期 reply 返回 `EPIPE`；服务持有 reply 退出时等待者得到 `EPIPE`，原通道由替代服务继续使用（ADR-0012，见 3.79）。
- [x] 已认领 IPC 请求到期后，调用方与迟到 reply 均返回 `ETIMEDOUT`；服务只在调用方报告超时后回复（ADR-0012，见 3.80）。
- [x] 已认领同步调用在 reply 先提交时保持回复结果，在信号与 reply 并发时调用方和服务端只观察到一致的胜者（ADR-0012，见 3.81）。
- [x] 初始启动域可铸造可转交的执行域工厂 capability；持有 `DOMAIN_SPAWN` 权限的用户态装载者可提交页面、初始栈和显式继承句柄来启动无 POSIX 父关系的原生域。三架构九预设 53/53 CTest 与实际新域执行通过（ADR-0024/0025，见 3.82）。
- [x] 原生域的可执行页面只能取自不可变代码版本；代码版本可读取供策略服务审核，必须另持 `CODE_APPROVE` 才能派生 `CODE_EXEC` 句柄。域工厂、普通内存写权限及未批准版本均不能直接授予执行权。九预设 53/53 CTest 与新域执行通过（ADR-0026 的机制切片，见 3.83）。
- [x] 一个不可变代码版本可覆盖至多 4096 页，批准一次后按页索引构造可执行页；65 页范围的快照、读取及新域执行已验证，旧单页接口保留（ADR-0025/0026 的装载前提，见 3.84）。
- [x] 审批服务可用要求 `MAP_READ` 的内核接口取得代码版本完整页数，避免依赖请求者报告的长度；单页、65 页及仅执行句柄权限边界已验证（ADR-0026，见 3.85）。
- [x] 原生新域的代码批准具有独立实例和作用域；撤销旧实例阻止复制、IPC 转交及重批后的旧句柄新建可执行域，已准入域仍能正常退出，批准/撤销/目标识别权限可独立削减（ADR-0028/0030/0032 的限定机制，见 3.87）。
- [x] 已发行批准在发行子进程退出后仍可准入原生域；父进程保留的独立撤销权继续有效，未受委托的替代进程不能取得旧权能，显式重新委托可发行新实例（ADR-0031/0032 的限定生命周期验收，见 3.88）。
- [x] 三架构真实用户态 IPC 在同核八个中优先级 CPU 负载下，由高优先级调用低优先级服务，服务完成计算后在调用截止前回复；九预设通过，关闭捐赠的 x64 反向对照超时（ADR-0012/0023 的限定时延验收，见 3.86）。
- [ ] 补齐回复句柄交接；CPU 预算、deadline 传播和死锁策略仍需单独设计（ADR-0012/0023）。
- [ ] 建立 capability 寻址的执行域、用户态兼容进程与普通程序 Loader Service，逐步迁出内核 PID/信号/ELF 政策（ADR-0024/0025）。
- [ ] 将实际 VFS、pager 和非启动设备迁到隔离服务，补资源授权、失败恢复和 DMA 限制；保留有依据的启动机制例外（ADR-0015～0019）。
- [ ] 将代码批准及撤销准入扩展到普通 exec、fork、权限升级和 pager，建立 Code Authority Service、supervisor 独立保留的撤销作用域及启动信任链，验证服务失败、并发撤销和真实平台交接；现有原生域机制不等于这些目标已实现（ADR-0026～0033）。

## P0：隔离与基础所有权

优先级沿用审计，不沿用旧清单按“新增功能”划分的 P0/P1。

- [x] **MOSS-001（3.30 已关闭）**：x86 内核映射去掉 USER；收紧三架构最终内核 W^X，验证 supervisor/RO/NX 权限及受控用户异常隔离。
  - [x] 内核叶子/专用上级表默认 supervisor-only；用户页表显式启用 U/S，内核 map_page 拒绝 USER 属性（工作区，moss-todo.md 第 3.11 节）。
  - [x] 生产内核表与活动用户页表结构检查；三架构 Debug/Release CTest 15/15，RV64 额外 Sv39 用户路径通过（3.11）。
  - [x] 三架构最终 text RX、rodata RO/NX、其余 RAM/device RW/NX；全部直映 NX，text/rodata 别名只读。共享内核子树按 VA 归属借用，生产创建/clone/回收与六配置 CTest 通过（工作区，3.12）。
  - [x] 五类内核对象（text/rodata/data/活动页表/MMIO）的 identity/direct-map 用户读写共 20 次均只终止违规子进程；父进程用户页、内核哨兵与根表保持，三架构九配置及同镜像 Sv39/GICv3 补验通过。合法用户地址替换攻击目标的反向对照准确变红（3.30）。
- [ ] **MOSS-002**：统一用户地址域，限制 VMA/mmap，移除 syscall 0 原始 UART 指针旁路；实现可恢复、跨页/跨 VMA 的 uaccess。
  - [x] 用户域、VMA 准入与保留 sigreturn 页检查；mmap/munmap 拒绝完整范围溢出和不支持的 flags，复制策略覆盖相邻 VMA（工作区，3.13）。
  - [x] 删除分发前原始 puts，复用有界字符串路径；超长字符串报错而非静默截断，真实 syscall 检查已加入 users（3.13）。
  - [x] 三架构共享复制及异常 fixup；真实 PFA 耗尽时输出复制返回 EFAULT，进程存活且释放压力后同址重试成功（3.20）。
  - [x] VFS 使用有界输入/输出视图、信号帧复用共享复制；输入/输出缺页 OOM、跨页部分 I/O、失败后的 offset/管道数据保留及未复制输入尾部清零通过九配置（3.21）。
  - [x] 真实 fork/PFA 耗尽下的 COW 输出 EFAULT、跨页部分复制、直接用户写隔离及恢复后恰好一页分裂；PTE/内容/引用与 reap 后完整资源基线保持，九配置 CTest 43/43、Sv39 Debug/Release 同镜像补验通过（工作区，3.31）。
  - [x] 地址空间返回持有快照，发布/取得引用使用短 IRQ 锁、锁外析构；替换、exit 摘除和 Process 销毁不提前释放读者的页表/数据/ASID。双 CPU 持有读者红绿回归、九配置及 Sv39/GICv3 补验通过（3.32）。
  - [x] 每地址空间的软件 VM 事务；缺页桥接全程持有同一 VMA/root/backing 上下文，fork/mmap/munmap/brk 使用同一锁。双 CPU COW/demand 重复故障与资源回收有红绿回归，九配置及同镜像 Sv39/GICv3 通过（3.33）。
  - [x] 公共 uaccess 绑定所持地址空间版本，逐页短 VM 租约覆盖解析和物理别名复制；版本替换、复制与 unmap/fork 竞争、用户 PTE A/D 及独立 raw fixup 已有回归，九配置最终 43/43 CTest 及同镜像 Sv39/GICv3 通过；原始失败和验证边界见 3.34。
  - [x] ARM64 按地址失效改为全 ASID/全层级广播，MM 复用 AAL；真实活动 root 的本核/远程重映射、去广播红例与架构指令门禁通过。QEMU 未暴露旧 ASID 错误，不能把它的通过外推为真机验收（3.35）。
  - [x] x64/RV64 同步远程 TLB 请求—确认、IRQ 关闭时的协作处理与原生 IPI 入口；三架构本核/远端、VM 锁竞争、纯 IPI 和表页剪枝五场景通过，锁/IRQ 变异与 RV64 控制台互等的红绿证据见 3.36。
  - [x] 两个 CPU 分别先获 TLB 发布锁的确定性交错；双地址空间/不同 VA 的真实重映射、旧/新翻译与资源恢复、去除发布锁协作处理的负向对照已补（3.37）。ARM64 使用原生广播并发验收。
  - [x] 活动硬件 root 独立持有页表/数据/ASID，切换后才退休；原生缺页绑定实际安装版本。双 CPU 替换/销毁、用户/内核 root 退休红绿回归、九配置及 16 CPU/Sv39/GICv3 同镜像补验通过（3.38）。
  - [x] 真实启动中首个辅助 CPU 注册与 TLB 请求的两个确定性顺序；硬件旧/新翻译、目标 mask 和资源基线、漏注册刷新/漏目标的双架构负向对照通过。九配置 43/43 CTest 及同镜像 16 CPU/Sv39/GICv3 补验通过，不外推为热插拔或全部交错验收（3.39）。
  - [x] 单次 `execve` 的 pathname、argv/envp 指针及字符串固定从同一持有的地址空间版本读取；路径读取后强制发布另一版本的真实用户态回归先红后绿，九配置 `users.exec` 与 CTest 43/43 通过（3.42）。
  - [x] 双 CPU 缺页与同址 `unmap` 的确定性交错：缺页持 VM 事务时对端等待，提交后摘除 PTE/VMA 并回收页；去掉验证侧 `unmap` 锁只有新增用例变红，九配置 `mm.concurrent` 与 CTest 43/43 通过（3.43）。
  - [x] 未支持多线程 `exec` 前先拒绝不安全组合：已有第二线程时 `execve` 返回 EAGAIN，准备期间注册线程失败；两条真实 `execve` 回归各有旧实现红例，九配置 `users.exec` 及 CTest 43/43 通过（3.44）。
  - [x] 双 CPU 缺页与 fork 页表克隆的确定性交错：克隆等待缺页提交，父子 COW/ref 与资源基线一致；验证侧去锁只让新增项变红，九配置 `mm.concurrent` 各 4/4、CTest 43/43 通过（3.45）。
  - [x] 双 CPU fork 页表/VMA 克隆与同址 `unmap` 的确定性交错：克隆后摘除父映射，子 COW/ref 与资源基线一致；验证侧去锁只让新增项变红，九配置 `mm.concurrent` 各 5/5、CTest 43/43 通过（3.46）。
  - [ ] 完整共享 exec 的线程/root 协调、异步访问的长期页 pin 及更多并发 unmap/fork/fault 交错；不把同步页租约、root 拥有权或所测 TLB 场景当作完整硬件访问隔离。
- [x] **MOSS-003（3.40 已关闭）**：将用户信号帧/altstack 当不可信输入，安全复制并净化 PC/SP/特权状态。
  - [x] 信号帧 V2、原生 GP、PC/SP 用户域和按 ISA 的状态白名单、x86 MXCSR 检查；基本信号返回在三架构九配置通过（`3461c749`，3.19）。
  - [x] 信号帧写出/读回移除普通用户指针循环；合法未驻留备用栈 OOM 只终止目标子进程，伪造 sigreturn SP 在 OOM 时返回 EFAULT，九配置通过（3.21）。
  - [x] 注册栈容量越界先红后绿；恶意特权字段、无效 PC/SP/magic/帧地址、只读/未映射/回绕及内核栈地址、注册后撤销/只读替换、合法嵌套 GP/标志/mask 往返和内核哨兵验收通过，九配置 43/43 CTest（3.40）。不外推为完整信号语义、共享 exec 协调或真机验收。
- [x] **MOSS-004（`e7fe8c91`）**：当前堆、活动页表树/early pool/链接表区、PFA 元数据布局与耗尽校验和，以及坏布局启动拒绝已验证；不外推到并发进程页表生命周期。
  - [x] heap 耗尽返回失败，缓冲区模式/PFA 页哨兵及页计数不变，释放后可重新分配合并大块。
  - [x] 4/64/256 KiB 边界写入及堆/PFA 耗尽时检查页表与元数据；三架构 Debug/Release 拒绝重叠堆布局（[证据](moss-todo.md#37-堆页表与-pfa-元数据所有权2026-09-06工作区)）。
- [ ] **MOSS-005（部分验收）**：保留洞、区间重叠/非对齐、容量/溢出、多 bank 耗尽及当前布局已验证；x64 PVH 非 RAM 描述符已纳入保留集合，initrd 与保留区别名会被拒绝。仍保守保留 kernel_end 以下，需补完整启动保留集合、回收和其他 PVH 异常内存表。
  - [x] x64 PVH 非 RAM 区间与主 RAM 重叠时，启动保留优先于可分配 RAM；旧逻辑错误完成启动，修复后三构建配置八场景及 CTest 43/43 通过（3.47）。
  - [x] x64 PVH 非 RAM 条目长度溢出时启动早期拒绝；旧逻辑错误完成启动，三构建配置九场景及 CTest 16/16 通过（3.48）。
  - [x] x64 PVH initrd 同时落在 RAM 与非 RAM 描述符内时拒绝启动；旧逻辑错误完成启动，三构建配置十场景及 CTest 16/16 通过（3.49）。
  - [x] x64 PVH 重叠 RAM 描述符在内存初始化阶段拒绝；移除 PFA 重叠检查会在后续触发 GP，三构建配置十一场景及各自 PVH CTest 通过（3.50）。
  - [x] x64 PVH 内存表计数超 128、描述符保留位非零及 RAM 起点达到 4 GiB 时启动早期拒绝；三构建配置十四场景及各自 PVH CTest 通过（3.51）。
  - [ ] 完整显式保留集合、kernel_end 以下可用页回收及其他 PVH 异常表。
  - [x] 三架构 PFA 耗尽不返回保留页；逐页模式、initrd 校验和及释放后页数恢复通过。
  - [x] ARM64/RV64 Debug/Release 固件输入共 18 项检查；有效区间末端 UINT64_MAX 在裁剪前取整导致回绕的问题已修复，异常输入在启动阶段拒绝（[证据](moss-todo.md#35-pfa-分配归属与固件边界2026-09-06工作区)）。
  - [x] PFA 联合管理 kernel_end 以上多 bank、排序/合并相邻段、独立排除元数据；RV64 实际 DTB 验证两段/乱序八段、元数据放入后续 bank、非对齐相邻段及 RAM 表溢出（[证据](moss-todo.md#36-多-ram-bank-分配2026-09-06工作区)）。
- [ ] **MOSS-006（部分修复）**：伪 RCU 已删除，锁容器及全部调用者已迁移；生产启动改为直接验证计时器并初始化控制台，旧 `DeviceManager` 仅留验证镜像；IRQ 注销已等待旧回调退出，IPI 部分注册失败已回滚；旧 IPC 服务/连接发布与注销已串行化，但模块现仅留验证镜像。实际硬件回调、隔离驱动资源解绑和 native IPC 进程清理/在途消息仍待闭合。
  - [x] 删除 RcuPtr 隐式析构；A/B/C 插入误删可达值已复现并修复（`c895c876`，moss-todo.md 第 3.8 节）。
  - [x] LockedList/LockedHashMap 查找复制值/拥有者，串行化摘除与发布；析构及快照回调在锁外执行（工作区，3.9）。
  - [x] 持有读者、1,024 次清空/复用、回调/析构重入与真实双 CPU 同 key 创建/删除交错用例（工作区，3.9）。
  - [x] 启动时沿用 boot-owned irqchip 和 timer、验证计时器就绪并按发现的 UART 初始化 console；HAL 承担实际硬件操作。3.41 的 `BootDriver` 静态注册是历史状态，现已退出生产路径（3.77）。
  - [x] IRQ 描述符注销停止新回调并等待已有回调退出；双 CPU 去同步红例、九配置目标用例、完整 CTest 43/43 及宿主回归 151/151 通过（3.52）。
  - [x] 硬件 IPI 部分 SGI 注册/启用失败回滚；故障注入覆盖原有注册、两个启用失败和失败后重试（3.53）。
  - [x] IPC 服务注销与连接发布竞态已修复；旧实现会发布孤儿通道，三架构真实内核服务/连接/回收用例通过（3.54）。
- [ ] **MOSS-007（部分实现）**：原生 TrapFrame 与用户返回已统一，完整扩展状态及全异常交错仍待验收。
  - [x] 三 ISA 原生帧单指针入口、全部字段/大小偏移断言、六参数/GP/条件码往返与 native sigreturn；默认 users.frame 三项九配置通过（`3461c749`，3.19）。
  - [x] 首次上下文/入口栈发布提前关 IRQ，恢复调用者原有 IRQ 状态；已捕获 RV64 `sd tp, 256(sp)` 覆盖初始 PC 的真实交错。修复前 1/4 CPU 注入均失败，修复后各 10 次通过，Sv39 另 3 次同镜像通过，均包含完整 users.vm（3.18）。
  - [ ] 完整异常入口与抢占交错验收；最初自然 `pc=0` 报告 `1789054088760333000` 未抓寄存器现场，不能以本次确认的一个原因宣称所有历史/后续启动故障已解决。
- [ ] **MOSS-008**：仅可写私有页允许 COW；RO/text/NX/NONE 不能因 fork 或 fault 被放宽权限。
  - [x] clone 保留真实只读页，fault 检查可写 VMA；真实页表三代引用/释放及用户态多代 COW、最后引用写入、fork 后 text/rodata 写入拒绝，三架构九配置默认回归通过（3.16）。
  - [x] 双引用 COW 页分配失败不放宽权限或损坏内容/引用；共享页错误放开写权限的变异准确变红，恢复后单页分裂、用户异常和回收验收通过（3.31）。
  - [x] 修复 `mm.concurrent/fault_unmap` 的提交页身份采样竞态；旧版可复现、修复版 200 次通过且去锁红例仍失败（3.55）。
  - [ ] 同时写故障、fork/unmap 交错的 VMA/PTE/ref/TLB 事务锁及并发 OOM 回滚；fork 前只读写入和遗留 COW/VMA 权限冲突的独立异常验收。

## P1：VM、进程、并发、VFS 与验收

- [ ] **MOSS-009**：修正 RV64 demand/COW 分类及 PPN 编码；已驻留页不得被缺页路径重新填充。
  - [x] fault 区分读/写/执行，demand 拒绝驻留页与无权限访问，COW 使用 HAL PTE 编码；匿名页内容隔离、PROT_NONE/NX 拒绝在三架构及 RV64 Sv39/Sv48 上通过（3.16）。
  - [x] 受控 COW OOM 保持旧物理页与双引用，恢复后的编码/属性与单页分裂、父子退出后的页回收通过真实压力验收（3.31）。
  - [ ] 补多物理地址编码往返、其他拒绝访问零额外分配，以及覆盖/并发失败时的页引用生命周期专项；不以单线程父子回归替代并发故障注入。
- [ ] **MOSS-010**：页表 clone/map 显式失败、完整回滚；不得发布部分成功的 fork。
  - [x] map 拒绝已有叶子与中间 block；完整缺失路径准备失败不改原树/数据页引用，逐级真实 PFA 耗尽与恢复后成功通过（3.17）。
  - [x] clone 校验与表页分配先于 PTE/引用修改，显式返回错误且 fork 检查结果；跨表级/分支的逐点 PFA 耗尽、父页权限/数据/引用保持、目标拒绝及恰好足够预算成功，三架构九配置与 Sv39 专项通过（3.17）。
  - [x] AddressSpace 对象和共享引用控制块的堆分配失败均显式返回 OOM，归还页表、ASID 和堆对象；真实控制块耗尽、泄漏变异反例、九配置及 Sv39 补验通过（3.32）。
  - [ ] 把 clone 成功后的 kernel-stack、VMA、FD 等失败纳入整次 fork 的事务，补真实系统调用逐点 OOM 验收；3.33 已补父地址空间软件事务，更多并发 clone/unmap 交错与活动 root 协调仍待完成。
- [x] **MOSS-011（3.23 已关闭）**：活跃 ASID 使用加锁租约表，地址空间销毁前不复用，ARM64 复用前广播失效；255 个用户标签耗尽显式失败。
  - [x] `mm.transactions/asid_leases` 验证全部活跃标签唯一、耗尽、隔项释放后只复用已归还标签并恢复页计数；当前三架构 Debug/Release 均通过。
  - [x] `users.signals/pid_lifecycle` 顺序创建 300 个子进程，在同一用户 VA 写入逐次唯一模式，交替迁移 CPU0/CPU1 并重复调度；父地址空间始终保持原值，当前六配置均通过。
- [x] **MOSS-012（3.24 已关闭）**：brk/mmap/munmap 的成功操作同步维护 VMA/PTE/引用/TLB；PROT_NONE、缩堆、重新增长与增长冲突均有真实用户态回归。
  - [x] HEAP 从固定 `HEAP_START` 的空 VMA 开始；跨整页缩小先清 PTE、失效 TLB、递减引用并回收空页表，再发布新端点。页内缩小明确保留所在页。
  - [x] `users.vm/brk_lifecycle` 的修复前 ARM64 Debug 红例与当前六配置绿例均保留；`mm.permissions`、`mm.transactions` 及 libc/exec/BusyBox 消费者路径同步通过。
  - [x] 部分 munmap 仍被明确拒绝，只支持整段匹配；这是后续兼容能力，不把未执行的拆分伪报成功。共享地址空间并发 fault/resize 锁仍归 008～010。
- [x] **MOSS-013（`e7fe8c91`）**：heap 对齐/极值/错误释放和 PFA 原分配头/order、边界、保留洞分段及耗尽/合并已验证；SMP 页引用生命周期仍归 008～010/028，不能以此声称 COW 安全。
  - [x] PFA 拒绝错误 order、内部/非对齐地址、重复释放及仍有共享引用的整块释放，失败不部分改变元数据、数据或统计。
  - [x] heap 返回地址对齐、溢出拒绝、原块/请求大小追踪与错误释放检查；4,096 次混合分配/释放和计数恢复，六配置真实内核测试通过。
- [ ] **MOSS-014（部分实现）**：RV64/x86 首次用户返回及 GP 快照已补；补 fork 不立即 exec 的寄存器、VM 游标、凭据、FD 及扩展状态继承。
  - [x] fork 从实际用户帧复制 GP，不再猜栈顶偏移；ARM64 x30 和 x86 CF 修复有不立即 exec 的用户汇编回归，九配置通过（`3461c749`，3.19）。
  - [x] x86 legacy FP 实时快照/切换/exec 默认化；yield、fork、真实 CPU1 子进程状态及 x87 异常后父继续的子检查（工作区 3.14）。
  - [ ] 全扩展状态与信号帧；显式 users.simd_fault 在本机 TCG 仍失败，不能算 #XM 验收。
- [x] **MOSS-015（3.25 已关闭）**：exec 在独立地址空间完成参数、ELF/VMA、栈和返回现场准备后一次提交；失败保留旧进程并回收全部临时所有权。
  - [x] 不存在文件、损坏 ELF、超限/坏指针、页表逐级 PFA OOM，以及参数、可变映像对象/控制块/字节、地址空间、部分 VMA 的六级真实 heap OOM 均返回错误，旧 AS/root hash/name、栈 canary 与 FD offset 保持。
  - [x] 可变 ramfs ELF 使用可失败共享所有权和独立字节快照；libc 回归验证成功 exec 后 backing 可截断/删除，以及普通 FD 保留、CLOEXEC FD 关闭。
  - [x] 当前六个 Debug/Release 预设的 `users.exec`、VM/页表及 libc/BusyBox 消费者共 73 case 全通过；多线程共享地址空间 exec 仍归 002/008～010。
- [x] **MOSS-016（3.26 已关闭）**：ELF checked arithmetic、不可变 LoadPlan、段/入口/权限校验、边界页与明确重叠策略已有构造验收。
  - [x] 自有 userspace 链接产物 RX/R/RW 页分离，覆盖 x86 large-model/small-data/GOT；不关闭外来 ELF 加载器校验（3.14）。
  - [x] loader 一次生成并只读消费固定容量 LoadPlan；拒绝截断/错误 header、文件与地址回绕、`filesz > memsz`、W+X、非法对齐、保留区/页重叠、过多段、解释器/动态段、孤立 file-backed TLS 和非可执行入口。
  - [x] 独立非页对齐 RX/RW fixture 检查页前缀、跨页文件字节、BSS/圆整页尾清零、真实执行/写入，以及 RX 禁写、RW 禁执行；严格拒绝 oracle 防止误接受伪装成成功，六配置 `users.exec` 28/28 通过。
  - [x] 当前契约明确拒绝 PT_INTERP/PT_DYNAMIC 和共享页的 LOAD；纯 NOBITS TLS 可作为 runtime 元数据，file-backed TLS 必须由一个 LOAD 覆盖。关闭本项不表示支持动态加载、内核 TLS 初始化或任意可加载 ELF。
- [ ] **MOSS-017**：原子取任务、去重入队、on-CPU 交接、迁移锁序和 affinity；对已有 RB 算法补不变量/交错测试，不重写一套。
  - [x] current task、初始上下文、地址空间及入口栈在本 CPU 的同一 IRQ 临界区内发布；这不替代跨 CPU 的运行队列/on-CPU 所有权协议（3.18）。
  - [x] 修复实际双 CPU 用例暴露的 ARM64 从核未开启 MMU：复用主核已建页表，在各从核配置 MMU 和高地址直映后才发布 online（工作区，3.9）；不代表运行队列所有权或运行中任务迁移已完成。
  - [x] ARM64 激活从核时补齐 `Active` 发布后的 `DSB SY → SEV` 完成顺序；三模式构建、Debug/Release CTest 5/5 与实际指令检查通过，偶发挂起完整验收另列（3.15）。
  - [x] 调用线程收紧自身 affinity 时，在返回用户态前经 bootstrap 保存 continuation，再发布到目标 CPU；旧实现的 cycle 1 确定性红例、单例绿例及 32/32 并发压力已保留（3.28）。远程目标、一般抢占迁移及完整 on-CPU 协议仍未关闭。
- [ ] **MOSS-018（部分修复）**：wait/console 已有登记—睡眠协议和坏 status 可重试；child-exit 交错、wait EINTR、真实 RX 排队窗口、双读者及跨 CPU IRQ/登记交错已验收；SA_RESTART 限定重启子集亦已通过，历史 SMP 超时因果仍待确认。
  - [x] `sys_wait4()` 准备睡眠后登记 waiter、重查 Zombie；status copyout 失败不 reap。console 受锁保护检查/登记、支持多 waiter，已有 console 信号中断用例（3.41）。
  - [x] child exit 在 wait 登记前完成真实唤醒的双 CPU 顺序已有验证钩子固定；移除登记后的 Zombie 重查使新用例超时，恢复后九预设 `users.signals` 各 20/20、完整 CTest 43/43（3.59）。
  - [x] waitpid 被捕获信号唤醒后返回 EINTR，保留未写的 status 和可重试回收；旧实现超时红例、九预设 `users.signals` 各 21/21、完整 CTest 43/43（3.60）。
  - [x] ARM64 Debug 在 console 空 ring 检查之后、waiter 登记之前暂停，确认真实 PL011 RX 已排队后恢复，shell 完成；去掉 waiter 登记的负向变异在首次命令超时（3.62）。该探针不强制 IRQ handler 并发，另见 3.64。
  - [x] CPU1/CPU2 两个 `/dev/console` 读者都进入真实 read 后，宿主注入 `ab`；三架构九预设各 22/22，单 waiter 唤醒变异只使新用例超时（3.63）。RV64 验证轮询读者，ARM64/x64 验证等待队列。
  - [x] ARM64/x64 的 CPU1 读者持锁停在空检查与等待登记之间，注入真实串口字节使 CPU0 IRQ handler 抵达同一锁；读者登记后成功读回，去掉 waiter 发布的负向试验超时（3.64）。RV64 是轮询路径，不运行该 IRQ 专项。
  - [x] SA_RESTART 限定重启子集：waitpid、pipe 读写及 console 读；部分 I/O 保持已传字节数，nanosleep 仍返回 EINTR（3.65）。
  - [ ] 3.14～3.15 的 ARM64 Debug `containers.smp` Zombie 超时及 CPU3 WFE 现场保留为历史失败；当前镜像同配置 100 次通过且已有逐次重放脚本，旧故障因果仍未确认，不写成当前稳定超时（3.58）。
  - [x] 独立诊断确认本机 QEMU MTTCG 事件已置位但宿主线程仍睡眠；不加载 Moss 也能稳定复现，仅补宿主 kick 即继续。该诊断不算内核修复或 SMP 验收，正式 runner 模式不变（3.15）。
- [ ] **MOSS-019（部分修复）**：三架构 nanosleep、timer 容量错误、同步取消、deadline 溢出及捕获信号的 EINTR 已实现；补更广跨 CPU 定时交错。
  - [x] `sleep_until()` 以 prepare/arm/commit 交接睡眠；arm 失败回滚，返回前同步取消栈上 timer（3.41）。
  - [x] `clock_getres` 以原生 `u64` 纳秒 ABI 实现：支持 clock-id 0/1、以 `ceil(10^9 / frequency_hz)` 报告硬件 tick，拒绝空输出指针和其他 clock-id；用例已进入默认 `users.timers`，最近九预设矩阵包含它。2026-09-20 的 ARM64 Debug 5/5 是较早记录。
  - [x] `nanosleep`/`clock_nanosleep` 在真实跨 CPU SIGUSR1 唤醒后返回 EINTR；相对调用写剩余纳秒，绝对调用不改 remaining；旧实现断言失败，九预设 `users.timers` 各 10/10（3.61）。
- [ ] **MOSS-020（部分修复）**：统一 syscall/IRQ 返回信号检查；结果/handler 参数写回、SIGCHLD 基本投递、pipe/console、wait 和 nanosleep 信号中断已有覆盖；SA_RESTART 限定重启子集、CPU-bound IRQ 返回投递、基本 STOP/CONT 交接、SA_NOCLDSTOP、SA_NOCLDWAIT/显式 SIG_IGN 自动回收、信号 action 一致快照与忽略动作的待处理清理、进程组 wait 筛选和成员变更唤醒及 waitpid 停止/继续与信号致死状态报告已验收，继续补完整 job control 与更广交错。
  - [x] 统一用户返回检查点、结果先写回及准确终止 signo；三 ISA 基本信号与 handler 嵌套 syscall 后的 GP/返回值恢复通过（`3461c749`，3.19）。
  - [x] 默认 `users.signals` 已有 SIGCHLD、SIGPIPE、pipe/console 中断及部分传输用例；不等于所有阻塞调用已处理信号（3.41）。
  - [x] 被捕获的 SIGUSR1 中断 waitpid 后返回 EINTR，status 未写且子进程仍可 reap；九预设 `users.signals` 21/21（3.60）。
  - [x] 捕获信号中断相对/绝对 nanosleep 的返回与 remaining 语义；九预设 `users.timers` 10/10（3.61）。
  - [x] SA_RESTART 对 waitpid、pipe 读写及 console 读重试；部分传输与 nanosleep 不重试，libc 标志往返通过（3.65）。
  - [x] CPU1 纯用户计算循环经两次 IRQ 返回后，CPU0 发 SIGUSR1；子进程不再执行系统调用即可进入 handler 并正常退出。x64 空闲后重新装载单次 LAPIC 定时器（3.66）。
  - [x] STOP 后切出 CPU、CONT 后恢复、已屏蔽或忽略 CONT 仍能唤醒，停止期间 SIGKILL 及 STOP/CONT pending 互斥；三架构 Debug `users.signals` 各 27/27（3.67）。
  - [x] `waitpid(WUNTRACED/WCONTINUED)` 一次性报告停止/继续，不带选项时不误报，EFAULT 不消耗事件；父进程阻塞后子进程自发 STOP 的真实唤醒通过三架构 Debug `users.signals` 各 28/28（3.68）。
  - [x] `waitpid` 区分 SIGKILL/SIGSEGV 致死和普通 `_exit(-SIGSEGV)`；旧 VM/uaccess/FP 等断言同步更新，三架构 Debug `users.signals` 各 29/29 且九预设 CTest 44/44（3.69）。
  - [x] `SA_NOCLDSTOP` 抑制停止/继续的 SIGCHLD，但保留 waitpid 状态与唤醒，退出仍发送 SIGCHLD；三架构 Debug `users.signals` 各 30/30、`users.libc` 各 2/2，九预设 CTest 44/44（3.70）。
  - [x] `waitpid(0/-pgid)` 按子进程进程组筛选停止、继续与退出状态，无匹配子进程返回 ECHILD；三架构 Debug `users.signals` 各 31/31，九预设 CTest 44/44（3.71）。
  - [x] `SA_NOCLDWAIT` 或显式 `SIG_IGN` 自动回收退出子进程，等待中的父进程返回 ECHILD；三架构 Debug `users.signals` 各 32/32、`users.libc` 各 2/2，九预设 CTest 44/44（3.72）。
  - [x] 子进程通过 `setpgrp`/`setsid` 在父进程睡入 `waitpid(0)` 后离组，唤醒父进程返回 ECHILD；三架构 Debug `users.signals` 各 33/33，九预设 CTest 44/44（3.73）。
  - [x] `sigaction` action 用进程锁保护完整快照与替换；oldact 复制后遇受控更新会重试，三架构 Debug `users.signals` 各 34/34、`users.libc` 各 2/2、`users.exec` 各 31/31，九预设 CTest 44/44（3.74）。
- [x] **MOSS-021（3.23 已关闭）**：信号状态由 `Process` 拥有；fork 复制 disposition、线程 mask/altstack，exec 重置非忽略 disposition 与 altstack，退出随进程回收。
  - [x] 继承/exec-reset 用例及跨越原 256 槽边界的 300 次生命周期用例在当前三架构 Debug/Release 均通过。
- [ ] **MOSS-022（实现已修复，验收部分）**：进入 Zombie 前关闭 FD、析构兜底及 exec 继承已接通；专项 warmup 在 wait/reap 前观测最后 writer 关闭后的 EOF。
  - [x] 默认 `users.lifecycle` 的 1,000 次 fork/COW/信号/timer/pipe/exec/exit/wait 与资源恢复随最近记录的九预设矩阵通过（3.40～3.41）。
  - [ ] 3.23 的 RV64 Debug 默认 30 秒超时和 60 秒诊断保留为旧实测；补原故障因果对照及更广负载/CPU 配置，不把旧报告写成当前稳定失败。
- [ ] **MOSS-023（部分验收）**：已复用 `context_switch` 返回活跃 bootstrap 栈，默认 4 CPU 的 1,000 次生命周期与资源检查有九预设通过记录；补原第 28 次停滞的前后对照及 1/16 CPU 长循环。
- [ ] **MOSS-024（部分修复）**：FD 访问模式、稳定 File 引用、FD/File 池锁及 ramfs offset 锁已有实现；补共享 offset、close/read 与池分配/关闭的确定性并发验收。
- [ ] **MOSS-025（主路径已实现）**：pipe 空且有 writer 时阻塞，最后 writer 关闭才 EOF；满缓冲区等待、EPIPE/SIGPIPE、O_NONBLOCK/部分传输已有实现。补多读写者/端点关闭的确定性交错和写入原子性验收。
- [ ] **MOSS-026（部分修复）**：inode/pipe/File/FD 槽复用、端点清理及用户 copyout 失败回滚已有实现，1,000 次 pipe 复用与 FD 回滚有用例；补各创建阶段故障注入和并发池复用验收。
- [x] **MOSS-027（3.22 已关闭）**：PVH 模块表/可用 RAM 校验、严格 newc 和真实完成标记已修复；同一生产镜像的大小/位置变化、缺失/非法模块、坏 archive 与必需 init 失败七场景在 Debug/Release 通过且无假完成标记。
- [ ] **MOSS-028（框架已落地，覆盖待补）**：保留真实内核套件、协议、失败/panic/timeout 自检；逐项补审计 T01～T12，特别是信号、坏指针、COW、资源长循环、失败回滚及确定性交错；纳入持续验收。
  - [x] 断言失败后继续执行的 case 不再误报 `invalid case start`；原始失败串口重放为 `failed/assertion`（3.56）。
  - [x] 缺少验证内核/initramfs 时装载前拒绝；取消后未运行套件保持非零退出并在 JUnit 标为 skipped（3.57）。
  - [x] ARM64 Debug `containers.smp` 同镜像逐次重放入口及当前 100 次检查（3.58）；历史 Zombie 后超时未据此关闭。
  - [x] 3.21 扩展 users.uaccess 为九项，九个 workflow/CTest 21/21、宿主 121/121；两份 RV64 Debug 超时继续保留，不能以之后矩阵通过关闭（3.21）。
  - [x] 3.20 新增 users.uaccess 后九个 workflow/CTest 21/21、宿主 121/121；原始报告及仍未验收的范围见 3.20。
  - [x] `3461c749` 提交前九个 workflow/CTest 21/21、宿主 121/121，通过范围和全部原始报告见 3.19；不是全部 T01～T12 验收。
  - [ ] 3.18 历史矩阵 CTest 20/21：RV64 Debug 的 `mm.transactions.map_allocation_rollback` 在 5.012 秒超时；报告 `1789058648884619000` 保留，本轮矩阵通过不关闭该间歇故障。
  - [x] 记录逐 case 宿主观测耗时及实际 deadline；四并发复现并修复 PFA 全 RAM 工作超过普通预算的问题，保留短超时拒绝和所有旧失败（工作区，3.10）。

## P2：能力契约与文档

- [ ] **MOSS-029（部分实现）**：x86 TSC/LAPIC 分别用 PIT 校准，RV64 使用 SBI TIME 且已有 `sstc=false` 验收；补缺失 SBI/计时设备、失败返回、时钟误差与 CPU 能力变化测试。
- [x] **MOSS-030（3.28～3.29 已关闭）**：内存/IPC 未实现操作返回 Unsupported，不返回固定地址、假成功率或假压力；初始化 Ready 只能在实例发布后可见。
  - [x] UnifiedMemoryManager 的 allocation-info、prefault、usage advice、reclaim/compaction、NUMA 与 huge-page 操作不再返回空成功；统一返回新增的 `MMError::NotSupported`。
  - [x] 初始化改为 `Uninitialized → Initializing → Ready → ShuttingDown` 原子状态机，实例指针先发布、Ready 后发布；确定性边界测试能在顺序变异时变红，恢复后通过（3.28）。公开原始 singleton 借用已删除，测试只接收发布边界的 opaque 指针。
  - [x] Buddy V2 的初始化/压缩/watermark/压力/碎片/统计，以及 Unified 的压力/性能/reset/history/leak 等占位接口返回明确的 `NotSupported`；未知压力使用 `UNKNOWN`，不再伪造 LOW、100% 或零统计。
  - [x] 旧 IPC 模块的共享区 create/destroy/stats/sync 只报告真实对象状态；其尚无进程地址空间所有权协议的 map/unmap/process-cleanup 明确返回 `NotSupported`，不再返回固定地址或空成功。新 capability 内存对象有独立的映射路径，不适用这条旧接口结论。三架构九 preset 的 `mm.unsupported_contracts` 均为 27/27，完整 CTest 43/43（3.29）。
- [ ] **MOSS-031**：收敛 UserAccess/AddressSpace/TrapFrame/ProcessResources/ExecLoader 等已有职责；明确 Moss ABI、号表、定长结构与错误语义，不把同名 syscall 宣称为 Linux/POSIX ABI。
- [ ] **MOSS-032（本文档部分已更新）**：持续同步代码、启动错误、统计和 pass/fail/skip；MOSS-030 的假能力契约已关闭，其他文档与验收缺口仍需继续同步。

依赖顺序及每阶段退出条件见 [审计实施顺序](moss-todo.md#11-实施顺序与可交付阶段)。各编号只有满足原验收条件后才整体关闭；上面的“已修复”子问题不免除同项剩余工作。

## 后续功能：保留需求，但不抢在 P0/P1 之前

- [ ] **KPTI / 高半区布局**：作为后续隔离加固；MOSS-001 的直接 U/S 权限错误和当前页权限验收已关闭，用户/内核页表拆分及入口切换仍需另行设计。
- [ ] **栈增长加固与 VM 兼容扩展**：已有 demand-zero 栈增长；补 guard/边界/冲突测试。文件后备 mmap、MAP_SHARED、部分 munmap 在 VM 事务稳定后实现。
- [ ] **x64 端口完善**：复用已有 IDT、MMU、APIC、timer、context switch、SYSCALL、fault 和 AP startup；先完成 007/014/019 等跨架构契约，001 的页权限隔离已验收，不再从 boot stub 重写。
- [ ] **RV64 端口完善**：复用已有 satp/trap/PLIC/SBI/context switch/ecall/HSM；先完成 007/009/014/019，按需求另增 AIA 等驱动。
- [ ] **同 ISA 通用镜像扩展与真机验收**：已有低于 4 GiB 的多 RAM bank 分配；继续精确回收启动区、细粒度 RAM/MMIO 映射、更多启动协议/设备与真机固件交接，以不变镜像 hash 验收，不能退回 virt/板名编译矩阵。
- [ ] **块设备与持久文件系统**：按 ADR-0015/0018 在隔离服务实现块设备与持久文件系统；virtio-blk 可作为首个可验证设备契约，不是内核对 QEMU 的依赖。
- [ ] **网络设备与协议栈**：按隔离服务边界实现设备驱动（可先 virtio-net）、Ethernet/ARP/IP/UDP 与实际 socket 路径。
- [ ] **隔离驱动资源契约**：内核只保留启动所需机制；补 capability 限定的 MMIO/端口、IRQ、DMA 与独占复位权限，再建立用户态枚举/绑定、故障回收及专项验收。旧 `DeviceManager` 不再是生产驱动扩展入口。
- [ ] **userspace libc/ABI 兼容**：已集成静态 mlibc，BusyBox 和验证程序使用现有用户态运行时；继续按实际程序需求补 syscall/errno/启动契约与不支持能力的明确返回，不重复实现一套最小 libc。
- [ ] **NUMA**：固件拓扑、per-node zones 与实际分配策略；现有策略/距离矩阵不是完整 NUMA。
- [ ] **Huge pages**：已有早期大块映射和管理接口；补用户大页分配/回收与 PMD/PUD 映射。THP/hugetlbfs 按需求单列。
- [ ] **Memory reclaim**：实际 LRU、可回收页和水位触发；依赖可信所有权及文件后备页等可回收对象。
- [ ] **Memory compaction**：迁移、PTE/TLB 更新和扫描/触发；先修空成功接口，再接真实算法。
- [ ] **共享内存 IPC**：真实页面/映射/引用及进程退出清理；选择 shm API 或 MAP_SHARED 后再公开用户能力。
- [ ] **多用户/权限**：复用 Process 已有 uid/gid/euid/egid 字段，补继承、鉴权、setuid/setgid 和文件权限，不再新增重复凭据字段。

## `e7fe8c91` 内存修复的验证证据

- [x] 布局修复后运行 `uv run pytest -q scripts/tests`：**102 passed**（37.84 s），验证宿主工具与构建回归，不代表 102 个内核功能均已验收。
- [x] 布局修复后重建三架构 Debug/Release 并执行各自 CTest：Debug 各 2 项、Release 各 3 项，全部 15 个入口通过；日志 `build/<preset>/layout-{build,ctest}.log`，原始报告见 [布局修复证据](moss-todo.md#37-堆页表与-pfa-元数据所有权2026-09-06工作区)。
- [x] ARM64/RV64 Debug/Release 共 26 项固件输入检查符合预期；同配置镜像 hash 不变，正向真实耗尽，负向准确失败且不发布 ready。
- [x] `check_heap_layout.py` 的 6 项重叠布局负向检查符合预期；实际链接输入复用，默认产物不变，临时坏镜像明确启动失败。
- [x] RV64 Debug/Release 同镜像补跑 Sv39 的 heap/pfa，两套件通过；默认矩阵为 Sv48，串口模式与镜像 hash 均已核对。
- [x] 对照生产内核测试和 runner：默认功能是 resources/mm/pfa/heap/vfs/users 共 14 个用例；heap 5 项、pfa 2 项，进程仍只是一次 fork/exec/exit/wait，不是 1,000 次压力或完整信号测试。
- [x] 修复实测暴露的 CPU 就绪误报：统一等待从核发布 online、真实时钟超时，缺核不再假成功；ARM64 四核 heap 连续 10 次和 GICv3 16 核通过，保留原失败报告。
- [x] 9 个 workflow/test preset 的匹配由 `scripts/tests/test_artifacts.py` 回归覆盖；仅六个 Debug/Release 有上述运行证据。
- [x] RelWithDebInfo 三架构默认功能/框架 workflow 已在 `3461c749` 提交前完成（3.19）；完整审计/真机验收仍列于下一项。
- [ ] 审计 T01～T12 的完整覆盖、资源耗尽/故障注入、长时间 SMP 和真实硬件验收。

`c895c876` 阶段新增 containers 三项用例，当时为 7 套件/17 用例。该轮 CTest **14/15**：x64 Release 的 bench.allocate 一次校准失败，后续同镜像 10 次通过但不覆盖原失败，具体拒绝原因仍待定位（028/029/032）。历史报告见 [moss-todo.md](moss-todo.md) 第 3.8 节。

3.9 阶段新增 held_reader、reentry 和 containers.smp.interleaving，当时默认为 **8 套件/20 用例**，实际运行记录及未覆盖范围见第 3.9 节。完整默认集合至少需要 2 CPU；单 CPU 时显式选择单 worker 套件。三架构双 CPU 交错通过不等于长时间 SMP、运行队列竞争或所有管理器生命周期验收。

3.9 阶段六配置构建通过；串行 CTest **14/15**，RV64 Debug 的 pfa.exhaustion 超时，宿主 **104 项通过**。这些原始失败继续保留。

3.10 已用相同镜像的四并发实验复现预算不足：5 s 时 4/4 超时，20 s 诊断窗口下同样的完整校验耗时约 14.4 s 并通过。PFA 默认 case 预算改为 30 s，其他套件仍为 5 s；显式参数优先，CTest 外层留足 600 s。逐 case 耗时和实际预算写入报告，未删除断言或增加重试；宿主 **109 项通过**。详见 [moss-todo.md](moss-todo.md) 第 3.10 节。

3.11～3.12 新增默认 mm.permissions，当时为 **9 套件/25 用例**，覆盖真实内核/活动用户页表的 U/S、最终内核 W^X 及共享表创建/clone/回收。该轮六配置 build 与串行 CTest **15/15 通过**，宿主 **109 项通过**；原始报告和镜像 hash 见第 3.12 节。拆页后 x86 页表快照的 A/D 硬件状态误报已用红绿对照定位，只过滤 A/D、不忽略地址或权限改变。这不关闭 MOSS-001 的受控用户异常隔离、MOSS-002/003 或 MOSS-006/017 尚缺的生命周期与并发验收。

同轮补验 RV64 Debug/Release Sv39 权限/用户/SMP 用例、26 项固件内存输入与 6 项坏堆布局，均符合预期；各配置原始镜像保持不变。详见第 3.12 节，真机、RelWithDebInfo 和完整 T01～T12 仍未验收。

3.13 新增 vma_boundaries 和真实用户态 user_ranges，当时默认 **9 套件/27 用例**。该轮验证地址准入、跨 VMA 权限、有界字符串与旁路删除，不证明故障恢复；共享 uaccess 及信号返回的后续实现见 3.19～3.21。

3.21 默认功能集合为 **13 套件/48 用例**。VFS 和信号帧已复用共享安全复制；九配置均通过包含真实 PFA 耗尽、跨页短 I/O、信号帧写出/读回故障和 devfs 行为的九项 users.uaccess。完整报告、测试前置条件修正及仍未定位的超时见 [moss-todo.md](moss-todo.md) 第 3.21 节；MOSS-002/003 整项仍打开。

原始通用启动/同镜像记录见 [generic-boot-acceptance.md](docs/generic-boot-acceptance.md)；更早的 [kernel-validation-acceptance.md](docs/kernel-validation-acceptance.md) 已标历史，其旧预设命令不再使用。后续关闭任务需附修复提交、对应原始结果和未覆盖边界，不能只改勾选。
