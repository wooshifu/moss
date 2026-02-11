# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

Moss 是一个现代化的多架构混合内核操作系统，支持 ARM64、x86_64 和 RISC-V 架构。项目使用 C++26 标准，并基于 Clang 21 构建系统，采用模块化设计架构。

## 构建系统

### 核心构建命令

```bash
# 使用构建脚本（推荐）
./build.sh --preset debug              # 构建默认debug版本 (ARM64)
./build.sh --main --debug              # 构建主要架构(ARM64, x86_64)的debug版本
./build.sh --arch arm64 --release      # 构建指定架构的release版本
./build.sh --all --clean               # 清理后构建所有架构

# 直接使用CMake
cmake --list-presets workflow          # 列出所有workflow预设
cmake --workflow --preset arm64-qemu-debug
cmake --workflow --preset x86_64-qemu-release

# 运行和调试
cmake --build --preset debug
make test                              # 在QEMU中运行内核
make debug                             # 启动GDB调试会话
```

### CMake预设继承体系

项目使用基于继承的CMake预设系统：
- `CMakePresets.json` - 主配置文件，包含架构特定预设
- `presets/arch/arm64.json` - ARM64架构预设（base-arm64 → arm64-base → arm64-qemu-debug/release）
- `presets/arch/x86_64.json` - x86_64架构预设
- `presets/arch/riscv.json` - RISC-V架构预设

每个架构都有完整的configure、build、workflow和test预设。

## 架构设计

### 模块化内核架构

内核采用分层模块设计，按依赖关系组织：

1. **基础层** (`src/containers/`) - 容器库和数据结构
   - 无锁队列 (lockfree_queue.hpp)
   - RCU链表 (rcu_list.hpp)
   - Slab分配器 (slab_allocator.hpp)
   - 原子类型和Per-CPU数据结构

2. **内存管理层** (`src/mm/`) - 依赖容器库
   - 页表管理 (page_table.hpp/cpp)
   - 物理内存管理

3. **硬件抽象层** (`src/drivers/`, `src/interrupts/`)
   - 设备管理器 (device_manager.hpp)
   - 中断控制器 (gic.hpp)
   - UART驱动 (uart_driver.hpp)

4. **进程间通信层** (`src/ipc/`)
   - IPC管理器 (ipc_manager.hpp)
   - 共享内存 (shared_memory.hpp)
   - 零拷贝通道 (zero_copy_channel.hpp)

5. **进程管理层** (`src/process/`)
   - CFS调度器 (cfs_scheduler.hpp)
   - 负载均衡器 (load_balancer.hpp)
   - 上下文切换 (context_switch.S)

6. **内核核心层** (`src/kernel/`) - 汇聚所有功能
7. **启动层** (`src/boot/`) - 独立的启动代码

### 多架构支持

#### 架构抽象层
`src/include/arch/arch_abstraction.hpp` 提供统一的架构抽象接口：
- **内存屏障操作**: memory_barrier(), read_barrier(), write_barrier()
- **CPU操作**: cpu_yield(), get_current_cpu_id(), get_timestamp_counter()
- **MMU管理**: setup_kernel_mmu(), flush_tlb(), flush_tlb_addr()
- **调试支持**: kernel_panic() - 各架构的断点指令

#### CMake架构支持
- `cmake/ArchSupport.cmake` - 多架构配置主模块
- `cmake/arch/{ARM64,X86_64,RISCV}.cmake` - 架构特定配置
- `cmake/platforms/QEMU_VIRT.cmake` - 平台特定配置

#### 架构特定实现细节
- **ARM64**: 支持Cortex-A57, ARMv8-A+LSE指令集
- **x86_64**: 禁用red-zone、SSE，适用于内核环境
- **RISC-V**: RV64IMAC指令集，medany代码模型

## 开发工作流

### 添加新功能
1. 确定功能属于哪个模块层
2. 在对应的 `src/{module}/` 目录添加实现
3. 更新相应的 `CMakeLists.txt`
4. 如果需要配置选项，在主 `CMakeLists.txt` 中添加 `option()`
5. 使用 `./build.sh --main --debug` 验证主要架构

### 架构特定代码
- 使用 `#if defined(MOSS_ARCH_ARM64)` 等宏进行条件编译
- 架构特定汇编代码放在对应架构目录
- 通过 `arch_abstraction.hpp` 提供统一接口

### 配置系统
项目使用动态配置生成系统：
- 配置模板：`src/include/config/config.h.in`
- 生成文件：`build/{preset}/include/config/config.h`
- 配置变量在主 `CMakeLists.txt` 中定义

### 调试和测试
- 使用 `./build.sh --preset debug-test` 运行测试
- QEMU运行脚本自动生成在构建目录
- GDB调试支持：`make debug` 启动调试会话
- 所有构建都会生成反汇编文件(`.dis`)和符号表(`.sym`)

## 技术特性

- **编译器**: 仅支持Clang 18+，推荐Clang 21
- **C++标准**: C++23，使用现代C++特性
- **链接器**: 统一使用LLD链接器
- **内存模型**: 无栈保护、无异常、无RTTI的freestanding环境
- **构建时间**: ARM64约1秒（基于24核系统）

## 构建输出结构

```
build/
├── arm64-qemu-debug/
│   ├── bin/moss.elf          # 内核二进制
│   ├── moss.dis              # 反汇编
│   ├── moss.sym              # 符号表
│   ├── compile_commands.json # 编译数据库
│   └── run_qemu.sh           # QEMU运行脚本
├── arm64-qemu-release/
├── x86_64-qemu-debug/
└── ...
```

## 故障排除

### 常见构建问题
- **工具链问题**: 确保安装Clang 21和LLD链接器
- **架构支持**: x86_64和RISC-V需要对应的启动代码实现
- **权限问题**: 构建脚本可能需要可执行权限 `chmod +x build.sh`

### 调试技巧
- 使用 `--verbose` 查看详细构建输出
- 构建失败时会生成日志文件，路径会显示在错误信息中
- 使用 `--dry-run` 预览将要执行的构建命令