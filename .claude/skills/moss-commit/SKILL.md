---
name: moss-commit
description: This skill should be used when creating git commits, writing commit messages, staging changes, or preparing code for commit in the moss project. Also use when the user says "commit", "提交", asks to save changes, or when you need to create a commit after completing implementation work.
---

# Moss Commit Convention

```
Every commit message starts with [module][subsystem] in lowercase.
English only. No Co-Authored-By line.
```

## Pre-Commit: Quality Gate

**Before staging any files**, verify the `moss-quality-gate` skill passes:

1. `cmake --build build/arm64-qemu-debug` — zero warnings
2. `uv run lint.py --check` and `uv run ruff check .` — zero issues
3. `uv run scripts/format.py format --check` — zero diffs

Do not commit until all three pass. If any fail, fix and re-verify.

## Format

```
[module][subsystem] brief description in imperative mood

Optional body:
- Why this change was made (not what — the diff shows what)
- Impact on other modules or behavior
- Technical details worth preserving
```

The subject line is the only required part. The body is optional but encouraged for non-trivial changes.

## Rules

| Rule | Reason |
|------|--------|
| English only | Consistent with codebase language and git tooling |
| No `Co-Authored-By: Claude` | Project policy — never include AI attribution lines |
| Imperative mood, lowercase start | Matches git convention: "fix X" not "Fixed X" or "Fixes X" |
| `[module]` = top-level area | Maps to `src/` subdirectories: mm, boot, kernel, process, etc. |
| `[subsystem]` = specific component | Narrows scope: scheduler, allocator, page_table, arm64, etc. |

## Module Tags

Pick from these based on which `src/` directory the change primarily affects:

| Tag | Scope |
|-----|-------|
| `[mm]` | Memory management (page tables, allocators, page fault) |
| `[kernel]` | Kernel core (syscalls, ELF loader, main) |
| `[process]` | Process management (scheduler, load balancer, fork) |
| `[boot]` | Boot sequence and arch-specific startup |
| `[ipc]` | Inter-process communication |
| `[drivers]` | Device drivers (UART, etc.) |
| `[interrupts]` | Interrupt handling (GIC, exception vectors) |
| `[containers]` | Data structures (lists, queues, slab allocator) |
| `[core]` | Core modules (std, types, result, arch, platform) |
| `[abi]` | ABI definitions (linker symbols, extern C) |
| `[cmake]` | Build system changes |
| `[docs]` | Documentation, CLAUDE.md, skills |
| `[infra]` | Scripts, CI, Docker, tooling |
| `[cleanup]` | Refactoring that spans multiple modules |

For cross-cutting changes, use the most impactful module as `[module]` and `[cleanup]` or a descriptive subsystem as `[subsystem]`.

## Good vs Bad Examples

**Good** — imperative, explains why in body:
```
[mm][allocator] optimize slab allocation for multi-core systems

Per-CPU slab caches reduce cross-core contention by 40%. Each CPU
now maintains a local free list, falling back to the global pool
only when empty.
```

**Good** — concise single-line for small changes:
```
[core][std] replace hand-written type traits with Clang builtins
```

**Good** — cross-cutting cleanup:
```
[cleanup][extern-c] remove dead extern C declarations and unnecessary C linkage
```

**Bad** — past tense, no tags:
```
Fixed a bug in the memory allocator
```

**Bad** — too vague:
```
[mm][core] update code
```

**Bad** — includes AI attribution:
```
[kernel][main] add error handling

Co-Authored-By: Claude <noreply@anthropic.com>
```
