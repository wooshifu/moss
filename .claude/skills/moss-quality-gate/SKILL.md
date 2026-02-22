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
uv run scripts/format.py lint
```

Runs clang-tidy (C++) and ruff check (Python). All clang-tidy warnings are errors (`WarningsAsErrors: '*'`).

If there are violations, fix them. For auto-fixable issues:

```bash
uv run scripts/format.py lint --fix
```

After `--fix`, **always rebuild** (Step 1) to confirm the fixes compile, then re-run lint without `--fix` to verify clean.

### Step 3 — Format

```bash
uv run scripts/format.py format --check
```

Runs clang-format (C++), cmake-format (CMake), and ruff format (Python) in check mode.

If files need formatting:

```bash
uv run scripts/format.py format
```

Then re-run `format --check` to verify clean.

## Quick Reference

| Goal | Command |
|------|---------|
| Full quality check | Build, then `lint`, then `format --check` |
| Auto-fix lint issues | `uv run scripts/format.py lint --fix` |
| Auto-fix format issues | `uv run scripts/format.py format` |
| C++ only lint | `uv run scripts/format.py lint --cpp-only` |
| Python only lint | `uv run scripts/format.py lint --py-only` |

## Rules

| Rule | Reason |
|------|--------|
| Build before lint | clang-tidy reads `.pcm` files; stale modules cause false positives |
| Lint before format | clang-tidy `--fix` may change code that needs reformatting |
| Never skip checks | `WarningsAsErrors: '*'` means CI will reject unclean code |
| Rebuild after `--fix` | `--fix` modifies source but not `.pcm`; rebuild refreshes module AST |

## What Gets Checked

**clang-tidy** (59 checks): identifier naming, braces, bugprone patterns, modernize idioms, designated initializers, and more. Full list in `.clang-tidy`.

**clang-format**: Code formatting per `.clang-format`.

**cmake-format**: CMake file formatting per `.cmake-format.yaml`.

**ruff**: Python linting and formatting per `pyproject.toml`.
