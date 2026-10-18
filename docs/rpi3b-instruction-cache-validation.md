# Raspberry Pi 3B+：可执行页发布与 Loader 启动

2026-10-18，以 `20b42f9` 加本批改动构建。生产及物理验证镜像使用
Clang 23.1.2、ARM64 Release `-g0 -Os -DNDEBUG`、PMU OFF。
板型为 Raspberry Pi 3 Model B Plus Rev 1.3，四核、948 MiB firmware ARM RAM；
J-Link `000050120677`、115200 8N1 UART；功能与最终恢复部署为 1 MHz JTAG，
性能对照在首次回读失败后使用 500 kHz。
每次部署先回到 U-Boot EL2h，完成 cache-off、全量上传和完整回读。

## 根因与修复

第六批旧生产镜像在 Loader Service 重启时出现用户缺页。独立复现分别在
第四次、第一次和第二次重启失败，保留最早串口与原始判断。第二次完整故障为
`addr=0x1203216200 pc=0x20002c6d0 write=true`、`ESR=0x92000045`。
JTAG 在 fatal handler 入口保留 TrapFrame、栈和实际指令；当前 ELF 在该 PC
为 `mov x9,xzr`，与访存异常不符。OpenOCD 读内存可能影响缓存，不能据此
排除缓存一致性，也不能把单步重试正常当作修复。

普通 ELF 的可执行 VMA 在首次 dispatch 之后才由 demand fault 填充。
原 dispatch 的本核 `ic iallu` 早于写入，无法发布随后通过 kernel direct-map
写入的指令，也无法覆盖迁移后的其他核。缺页处理直接发布 RX PTE，遗漏了
D-cache 到 I-cache 的同步。原生域构造另有逐行 CIVAC，但依赖首次 dispatch
的本核失效，两个入口没有共享完整的发布契约。

本批新增 ARM64 `arch::publish_instruction_range()`：按 `CTR_EL0.DminLine`
清写入 alias 的 D-cache 到 PoU，依次执行 `DSB ISH → IC IALLUIS → DSB ISH → ISB`，
然后才安装可执行 PTE。普通 demand resolver 与 `sys_domain_spawn` 共用该路径。
代码发布覆盖 inner-shareable CPU，避免物理 frame 复用后取到旧指令。
普通数据页和普通 syscall 不增加缓存维护；原生域用 CVAU 替代 CIVAC，保留写端
D-cache line，并将清理要求限定到 PoU。本批仍逐页执行全 I-cache 失效，
大镜像装载的批量发布优化需另有测量和事务契约。

## 真机红绿回归

新增 `users.vm/instruction_publication`，catalog v13。测试专用控制建立 RX VMA，
首次真正的 EL0 fetch 经生产 demand resolver 填入返回 37 的代码，并在 CPU0/CPU1
执行。CPU0 清除 PTE/TLB、释放 frame，重新通过同一 resolver 填入返回 41 的代码，
必须断言重用了同一物理 frame；CPU0、CPU1、回到 CPU0 均须返回 41，最后清理映射。
没有给生产 ABI 或用户态新增可写代码 alias。

红例仅还原三个生产文件，保留同一测试并构建独立快照；构建后恢复新源码。
红绿 fixture 字节完全相同，正常生产 kernel 的红例也与第六批字节相同。

| 运行 | 结果 |
| --- | --- |
| 旧生产 kernel，Pi Loader 重启 | 三次独立复现分别在第 4、1、2 次失败 |
| 新生产 kernel，Pi Loader 重启 | 50/50；每次要求服务换代、新 shell、独立 marker 与下一 prompt，无用户 fault |
| 旧实现 + 新回归，Pi users.vm | 前四 case 通过；新 case 13 passed、1 failed，用户 mask `0x104`；其余五 case not_run |
| 相同旧实现，QEMU ARM64 users.vm | 10/10，新 case 14/14；QEMU 未暴露物理指令缓存故障 |
| 新实现，Pi users.vm | 10/10，新 case 14/14；CPU0/CPU1 及回迁检查全部通过 |

物理 runner 同时接通 `users.vm`。BCM2836 中断控制器的 pending 寄存器会变化，
隔离 fixture 在确认 UART 类型为 PL011 后改读 immutable Peripheral ID0，
偏移 `0xfe0`，依据 Arm DDI0183G。GIC、APIC、PLIC 分支保留；Pi `kernel_mmio`
真实用户态禁止读写检查 57/57，不能把未映射地址当作权限拒绝。

| 文件 | SHA-256 |
| --- | --- |
| 旧生产 kernel | `2c6eb12755952f5a95bafcc0a36815ab790e76ce13b455209233e2abba58946f` |
| 新生产 kernel | `5cd1adb36c921bff738e89dba39ad21ba6f87fd38e1fce8310d1849db61fb4e7` |
| 正常生产 initramfs | `839e2f1d42fc0ff96e5851a8db283255ebbe936c6b6d375cb20cae656c15f8ad` |
| 红例验证 kernel | `594b12d6c0e89c7220b83cb311bd3812e5f475f634b8469fada49c19662b528e` |
| 绿例验证 kernel | `3186571f3321ab2380dac51c2cd8fee95c76ccffd4f49517d8c44839669e23ab` |
| 红绿共用验证 fixture | `c3cff96a1b39a1f4fade7e4c4125b2ce1b9a0f6fc002901210fecf7900f11a57` |
| 绿例匹配 symbols | `a68955c76839f679897f48b33b4371a781b813dba1bcac4ed6e8296bf585c5c6` |

