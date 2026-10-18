# Raspberry Pi 3B+：shell 输入检查与异步日志

2026-10-18，基于 `c79f2c6` 修复主机 `scripts/rpi.py:console()` 的完成判定。
真实板沿用[第七批已完整回读的普通生产镜像](rpi3b-instruction-cache-validation.md)，
该镜像持续运行，本批没有重新构建或部署内核。

## 问题与契约

第七批性能基线启动实际收到了独立 `MOSS_RPI_INPUT_OK` 结果行和下一 `moss$ `，
随后 supervisor 输出 `ready since_init_ms=117 delta_ms=22`。旧逻辑要求当前
接收缓冲区以 prompt 结尾，因此按原 90 秒期限失败。原部署 JSON 仍为
`boot_check=failed`；独立恢复只证明后续输入通过，没有重写该 verdict。

修复后的检查依次要求：先收到初始 prompt 并发送探针；只从此后读取的字节中
接受独立结果行；该结果行之后出现下一 prompt 即完成。后续服务日志不撤销
已经完成的往返。发送探针时清空仅供判定的旧接收尾部，原始串口日志完整保留。
echo-only、嵌入其他行的 marker、结果前的 prompt 和发送前的旧结果都不能通过。
该逻辑位于共享 `console()`；生产部署的 `--check` 调用它。

## 主机红绿与真机

扩充现有 POSIX PTY 回归，保持检查的原一秒期限。分包用例在 console 读出并回显
每段后才写下一段，保证宿主确实执行了多次读取。

| 用例 | 旧实现 | 修复后 |
| --- | --- | --- |
| echo-only | 正确拒绝 | 正确拒绝 |
| 独立结果行及下一 prompt | 通过 | 通过 |
| prompt 后同包 supervisor 日志 | 超时误报 | 通过 |
| marker/prompt 分包并带后续日志 | 超时误报 | 通过 |
| 只有结果行 | 正确拒绝 | 正确拒绝 |
| prompt 在结果之前 | 正确拒绝 | 正确拒绝 |
| 其他日志行内嵌 marker | 正确拒绝 | 正确拒绝 |
| 初始 prompt 前残留旧结果 | 误接受 | 正确拒绝 |

旧实现 **3 failed、5 passed**，新实现八例全部通过。另从原失败串口文件截取首个
prompt 后的完整响应，字节相同地回放到旧/新 console；旧版一秒超时，新版通过。
响应 SHA-256 为 `4e8b04dd3f70c2f23affe448d23790dea8b54c700360754466892089ee5ec28f`。
这项回放是主机 PTY 证据。

同一新 runner 独占打开真实 Pi UART，发送换行取得新 prompt，再发输入探针，
收到独立结果行和下一 prompt，检查通过。运行结束已释放 UART，未 halt CPU。
这项是真机新往返；本次稳定运行阶段没有再次触发启动时的 supervisor 异步输出。

| 身份 | SHA-256 |
| --- | --- |
| 旧 runner | `44a08e4b530b002dede3f7c03d6d733c5f6194778ca0356b24f8e65a77b287a1` |
| 新 runner，PTY 回放与真机相同 | `3f4e98f4991b222f3322c072b0e18bdff5ec27f36385dfc7c234a9116ea6a825` |
| 持续运行的 ARM64 Release kernel，PMU OFF | `5cd1adb36c921bff738e89dba39ad21ba6f87fd38e1fce8310d1849db61fb4e7` |
| 普通生产 initramfs | `839e2f1d42fc0ff96e5851a8db283255ebbe936c6b6d375cb20cae656c15f8ad` |
| 部署时 `/chosen` 已处理的本板 firmware DTB | `8ad5e579a69a3ea6c043e9a6e6231d3e31d953f8ec5859fe79fe761795b2b407` |

相关宿主验证协议、Pi、性能采集及 artifacts 测试 **260 passed、1 skipped**；
Ruff check/format 与差异检查通过。主机判定修复没有改动内核执行路径，本批未重跑
内核构建、QEMU、JTAG 上传或性能测量。第七批 RV64 首轮超时、其他 IRQ 屏蔽窗口、
持续全双工及完整调度交接继续开放。

[机器记录](validation/rpi3b-shell-check-2026-10-18.json) 与
[文字证据包](validation/rpi3b-shell-check-2026-10-18-evidence.tar.gz) 保留旧失败、
八例红绿 PTY 原始日志、精确响应回放、新真机往返、源码 patch 和 file-index；
完整证据包的外部位置与哈希亦记录在 JSON 中。
