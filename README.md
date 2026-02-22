# Moss

Moss is a modern multi-architecture hybrid kernel operating system supporting ARM64, x86_64, and RISC-V architectures. Built with C++26 using latest techniques, it emphasizes performance, modularity, and a clean codebase.

## Quick Start

```bash
# Build all architectures
uv run build.py

# List available build presets
uv run build.py list
```

## Testing

Built kernels can be tested using QEMU with the generated run scripts in `build/*/run_qemu.sh`.
