# nproc / free：设计、实现与验证

## 原有缺口与数据链

- 现有 BusyBox 已有 applet，但 Moss profile 没有编译 `nproc`、`free`。
- `nproc` 默认读取 `sched_getaffinity`，`--all` 在上游扫描 sysfs；Moss 没有 sysfs。
- 新线程的亲和性默认允许全部位，原生 getaffinity 原先直接返回低 32 位，必须与实际上线掩码相交。
- mlibc 未接通 affinity、sysinfo 和动态 sysconf，CPU 查询会回退成 1。
- `free` 原先同时依赖 sysinfo 和 `/proc/meminfo`；Moss 没有 procfs。
- 物理页分配器已按固件内存布局建立可管理页池，并在分配/释放时更新计数；统计受同一把锁保护。
- 工作区并行新增了 `moss/monitor.h`、`SYS_SYSTEM_INFO`。复用该 ABI，不另建统计入口。

## 实现顺序与范围

1. 在现有系统快照补充实际上线 CPU 数和进程数，保留已定义字段；统计使用 `PageFrameAllocator`、boot 在线掩码和进程管理器。
2. 修正 native affinity 的长度和空指针检查；读取、设置时都排除未上线 CPU。保留原生 u32 掩码契约。
3. mlibc 实现 sysinfo/sysconf/affinity；沿用已有 CPU set 和 sysinfo 公共类型、格式转换。CPU set 的高位必须清零，避免 BusyBox 数到未初始化位。受 Process Service 管理的外部 PID 暂返回 ENOSYS，避免误操作同号内核 PID。
4. 启用 BusyBox applet、长选项和对应源文件。`nproc --all` 从 sysconf 获取固件发现数；`free` 保留标准单位换算，用实际空闲 RAM 作为保守 available 值。
5. 增加 libc 与 BusyBox 的真实 guest 用例、生产 shell 检查；在不同 CPU/RAM 配置及三种架构运行。

## 统计口径与限制

实际调用链：

- `nproc` → `sched_getaffinity` → Moss `GetAffinity` → `SYS_SCHED_GETAFFINITY` → 线程 `CpuBitmap` 与 `boot::online_cpu_mask` 相交。
- `nproc --all` → `sysconf(_SC_NPROCESSORS_CONF)` → `SYS_SYSTEM_INFO` → 固件发现后的 `g_num_cpus`。
- `free` → `sysinfo` → Moss `Sysinfo` → `SYS_SYSTEM_INFO` → `PageFrameAllocator::get_memory_stats()`；页数在内核按 `PAGE_SIZE` 换成字节。
- `get_nprocs()` / `_SC_NPROCESSORS_ONLN` 使用实际在线掩码计数；`_SC_PHYS_PAGES` / `_SC_AVPHYS_PAGES` 复用同一内存快照。

- total 是物理页分配器管理的 RAM，不是固件报告的所有地址范围；已排除内核、固件保留区、分配器元数据和启动归档。
- free 随物理页分配/释放变化，used = total - free；单纯保留虚拟地址不会被当成已用物理页，测试必须实际触页。
- 当前没有 swap/highmem 池和可回收块/页缓存，相关字段为零。共享映射仍计入实际使用的物理页，尚无独立 shmem 分类统计；available 不包含不可回收内存。
- 配置 CPU 数来自固件；在线数来自各 CPU 完成运行时初始化后的发布；默认 nproc 还受当前线程 affinity 限制。
- 不为这两个命令引入 procfs/sysfs，不扩展交换、缓存或 CPU 热插拔机制。

## 验收证据

- 核心 ABI：快照大小错误、空地址、未映射地址及跨页输出安全失败；固定字段写出完整，周围哨兵不变。
- CPU：不同 QEMU 核数对应不同结果；缩小 affinity 后 nproc 变小，`--all` 保持配置数；`--ignore` 遵循 BusyBox 最少输出 1 的语义。
- 内存：不同 RAM 配置总量变化；匿名映射触页后 free 下降，解除映射后回升；字节/KiB/MiB/人类可读输出一致。
- 集成：构建 ARM64、x64、RISC-V；运行 libc 和 BusyBox guest suite；生产 ash 直接运行命令并记录结果。

## 验证结果（2026-10-16）

以下均来自真正启动的 QEMU guest，执行的是内核原生 syscall、mlibc 和 BusyBox applet。每组 `users.sysinfo` 的 snapshot、affinity、memory、commands 四项均通过。

| 架构 / 配置 | 配置 CPU / 在线 CPU | 管理的物理内存 total（字节） |
| --- | --- | --- |
| ARM64，1 CPU / 256 MiB | 1 / 1 | 251203584 |
| ARM64，4 CPU / 768 MiB | 4 / 4 | 785977344 |
| x64，4 CPU / 512 MiB | 4 / 4 | 519467008 |
| RISC-V，2 CPU / 512 MiB | 2 / 2 | 519450624 |

每组实测分配并触碰 65536 字节后，空闲内存恰好下降 65536 字节；`munmap` 后恢复原值。命令测试还覆盖了缩小 affinity 后 `nproc` 变为 1、`--all` 保持配置数、`--ignore` 的下限，以及 `free` 默认和 `-b/-k/-m/-g/-h` 输出。

可复现命令（构建产物目录可替换）：

```sh
uv run scripts/kernel_validation.py run \
  --manifest build/nproc-free-arm64/moss-artifacts.json \
  --workload users.sysinfo --cpus 1 --memory-mib 256 \
  --output build/nproc-free-results/arm64-1cpu-256m-new
```

