"""Build artifacts shared by boot/deployment tools, independent of any runner."""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any

BOOT_PROTOCOLS = {"ARM64": "linux-image", "RISCV64": "sbi", "X64": "xen-pvh"}


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
        for name in (
            "kernel",
            "debug_symbols",
            "initramfs",
            "validation_kernel",
            "validation_initramfs",
            "validation_debug_symbols",
        ):
            # Old manifests remain usable, with explicitly unavailable test symbols.
            if name not in artifacts and name != "validation_debug_symbols":
                raise ValueError(f"missing artifact field: {name}")
            value = artifacts.get(name)
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

    def validation_identity(self) -> dict[str, Any]:
        identity = {"build": self.build}
        for name, field in (("validation_kernel", "image_sha256"), ("validation_initramfs", "fixture_sha256")):
            with self.require(name).open("rb") as stream:
                identity[field] = hashlib.file_digest(stream, "sha256").hexdigest()
        if self.files.get("validation_debug_symbols") is not None:
            with self.require("validation_debug_symbols").open("rb") as stream:
                identity["symbols_sha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
        return identity

    def validation_provenance(self) -> dict[str, Any]:
        image = self.require("validation_kernel")
        path = image.with_name(image.name + ".provenance.json")
        identity = self.validation_identity()
        if not path.exists():
            return dict(identity, source_status="unrecorded", revision=None, dirty=None, source_sha256=None)
        record = json.loads(path.read_text())
        if (
            not isinstance(record, dict)
            or type(record.get("schema_version")) is not int
            or record.get("schema_version") != 1
            or any(record.get(key) != value for key, value in identity.items())
            or record.get("source_status") not in ("recorded", "unrecorded")
        ):
            raise ValueError("validation provenance does not match the built artifacts; rebuild the validation target")
        if record["source_status"] == "recorded" and (
            not isinstance(record.get("revision"), str)
            or type(record.get("dirty")) is not bool
            or not isinstance(record.get("source_sha256"), str)
            or not re.fullmatch(r"[0-9a-f]{64}", record["source_sha256"])
            or not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", record["revision"])
        ):
            raise ValueError("invalid validation source provenance")
        if record["source_status"] == "unrecorded" and any(
            record.get(field) is not None for field in ("revision", "dirty", "source_sha256")
        ):
            raise ValueError("unrecorded validation provenance claims a source identity")
        return record

    def snapshot_validation(self, directory: Path) -> "Artifacts":
        """Keep every guest in a run on the same verified image and fixture."""
        provenance = self.validation_provenance()
        directory.mkdir(parents=True, exist_ok=False)
        files = dict(self.files)
        names = ["validation_kernel", "validation_initramfs"]
        if self.files.get("validation_debug_symbols") is not None:
            names.append("validation_debug_symbols")
        for name in names:
            source = self.require(name)
            target = directory / name / source.name
            target.parent.mkdir()
            shutil.copy2(source, target)
            files[name] = target
        snapshot = replace(self, files=files)
        image = snapshot.require("validation_kernel")
        image.with_name(image.name + ".provenance.json").write_text(
            json.dumps(dict(provenance, schema_version=1), indent=2) + "\n"
        )
        snapshot.validation_provenance()  # Fail closed if a build raced the copies.
        return snapshot


def record_validation_provenance(artifacts: Artifacts, source: Path) -> None:
    """Capture source identity after linking, alongside the exact image and fixture."""
    record = dict(
        artifacts.validation_identity(),
        schema_version=1,
        source_status="unrecorded",
        revision=None,
        dirty=None,
        source_sha256=None,
    )

    def git(*args: str) -> bytes:
        return subprocess.check_output(["git", *args], cwd=source, stderr=subprocess.DEVNULL, timeout=30)

    try:
        revision = git("rev-parse", "HEAD").strip().decode()
        digest = hashlib.sha256(revision.encode() + b"\0" + git("diff", "--binary", "HEAD", "--"))
        for name in sorted(git("ls-files", "--others", "--exclude-standard", "-z").split(b"\0")):
            if name:
                digest.update(name + b"\0")
                with (source / name.decode()).open("rb") as stream:
                    digest.update(hashlib.file_digest(stream, "sha256").digest())
        record.update(
            source_status="recorded",
            revision=revision,
            dirty=bool(git("status", "--porcelain")),
            source_sha256=digest.hexdigest(),
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        # Source archives can build without Git; they cannot claim a Git revision.
        pass
    image = artifacts.require("validation_kernel")
    path = image.with_name(image.name + ".provenance.json")
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(record, indent=2) + "\n")
    temporary.replace(path)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Record validation build provenance after linking")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    arguments = parser.parse_args()
    record_validation_provenance(Artifacts.load(arguments.manifest), arguments.source)
