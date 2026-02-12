# CLAUDE.md

Moss - 现代化多架构混合内核操作系统，支持ARM64/x86_64/RISC-V，基于C++23和Clang 21构建。

## 快速开始

```bash
# 构建和测试（最常用命令）
./build.sh --preset debug        # 构建ARM64 debug版本
./build.sh --main --debug        # 构建主要架构(ARM64+x86_64)
make test                         # 在QEMU中运行内核
make debug                        # 启动GDB调试

# 清理重建
./build.sh --all --clean         # 清理所有架构并重建
```

## 核心架构

内核采用分层模块设计：

```
src/
├── containers/     # 基础层：无锁队列、RCU链表、Slab分配器
├── mm/            # 内存管理：页表、物理内存
├── drivers/       # 硬件抽象：设备管理器、中断控制器
├── interrupts/    # 中断处理
├── ipc/           # 进程间通信：共享内存、零拷贝通道
├── process/       # 进程管理：CFS调度器、负载均衡
├── kernel/        # 内核核心：汇聚所有功能
└── boot/          # 启动代码：独立启动层
```

## 多架构支持

- **架构抽象层**: `src/include/arch/arch_abstraction.hpp`
  - 内存屏障、CPU操作、MMU管理、调试支持
- **架构特定代码**: 使用 `#if defined(MOSS_ARCH_ARM64)` 等宏
- **CMake预设**: `presets/arch/{arm64,x86_64,riscv}.json`

## 开发工作流

### 添加新功能
1. 确定属于哪个模块层（containers→mm→drivers→ipc→process→kernel）
2. 在`src/{module}/`添加实现
3. 更新对应的`CMakeLists.txt`
4. 使用`./build.sh --main --debug`验证

### 架构特定代码
- 条件编译：`#if defined(MOSS_ARCH_ARM64)`
- 汇编代码：放在对应架构目录
- 统一接口：通过`arch_abstraction.hpp`

## 技术要点

- **编译器**: Clang 21，C++23标准
- **链接器**: LLD
- **内存模型**: freestanding环境（无栈保护/异常/RTTI）
- **构建时间**: ~1秒（ARM64，24核系统）

## 构建输出

```
build/arm64-qemu-debug/
├── bin/moss.elf     # 内核ELF二进制
├── moss.bin         # 原始二进制镜像
├── moss.dis         # 反汇编文件
├── moss.sym         # 符号表
└── run_qemu.sh      # QEMU运行脚本
```

## 常见问题

- **工具链**: 确保安装Clang 21和LLD
- **权限**: `chmod +x build.sh`
- **调试**: 使用`--verbose`查看详细输出
- **配置**: 动态生成到`build/{preset}/include/config/config.h`