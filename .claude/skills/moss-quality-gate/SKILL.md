---
name: moss-quality-gate
description: This skill should be used before creating any git commit, after writing or modifying code, or when the user asks to check code quality, run lint, run format, or verify changes. Also use when the user says "check", "verify", "validate", or when you need to ensure code meets project standards before committing. This skill MUST be invoked before the moss-commit skill.
---

# Moss Quality Gate

```
Every commit must pass: build + lint + format. Zero errors, zero warnings.
Run all three checks before staging. Fix issues before committing.
```

## Workflow

Run these three checks **in order** before every commit. Stop and fix on first failure.

### Step 1 — Build

```bash
cmake --build build/arm64-qemu-debug
```

The project uses `-Weverything -Werror`. Any warning is a build failure. Fix all warnings before proceeding.

### Step 2 — Lint

```bash
uv run lint.py --check
uv run ruff check .
```

Runs system clang-tidy (C++, including modules) and ruff check (Python). All clang-tidy warnings are errors (`WarningsAsErrors: '*'`). The C++ entrypoint incrementally builds before checking to refresh BMIs; use `--preset <name>` for another configured architecture.

If there are violations, fix them. For auto-fixable issues:

```bash
uv run lint.py --fix
uv run ruff check --fix .
```

`lint.py --fix` collects replacements in parallel, applies them once, then automatically rebuilds and runs a final check. A nonzero exit means the operation did not finish cleanly.

### Step 3 — Format

```bash
uv run format --check
```

Runs clang-format (C++), cmake-format (CMake), and ruff format (Python) in check mode.

If files need formatting:

```bash
uv run format
```

Then re-run `format --check` to verify clean.

## Quick Reference

| Goal | Command |
|------|---------|
| Full quality check | Build, then `uv run lint.py --check`, `uv run ruff check .`, then `format --check` |
| Auto-fix C++ lint issues | `uv run lint.py --fix` |
| Auto-fix format issues | `uv run format` |
| C++ only lint | `uv run lint.py --check` |
| Python only lint | `uv run ruff check .` |

## Rules

| Rule | Reason |
|------|--------|
| Build before lint | The lint entrypoint updates BMIs; stale modules can hide source changes |
| Lint before format | clang-tidy `--fix` may change code that needs reformatting |
| Never skip checks | `WarningsAsErrors: '*'` means CI will reject unclean code |
| Rebuild after `--fix` | The lint entrypoint rebuilds before the final check to refresh module ASTs |

## What Gets Checked

**clang-tidy** (59 checks): identifier naming, braces, bugprone patterns, modernize idioms, designated initializers, and more. Full list in `.clang-tidy`.

**clang-format**: Code formatting per `.clang-format`.

**cmake-format**: CMake file formatting per `.cmake-format.yaml`.

**ruff**: Python linting and formatting per `pyproject.toml`.