原始报告保存于 `build/nproc-free-results/`，包括各组 `results.json`、逐例事件和 `serial.log`。运行器会复制并记录镜像哈希；报告目录不纳入版本控制。

回归验证：ARM64、x64、RISC-V 的 `users.libc`（各 2 项）、`users.busybox`（各 23 项）通过；ARM64 的 `users.signals`（35 项）通过。主机验证脚本测试通过 168 项。共享构建负载下曾出现用例/guest 超时，以及一次 x64 启动时钟校准失败；保留失败日志，随后以 30 秒用例、180 秒 guest 上限重跑回归并全部通过，未修改内核时钟校准逻辑。

三架构的生产 ash 均实际完成了 `MOSS_NPROC_OK` 和 `MOSS_FREE_OK` 检查，输出了真实的字节与人类可读内存表。完整生产套件的后续并行进程监控检查仍未通过：x64、RISC-V 出现缺失 `Sigsuspend` 的错误，ARM64 在 `top` 检查处输出用法及 `MOSS_PROCESS_ERROR`，均未完成整套流程。因此这些命令通过不能被表述成整套生产启动检查通过。

## 树莓派真机验证（2026-10-17）

在连接的 **Raspberry Pi 3 Model B Plus Rev 1.3** 上完成验证。真实固件 DTB 提供四个 CPU 节点和 994050048 字节（948 MiB）ARM RAM，启动串口确认四核在线。使用 `build/nproc-free-arm64` 重建当前工作区的 Debug 生产内核与用户程序，在生产 initramfs 中追加已有的 `libc_validation.elf`；没有修改生产启动入口。该镜像包含工作区的并行改动，不是仅包含本功能的隔离提交。

通过现有 `scripts/rpi.py` 执行 watchdog 复位、JTAG 重新连接、RAM 上传；内核、归档和 DTB 均完整回读比对一致，`deploy --check` 的真实 shell 输入检查通过。未写入 SD 卡。

| 真机检查 | 结果 |
| --- | --- |
| `system-snapshot` | 配置 CPU = 4，在线 CPU = 4；ABI 大小、空地址、无效地址、只读页和跨未映射页检查通过 |
| `system-affinity` | 缩小到单核、读取掩码、拒绝空掩码、恢复原掩码及错误参数检查通过 |
| `system-memory` | 触碰 65536 字节后 free 从 935743488 降至 935677952，释放后恢复 935743488 |
| `system-commands` | 单核亲和性下 `nproc` = 1、`nproc --all` = 4；`--ignore` 下限及 `free` 默认和 `-b/-k/-m/-g/-h` 全通过 |
| 生产 ash 直接执行 | `nproc` = 4，`nproc --all` = 4，`nproc --ignore=1` = 3；`free`、`free -b`、`free -h` 成功，结束后 shell 继续响应 |

四个 C worker 均返回测试协议的成功状态 **37**；直接 shell 命令均返回 **0**。主机记录共 11 个成功步骤，并核对完整状态行及下一次 shell 提示符，不能由输入回显误判成功。

真机 `free -b` 一次快照为 total = **968249344**、used = 30654464、free = 937594880 字节，满足 used + free = total，available = free。`free -h` 显示 total = **923.4M**。total 是物理页分配器管理的内存，已扣除启动及保留区域，不能与板上标称 RAM 或固件提供的 948 MiB 等同；不同进程运行时的 free 快照可以不同。

本次部署的 SHA256：

| 产物 | SHA256 |
| --- | --- |
| 内核 | `c7e0c663c1823e53efb66ec9df849b1cbb63206e5308bb3b72a4010958a5e374` |
| 含测试程序的 initramfs | `e932bd02d53edccbf9b0f7a6a06d8c36f898e0b93c2b7f389dc6a4f108f99998` |
| 部署 DTB | `592d326a53ccf736083286fce63d99768154e764ab1783bb3f631292f1be4a3c` |

原始证据位于 `build/nproc-free-rpi3b/`：`results.json` 记录检查结果、板型、镜像和日志哈希，`deploy/serial.log` 保留启动输出，各用例 `.log` 保留完整串口响应，`provenance.json` 和 `source.patch` 记录构建来源。该目录是忽略的本地产物；本节保留关键结果与部署哈希。部署本次含测试程序的归档后，可在板上直接运行：

```sh
/libc_validation.elf system-snapshot; echo $?
/libc_validation.elf system-affinity; echo $?
/libc_validation.elf system-memory; echo $?
/libc_validation.elf system-commands; echo $?
```

本次真机验收覆盖 `nproc` / `free` 及其底层统计和亲和性接口，未运行长期压力测试、完整生产回归或 SD 持久启动验证。共享内存分类、负载均值、swap 和页缓存的功能边界见上文；本次不新增这些子系统。

## 提交候选独立验证（2026-10-17）

为隔离并行的进程监控改动，在基线 `efc58e2` 的临时工作树中只加入本功能及必要的系统快照依赖。独立 ARM64 Debug 生产/测试镜像构建成功，4 CPU / 512 MiB QEMU 的 `users.sysinfo` 四项全部通过；主机验证脚本测试通过 163 项。新增套件将目录版本从 7 升至 8，版本相关三项测试再次通过。原始报告位于 `build/nproc-free-commit/guest-results/results.json`。

系统调用 179 保持与前述真机镜像一致；相邻的 domain inspection 槽位在此提交中保留为未实现，独立的进程监控工作后续补齐。此次独立复核未重新部署真机，前述硬件记录仍对应包含并行改动的已记录镜像哈希。
