---
name: moss-commit
description: Use when creating git commits in the moss project. Defines the required commit message format with [module][subsystem] tags, English-only requirement, and prohibited Co-Authored-By line. Use this skill whenever committing changes, writing commit messages, or preparing code for commit.
---

# Commit Message Format

**使用英文编写所有提交信息，格式如下：**

```
[module][subsystem] brief description of changes

Detailed explanation (optional):
- Explain the reason and impact of changes
- List important technical details
- Reference related issues or discussions
```

## Rules

- **必须使用英文**
- **禁止在提交信息中包含**: Co-Authored-By: Claude <noreply@anthropic.com>
- **module**: 主要模块名（如 smp, boot, mm, process, ipc, driver）
- **subsystem**: 具体组件（如 scheduler, allocator, driver）
- **description**: 使用祈使句，首字母小写

## Examples

```
[smp][scheduler] implement dynamic CPU load balancing

[boot][arm64] fix CPU topology detection using MPIDR register

[mm][allocator] optimize slab allocation for multi-core systems
```
