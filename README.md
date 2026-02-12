# Moss

A modern multi-architecture hybrid kernel (混合内核) operating system supporting ARM64, x86_64, and RISC-V architectures. Built with C++26.

## Quick Start

```bash
# Build all architectures
uv run build.py

# List available build presets
uv run build.py list
```

## Testing

Built kernels can be tested using QEMU with the generated run scripts in `build/*/run_qemu.sh`.
