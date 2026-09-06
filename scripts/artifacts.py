"""Build artifacts shared by boot/deployment tools, independent of any runner."""

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

BOOT_PROTOCOLS = {"ARM64": "linux-image", "RISCV": "sbi", "X86_64": "xen-pvh"}


@dataclass(frozen=True)
class Artifacts:
    manifest: Path
    arch: str
    boot_protocol: str
    build: dict[str, Any]
    files: dict[str, Path | None]

    @classmethod
    def load(cls, path: Path) -> "Artifacts":
        path = path.resolve()
        data = json.loads(path.read_text())
        if not isinstance(data, dict) or type(data.get("schema_version")) is not int or data["schema_version"] != 1:
            raise ValueError("unsupported artifact manifest version")
        target, build, artifacts = (data.get(name) for name in ("target", "build", "artifacts"))
        if not all(isinstance(value, dict) for value in (target, build, artifacts)):
            raise ValueError("target, build and artifacts must be objects")
        arch, protocol = target.get("architecture"), target.get("boot_protocol")
        if not isinstance(arch, str) or arch not in BOOT_PROTOCOLS or protocol != BOOT_PROTOCOLS[arch]:
            raise ValueError(f"unsupported architecture/boot protocol: {arch}/{protocol}")
        if build.get("type") not in ("Debug", "Release", "RelWithDebInfo"):
            raise ValueError("invalid build type")
        files = {}
        for name in ("kernel", "debug_symbols", "initramfs", "validation_kernel", "validation_initramfs"):
            if name not in artifacts:
                raise ValueError(f"missing artifact field: {name}")
            value = artifacts[name]
            if value is None and name not in ("kernel", "debug_symbols"):
                files[name] = None
            elif isinstance(value, str) and value and not Path(value).is_absolute():
                resolved = (path.parent / value).resolve()
                if not resolved.is_relative_to(path.parent):
                    raise ValueError(f"artifact escapes manifest directory: {name}")
                files[name] = resolved
            else:
                raise ValueError(f"invalid relative artifact path: {name}")
        return cls(path, arch, protocol, build, files)

    def require(self, name: str) -> Path:
        path = self.files[name]
        if path is None or not path.is_file():
            raise ValueError(f"artifact {name} is not built: {path}")
        return path