绿例 source provenance 为 `recorded`、revision `20b42f90a8897ac742b028cc015246a2834420ef`、
dirty true、source SHA-256 `00b17efa4a1ecc5f76b43e213080286b9ca5258845c04f98c0b14ea5a3626e2a`。
红例快照遗漏 provenance sidecar，报告诚实保留 `unrecorded`；另存构建日志、
source patch 和精确二进制身份，不以事后源码哈希补造构建来源。

## 构建、回归与首次失败

ARM64 Release、ARM64/x64/RV64 Debug 构建通过。宿主验证协议、Pi、性能采集和
artifacts 回归为 **254 passed、1 skipped**。定向 ARM64 clang-tidy **3 TU**、
Ruff、clang-format 和差异检查通过，不表示全仓 lint。
首次 fixture 编译因 `PAGE_SIZE` 名称歧义失败，使用既有 `page_size` 后才构建成功，
原失败日志保留。

六套件为 `users.vm`、`mm.permissions`、`mm.transactions`、`users.exec`、
`users.frame`、`users.ipc`，每架构 88 case。ARM64/x64 Debug 均 **88/88**。
RV64 首轮为 **82 passed、1 timeout、5 not_run**；
`mm.transactions/clone_allocation_rollback` 达到原 **5 秒**期限，
GDB 在 `PagePressure` 析构的 order-10 `PageFrameAllocator::free_pages` 引用计数扫描；
其余 CPU idle。保留现场和后续 not_run，不放宽期限，也不把该现象断言为死锁。
这与前批压力释放现场相近，但是否同一根因继续开放。

同镜像隔离诊断中，512 MiB `mm.transactions` 12/12，原 2 GiB 两次各 12/12，
都保留原 5 秒 case 期限。clone 压力 case 分别约 0.264、1.037、0.933 秒，
符合全内存扫描随页数增加的趋势；不能据此把首轮超时归因于宿主或关闭历史失败。
这些是 QEMU 主机观测时间，不是 Pi 内核性能。

## Release 成本对照

同板顺序为修复前、修复后，各启动两轮；两版 PMU OFF，使用字节一致的固定服务
归档和探针，服务 wrapper 要求成功固定 CPU2，探针固定 CPU0。归档 SHA-256 为
`13109e659f832d03b635a2a4ae6286f8fbcf90c210ee4220e22f7d831d1f03f3`。
沿用探针每轮两个预热、七个保留批次，并要求全部行、DONE、真实退出码 0 和下一 prompt。
温度 42.932–43.470°C，只在轮次前后通过 CPU3 读取，计时期间不 halt。

下表为每轮七个批次平均耗时的中位数；fault 包含 1 MiB 的 mmap、逐页触碰和 munmap，
fork_libc 包含 fork+wait，exec 包含 fork+exec+wait。均不是单个缺页或函数的独占成本。

| 场景 | 修复前两轮 | 修复后两轮 |
| --- | --- | --- |
| raw getpid（µs/op） | 0.898 / 0.893 | 0.893 / 0.897 |
| 1 MiB 匿名映射（ms/op） | 2.518 / 2.518 | 2.510 / 2.514 |
| libc fork+wait（ms/op） | 1.815 / 1.754 | 1.758 / 1.754 |
| fork+exec+wait（ms/op） | 9.658 / 9.663 | 9.711 / 9.724 |

本次 exec 观测约增加 0.6%；没有回切或随机交错，fork 首轮也有漂移，不能将全部差值
归因缓存维护。修复前镜像已有正确性缺陷，其耗时仅供成本参考。原始批次和参数保留，
不把这些结果称为单次 p99、Linux 同板比较或整体高性能达标。

首次 1 MHz 基线部署在 initramfs 完整回读发现 106,942 个不同字节，进入 kernel 前
停止，保存了不匹配副本。复位后改 500 kHz，在新目录完整上传/回读匹配；修复版也使用
500 kHz。基线默认启动收到完整独立结果行及下一 prompt，但随后 supervisor 日志使
缓冲区不再以 prompt 结尾，原检查仍按 90 秒失败。原记录不改判；另发换行、独立 marker
并等下一 prompt，恢复通过后才完成上表采集。该 host runner 误报需单独修复。
一次过早启动的 collector 被 UART 独占保护拒绝，没有计时样本；失败 JSON 及采集记录保留，
后续两轮独立记录。修复版默认启动检查通过，两版采集均无用户 fault 或探针错误。

最终已恢复上表普通生产 kernel/initramfs，1 MHz 全量上传及完整回读匹配，
默认 `--check` 得到独立结果行与下一 prompt，记录为 `shell_input_passed`。
只读确认四个 AArch64 CPU 均为 running 后关闭 OpenOCD，控制端口关闭，
server 正常退出，UART 无 reader。首次收尾脚本误把 MEM-AP 行计入 CPU 数，
仅断言失败，未 halt 或 shutdown；改按 AArch64 类型筛选后完成，原失败另存。
本次为 RAM 部署，断电不会保留。

[机器记录与证据包身份](validation/rpi3b-instruction-cache-2026-10-18.json) 汇总红绿、
QEMU、Release 成本和恢复结果；[文字证据包](validation/rpi3b-instruction-cache-2026-10-18-evidence.tar.gz)
保留原始日志、源码 patch、构建记录、首次失败和 file-index。包含二进制、回读副本及
symbols 的完整证据包位置、大小和 SHA-256 记录在同一 JSON 中。
